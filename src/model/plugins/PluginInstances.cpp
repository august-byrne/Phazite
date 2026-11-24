// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginInstances.h"
#include "devices/core/Clock.h"
#include "internal/DeviceStore.h"
#include "internal/PluginHost.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "sandbox/core/Vst3State.h"

#include <algorithm>
#include <set>

namespace phazite
{
namespace
{
    constexpr double defaultSampleRate = 44100.0;
    constexpr int openBlockSize = 512;     // what a new instance is opened for, before the engine prepares it
    constexpr int lookMilliseconds = 10;   // how often what opens in the background, and the states asked for, are looked at
    constexpr size_t maxReportsKept = 256; // the changes a plug-in told that it hasn't given its state for, at most

    // The program the song keeps a plug-in on, or -1 for none (by its ID: DeviceStore.h).
    int readPluginProgram (const Song& song, DeviceID id)
    {
        const auto program = getPluginProgram (song, id);
        return program.has_value() ? std::max (0, *program) : -1;
    }

    // The song's plug-ins, each by its ID with which plug-in it is, in the outline's order (Mixer.h: getSongDeviceOutline():
    // every track's, those in its containers' chains too, then the master's): one missed here would be made anew from what its
    // device stored at each change of the song, and never store what it keeps of its own.
    std::vector<DeviceOutline> findPlugins (const Song& song)
    {
        auto found = getSongDeviceOutline (song);
        std::erase_if (found, [] (const DeviceOutline& each) { return each.form != DeviceValue::Form::plugin; });
        return found;
    }

    // Which plug-in a device of the song's is, by its ID: none for one the song hasn't, or that is no plug-in.
    std::optional<PluginIdentity> findIdentity (const Song& song, DeviceID id)
    {
        const auto* device = findDevice (song, id);
        return device != nullptr && device->isPlugin() ? std::optional (device->plugin) : std::nullopt;
    }

    juce::String findName (const Song& song, DeviceID id)
    {
        const auto identity = findIdentity (song, id);
        return identity.has_value() ? identity->name : juce::String();
    }
} // namespace

// A plug-in opening in the background: what its job made, once done, which the message thread makes an instance of; and the
// program the song had for it as it began. A reload's besides: what it calls once done; whether it gives a crashed instance
// what opened in place of its remote (else it opens anew one that crashed as it loaded), and at what rate; and the states
// it chose between (reload()).
struct PluginInstances::Opening
{
    PluginHost::Opened opened;
    int program = -1;
    std::atomic<bool> done { false };

    Reloaded then;
    bool replaces = false;
    double sampleRate = 0.0;
    bool fromSong = false;
    juce::MemoryBlock own, tried;

    // A reload's: what the song held of it as the reload began (its program and values set by hand), which tells what
    // the song gave it meanwhile (finishReloading()).
    int songProgram = -1;
    std::map<juce::String, float> songValues;
};

namespace
{
    // What a reload calls, on the message thread, a message later: nothing of what it was called from is touched then.
    void tellReloaded (const PluginInstances::Reloaded& then, bool reloaded, const juce::String& error)
    {
        if (then != nullptr)
            juce::MessageManager::callAsync ([then, reloaded, error] { then (reloaded, error); });
    }
} // namespace

PluginInstances::PluginInstances (Song& s, PluginHost& h, double rate)
    : song (s),
      host (h),
      sampleRate (rate > 0.0 ? rate : defaultSampleRate)
{
    song.setDeviceStateStore (
        [this] (std::uint64_t track)
        {
            if (track == 0)
                storeAll();
            else
                storeTrack (TrackID::fromRaw (track));
        });

    if (auto* values = song.getValues())
        values->addListener (this);

    host.addChangeListener (this);
}

PluginInstances::~PluginInstances()
{
    // What still opens is abandoned (its jobs see their openings gone) and closed in the background: nothing waits for it.
    // What waits for the states is told that not all came, so that a close goes on.
    stopTimer();
    callWaiting (false);
    host.removeChangeListener (this);

    if (auto* values = song.getValues())
        values->removeListener (this);

    song.setDeviceStateStore ({});

    // Told of nothing more: what plays them may hold them a little longer.
    std::vector<std::shared_ptr<SandboxedInstance>> held;

    for (auto& [id, entry] : entries)
        held.push_back (entry.instance);

    for (auto& waiting : answering)
        held.push_back (waiting.instance);

    for (const auto& instance : held)
    {
        if (instance != nullptr)
        {
            instance->onParameterChanged = nullptr;
            instance->onParameterSetByProgram = nullptr;
            instance->onGesture = nullptr;
            instance->onStateChanged = nullptr;
            instance->onLatencyChanged = nullptr;
            instance->onStateMayChange = nullptr;
            instance->answerTextsAsGone();
        }
    }
}

void PluginInstances::setSampleRate (double rate)
{
    if (rate > 0.0)
        sampleRate = rate;
}

void PluginInstances::forgetJoinsLate()
{
    for (auto& [id, entry] : entries)
        if (entry.instance != nullptr)
            entry.instance->takeJoinLate();
}

std::shared_ptr<SandboxedInstance> PluginInstances::find (DeviceID id)
{
    if (const auto found = entries.find (id); found != entries.end())
        return found->second.instance;

    const auto identity = findIdentity (song, id);

    if (! identity.has_value())
        return nullptr;

    // In the background, as sync() opens them (known bug 166: one added opened while the message thread waited); at once for
    // a song no session plays, which a render's own thread opens.
    auto* entry = open (id, *identity, synced);
    return entry != nullptr ? entry->instance : nullptr;
}

bool PluginInstances::isLoading() const
{
    return std::any_of (entries.begin(), entries.end(), [] (const auto& entry) { return entry.second.opening != nullptr; });
}

void PluginInstances::whenLoaded (std::function<void()> then)
{
    if (then == nullptr)
        return;

    if (! isLoading())
    {
        then();
        return;
    }

    waitingForLoads.push_back (std::move (then));
    startTimer (lookMilliseconds);
}

bool PluginInstances::isLoading (DeviceID id) const
{
    const auto found = entries.find (id);
    return found != entries.end() && found->second.opening != nullptr;
}

PluginInstances::Entry* PluginInstances::open (DeviceID id, const PluginIdentity& plugin, bool inBackground, Reloaded then)
{
    auto& entry = entries[id];
    entry.instance = nullptr;
    entry.opening = nullptr;
    entry.error = {};
    const auto known = findKnownPlugin (host, toDescription (plugin));
    entry.wasKnown = known.has_value();

    if (! host.loadsPlugins())
    {
        entry.unloaded = Status::off;
        entry.error = "Plug-ins are off after Phazite closed unexpectedly.";
        tellReloaded (then, false, entry.error);
        return &entry;
    }

    if (! known.has_value())
    {
        entry.unloaded = Status::missing;
        entry.error = plugin.name + " isn't installed on this computer.";
        tellReloaded (then, false, entry.error);
        return &entry;
    }

    // From the state the song keeps of it; where that left another program than the song's, the song's program, then the
    // state again, so that the settings are never overridden by a program set after them (a plug-in's state needn't say
    // which program it is on: PluginHost::open()); then the values the song sets by hand under its automation.
    const auto stored = getPluginState (song, id);
    const auto program = readPluginProgram (song, id);

    if (inBackground)
    {
        auto opening = std::make_shared<Opening>();
        opening->program = program;
        opening->then = std::move (then);
        startOpening (entry, std::move (opening), { *known, stored, program, sampleRate, openBlockSize });
        return &entry;
    }

    std::unique_ptr<SandboxedInstance> made = host.createInstance (*known, stored, program, sampleRate, openBlockSize, entry.error);

    if (made == nullptr)
    {
        entry.unloaded = host.hasCrashedWhileLoading (*known) ? Status::crashedWhileLoading : Status::missing;
        tellReloaded (then, false, entry.error);
        return &entry;
    }

    setUp (*made, id, stored, program);
    entry.instance = std::move (made);
    listen (id, *entry.instance);
    tellReloaded (then, true, {});
    return &entry;
}

void PluginInstances::startOpening (Entry& entry, std::shared_ptr<Opening> opening, const PluginHost::Reopening& what)
{
    // In its helper, on a thread of the host's, after those before it in the same helper and beside the other helpers: the
    // song plays meanwhile without it (silent in its place: SongPlayer). Abandoned if this lets go of it first (the project
    // closed, the device taken away), before it opens or between its steps; what it opened for nothing is closed there and
    // then.
    entry.opening = opening;
    entry.unloaded = Status::loading;
    host.runInBackground (PluginHost::getHelperKey (what.description),
                          [wanted = std::weak_ptr<Opening> (opening), &plugins = host, what] (const BackgroundJobs::ShouldStop& shouldStop)
                          {
                              const auto abandoned = [&wanted, &shouldStop] { return wanted.expired() || shouldStop(); };

                              if (abandoned())
                                  return;

                              auto opened =
                                  plugins.open (what.description, what.state, what.program, what.sampleRate, what.blockSize, abandoned);

                              if (const auto kept = wanted.lock())
                              {
                                  kept->opened = std::move (opened);
                                  kept->done.store (true, std::memory_order_release);
                              }
                          });

    startTimer (lookMilliseconds);
}

void PluginInstances::setUp (SandboxedInstance& made, DeviceID id, const juce::MemoryBlock& openedFrom, int openedProgram)
{
    // What the song changed of it while it opened (an undo, a preset loaded): its program, then its settings, as the song has
    // them now, by its ID (DeviceStore.h).
    const auto stored = getPluginState (song, id);

    if (const auto program = readPluginProgram (song, id); stored != openedFrom || program != openedProgram)
    {
        if (program >= 0 && program != made.getCurrentProgram())
            made.setCurrentProgram (program);

        if (! stored.isEmpty())
            made.setStateInformation (stored.getData(), (int) stored.getSize());
    }

    // The values the song sets by hand under its automation, as it has them now.
    for (const auto& [parameter, value] : getPluginValues (song, id))
        if (const auto index = made.findParameter (parameter); index >= 0)
            made.setParameterValue (index, value);

    made.keepStateAsOpened();
}

void PluginInstances::finishOpening (DeviceID id, Entry& entry)
{
    const auto opening = std::exchange (entry.opening, nullptr);
    auto made = host.makeInstance (opening->opened);
    const auto& then = opening->then;

    if (made == nullptr)
    {
        entry.error = opening->opened.error;
        entry.unloaded = host.hasCrashedWhileLoading (opening->opened.description) ? Status::crashedWhileLoading : Status::missing;
        tellReloaded (then, false, entry.error);
        return;
    }

    // What the song set it to meanwhile (a preset, an undo, its values), as it has them now: what a window or a control set
    // while it loaded reaches it so.
    setUp (*made, id, opening->opened.state, opening->program);
    made->joinLate(); // the engine played silence in its place: its sound fades in
    entry.instance = std::move (made);
    listen (id, *entry.instance);

    if (then != nullptr)
        ++entry.generation; // a reload of one that crashed as it loaded: its windows make their editors again

    tellReloaded (then, true, {});
}

void PluginInstances::finishReloading (DeviceID id, Entry& entry)
{
    // What opened in a new helper, given to the crashed instance in place of its remote (the engine skips it meanwhile); one
    // that didn't open keeps the state it had before the reload chose another (reload()).
    const auto opening = std::exchange (entry.opening, nullptr);
    juce::String error;

    if (entry.instance == nullptr || ! host.replaceRemote (*entry.instance, opening->opened, opening->sampleRate, error))
    {
        if (entry.instance != nullptr)
            entry.instance->setStateInformation (opening->own.getData(), (int) opening->own.getSize());

        entry.reloadedFromSong = false;
        entry.ownState.reset();
        tellReloaded (opening->then, false, error.isNotEmpty() ? error : juce::String ("It didn't load."));
        return;
    }

    entry.reloadedFromSong = opening->fromSong;
    entry.ownState = opening->own;
    entry.songStateTried = opening->fromSong ? opening->tried : juce::MemoryBlock();
    entry.reloadedTime = devices::Clock::getMilliseconds();

    // What the song gave it while it reloaded (a preset loaded, an undo, a value set), which reached only the crashed
    // instance, as setUp() gives one opened in the background what the song set meanwhile: else it would play what it
    // reloaded from while Phazite holds the song's (the review of known bug 166). Compared with what it reloaded from, not
    // with what the song held as the reload began: a preset loaded and undone leaves the song as it was, but Phazite holds
    // the song's state, which needn't be the plug-in's own it reloaded from (its second review).
    auto& instance = *entry.instance;
    const auto program = readPluginProgram (song, id);
    const auto held = instance.getLastState(); // as the song gave it meanwhile, else what it reloaded from

    if (held != opening->tried || program != opening->songProgram)
    {
        if (program >= 0 && program != opening->program)
            instance.setCurrentProgram (program);

        if (! held.isEmpty())
            instance.setStateInformation (held.getData(), (int) held.getSize());
    }

    for (const auto& [parameter, value] : getPluginValues (song, id))
    {
        if (const auto before = opening->songValues.find (parameter);
            before == opening->songValues.end() || ! juce::exactlyEqual (before->second, value))
            if (const auto index = instance.findParameter (parameter); index >= 0)
                instance.setParameterValue (index, value);
    }

    ++entry.generation;
    tellReloaded (opening->then, true, {});
}

juce::StringArray PluginInstances::getLoading() const
{
    juce::StringArray names;

    for (const auto& [id, entry] : entries)
        if (entry.opening != nullptr)
            names.add (findName (song, id));

    return names;
}

void PluginInstances::timerCallback()
{
    // Those opened in the background, each coming in as it is ready: what plays the song makes its plan again, and the
    // views hear of it as of a plug-in reloaded.
    auto anyOpening = false, anyOpened = false;

    for (auto& [id, entry] : entries)
    {
        if (entry.opening == nullptr)
            continue;

        if (entry.opening->done.load (std::memory_order_acquire))
        {
            if (entry.opening->replaces)
                finishReloading (id, entry);
            else
                finishOpening (id, entry);

            anyOpened = true;
        }
        else
        {
            anyOpening = true;
        }
    }

    // The answers that came; then those that said they changed and are quiet since asked unasked, so that a store, which
    // doesn't wait, finds their own states (known bug 166). Looked at again while any is asked, or may be.
    takeAnswers();
    auto statesIn = true, anyUnasked = false;

    for (auto& [id, entry] : entries)
    {
        if (entry.instance == nullptr)
            continue;

        pruneReports (id, *entry.instance);

        // At once while a save waits: one it waits for whose answer (to an ask before it: a store's, or one unasked) is older
        // than its changes as the save asked, is asked again, once. The save waits for its own asks alone (known bug 166:
        // one asked unasked meanwhile held it off).
        const auto wanted = waitingForStates.empty() ? wantedStates.end() : wantedStates.find (id);
        const auto waitedFor = wanted != wantedStates.end();

        if (waitedFor && ! wanted->second.askedAgain && ! entry.instance->isAskingState()
            && ! entry.instance->hasGivenStateSince (wanted->second.changes))
        {
            wanted->second.askedAgain = true;
            entry.instance->askState (SandboxedInstance::Ask::all);
        }
        else if (entry.instance->shouldBeAsked())
        {
            entry.instance->askState (SandboxedInstance::Ask::changedAndAnswering, true);
        }

        statesIn = statesIn && ! (waitedFor && entry.instance->isAskingState());
        anyUnasked = anyUnasked || entry.instance->isAskingState() || entry.instance->isStateUnasked();
    }

    if (! anyOpening && ! anyUnasked && answering.empty() && (waitingForStates.empty() || statesIn))
        stopTimer();

    if (anyOpened)
    {
        if (listener != nullptr)
            listener->pluginLatencyChanged();

        host.sendChangeMessage();
    }

    // Last, what waited for the loads taken out first: what is called may let go of this.
    const auto loaded = anyOpening ? std::vector<std::function<void()>>() : std::exchange (waitingForLoads, {});

    if (! waitingForStates.empty() && statesIn)
        callWaiting (true);

    for (const auto& then : loaded)
        then();
}

void PluginInstances::callWaiting (bool allIn)
{
    // Each once, taken out first; and nothing of this touched after them, which one may let go of all the same.
    wantedStates.clear();

    for (const auto& then : std::exchange (waitingForStates, {}))
        then (allIn);
}

bool PluginInstances::askStates (bool all)
{
    return askStates (all ? SandboxedInstance::Ask::all : SandboxedInstance::Ask::changed);
}

bool PluginInstances::askStates (SandboxedInstance::Ask which)
{
    auto statesIn = true;

    // The plug-ins asked now are those a wait waits for (known bug 166: one asked unasked meanwhile isn't), each by its
    // count of changes as it was asked.
    if (waitingForStates.empty())
        wantedStates.clear();

    for (auto& [id, entry] : entries)
    {
        if (entry.instance != nullptr)
        {
            // An answer to an earlier ask taken in first, so that this one asks again (a store's or one asked unasked, which
            // may be older than the plug-in's changes since); one still to come is asked again, once, when it is in, if the
            // plug-in changed after it was asked (timerCallback()). Auto Save asks again one that didn't answer before,
            // which a store doesn't (store()).
            entry.instance->takeAskedStateIfIn(); // never given up on here: it would be asked again at once, for ever
            entry.instance->askState (which);

            if (entry.instance->isAskingState() && ! wantedStates.contains (id))
                wantedStates[id] = { entry.instance->countStateChanges(), false };

            statesIn = entry.instance->takeAskedState() && statesIn;
        }
    }

    return statesIn;
}

void PluginInstances::whenStatesIn (bool all, std::function<void (bool)> then)
{
    whenStatesIn (all ? SandboxedInstance::Ask::all : SandboxedInstance::Ask::changed, std::move (then));
}

void PluginInstances::whenStatesIn (SandboxedInstance::Ask which, std::function<void (bool)> then)
{
    if (askStates (which) && waitingForStates.empty())
    {
        then (true);
        return;
    }

    waitingForStates.push_back (std::move (then));
    startTimer (lookMilliseconds);
}

void PluginInstances::giveUpStates()
{
    // As for a time-out: the states the plug-ins gave last stay.
    for (auto& [id, entry] : entries)
        if (entry.instance != nullptr)
            entry.instance->takeAskedState (0.0, true);

    callWaiting (false);
}

juce::StringArray PluginInstances::getAsked() const
{
    juce::StringArray names;

    for (const auto& [id, entry] : entries)
        if (entry.instance != nullptr && entry.instance->isAskingState())
            names.addIfNotAlreadyThere (findName (song, id));

    return names;
}

juce::StringArray PluginInstances::getOlderStates() const
{
    juce::StringArray names;

    for (const auto& [id, entry] : entries)
        if (entry.instance != nullptr && entry.instance->isStateOlder())
            names.addIfNotAlreadyThere (findName (song, id));

    return names;
}

void PluginInstances::listen (DeviceID id, SandboxedInstance& instance)
{
    const auto refOf = [&instance, id] (int parameter)
    {
        const auto& infos = instance.getParameterInfos();
        return juce::isPositiveAndBelow (parameter, (int) infos.size()) ? ParameterRef { id, infos[(size_t) parameter].id }
                                                                        : ParameterRef {};
    };

    // A value it set itself: a VALUE of the song's takes it in, without an undo step, and the song tells what listens to it.
    instance.onParameterChanged = [this, id, refOf, &instance] (int parameter, float value)
    {
        const auto ref = refOf (parameter);

        if (! ref.isValid())
            return;

        {
            const juce::ScopedValueSetter<bool> writing (storing, true);
            setPluginValueOutsideHistory (song, id, ref.parameter, value); // by ID: it holds no element
        }

        // A change of its own (the helper tells none it was given), which the next save stores: an unsaved change.
        noteReport (id, instance);
        song.tellValueSetByHand (ref, value, true);
        lookSoon();
    };

    // A value a program the song set set: the song's own, as the program is (which the next save stores with the plug-in's
    // state), for what plays the song alone: no unsaved change, no undo step, and no one's touch.
    instance.onParameterSetByProgram = [this, refOf] (int parameter, float value)
    {
        if (const auto ref = refOf (parameter); ref.isValid())
            song.tellValueSetByProgram (ref, value);
    };

    instance.onGesture = [this, refOf] (int parameter, bool starting)
    {
        if (const auto ref = refOf (parameter); ref.isValid() && listener != nullptr)
            listener->pluginGestureChanged (ref, starting);
    };

    // Its own settings changed: an unsaved change, which the next save stores.
    instance.onStateChanged = [this, id, &instance]
    {
        noteReport (id, instance);
        lookSoon();
    };

    instance.onLatencyChanged = [this]
    {
        if (listener != nullptr)
            listener->pluginLatencyChanged();
    };

    // Asked for its state once it is quiet after a change, whoever made it.
    instance.onStateMayChange = [this] { lookSoon(); };
    lookSoon(); // one opened from no state of the song's is asked for its own
}

void PluginInstances::sync()
{
    // Those the song no longer holds go (what plays them may hold them until its plan is handed back).
    std::set<DeviceID> held;

    for (const auto& device : findPlugins (song))
        held.insert (device.id);

    // Told of nothing more as they go (known bug 166: one kept for an answer still to come told its changes to the song).
    std::erase_if (entries,
                   [&held] (const auto& entry)
                   {
                       if (held.contains (entry.first))
                           return false;

                       if (const auto& instance = entry.second.instance)
                       {
                           instance->onParameterChanged = nullptr;
                           instance->onParameterSetByProgram = nullptr;
                           instance->onGesture = nullptr;
                           instance->onStateChanged = nullptr;
                           instance->onLatencyChanged = nullptr;
                           instance->onStateMayChange = nullptr;
                           instance->answerTextsAsGone(); // a text typed for it is set on nothing
                       }

                       return true;
                   });

    // In the background, beside each other (known bugs 69 and 166): as the song opens, as one is added, and as an undo or a
    // redo brings one back.
    for (const auto& device : findPlugins (song))
        if (! entries.contains (device.id))
            open (device.id, device.plugin, true);

    synced = true;
}

std::optional<PluginInstances::Status> PluginInstances::getStatus (DeviceID id)
{
    if (! isPlugin (song, id))
        return {};

    const auto instance = find (id);
    const auto found = entries.find (id);

    if (found != entries.end() && found->second.opening != nullptr)
        return Status::loading; // a crashed one reloading too

    if (instance != nullptr)
        return instance->hasCrashed() ? Status::crashed : Status::playing;

    return found != entries.end() ? found->second.unloaded : Status::missing;
}

juce::String PluginInstances::getError (DeviceID id) const
{
    const auto found = entries.find (id);
    return found != entries.end() && found->second.instance == nullptr && found->second.opening == nullptr ? found->second.error
                                                                                                           : juce::String();
}

bool PluginInstances::reload (DeviceID id, Reloaded then)
{
    const auto identity = findIdentity (song, id);
    const auto status = getStatus (id);

    if (! identity.has_value() || ! status.has_value() || (*status != Status::crashed && *status != Status::crashedWhileLoading))
        return false;

    // One whose helper went opens in a new one, the same instance, from the state it last had: its own, the user's latest
    // settings; unless it crashed again since it was reloaded so, before anything of it changed, when those settings may be
    // what it crashes for: from the state the song holds then (known bug 166), which its notice says. In the background
    // (known bug 166: the message thread waited for the helper to start and the plug-in to open): it loads meanwhile, the
    // engine skipping it as a crashed one.
    if (*status == Status::crashed)
    {
        // Its own last state kept aside whatever is tried (the one kept already, if the last reload opened it from the
        // song's); the song's tried in its place only for a crash soon after a reload from its own, which its own may be
        // what it crashes for; the next reload tries its own again, as does one that fails (finishReloading()). By hand, it
        // is tried though it crashed as it reloaded before.
        auto& entry = entries[id];
        auto& instance = *entry.instance;
        host.forgetCrashWhileLoading (instance.getDescription());
        const auto now = devices::Clock::getMilliseconds();
        const auto keptAside = entry.reloadedFromSong && entry.ownState.has_value() && instance.getLastState() == entry.songStateTried;
        const auto own = keptAside ? *entry.ownState : instance.getLastState(); // its newer one, if it gave one since
        const auto fromSong =
            ! entry.reloadedFromSong && entry.reloadedTime.has_value() && now - *entry.reloadedTime < getSoonAfterReload();
        const auto tried = fromSong ? getPluginState (song, id) : own;
        instance.setStateInformation (tried.getData(), (int) tried.getSize());

        auto opening = std::make_shared<Opening>();
        opening->then = std::move (then);
        opening->replaces = true;
        opening->fromSong = fromSong;
        opening->own = own;
        opening->tried = tried;
        opening->songProgram = readPluginProgram (song, id);

        for (const auto& [parameter, value] : getPluginValues (song, id))
            opening->songValues[parameter] = value;

        const auto what = PluginHost::describeReopening (instance, tried);
        opening->sampleRate = what.sampleRate;
        opening->program = what.program; // the program it reloads at
        startOpening (entry, std::move (opening), what);
    }
    else
    {
        if (const auto known = findKnownPlugin (host, toDescription (*identity)))
            host.forgetCrashWhileLoading (*known);

        open (id, *identity, true, std::move (then));
    }

    host.sendChangeMessage(); // what shows it shows it loading
    return true;
}

namespace
{
    constexpr double defaultSoonAfterReload = 5000.0; // how soon a crash after a reload from its own state falls back
    std::atomic<double> soonAfterReload { defaultSoonAfterReload };
} // namespace

void PluginInstances::setSoonAfterReload (double milliseconds) noexcept
{
    soonAfterReload = milliseconds >= 0.0 ? milliseconds : defaultSoonAfterReload;
}

double PluginInstances::getSoonAfterReload() noexcept
{
    return soonAfterReload.load();
}

bool PluginInstances::wasReloadedFromSong (DeviceID id) const
{
    const auto found = entries.find (id);
    return found != entries.end() && found->second.reloadedFromSong;
}

int PluginInstances::getGeneration (DeviceID id) const
{
    const auto found = entries.find (id);
    return found != entries.end() ? found->second.generation : 0;
}

void PluginInstances::store (DeviceID id)
{
    store (std::vector { id });
}

void PluginInstances::store (const std::vector<DeviceID>& ids)
{
    // Nothing waits for a plug-in (known bug 166: these waited for those whose states may have changed, up to 10 seconds):
    // what each gave last is written now, which is its own state unless it changed a moment ago (it is asked as it changes:
    // timerCallback()); and those that said they changed since, and answered when last asked (a save just before took the
    // rest, and asks again one that didn't), are asked at once, their answers written into the same
    // elements as they come (outside the undo history, as this is: a device taken away gets its answer where an undo
    // brings it back from). One that doesn't answer keeps the state it gave last. One whose answer to an earlier ask (a
    // save's) is still to come has that answer written.
    for (const auto id : ids)
    {
        if (const auto found = entries.find (id); found != entries.end() && found->second.instance != nullptr)
        {
            const auto plugin = findIdentity (song, id);

            if (! plugin.has_value())
                continue;

            const auto identity = toDescription (*plugin);
            auto& instance = *found->second.instance;
            instance.askState (SandboxedInstance::Ask::changedAndAnswering);
            write (id, identity, instance);

            // Asked: which device of the ID it is for, by the store's state entry as it holds it now.
            if (instance.isAskingState())
                answering.push_back (
                    { .id = id, .identity = identity, .stateAsked = getPluginStateBytes (song, id), .instance = found->second.instance });
        }
    }

    if (! answering.empty())
        startTimer (lookMilliseconds);
}

void PluginInstances::takeAnswers()
{
    // Every answer that came, taken in; those a store asked for, written where it stored them. Where the song has another
    // instance of it by then (an undo brought the device back, opened from what was there), that one is given what came.
    // Given up on once their time has passed only while a save waits for them, or a store: Auto Save gives up on its own
    // asks itself, and would ask again one given up on here before it looked (AutosaveTimer).
    for (auto& [id, entry] : entries)
        if (entry.instance != nullptr)
        {
            if (waitingForStates.empty())
                entry.instance->takeAskedStateIfIn();
            else
                entry.instance->takeAskedState();
        }

    for (auto& waiting : std::exchange (answering, {}))
    {
        if (! waiting.instance->takeAskedState())
        {
            answering.push_back (std::move (waiting));
            continue;
        }

        // An answer is written, by the device's ID, only for the device it was asked for: while the store's state entry of the
        // ID is still the one it held as the answer was asked for (by its identity: the Undo of the device's removal brings
        // back that same entry; a version or a preset brings other bytes, whether the same instance plays the device or not).
        const auto found = entries.find (waiting.id);
        const auto now = found != entries.end() ? found->second.instance : nullptr;
        const auto* values = song.getValues();
        const auto* held = values != nullptr ? values->deviceStore.get().states.find (waiting.id.getRaw()) : nullptr;
        const auto heldBytes = held != nullptr ? *held : PluginBytes();
        const auto isTheSongs = heldBytes.bytes == waiting.stateAsked.bytes;

        if (now == waiting.instance)
        {
            if (isTheSongs)
                write (waiting.id, waiting.identity, *waiting.instance);

            continue;
        }

        // Another instance plays the device now (an undo brought it back), or none (a step took it away): the old one's
        // answer given to another instance only while it is as it opened, from what the store holds; one changed since keeps
        // its own (known bug 166: the old answer overwrote what was set in the new one).
        if (isTheSongs && (now == nullptr || (! now->hasChangedSinceOpened() && heldBytes.get() == now->getLastState())))
        {
            write (waiting.id, waiting.identity, *waiting.instance);

            if (now != nullptr && values != nullptr)
                takeFromSong (waiting.id, values->deviceStore.get());
        }

        // Told of nothing more: it isn't the song's.
        waiting.instance->onParameterChanged = nullptr;
        waiting.instance->onParameterSetByProgram = nullptr;
        waiting.instance->onGesture = nullptr;
        waiting.instance->onStateChanged = nullptr;
        waiting.instance->onLatencyChanged = nullptr;
        waiting.instance->onStateMayChange = nullptr;
    }
}

void PluginInstances::write (DeviceID id, const juce::PluginDescription& identity, SandboxedInstance& instance)
{
    const juce::ScopedValueSetter<bool> writing (storing, true);
    StoredPlugin stored;

    // Its settings, as it last gave them, or as it last had them, if its helper went or didn't give them in time: then the
    // song keeps what it holds rather than nothing (none taken away but for a state of its own now). And its program.
    const auto& block = instance.getLastState();

    if (block.getSize() > 0 || ! instance.isStateOlder())
        stored.state = &block; // the instance's, for this call alone

    // How far it holds the changes the plug-in said it made: the versions count those after as still to commit (known bug
    // 166), as its answer, written in later, then shows.
    song.setDeviceChangesHeld (id.getRaw(), countReportsHeld (id, instance));

    // With it, the plug-in's ID in its format (a VST3's class ID, from its preset file), and the version of the plug-in that
    // made it, which the versions name when it changes (each where there is one).
    stored.formatID = sandbox::readVst3ClassID (block);

    if (const auto known = findKnownPlugin (host, identity); known.has_value())
        stored.version = known->version;

    stored.program = instance.getNumPrograms() > 1 ? instance.getCurrentProgram() : -1;

    // Its values set by hand that automation or modulation moves, which its settings don't hold as set by hand (the engine
    // plays the moved ones): each a VALUE, as the song keeps them. Those set by Phazite's controls are there already.
    auto* session = findSession (song);

    if (const auto* node = session != nullptr ? session->getPlayer().findDevice ((std::int64_t) id.getRaw()) : nullptr)
    {
        const auto& infos = instance.getParameterInfos();

        for (size_t i = 0; i < infos.size(); ++i)
        {
            const ParameterRef ref { id, infos[i].id };

            if (session->getPlayer().isMoved (ref))
                stored.values.push_back ({ .parameter = infos[i].id, .value = node->getValue ((int) i) });
        }
    }

    // Into the store, by the device's ID (DeviceStore.h): for a device a step took out of the song meanwhile (an answer that
    // came after it went) too, which the store keeps for an Undo of the step.
    storePluginOutsideHistory (song, id, stored);
}

void PluginInstances::storeTrack (TrackID track)
{
    auto tracks = getTracksInFolder (song, track);
    tracks.insert (tracks.begin(), track);
    std::vector<DeviceID> ids;

    // Each one's plug-ins by their IDs, in its devices' order, those in its containers' chains too (Mixer.h: getDeviceOutline()).
    for (const auto id : tracks)
        for (const auto& device : getDeviceOutline (song, id))
            if (device.form == DeviceValue::Form::plugin)
                ids.push_back (device.id);

    store (ids);
}

void PluginInstances::storeAll()
{
    std::vector<DeviceID> ids;

    for (const auto& device : findPlugins (song))
        ids.push_back (device.id);

    store (ids);
}

//==============================================================================
void PluginInstances::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The plug-ins scanning found changed, or one crashed or was reloaded: one that wasn't installed may load now (one kept
    // off, or that crashed as it loaded, waits for a reload by hand).
    for (const auto& device : findPlugins (song))
    {
        if (const auto found = entries.find (device.id); found != entries.end() && found->second.instance == nullptr)
            if (found->second.unloaded == Status::missing && ! found->second.wasKnown
                && findKnownPlugin (host, toDescription (device.plugin)).has_value())
                open (device.id, device.plugin, synced);
    }
}

void PluginInstances::takeFromSong (DeviceID id, const DeviceStore& store)
{
    // A preset loaded into it, or an undo of one: its program, then its settings, whichever of them changed, so that the
    // settings are never overridden by a program set after them (an undo takes back the settings first). Both from the store
    // as it is handed (DeviceStore.h).
    const auto found = entries.find (id);

    if (found == entries.end() || found->second.instance == nullptr)
        return;

    auto& instance = *found->second.instance;

    if (const auto* wanted = store.programs.find (id.getRaw()); wanted != nullptr && *wanted != instance.getCurrentProgram())
        instance.setCurrentProgram (*wanted);

    const auto* state = store.states.find (id.getRaw());
    const juce::MemoryBlock none;
    const auto& block = state != nullptr ? state->get() : none;
    instance.setStateInformation (block.getData(), (int) block.getSize());
    lookSoon();
}

void PluginInstances::giveSongState (DeviceID id)
{
    if (const auto* values = song.getValues(); values != nullptr && isPlugin (song, id))
        takeFromSong (id, values->deviceStore.get());
}

void PluginInstances::takeValueFromSong (DeviceID id, const juce::String& parameter, float value)
{
    const auto found = entries.find (id);

    if (found == entries.end() || found->second.instance == nullptr)
        return;

    if (const auto index = found->second.instance->findParameter (parameter); index >= 0)
        found->second.instance->setParameterValue (index, value);

    lookSoon();
}

void PluginInstances::deviceStoreChanged (const std::vector<SongValues::DeviceStoreChange>& changes, const DeviceStore& now)
{
    // What the song changed of a plug-in, given to its instance as the store tells it: a state or a program, the device's
    // program then its state, once for each device the call names; then the values set by hand (or the held one an Undo of a
    // first comes back to), after them, as setUp() gives them: a state given after a value set by hand would override it (a
    // version brought back, and its Redo). Nothing of its own writes (storing), which the store tells inside them.
    if (storing)
        return;

    using Kind = SongValues::DeviceStoreChange::Kind;
    std::vector<DeviceID> settings;

    for (const auto& change : changes)
        if ((change.kind == Kind::state || change.kind == Kind::program)
            && std::find (settings.begin(), settings.end(), change.item) == settings.end())
            settings.push_back (change.item);

    for (const auto id : settings)
        takeFromSong (id, now);

    for (const auto& change : changes)
        if (change.kind == Kind::pluginValue && change.valueToGive.has_value())
            takeValueFromSong (change.item, change.parameter, *change.valueToGive);
}

void PluginInstances::noteReport (DeviceID id, const SandboxedInstance& instance)
{
    // An unsaved change, counted by the song; and which of the plug-in's changes it came with, so that a state written
    // later says how far it holds them (write()).
    song.markChangedOutsideHistory (id.getRaw());
    auto& noted = reports[id];
    noted.push_back ({ instance.countStateChanges(), song.getChangesOutsideHistory (id.getRaw()) });

    // Of one that never answers, as a knob turns dozens of times a second: the first and the latest kept, those between
    // let go (a state held is then taken to hold fewer of them, which shows a change to commit at worst).
    if (noted.size() > maxReportsKept)
        noted.erase (noted.begin() + 1, noted.end() - (std::ptrdiff_t) (maxReportsKept / 2));
}

void PluginInstances::pruneReports (DeviceID id, const SandboxedInstance& instance)
{
    // As answers come in: those the state it last had holds forgotten but the last of them, which says how many it holds.
    const auto found = reports.find (id);

    if (found == reports.end())
        return;

    auto& noted = found->second;

    if (! instance.isStateOlder())
    {
        noted.clear();
        return;
    }

    auto keepFrom = noted.begin();

    for (auto it = noted.begin(); it != noted.end(); ++it)
        if (it->first <= instance.getStateTakenAt())
            keepFrom = it;

    noted.erase (noted.begin(), keepFrom);
}

size_t PluginInstances::countReportsKept (DeviceID id) const
{
    const auto found = reports.find (id);
    return found != reports.end() ? found->second.size() : 0;
}

int PluginInstances::countReportsHeld (DeviceID id, const SandboxedInstance& instance)
{
    // The song's count of the plug-in's own changes as of the state it last had: all of them for one that is its own (and
    // the reports before it forgotten), else those that came with changes up to the one it was taken at.
    const auto now = song.getChangesOutsideHistory (id.getRaw());
    auto& noted = reports[id];

    if (! instance.isStateOlder())
    {
        noted.clear();
        return now;
    }

    auto held = noted.empty() ? now : noted.front().second - 1;

    for (const auto& [changes, count] : noted)
        if (changes <= instance.getStateTakenAt())
            held = count;

    return held;
}

void PluginInstances::lookSoon()
{
    // Not started again while it runs: changes many times a second would put it off for ever.
    if (! isTimerRunning())
        startTimer (lookMilliseconds);
}

//==============================================================================
SandboxedInstance* findPluginInstance (const Song& song, DeviceID id)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().find (id).get() : nullptr;
}

bool hasCrashed (const Song& song, DeviceID id)
{
    const auto status = getPluginStatus (song, id);
    return status == PluginInstances::Status::crashed || status == PluginInstances::Status::crashedWhileLoading;
}

std::optional<PluginInstances::Status> getPluginStatus (const Song& song, DeviceID id)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().getStatus (id) : std::nullopt;
}

juce::String getPluginError (const Song& song, DeviceID id)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().getError (id) : juce::String();
}

void whenPluginStatesIn (const Song& song, SandboxedInstance::Ask which, std::function<void (bool)> then)
{
    if (auto* session = findSession (song))
        session->getPlugins().whenStatesIn (which, std::move (then));
    else
        then (true);
}

void whenPluginStatesIn (const Song& song, bool all, std::function<void (bool)> then)
{
    if (auto* session = findSession (song))
        session->getPlugins().whenStatesIn (all, std::move (then));
    else
        then (true);
}

void giveUpPluginStates (const Song& song)
{
    if (auto* session = findSession (song))
        session->getPlugins().giveUpStates();
}

juce::StringArray getPluginsAnswering (const Song& song)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().getAsked() : juce::StringArray();
}

bool askPluginStates (const Song& song, bool all)
{
    auto* session = findSession (song);
    return session == nullptr || session->getPlugins().askStates (all);
}

juce::StringArray getPluginsWithOlderStates (const Song& song)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().getOlderStates() : juce::StringArray();
}

juce::StringArray getPluginsLoading (const Song& song)
{
    auto* session = findSession (song);
    return session != nullptr ? session->getPlugins().getLoading() : juce::StringArray();
}

juce::StringArray getPluginsMissing (const Song& song)
{
    juce::StringArray missing;

    if (auto* session = findSession (song))
        for (const auto& device : findPlugins (song))
            if (session->getPlugins().getStatus (device.id) == PluginInstances::Status::missing)
                missing.addIfNotAlreadyThere (device.plugin.name);

    return missing;
}

bool reloadPlugin (Song& song, DeviceID id, PluginInstances::Reloaded then)
{
    auto* session = findSession (song);
    return session != nullptr && session->getPlugins().reload (id, std::move (then));
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginInstances.h"
#include "devices/core/Clock.h"
#include "internal/PluginHost.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
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

    // The program the song keeps a plug-in on, or -1 for none.
    int readPluginProgram (const juce::ValueTree& device)
    {
        const auto* program = device.getPropertyPointer (schema::pluginProgram);
        return program != nullptr ? std::max (0, (int) *program) : -1;
    }

    // Sets a property as ValueTree::setProperty() does with an undo manager, which leaves a value that is the same as JUCE
    // compares values alone (a number read from a file is text), but without one. JUCE workaround W8
    // (docs/juce-workarounds.md).
    void storeProperty (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = state.getPropertyPointer (property); stored == nullptr || *stored != value)
            state.setProperty (property, value, nullptr);
    }

    // Its state's bytes, as the song keeps them in standard base 64 (model/base/internal/SongText.h); none for none, or for
    // text that isn't base 64.
    juce::MemoryBlock readPluginState (const juce::ValueTree& device)
    {
        if (const auto text = device[schema::pluginState].toString(); text.isNotEmpty())
            if (auto bytes = readBytes (text))
                return std::move (*bytes);

        return {};
    }

    // The plug-ins' elements in a part of the song (a track's element, with its containers' chains'), in their order.
    void findPlugins (const juce::ValueTree& tree, std::vector<juce::ValueTree>& found)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::DEVICE))
            {
                if (isPluginState (child))
                    found.push_back (child);
            }
            else if (child.hasType (schema::CONTAINER) || child.hasType (schema::CHAIN))
            {
                findPlugins (child, found);
            }
        }
    }

    // Every track's and the master's (which isn't among the tracks): one missed here would be made anew from what its device
    // stored at each change of the song, and never store what it keeps of its own.
    std::vector<juce::ValueTree> findPlugins (const Song& song)
    {
        std::vector<juce::ValueTree> found;

        for (const auto& track : getTrackStates (song))
            findPlugins (track, found);

        findPlugins (song.getState().getChildWithName (schema::MASTER), found);
        return found;
    }

    // The device a VALUE, a plug-in's DEVICE or a property of one is of, if it is a plug-in's.
    juce::ValueTree findPluginOf (juce::ValueTree tree)
    {
        if (tree.hasType (schema::VALUE))
            tree = tree.getParent();

        return tree.hasType (schema::DEVICE) && isPluginState (tree) ? tree : juce::ValueTree();
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
      sampleRate (rate > 0.0 ? rate : defaultSampleRate),
      state (s.getState())
{
    song.setDeviceStateStore (
        [this] (std::uint64_t track)
        {
            if (track == 0)
                storeAll();
            else
                storeTrack (TrackID::fromRaw (track));
        });

    state.addListener (this);
    host.addChangeListener (this);
}

PluginInstances::~PluginInstances()
{
    // What still opens is abandoned (its jobs see their openings gone) and closed in the background: nothing waits for it.
    // What waits for the states is told that not all came, so that a close goes on.
    stopTimer();
    callWaiting (false);
    host.removeChangeListener (this);
    state.removeListener (this);
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

    const auto device = findDeviceState (song, id);

    if (! isPluginState (device))
        return nullptr;

    // In the background, as sync() opens them (known bug 166: one added opened while the message thread waited); at once for
    // a song no session plays, which a render's own thread opens.
    auto* entry = open (id, device, synced);
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

PluginInstances::Entry* PluginInstances::open (DeviceID id, const juce::ValueTree& device, bool inBackground, Reloaded then)
{
    auto& entry = entries[id];
    entry.instance = nullptr;
    entry.opening = nullptr;
    entry.error = {};
    const auto known = findKnownPlugin (host, getPluginIdentity (device));
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
        entry.error = getPluginIdentity (device).name + " isn't installed on this computer.";
        tellReloaded (then, false, entry.error);
        return &entry;
    }

    // From the state the song keeps of it; where that left another program than the song's, the song's program, then the
    // state again, so that the settings are never overridden by a program set after them (a plug-in's state needn't say
    // which program it is on: PluginHost::open()); then the values the song sets by hand under its automation.
    const auto stored = readPluginState (device);
    const auto program = readPluginProgram (device);

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

    setUp (*made, device, stored, program);
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

void PluginInstances::setUp (SandboxedInstance& made, const juce::ValueTree& device, const juce::MemoryBlock& openedFrom, int openedProgram)
{
    // What the song changed of it while it opened (an undo, a preset loaded): its program, then its settings, as the song has
    // them now.
    const auto stored = readPluginState (device);

    if (const auto program = readPluginProgram (device); stored != openedFrom || program != openedProgram)
    {
        if (program >= 0 && program != made.getCurrentProgram())
            made.setCurrentProgram (program);

        if (! stored.isEmpty())
            made.setStateInformation (stored.getData(), (int) stored.getSize());
    }

    // The values the song sets by hand under its automation, as it has them now.
    for (const auto& child : device)
        if (child.hasType (schema::VALUE))
            if (const auto index = made.findParameter (child[schema::valueParameter].toString()); index >= 0)
                made.setParameterValue (index, (float) child[schema::valueSetByHand]);

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
    setUp (*made, findDeviceState (song, id), opening->opened.state, opening->program);
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
    const auto device = findDeviceState (song, id);
    const auto program = readPluginProgram (device);
    const auto held = instance.getLastState(); // as the song gave it meanwhile, else what it reloaded from

    if (held != opening->tried || program != opening->songProgram)
    {
        if (program >= 0 && program != opening->program)
            instance.setCurrentProgram (program);

        if (! held.isEmpty())
            instance.setStateInformation (held.getData(), (int) held.getSize());
    }

    for (const auto& child : device)
    {
        if (! child.hasType (schema::VALUE))
            continue;

        const auto parameter = child[schema::valueParameter].toString();
        const auto value = (float) child[schema::valueSetByHand];

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
            names.add (getPluginIdentity (findDeviceState (song, id)).name);

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
            names.addIfNotAlreadyThere (getPluginIdentity (findDeviceState (song, id)).name);

    return names;
}

juce::StringArray PluginInstances::getOlderStates() const
{
    juce::StringArray names;

    for (const auto& [id, entry] : entries)
        if (entry.instance != nullptr && entry.instance->isStateOlder())
            names.addIfNotAlreadyThere (getPluginIdentity (findDeviceState (song, id)).name);

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

        if (auto device = findDeviceState (song, id); device.isValid())
        {
            for (auto child : device)
            {
                if (child.hasType (schema::VALUE) && child[schema::valueParameter].toString() == ref.parameter)
                {
                    const juce::ScopedValueSetter<bool> writing (storing, true);
                    storeProperty (child, schema::valueSetByHand, value);
                }
            }
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
        held.insert (getDeviceID (device));

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
        if (const auto id = getDeviceID (device); ! entries.contains (id))
            open (id, device, true);

    synced = true;
}

std::optional<PluginInstances::Status> PluginInstances::getStatus (DeviceID id)
{
    if (! isPluginState (findDeviceState (song, id)))
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
    const auto device = findDeviceState (song, id);
    const auto status = getStatus (id);

    if (! status.has_value() || (*status != Status::crashed && *status != Status::crashedWhileLoading))
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
        const auto tried = fromSong ? readPluginState (device) : own;
        instance.setStateInformation (tried.getData(), (int) tried.getSize());

        auto opening = std::make_shared<Opening>();
        opening->then = std::move (then);
        opening->replaces = true;
        opening->fromSong = fromSong;
        opening->own = own;
        opening->tried = tried;
        opening->songProgram = readPluginProgram (device);

        for (const auto& child : device)
            if (child.hasType (schema::VALUE))
                opening->songValues[child[schema::valueParameter].toString()] = (float) child[schema::valueSetByHand];
        const auto what = PluginHost::describeReopening (instance, tried);
        opening->sampleRate = what.sampleRate;
        opening->program = what.program; // the program it reloads at
        startOpening (entry, std::move (opening), what);
    }
    else
    {
        if (const auto known = findKnownPlugin (host, getPluginIdentity (device)))
            host.forgetCrashWhileLoading (*known);

        open (id, device, true, std::move (then));
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
            const auto device = findDeviceState (song, id);
            auto& instance = *found->second.instance;
            instance.askState (SandboxedInstance::Ask::changedAndAnswering);
            write (id, device, instance);

            if (instance.isAskingState() && device.isValid())
                answering.push_back ({ id, device, found->second.instance });
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

        const auto found = entries.find (waiting.id);
        const auto now = found != entries.end() ? found->second.instance : nullptr;

        if (now == waiting.instance)
        {
            write (waiting.id, waiting.device, *waiting.instance);
            continue;
        }

        // Another instance plays the device now (an undo brought it back): the old one's answer is written and given to it
        // only while it is as it opened, from what its element still holds; one changed since keeps its own (known bug 166:
        // the old answer overwrote what was set in the new one).
        const auto isTheSongs = now != nullptr && waiting.device == findDeviceState (song, waiting.id);

        if (now == nullptr || (isTheSongs && ! now->hasChangedSinceOpened() && readPluginState (waiting.device) == now->getLastState()))
        {
            write (waiting.id, waiting.device, *waiting.instance);

            if (isTheSongs)
                takeFromSong (waiting.device, schema::pluginState);
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

void PluginInstances::write (DeviceID id, juce::ValueTree device, SandboxedInstance& instance)
{
    if (! device.isValid())
        return;

    const juce::ScopedValueSetter<bool> writing (storing, true);

    // Its settings, as it last gave them, or as it last had them, if its helper went or didn't give them in time: then the
    // song keeps what it holds rather than nothing. And its program.
    const auto& block = instance.getLastState();
    const auto isNow = ! instance.isStateOlder();

    if (block.getSize() > 0)
        storeProperty (device, schema::pluginState, writeBytes (block));
    else if (isNow)
        device.removeProperty (schema::pluginState, nullptr);

    // How far it holds the changes the plug-in said it made: the versions count those after as still to commit (known bug
    // 166), as its answer, written in later, then shows.
    song.setDeviceChangesHeld (id.getRaw(), countReportsHeld (id, instance));

    // With it, the plug-in's ID in its format (a VST3's class ID, from its preset file), and the version of the plug-in that
    // made it, which the versions name when it changes.
    if (const auto classID = sandbox::readVst3ClassID (block); classID.isNotEmpty())
        storeProperty (device, schema::pluginFormatID, classID);

    if (const auto known = findKnownPlugin (host, getPluginIdentity (device)); known.has_value() && known->version.isNotEmpty())
        storeProperty (device, schema::pluginVersion, known->version);

    if (instance.getNumPrograms() > 1)
        storeProperty (device, schema::pluginProgram, instance.getCurrentProgram());
    else
        device.removeProperty (schema::pluginProgram, nullptr);

    // Its values set by hand that automation or modulation moves, which its settings don't hold as set by hand (the engine
    // plays the moved ones): each a VALUE, as the song keeps them. Those set by Phazite's controls are there already.
    auto* session = findSession (song);
    const auto* node = session != nullptr ? session->getPlayer().findDevice ((std::int64_t) id.getRaw()) : nullptr;

    if (node == nullptr)
        return;

    const auto& infos = instance.getParameterInfos();

    for (size_t i = 0; i < infos.size(); ++i)
    {
        const ParameterRef ref { id, infos[i].id };

        if (! session->getPlayer().isMoved (ref))
            continue;

        auto value = device.getChildWithProperty (schema::valueParameter, infos[i].id);

        if (! value.isValid())
        {
            value = juce::ValueTree (schema::VALUE, { { schema::valueParameter, infos[i].id } });
            device.appendChild (value, nullptr);
        }

        storeProperty (value, schema::valueSetByHand, node->getValue ((int) i));
    }
}

void PluginInstances::storeTrack (TrackID track)
{
    auto tracks = getTracksInFolder (song, track);
    tracks.insert (tracks.begin(), track);
    std::vector<DeviceID> ids;

    for (const auto id : tracks)
    {
        std::vector<juce::ValueTree> found;
        findPlugins (findTrackState (song, id), found);

        for (const auto& device : found)
            ids.push_back (getDeviceID (device));
    }

    store (ids);
}

void PluginInstances::storeAll()
{
    std::vector<DeviceID> ids;

    for (const auto& device : findPlugins (song))
        ids.push_back (getDeviceID (device));

    store (ids);
}

//==============================================================================
void PluginInstances::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The plug-ins scanning found changed, or one crashed or was reloaded: one that wasn't installed may load now (one kept
    // off, or that crashed as it loaded, waits for a reload by hand).
    for (const auto& device : findPlugins (song))
    {
        const auto id = getDeviceID (device);

        if (const auto found = entries.find (id); found != entries.end() && found->second.instance == nullptr)
            if (found->second.unloaded == Status::missing && ! found->second.wasKnown
                && findKnownPlugin (host, getPluginIdentity (device)).has_value())
                open (id, device, synced);
    }
}

void PluginInstances::takeFromSong (const juce::ValueTree& device, const juce::Identifier& property)
{
    // A preset loaded into it, or an undo of one: its program, then its settings, whichever of them changed, so that the
    // settings are never overridden by a program set after them (an undo takes back the settings first).
    const auto found = entries.find (getDeviceID (device));

    if (found == entries.end() || found->second.instance == nullptr
        || (property != schema::pluginProgram && property != schema::pluginState))
        return;

    auto& instance = *found->second.instance;

    if (device.hasProperty (schema::pluginProgram))
        if (const auto wanted = (int) device[schema::pluginProgram]; wanted != instance.getCurrentProgram())
            instance.setCurrentProgram (wanted);

    const auto block = readPluginState (device);
    instance.setStateInformation (block.getData(), (int) block.getSize());
    lookSoon();
}

void PluginInstances::giveSongState (DeviceID id)
{
    if (const auto device = findDeviceState (song, id); findPluginOf (device).isValid())
        takeFromSong (device, schema::pluginState);
}

void PluginInstances::takeValueFromSong (const juce::ValueTree& value)
{
    const auto device = value.getParent();
    const auto found = entries.find (getDeviceID (device));

    if (found == entries.end() || found->second.instance == nullptr)
        return;

    if (const auto index = found->second.instance->findParameter (value[schema::valueParameter].toString()); index >= 0)
        found->second.instance->setParameterValue (index, (float) value[schema::valueSetByHand]);

    lookSoon();
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

void PluginInstances::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (storing)
        return;

    if (tree.hasType (schema::VALUE))
    {
        if (findPluginOf (tree).isValid())
            takeValueFromSong (tree);
    }
    else if (property == schema::pluginState || property == schema::pluginProgram)
    {
        if (const auto device = findPluginOf (tree); device.isValid())
            takeFromSong (device, property);
    }
}

void PluginInstances::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (! storing && child.hasType (schema::VALUE) && findPluginOf (child).isValid())
        takeValueFromSong (child);
}

void PluginInstances::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
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
            if (session->getPlugins().getStatus (getDeviceID (device)) == PluginInstances::Status::missing)
                missing.addIfNotAlreadyThere (getDeviceName (device));

    return missing;
}

bool reloadPlugin (Song& song, DeviceID id, PluginInstances::Reloaded then)
{
    auto* session = findSession (song);
    return session != nullptr && session->getPlugins().reload (id, std::move (then));
}
} // namespace phazite

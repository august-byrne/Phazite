// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-ins a song holds, each an instance in its helper (SandboxedInstance.h), by its device's ID: made from the
// device's element in the song (Schema.h: DEVICE, a plug-in's), which names the plug-in, its state and its program, and
// whose values set by hand (VALUE) it then takes; made again when the song gets the device back (an undo), and let go of
// when the song no longer has it. A playback session owns one for its project (model/playback/internal/PlaybackSession.h).
// What a plug-in changes itself (in its own window) comes back to the song: its values set by hand, which a VALUE the song
// has takes in without an undo step; its gestures; its state, which isn't stored until the song is saved or copied, but
// counts as an unsaved change (Song::markChangedOutsideHistory()); and its latency, which needs a new plan. What the song
// changes of a plug-in (a preset loaded into it, an undo, a value set by a control) goes to its instance. The song's store
// of its devices' states (Song::setDeviceStateStore()) is this one's while it lives. Message thread only, but for what it
// opens in the background.
//
// Nothing here keeps the message thread waiting for a helper (known bugs 69, 89 and 166): every plug-in a session's song
// holds opens in the background, each in its helper, beside each other (those it holds as it opens, one added, one an undo
// brings back, one scanning finds, one reloaded), the song playing meanwhile without it (silent in its place), each coming in
// as it is ready (the session's plan made again) with what the song set it to meanwhile; a save asks
// them all for their states at once and writes once the answers are in or their time is up (whenStatesIn()), a plug-in
// that doesn't answer keeping the state it last gave (getOlderStates() names it); a store that isn't a save (a copy, a
// commit, a preset, a window closed) writes what each gave last, asking those that may have changed, and a plug-in that
// says it changed is asked once it is quiet, so that what it gave last is its own (known bug 166); and an instance let go
// of closes in its helper, which goes on a thread of the host's, without anything waiting for either (PluginHost.h).
#pragma once

#include "PluginHost.h"
#include "SandboxedInstance.h"
#include "model/base/IDs.h"
#include "model/devices/ParameterRef.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace phazite
{
class PluginHost;
class Song;

class PluginInstances final : private juce::ValueTree::Listener,
                              private juce::ChangeListener, // the plug-ins scanning found, which a missing one may now be among
                              private juce::Timer           // what opens in the background, and the states asked for
{
public:
    // For the plug-ins a song holds, at a sample rate: each made as it is first found or synced.
    PluginInstances (Song&, PluginHost&, double sampleRate);
    ~PluginInstances() override;

    // A device's instance, its opening begun from its element if it hasn't begun (in the background once the session has
    // synced; at once for a song no session plays, whose render, on a thread of its own, has its own instances); nullptr for
    // a device that isn't a plug-in, or one that doesn't load (still loading, missing, crashed as it loaded, or off:
    // getStatus()).
    std::shared_ptr<SandboxedInstance> find (DeviceID);

    // Begins to open the plug-ins the song holds and has none for, in the background (the song's session's: known bug 166,
    // one added or brought back by an undo opened while the message thread waited), and lets go of those it no longer holds,
    // abandoning any still opening.
    void sync();

    // How a plug-in plays: in its helper; still loading, in the background (as the song opened, as it was added or brought
    // back, or as it is reloaded); crashed (its helper went, a crash or a hang it was ended for, and it keeps the state it
    // last had); crashed as it loaded, which keeps it off until it is reloaded; missing (not installed here, until scanning
    // finds it); or off, while plug-ins don't load after a crash of Phazite's (PluginHost::setLoadsPlugins()). Nothing for a
    // device that isn't a plug-in.
    enum class Status
    {
        playing,
        loading,
        crashed,
        crashedWhileLoading,
        missing,
        off
    };

    // Whether any plug-in is still loading, or one; and which, by the names the song gives them. And what to do once none is,
    // called on the message thread (at once if none is), each once, unless this goes first.
    bool isLoading() const;
    bool isLoading (DeviceID) const;
    juce::StringArray getLoading() const;
    void whenLoaded (std::function<void()>);

    // The plug-ins' states asked for at once, without waiting: every plug-in's (a save by hand, closing), or those whose state
    // may have changed since they last gave or were given it (Auto Save). Then called back once on the message thread, with
    // true once every answer is in or given up on as its time passed, or with false at once if this goes first (a version
    // switched to, the project closed) or the wait is given up by hand (giveUpStates(): the states the plug-ins gave last
    // are kept, as for a time-out). What it calls must not take this down then and there: it posts what does. And the
    // plug-ins whose states the song holds are older than theirs: they didn't answer since they may have changed, or their
    // helper went.
    void whenStatesIn (bool all, std::function<void (bool allIn)>);
    void whenStatesIn (SandboxedInstance::Ask, std::function<void (bool allIn)>); // asking as askState() does
    void giveUpStates();
    juce::StringArray getOlderStates() const;
    juce::StringArray getAsked() const; // those whose answers are still to come, by name

    // The same asked for, again as they change (none asked twice while its answer is to come), without a call: whether every
    // answer is in, or given up on, now.
    bool askStates (bool all);

    std::optional<Status> getStatus (DeviceID);

    // A crashed plug-in opened again, in the background (known bug 166: the message thread waited for its helper to start and
    // the plug-in to open): in a new helper from the state it last had, or, one that crashed as it loaded, loaded again from
    // the song; loading meanwhile (getStatus()). False, calling nothing, for one that isn't crashed or is reloading; else
    // then is called once it has reloaded (true) or failed (false, with why), unless the device or this goes first. Its
    // windows make its editor again (getGeneration()).
    using Reloaded = std::function<void (bool reloaded, const juce::String& error)>;
    bool reload (DeviceID, Reloaded then);
    int getGeneration (DeviceID) const; // one more at each reload

    // Whether the last reload of a crashed one opened it from the state the song holds, rather than its own last: it crashed
    // again soon after a reload from its own (within so many milliseconds: five seconds; shorter or longer in a test), so
    // that its own may be what it crashes for; the next reload tries its own again.
    bool wasReloadedFromSong (DeviceID) const;
    static void setSoonAfterReload (double milliseconds) noexcept;

    // How many of the changes a plug-in told the store still keeps, to say how far a state holds them (the tests' measure).
    size_t countReportsKept (DeviceID) const;
    static double getSoonAfterReload() noexcept;

    // What the plug-ins keep of their own, stored into their elements in the song, outside the undo history, where it
    // differs from what is there: a plug-in's state, its program, and the values set by hand of the parameters automation or
    // modulation moves (VALUE). One device, a track's (its folder's tracks' and its containers' too), or every one. Nothing
    // waits for a plug-in (known bug 166): each state is the one the plug-in gave last, which is its own unless it changed a
    // moment ago (a plug-in that says it changed is asked for its state once it is quiet, unasked) or doesn't answer; and
    // those that said they changed since are asked at once, their answers written into the same elements as they come.
    // What needs every plug-in's own state now asks first, and goes on once the answers are in (whenStatesIn(), then
    // this): a save, and every command that reads them (Command::needsPluginStates).
    void store (DeviceID);
    void storeTrack (TrackID);
    void storeAll();

    // A device's program and state as the song holds them, given to its instance though they may not have changed in the
    // song: what a preset loaded into it sets may be what a store just wrote, the state the plug-in gave last, which needn't
    // be its own.
    void giveSongState (DeviceID);

    // Hears what the plug-ins change themselves.
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void pluginGestureChanged (const ParameterRef&, bool starting) = 0;
        virtual void pluginLatencyChanged() = 0; // what plays needs a new plan
    };

    void setListener (Listener* l) noexcept { listener = l; }

    // The sample rate new instances open at (the engine's).
    void setSampleRate (double);

    // A render begins: the plug-ins that came in a moment ago fade in no more as the first processor made of them plays
    // (SandboxedInstance::joinLate()), the render's.
    void forgetJoinsLate();

private:
    struct Opening; // a plug-in opening in the background

    struct Entry
    {
        std::shared_ptr<SandboxedInstance> instance;
        Status unloaded = Status::missing; // why it has no instance, if it has none
        bool wasKnown = false;             // whether scanning had found it as it was opened
        juce::String error;                // why it didn't load
        int generation = 0;
        std::shared_ptr<Opening> opening;          // while it loads
        std::optional<double> reloadedTime;        // as it was last reloaded after a crash (juce::Time::getMillisecondCounterHiRes())
        bool reloadedFromSong = false;             // that reload opened it from the song's state, not its own (reload())
        std::optional<juce::MemoryBlock> ownState; // its own last state, kept aside as it was reloaded
        juce::MemoryBlock songStateTried;          // the song's, which that reload opened it from
    };

    Song& song;
    PluginHost& host;
    double sampleRate;
    Listener* listener = nullptr;
    juce::ValueTree state; // the song's
    std::map<DeviceID, Entry> entries;
    bool storing = false; // while it writes into the song, which it doesn't take back from it
    bool synced = false;  // the first sync() done
    std::vector<std::function<void (bool)>> waitingForStates;
    std::vector<std::function<void()>> waitingForLoads;

    void callWaiting (bool allIn); // what waits for the states, each once

    Entry* open (DeviceID, const juce::ValueTree& deviceState, bool inBackground, Reloaded then = {});
    void startOpening (Entry&, std::shared_ptr<Opening>, const PluginHost::Reopening&);
    void finishOpening (DeviceID, Entry&);
    void finishReloading (DeviceID, Entry&);
    void setUp (SandboxedInstance&, const juce::ValueTree& deviceState, const juce::MemoryBlock& openedFrom, int openedProgram);
    void listen (DeviceID, SandboxedInstance&);
    void store (const std::vector<DeviceID>&);
    void write (DeviceID, juce::ValueTree device, SandboxedInstance&); // what it keeps, as it last gave it, into its element
    void timerCallback() override;
    void lookSoon(); // the timer started, unless it runs

    // The changes each plug-in said it made, by the count of its state's changes each came at and the song's count of its
    // reports then; and the song's count as far as the state it last had holds them (Song::setDeviceChangesHeld()).
    std::map<DeviceID, std::vector<std::pair<int, int>>> reports;
    void noteReport (DeviceID, const SandboxedInstance&);
    void pruneReports (DeviceID, const SandboxedInstance&);
    int countReportsHeld (DeviceID, const SandboxedInstance&);

    // The answers a store asked for, still to come: for a device, to be written into its element (which an undo may bring
    // back, if it goes meanwhile), from its instance (kept until then).
    struct Answering
    {
        DeviceID id;
        juce::ValueTree device;
        std::shared_ptr<SandboxedInstance> instance;
    };

    std::vector<Answering> answering;
    void takeAnswers();

    // While a save waits (whenStatesIn()): the plug-ins it asked, by how many changes each had then (asked again, once, if
    // the answer it waits for is to an older ask), which it waits for alone.
    struct Wanted
    {
        int changes = 0;
        bool askedAgain = false;
    };

    std::map<DeviceID, Wanted> wantedStates;
    bool askStates (SandboxedInstance::Ask);
    void takeFromSong (const juce::ValueTree& deviceState, const juce::Identifier& property);
    void takeValueFromSong (const juce::ValueTree& value);

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;

    JUCE_DECLARE_NON_COPYABLE (PluginInstances)
};

// A song's plug-in instance by its device's ID, through its session (none while no session plays it, nor for a device that
// isn't a plug-in or doesn't load); whether a plug-in doesn't play because it crashed (its helper went, or it crashed as it
// loaded); and its reload (PluginInstances::reload(), in the background: the engine is given the song again once it is
// in).
SandboxedInstance* findPluginInstance (const Song&, DeviceID);
bool hasCrashed (const Song&, DeviceID);
bool reloadPlugin (Song&, DeviceID, PluginInstances::Reloaded then);

// How a song's plug-in plays (PluginInstances::getStatus()), through its session: nothing without one, or for a device
// that isn't a plug-in.
std::optional<PluginInstances::Status> getPluginStatus (const Song&, DeviceID);

// Through the song's session (none: nothing to wait for): its plug-ins' states asked for at once, and a call once they are
// in (PluginInstances::whenStatesIn(): with true at once without a session); the wait given up, and the plug-ins still
// answering, by name; the plug-ins whose states the song holds are older than theirs, and those still loading, by name.
void whenPluginStatesIn (const Song&, bool all, std::function<void (bool allIn)>);
void whenPluginStatesIn (const Song&, SandboxedInstance::Ask, std::function<void (bool allIn)>);
bool askPluginStates (const Song&, bool all);
void giveUpPluginStates (const Song&);
juce::StringArray getPluginsAnswering (const Song&);
juce::StringArray getPluginsWithOlderStates (const Song&);
juce::StringArray getPluginsLoading (const Song&);
} // namespace phazite

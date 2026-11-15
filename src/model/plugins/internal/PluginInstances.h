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
// Nothing here keeps the message thread waiting for a helper as a project opens, saves or closes (known bugs 69 and 89):
// the plug-ins a song holds as it opens (the first sync()) open in the background, each in its helper, beside each other,
// the song playing meanwhile without them, each coming in as it is ready (the session's plan made again); a save asks
// them all for their states at once and writes once the answers are in or their time is up (whenStatesIn()), a plug-in
// that doesn't answer keeping the state it last gave (getOlderStates() names it); and an instance let go of closes in its
// helper, which goes on a thread of the host's, without anything waiting for either (PluginHost.h).
#pragma once

#include "SandboxedInstance.h"
#include "model/base/IDs.h"
#include "model/devices/ParameterRef.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
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

    // A device's instance, made from its element if it isn't yet (so that a device just added has one at once); nullptr for a
    // device that isn't a plug-in, or one that doesn't load (still loading, missing, crashed as it loaded, or off:
    // getStatus()).
    std::shared_ptr<SandboxedInstance> find (DeviceID);

    // Makes the instances of the plug-ins the song holds and has none for, and lets go of those it no longer holds. The first,
    // as the song opens, opens them in the background (the song's session's), and so do those after it while any still
    // loads; later, one the song gets (back) opens at once, as a plug-in added does.
    void sync();

    // How a plug-in plays: in its helper; still loading, in the background, as the song opened; crashed (its helper went, a
    // crash or a hang it was ended for, and it keeps the state it last had); crashed as it loaded, which keeps it off until it
    // is reloaded; missing (not installed here, until scanning finds it); or off, while plug-ins don't load after a crash of
    // Phazite's (PluginHost::setLoadsPlugins()). Nothing for a device that isn't a plug-in.
    enum class Status
    {
        playing,
        loading,
        crashed,
        crashedWhileLoading,
        missing,
        off
    };

    // Whether any plug-in is still loading, and which, by the names the song gives them.
    bool isLoading() const;
    juce::StringArray getLoading() const;

    // The plug-ins' states asked for at once, without waiting: every plug-in's (a save by hand, closing), or those whose state
    // may have changed since they last gave or were given it (Auto Save). Then called back on the message thread once every
    // answer is in, or given up on as its time passed (nothing after this goes). And the plug-ins whose states the song
    // holds are older than theirs: they didn't answer since they may have changed, or their helper went.
    void whenStatesIn (bool all, std::function<void()>);
    juce::StringArray getOlderStates() const;

    // The same asked for, again as they change (none asked twice while its answer is to come), without a call: whether every
    // answer is in, or given up on, now.
    bool askStates (bool all);

    std::optional<Status> getStatus (DeviceID);

    // A crashed plug-in opened again: in a new helper from the state it last had, or, one that crashed as it loaded,
    // loaded again from the song; false, with why, if it didn't load. Its windows make its editor again (getGeneration()).
    bool reload (DeviceID, juce::String& error);
    int getGeneration (DeviceID) const; // one more at each reload

    // What the plug-ins keep of their own, stored into their elements in the song, outside the undo history, where it
    // differs from what is there: a plug-in's state, its program, and the values set by hand of the parameters automation or
    // modulation moves (VALUE). One device, a track's (its folder's tracks' and its containers' too), or every one. Those
    // whose states may have changed since they last gave them are asked at once and waited for together, within one time
    // limit; one that doesn't answer keeps the state it last gave.
    void store (DeviceID);
    void storeTrack (TrackID);
    void storeAll();

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

private:
    struct Opening; // a plug-in opening in the background

    struct Entry
    {
        std::shared_ptr<SandboxedInstance> instance;
        Status unloaded = Status::missing; // why it has no instance, if it has none
        bool wasKnown = false;             // whether scanning had found it as it was opened
        juce::String error;                // why it didn't load
        int generation = 0;
        std::shared_ptr<Opening> opening; // while it loads
    };

    Song& song;
    PluginHost& host;
    double sampleRate;
    Listener* listener = nullptr;
    juce::ValueTree state; // the song's
    std::map<DeviceID, Entry> entries;
    bool storing = false; // while it writes into the song, which it doesn't take back from it
    bool synced = false;  // the first sync() done
    std::vector<std::function<void()>> waitingForStates;

    Entry* open (DeviceID, const juce::ValueTree& deviceState, bool inBackground = false);
    void finishOpening (DeviceID, Entry&);
    void setUp (SandboxedInstance&, const juce::ValueTree& deviceState, const juce::MemoryBlock& openedFrom, int openedProgram);
    void listen (DeviceID, SandboxedInstance&);
    void store (const std::vector<DeviceID>&);
    void write (DeviceID); // what it keeps, as it last gave it
    void timerCallback() override;
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
// loaded); and its reload (PluginInstances::reload()), after which the engine is given the song again.
SandboxedInstance* findPluginInstance (const Song&, DeviceID);
bool hasCrashed (const Song&, DeviceID);
bool reloadPlugin (Song&, DeviceID, juce::String& error);

// How a song's plug-in plays (PluginInstances::getStatus()), through its session: nothing without one, or for a device
// that isn't a plug-in.
std::optional<PluginInstances::Status> getPluginStatus (const Song&, DeviceID);

// Through the song's session (none: nothing to wait for): its plug-ins' states asked for at once, and a call once they are
// in (PluginInstances::whenStatesIn()); the plug-ins whose states the song holds are older than theirs, and those still
// loading, by name.
void whenPluginStatesIn (const Song&, bool all, std::function<void()>);
bool askPluginStates (const Song&, bool all);
juce::StringArray getPluginsWithOlderStates (const Song&);
juce::StringArray getPluginsLoading (const Song&);
} // namespace phazite

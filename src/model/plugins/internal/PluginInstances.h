// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-ins a song holds, each an instance in its helper (SandboxedInstance.h), by its device's ID: made from the
// device's element in the song (Schema.h: DEVICE, a plug-in's), which names the plug-in, its state and its program, and
// whose values set by hand (VALUE) it then takes; made again when the song gets the device back (an undo), and let go of
// when the song no longer has it. A playback session owns one for its project (model/playback/internal/PlaybackSession.h).
// What a plug-in changes itself (in its own window) comes back to the song: its values set by hand, which a VALUE the song
// has takes in without an undo step; its gestures; its state, which isn't stored until the song is saved or copied, but
// counts as an unsaved change (Song::markChangedOutsideHistory()); and its latency, which needs a new plan. What the song
// changes of a plug-in (a preset loaded into it, an undo, a value set by a control) goes to its instance. The song's store
// of its devices' states (Song::setDeviceStateStore()) is this one's while it lives. Message thread only.
#pragma once

#include "SandboxedInstance.h"
#include "model/base/IDs.h"
#include "model/devices/ParameterRef.h"

#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <memory>
#include <optional>

namespace phazite
{
class PluginHost;
class Song;

class PluginInstances final : private juce::ValueTree::Listener,
                              private juce::ChangeListener // the plug-ins scanning found, which a missing one may now be among
{
public:
    // Makes the instance of every plug-in the song holds, at a sample rate.
    PluginInstances (Song&, PluginHost&, double sampleRate);
    ~PluginInstances() override;

    // A device's instance, made from its element if it isn't yet (so that a device just added has one at once); nullptr for a
    // device that isn't a plug-in, or one that doesn't load (missing, crashed as it loaded, or off: getStatus()).
    std::shared_ptr<SandboxedInstance> find (DeviceID);

    // Makes the instances of the plug-ins the song holds and has none for, and lets go of those it no longer holds.
    void sync();

    // How a plug-in plays: in its helper; crashed (its helper went, a crash or a hang it was ended for, and it keeps the state
    // it last had); crashed as it loaded, which keeps it off until it is reloaded; missing (not installed here, until
    // scanning finds it); or off, while plug-ins don't load after a crash of Phazite's (PluginHost::setLoadsPlugins()).
    // Nothing for a device that isn't a plug-in.
    enum class Status
    {
        playing,
        crashed,
        crashedWhileLoading,
        missing,
        off
    };

    std::optional<Status> getStatus (DeviceID);

    // A crashed plug-in opened again: in a new helper from the state it last had, or, one that crashed as it loaded,
    // loaded again from the song; false, with why, if it didn't load. Its windows make its editor again (getGeneration()).
    bool reload (DeviceID, juce::String& error);
    int getGeneration (DeviceID) const; // one more at each reload

    // What the plug-ins keep of their own, stored into their elements in the song, outside the undo history, where it
    // differs from what is there: a plug-in's state, its program, and the values set by hand of the parameters automation or
    // modulation moves (VALUE). One device, a track's (its folder's tracks' and its containers' too), or every one.
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
    struct Entry
    {
        std::shared_ptr<SandboxedInstance> instance;
        Status unloaded = Status::missing; // why it has no instance, if it has none
        bool wasKnown = false;             // whether scanning had found it as it was opened
        juce::String error;                // why it didn't load
        int generation = 0;
    };

    Song& song;
    PluginHost& host;
    double sampleRate;
    Listener* listener = nullptr;
    juce::ValueTree state; // the song's
    std::map<DeviceID, Entry> entries;
    bool storing = false; // while it writes into the song, which it doesn't take back from it

    Entry* open (DeviceID, const juce::ValueTree& deviceState);
    void listen (DeviceID, SandboxedInstance&);
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
} // namespace phazite

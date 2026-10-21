// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/LiveInput.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Containers.h"

namespace phazite
{
namespace
{
    // The Edit's instance of an input device, which holds the tracks it goes to. Only exists while the
    // Edit's playback context is allocated.
    te::InputDeviceInstance* findInstance (te::Edit& edit, const te::InputDevice& device)
    {
        for (auto* instance : edit.getAllInputDevices())
            if (&instance->getInputDevice() == &device)
                return instance;

        return nullptr;
    }
} // namespace

bool hasInstrument (const te::AudioTrack& track)
{
    for (auto* plugin : track.pluginList)
        if (isInstrument (*plugin))
            return true;

    return false;
}

te::AudioTrack* getSelectedAudioTrack (const te::SelectionManager& selection)
{
    if (auto* track = selection.getFirstItemOfType<te::AudioTrack>())
        return track;

    if (auto* clip = selection.getFirstItemOfType<te::Clip>())
        return dynamic_cast<te::AudioTrack*> (clip->getTrack());

    // Notes selected in the piano roll: their clip's track.
    if (auto* notes = selection.getFirstItemOfType<te::SelectedMidiEvents>())
        if (auto* clip = notes->getClips().getFirst())
            return dynamic_cast<te::AudioTrack*> (clip->getTrack());

    // Devices selected in the mixer: their channel's track (a container's devices: their container's).
    if (auto* device = selection.getFirstItemOfType<te::Plugin>())
    {
        auto* container = findContainerOf (*device);
        return dynamic_cast<te::AudioTrack*> (container != nullptr ? container->getOwnerTrack() : device->getOwnerTrack());
    }

    return nullptr;
}

void followSelectedTrack (const te::SelectionManager& selection)
{
    if (auto* track = getSelectedAudioTrack (selection); track != nullptr && hasInstrument (*track))
        setLiveMidiTrack (*track);
    else if (auto* edit = selection.getEdit())
        routeLiveMidiInput (*edit); // in case the track it played was deleted
}

std::shared_ptr<te::MidiInputDevice> getAllMidiInputs (te::Engine& engine)
{
    for (auto& device : engine.getDeviceManager().getMidiInDevices())
        if (auto* merged = dynamic_cast<te::VirtualMidiInputDevice*> (device.get()); merged != nullptr && merged->useAllInputs)
            return device;

    return nullptr;
}

void playIntoAllMidiInputs (te::Engine& engine, const juce::MidiMessage& m)
{
    if (const auto device = getAllMidiInputs (engine))
    {
        if (m.isNoteOn())
            device->keyboardState.noteOn (m.getChannel(), m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            device->keyboardState.noteOff (m.getChannel(), m.getNoteNumber(), m.getFloatVelocity());
    }
}

te::InputDeviceInstance* findLiveMidiInput (te::Edit& edit)
{
    if (const auto device = getAllMidiInputs (edit.engine))
        return findInstance (edit, *device);

    return nullptr;
}

te::AudioTrack* getLiveMidiTrack (te::Edit& edit)
{
    if (auto* instance = findLiveMidiInput (edit))
        if (const auto tracks = te::getTargetTracks (*instance); ! tracks.isEmpty())
            return tracks.getFirst();

    return nullptr;
}

bool setLiveMidiTrack (te::AudioTrack& track)
{
    auto& edit = track.edit;
    const auto device = getAllMidiInputs (edit.engine);

    if (device == nullptr)
        return false;

    edit.getTransport().ensureContextAllocated();
    auto* instance = findInstance (edit, *device);

    if (instance == nullptr)
        return false;

    // Without an undo step: this follows the track being played, it isn't an edit of the song.
    if (const auto tracks = te::getTargetTracks (*instance); tracks.size() != 1 || tracks.getFirst() != &track)
        if (const auto destination = instance->setTarget (track.itemID, true, nullptr, 0);
            ! destination.has_value() || *destination == nullptr)
            return false;

    // Armed from the start, so that Record can begin at once: arming rebuilds Tracktion's playback graph, which would
    // lose a recording begun meanwhile.
    if (! instance->isRecordingEnabled (track.itemID))
        instance->setRecordingEnabled (track.itemID, true);

    return true;
}

void routeLiveMidiInput (te::Edit& edit)
{
    edit.getTransport().ensureContextAllocated();

    // The track it plays already, armed as setLiveMidiTrack() arms it.
    if (auto* track = getLiveMidiTrack (edit))
    {
        setLiveMidiTrack (*track);
        return;
    }

    for (auto* track : te::getAudioTracks (edit))
    {
        if (hasInstrument (*track))
        {
            setLiveMidiTrack (*track);
            return;
        }
    }
}

void closeMidiOutputs (te::Engine& engine)
{
    auto& devices = engine.getDeviceManager();

    for (int i = 0; i < devices.getNumMidiOutDevices(); ++i)
        if (auto* output = devices.getMidiOutDevice (i); output != nullptr && output->isEnabled())
            output->setEnabled (false);
}
} // namespace phazite

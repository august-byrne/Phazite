// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LiveInput.h"

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
        if (plugin->isSynth())
            return true;

    return false;
}

void followSelectedTrack (const te::SelectionManager& selection)
{
    if (auto* track = selection.getFirstItemOfType<te::AudioTrack>(); track != nullptr && hasInstrument (*track))
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

te::AudioTrack* getLiveMidiTrack (te::Edit& edit)
{
    if (auto device = getAllMidiInputs (edit.engine))
        if (auto* instance = findInstance (edit, *device))
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

    if (const auto tracks = te::getTargetTracks (*instance); tracks.size() == 1 && tracks.getFirst() == &track)
        return true;

    // Without an undo step: this follows the track being played, it isn't an edit of the song.
    const auto destination = instance->setTarget (track.itemID, true, nullptr, 0);
    return destination.has_value() && *destination != nullptr;
}

void routeLiveMidiInput (te::Edit& edit)
{
    edit.getTransport().ensureContextAllocated();

    if (getLiveMidiTrack (edit) != nullptr)
        return;

    for (auto* track : te::getAudioTracks (edit))
    {
        if (hasInstrument (*track))
        {
            setLiveMidiTrack (*track);
            return;
        }
    }
}
} // namespace phazite

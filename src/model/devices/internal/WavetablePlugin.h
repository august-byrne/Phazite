// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's wavetable synth in Tracktion (devices/wavetable/WavetableSynth.h): a DevicePlugin that also keeps what each
// oscillator plays, in its state (schema::getOscillatorWavetable() and getOscillatorWavetableFile()): one of the factory
// tables by name, or a sound file in the project's folder, which it reads into a table when the project opens and
// whenever those properties change, as Undo changes them. Choose what an oscillator plays with Wavetables.h.
#pragma once

#include "devices/wavetable/WavetableSynth.h"
#include "model/devices/internal/DevicePlugin.h"

namespace phazite
{
class WavetablePlugin final : public DevicePlugin
{
public:
    static inline const char* const xmlTypeName = devices::WavetableSynth::typeID;

    explicit WavetablePlugin (te::PluginCreationInfo);

    devices::WavetableSynth& getSynth() noexcept;

    // The sound file an oscillator plays, or none while it plays a factory table; and what it plays, by name: the file's
    // name without its extension, or the factory table's ("Basic Shapes").
    juce::File getTableFile (int osc) const;
    juce::String getTableName (int osc) const;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

protected:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

private:
    void loadTable (int osc);
};

// A sound file as a wavetable (devices/wavetable/Wavetable.h: fromSound()), its channels mixed, with the length of its frames
// from Serum's 'clm ' chunk if it has one; or nullptr if it can't be read.
std::shared_ptr<const devices::Wavetable> readWavetable (te::Engine&, const juce::File&);
} // namespace phazite

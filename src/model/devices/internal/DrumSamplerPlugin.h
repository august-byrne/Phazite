// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler in Tracktion (devices/drums/DrumSampler.h): a DevicePlugin that also keeps what each pad plays, in
// its state: one of the factory sounds by its name (schema::getPadSound()), and a sound file that plays instead while it is
// there (schema::getPadSoundFile(), a path within the project's folder). It loads them into the sampler when the project
// opens and whenever they change, as a preset or Undo changes them. Put files on pads with DrumKit.h.
#pragma once

#include "devices/drums/DrumSampler.h"
#include "model/devices/internal/DevicePlugin.h"

namespace phazite
{
class DrumSamplerPlugin final : public DevicePlugin
{
public:
    static inline const char* const xmlTypeName = devices::DrumSampler::typeID;

    explicit DrumSamplerPlugin (te::PluginCreationInfo);

    devices::DrumSampler& getSampler() noexcept;

    // The file a pad plays, or none for its factory sound.
    juce::File getPadFile (int pad) const;

    // What a pad plays: its file's name without the extension, or its factory sound's ("808 Kick", or the default kit's
    // "Kick").
    juce::String getPadName (int pad) const;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

protected:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

private:
    int getFactorySound (int pad) const; // the factory sound it names, or its own in the default kit
    void loadPad (int pad);
};
} // namespace phazite

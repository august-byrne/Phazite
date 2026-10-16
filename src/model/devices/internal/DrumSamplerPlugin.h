// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler in Tracktion (devices/drums/DrumSampler.h): a DevicePlugin that also keeps the sound file each
// pad plays, in its state (schema::getPadSound(), a path within the project's folder), and loads it into the sampler
// when the project opens and whenever that property changes, as Undo changes it. Put files on pads with DrumKit.h.
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

    // The file a pad plays, or none for the default kit's sound.
    juce::File getPadFile (int pad) const;

    // What a pad plays: its file's name without the extension, or the kit sound's ("Kick").
    juce::String getPadName (int pad) const;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

protected:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

private:
    void loadPad (int pad);
};
} // namespace phazite

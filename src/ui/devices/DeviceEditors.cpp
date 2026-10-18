// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceEditors.h"
#include "CompressorEditor.h"
#include "DevicePanel.h"
#include "DrumSamplerEditor.h"
#include "EqEditor.h"
#include "LimiterEditor.h"
#include "ReverbEditor.h"
#include "WavetableEditor.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Maximizer.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "devices/utility/Utility.h"
#include "devices/wavetable/WavetableSynth.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int labelHeight = 18;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // The synth's controls, in the order the sound goes through them.
    std::vector<std::vector<DevicePanel::Group>> getSynthLayout()
    {
        return { { { "Oscillators",
                     { "osc1Wave", "osc2Wave", "osc2Pitch", "osc2Detune", "oscMix", "pulseWidth", "subLevel", "noiseLevel" } },
                   { "Unison", { "unisonVoices", "unisonDetune", "unisonSpread" } } },
                 { { "Filter", { "filterType", "cutoff", "resonance", "filterSlope", "filterEnvelope", "keyTracking" } },
                   { "Filter Envelope", { "filterAttack", "filterDecay", "filterSustain", "filterRelease" } } },
                 { { "Amp Envelope", { "attack", "decay", "sustain", "release" } },
                   { "LFO", { "lfoShape", "lfoTarget", "lfoRate", "lfoDepth" } } },
                 { { "Play", { "voiceMode", "glide", "bendRange", "velocity", "level" } } } };
    }

    // The effects of step 2.10, each a panel of its controls in the order the sound goes through them (their mix is in
    // the Devices view's header).
    std::vector<std::vector<DevicePanel::Group>> getDelayLayout()
    {
        return { { { "Time", { "sync", "time", "milliseconds" } },
                   { "Echoes", { "feedback", "pingPong" } },
                   { "Tone", { "lowCut", "highCut" } } } };
    }

    std::vector<std::vector<DevicePanel::Group>> getSaturatorLayout()
    {
        return { { { "Drive", { "drive", "shape" } }, { "Tone", { "tone" } }, { "Output", { "output", "autoGain" } } } };
    }

    std::vector<std::vector<DevicePanel::Group>> getFilterLayout()
    {
        return { { { "Filter", { "type", "cutoff", "resonance", "slope" } }, { "Drive", { "drive" } } } };
    }

    std::vector<std::vector<DevicePanel::Group>> getUtilityLayout()
    {
        return { { { "Level", { "gain", "balance" } },
                   { "Stereo", { "width", "bassMono", "bassMonoFrequency" } },
                   { "Polarity", { "invertLeft", "invertRight" } } } };
    }

    std::vector<std::vector<DevicePanel::Group>> getMultibandLayout()
    {
        return { { { "Amount", { "depth", "time", "upward", "downward" } }, { "Gain", { "inputGain", "outputGain" } } },
                 { { "Bands", { "lowFrequency", "highFrequency", "lowGain", "midGain", "highGain" } } } };
    }

    std::vector<std::vector<DevicePanel::Group>> getMaximizerLayout()
    {
        return { { { "Gain", { "gain" } },
                   { "Squeeze", { "lowSqueeze", "midSqueeze", "highSqueeze" } },
                   { "Limiter", { "ceiling", "release" } } },
                 { { "Bands", { "lowFrequency", "highFrequency", "lowLevel", "midLevel", "highLevel" } } } };
    }
} // namespace

std::unique_ptr<juce::Component> createDeviceEditor (const DeviceHandle& device)
{
    const auto* info = device.getInfo();

    if (info == nullptr)
        return {};

    if (info->typeID == devices::Equalizer::typeID)
        return std::make_unique<EqEditor> (device);

    if (info->typeID == devices::Compressor::typeID)
        return std::make_unique<CompressorEditor> (device);

    if (info->typeID == devices::Limiter::typeID)
        return std::make_unique<LimiterEditor> (device);

    if (info->typeID == devices::Reverb::typeID)
        return std::make_unique<ReverbEditor> (device);

    if (info->typeID == devices::Synth::typeID)
        return std::make_unique<DevicePanel> (device, getSynthLayout());

    if (info->typeID == devices::DrumSampler::typeID)
        return std::make_unique<DrumSamplerEditor> (device);

    if (info->typeID == devices::WavetableSynth::typeID)
        return std::make_unique<WavetableEditor> (device);

    if (info->typeID == devices::Delay::typeID)
        return std::make_unique<DevicePanel> (device, getDelayLayout());

    if (info->typeID == devices::Saturator::typeID)
        return std::make_unique<DevicePanel> (device, getSaturatorLayout());

    if (info->typeID == devices::FilterEffect::typeID)
        return std::make_unique<DevicePanel> (device, getFilterLayout());

    if (info->typeID == devices::Utility::typeID)
        return std::make_unique<DevicePanel> (device, getUtilityLayout());

    if (info->typeID == devices::Multiband::typeID)
        return std::make_unique<DevicePanel> (device, getMultibandLayout());

    if (info->typeID == devices::Maximizer::typeID)
        return std::make_unique<DevicePanel> (device, getMaximizerLayout());

    return std::make_unique<DeviceKnobs> (device);
}

//==============================================================================
DeviceKnobs::DeviceKnobs (const DeviceHandle& device)
{
    if (const auto* info = device.getInfo())
    {
        for (const auto& parameter : info->parameters)
        {
            const auto name = toString (parameter.name);
            auto* knob = knobs.add (new Knob (name, parameter.defaultValue));
            showAsParameter (*knob, parameter);
            attachments.add (new ParameterAttachment (*knob))->setParameter (device.getParameter (parameter.id));

            auto* label = labels.add (new juce::Label ({}, name));
            label->setJustificationType (juce::Justification::centred);
            label->setFont (juce::FontOptions (TextSize::small));

            addAndMakeVisible (knob);
            addAndMakeVisible (label);
        }
    }

    const auto rows = std::max (1, (knobs.size() + columns - 1) / columns);
    const auto shownColumns = std::clamp (knobs.size(), 1, columns);
    setSize (shownColumns * (knobSize + Space::l) + Space::l, rows * (knobSize + labelHeight + Space::s) + Space::l);
    startTimerHz (10);
}

DeviceKnobs::~DeviceKnobs()
{
    stopTimer();
}

void DeviceKnobs::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void DeviceKnobs::resized()
{
    for (int i = 0; i < knobs.size(); ++i)
    {
        const auto x = Space::l + (i % columns) * (knobSize + Space::l);
        const auto y = Space::l + (i / columns) * (knobSize + labelHeight + Space::s);
        knobs[i]->setBounds (x, y, knobSize, knobSize);
        labels[i]->setBounds (x - Space::s, y + knobSize, knobSize + 2 * Space::s, labelHeight);
    }
}

void DeviceKnobs::timerCallback()
{
    if (! isShowing()) // nothing to show behind another tab of the Devices view
        return;

    for (auto* attachment : attachments)
        attachment->update();
}
} // namespace phazite::ui

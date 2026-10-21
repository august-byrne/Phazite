// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceEditors.h"
#include "CompressorEditor.h"
#include "DelayEditor.h"
#include "DrumSamplerEditor.h"
#include "EqEditor.h"
#include "FilterEditor.h"
#include "FlangerEditor.h"
#include "LimiterEditor.h"
#include "MaximizerEditor.h"
#include "MultibandEditor.h"
#include "PhaserEditor.h"
#include "ReverbEditor.h"
#include "SaturatorEditor.h"
#include "StereoExpanderEditor.h"
#include "SynthEditor.h"
#include "WavetableEditor.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Maximizer.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
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
        return std::make_unique<SynthEditor> (device);

    if (info->typeID == devices::DrumSampler::typeID)
        return std::make_unique<DrumSamplerEditor> (device);

    if (info->typeID == devices::WavetableSynth::typeID)
        return std::make_unique<WavetableEditor> (device);

    if (info->typeID == devices::Delay::typeID)
        return std::make_unique<DelayEditor> (device);

    if (info->typeID == devices::Saturator::typeID)
        return std::make_unique<SaturatorEditor> (device);

    if (info->typeID == devices::FilterEffect::typeID)
        return std::make_unique<FilterEditor> (device);

    if (info->typeID == devices::StereoExpander::typeID)
        return std::make_unique<StereoExpanderEditor> (device);

    if (info->typeID == devices::Multiband::typeID)
        return std::make_unique<MultibandEditor> (device);

    if (info->typeID == devices::Maximizer::typeID)
        return std::make_unique<MaximizerEditor> (device);

    if (info->typeID == devices::Phaser::typeID)
        return std::make_unique<PhaserEditor> (device);

    if (info->typeID == devices::Flanger::typeID)
        return std::make_unique<FlangerEditor> (device);

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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceEditors.h"
#include "EqEditor.h"
#include "devices/eq/Equalizer.h"
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
            attachments.add (new ParameterAttachment (*knob, "Change " + name))->setParameter (device.getParameter (parameter.id));

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
    for (auto* attachment : attachments)
        attachment->update();
}
} // namespace phazite::ui

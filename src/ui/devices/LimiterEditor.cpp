// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LimiterEditor.h"
#include "devices/dynamics/Limiter.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Limiter;

    constexpr int refreshRateHz = 30;
    constexpr int meterHeight = 3 * knobSize; // tall enough to read, beside the knobs

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    juce::Label& setUp (juce::Label& label)
    {
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (TextSize::small));
        return label;
    }
} // namespace

LimiterEditor::LimiterEditor (const DeviceHandle& handle)
    : device (handle),
      ceiling (device.getParameter (Limiter::info.parameters[Limiter::ceiling].id))
{
    // Short names under the meters, whose titles say them in full.
    const std::pair<ValueMeter*, const char*> meters[] = { { &inputMeter, "In" }, { &reductionMeter, "GR" }, { &outputMeter, "Out" } };

    for (const auto& [meter, name] : meters)
    {
        addAndMakeVisible (meter);
        addAndMakeVisible (setUp (*meterNames.add (new juce::Label ({}, name))));
    }

    for (const auto& parameter : Limiter::info.parameters)
    {
        const auto name = toString (parameter.name);
        auto* knob = knobs.add (new Knob (name, parameter.defaultValue));
        showAsParameter (*knob, parameter);
        attachments.add (new ParameterAttachment (*knob))->setParameter (device.getParameter (parameter.id));
        addAndMakeVisible (knob);
        addAndMakeVisible (setUp (*names.add (new juce::Label ({}, name))));
        addAndMakeVisible (setUp (*texts.add (new juce::Label())));
        texts.getLast()->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    }

    // The meters' first readings would show what played before the editor opened.
    if (auto* limiter = device.getProcessor<Limiter>())
    {
        limiter->getInputMeter().take();
        limiter->getReductionMeter().take();
        limiter->getOutputMeter().take();
    }

    const auto meterWidth = 3 * (GraphTint::meterWidth + Space::l);
    setSize (2 * Space::l + meterWidth + Space::xl + knobs.size() * (knobSize + Space::l), 2 * Space::l + meterHeight + labelHeight);
    refresh();
    startTimerHz (refreshRateHz);
}

LimiterEditor::~LimiterEditor()
{
    stopTimer();
}

void LimiterEditor::refresh()
{
    for (int i = 0; i < knobs.size(); ++i)
    {
        attachments[i]->update();
        texts[i]->setText (knobs[i]->getTextFromValue (knobs[i]->getValue()), juce::dontSendNotification);
    }

    if (auto* limiter = device.getProcessor<Limiter>())
    {
        inputMeter.update (limiter->getInputMeter().take());
        reductionMeter.update (limiter->getReductionMeter().take());
        outputMeter.update (limiter->getOutputMeter().take());
    }

    const auto mark = ceiling.isValid() ? std::optional<float> (ceiling.getValue()) : std::nullopt;
    inputMeter.setMark (mark);
    outputMeter.setMark (mark);
}

void LimiterEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void LimiterEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::l);

    // The meters side by side, each with its name under it, then the knobs in a row, centred beside them.
    auto meters = area.removeFromLeft (3 * (GraphTint::meterWidth + Space::l));
    ValueMeter* const shown[] = { &inputMeter, &reductionMeter, &outputMeter };

    for (int i = 0; i < 3; ++i)
    {
        auto column = meters.removeFromLeft (GraphTint::meterWidth + Space::l);
        meterNames[i]->setBounds (column.removeFromBottom (labelHeight));
        shown[i]->setBounds (column.withSizeKeepingCentre (GraphTint::meterWidth, column.getHeight()));
    }

    area.removeFromLeft (Space::xl);
    auto row = area.withSizeKeepingCentre (area.getWidth(), knobSize + 2 * labelHeight);

    for (int i = 0; i < knobs.size(); ++i)
    {
        auto cell = row.removeFromLeft (knobSize + Space::l);
        knobs[i]->setBounds (cell.removeFromTop (knobSize).withSizeKeepingCentre (knobSize, knobSize));
        names[i]->setBounds (cell.removeFromTop (labelHeight));
        texts[i]->setBounds (cell.removeFromTop (labelHeight));
    }
}
} // namespace phazite::ui

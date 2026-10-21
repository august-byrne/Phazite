// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LimiterEditor.h"
#include "devices/dynamics/Limiter.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Limiter;

    constexpr float historyLowest = -48.0f, historyReduction = 24.0f;
    constexpr int historyTicks = 4 * DeviceTicker::ticksPerSecond; // four seconds

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;
} // namespace

LimiterDisplay::LimiterDisplay (ParameterHandle c)
    : ceiling (std::move (c)),
      history ("Limiter history", historyLowest, historyReduction, historyTicks)
{
    setTitle ("Limiter");
    history.setLine (&ceiling, "Ceiling");
    addAndMakeVisible (history);

    // Short names under the meters, whose titles say them in full.
    const std::pair<ValueMeter*, const char*> meters[] = { { &inputMeter, "In" }, { &reductionMeter, "GR" }, { &outputMeter, "Out" } };

    for (const auto& [meter, name] : meters)
    {
        addAndMakeVisible (meter);
        auto* label = meterNames.add (new juce::Label ({}, name));
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        addAndMakeVisible (label);
    }
}

void LimiterDisplay::show (float input, float reduction, float output)
{
    inputMeter.update (input);
    reductionMeter.update (reduction);
    outputMeter.update (output);

    const auto mark = ceiling.isValid() ? std::optional<float> (ceiling.getValue()) : std::nullopt;
    inputMeter.setMark (mark);
    outputMeter.setMark (mark);
    history.push (input, output, reduction);
}

int LimiterDisplay::getMetersWidth()
{
    return 3 * (GraphTint::meterWidth + Space::s);
}

void LimiterDisplay::resized()
{
    auto area = getLocalBounds();
    auto meters = area.removeFromRight (getMetersWidth());
    area.removeFromRight (Space::xs);
    history.setBounds (area);

    ValueMeter* const shown[] = { &inputMeter, &reductionMeter, &outputMeter };

    for (int i = 0; i < 3; ++i)
    {
        auto column = meters.removeFromLeft (GraphTint::meterWidth + Space::s);
        meterNames[i]->setBounds (column.removeFromBottom (labelHeight));
        shown[i]->setBounds (column.withSizeKeepingCentre (GraphTint::meterWidth, column.getHeight()));
    }
}

//==============================================================================
LimiterEditor::LimiterEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DeviceTicker::Display (static_cast<juce::Component&> (*this))
{
    auto& shown = keep (std::make_unique<LimiterDisplay> (device.getParameter (Limiter::info.parameters[Limiter::ceiling].id)));
    display = &shown;

    // The meters' first readings would show what played before the editor opened.
    if (auto* limiter = device.getProcessor<Limiter>())
    {
        limiter->getInputMeter().take();
        limiter->getReductionMeter().take();
        limiter->getOutputMeter().take();
    }

    const auto gain = makeControlsPart ("Gain", { "gain" }, 1, ParameterControls::Size::large);
    const auto limit = makeControlsPart ("Limit", { "ceiling", "release" }, 2);
    setParts ({ { { "History", &shown, 280 + LimiterDisplay::getMetersWidth(), 200, 96, true } }, { gain, limit } });
}

LimiterEditor::~LimiterEditor() = default;

void LimiterEditor::refresh()
{
    if (auto* limiter = device.getProcessor<Limiter>())
        display->show (limiter->getInputMeter().take(), limiter->getReductionMeter().take(), limiter->getOutputMeter().take());
}
} // namespace phazite::ui

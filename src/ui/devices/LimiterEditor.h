// SPDX-License-Identifier: AGPL-3.0-or-later
// The limiter's editor (devices/dynamics/Limiter.h): the last seconds of what it plays, as FabFilter's limiter shows them,
// what comes in (after its gain), what goes out and how far it turns the sound down, red icicles under its ceiling, which is
// a line to drag up or down; meters of the three beside them; then its Gain, large, its Ceiling and its Release.
#pragma once

#include "DeviceEditorBase.h"
#include "LevelHistoryView.h"
#include "ui/components/Controls.h"

namespace phazite::ui
{
// What a limiter plays, as the limiter's and the maximizer's editors show it: its history, with the ceiling's line, and
// meters of the loudest coming in, how far it turns the sound down and the loudest going out.
class LimiterDisplay : public juce::Component
{
public:
    explicit LimiterDisplay (ParameterHandle ceiling);

    // A tick's readings, in decibels (the reduction 0 or more).
    void show (float input, float reduction, float output);

    LevelHistoryView& getHistory() noexcept { return history; }
    const ValueMeter& getInputMeter() const noexcept { return inputMeter; }
    const ValueMeter& getReductionMeter() const noexcept { return reductionMeter; }
    const ValueMeter& getOutputMeter() const noexcept { return outputMeter; }

    static int getMetersWidth();
    void resized() override;

private:
    ParameterHandle ceiling;
    LevelHistoryView history;
    ValueMeter inputMeter { "Input", -48.0f, 0.0f, ValueMeter::Fill::fromBottom },
        reductionMeter { "Gain Reduction", 0.0f, 24.0f, ValueMeter::Fill::fromTop },
        outputMeter { "Output", -48.0f, 0.0f, ValueMeter::Fill::fromBottom };
    juce::OwnedArray<juce::Label> meterNames;
};

class LimiterEditor : public DeviceEditorBase,
                      private DisplayTicker::Display
{
public:
    explicit LimiterEditor (const DeviceHandle&);
    ~LimiterEditor() override;

    LimiterDisplay& getDisplay() const noexcept { return *display; }

    // Takes the limiter's meters into the display; the ticker does it.
    void refresh();

private:
    LimiterDisplay* display = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui

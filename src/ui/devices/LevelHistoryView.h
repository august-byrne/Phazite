// SPDX-License-Identifier: AGPL-3.0-or-later
// The last seconds of a dynamics device's sound, scrolling from right to left, as FabFilter's limiter and compressor show
// theirs: how loud it comes in (pale) and goes out (in the device's colour), how far it is turned down (red, hanging from
// the top, its deepest dips numbered), and a line across at a level the device sets (a limiter's ceiling, a compressor's
// threshold), which a drag up or down moves, as one undo step. Its editor pushes a reading each tick.
#pragma once

#include "DeviceGraph.h"
#include "model/devices/ParameterHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class LevelHistoryView : public juce::Component
{
public:
    // Levels from lowestLevel to 0 dB; reductions down to reductionRange dB, from the top.
    LevelHistoryView (const juce::String& title, float lowestLevel, float reductionRange, int ticks);

    // A tick's reading, in decibels: the loudest in, the loudest out, and how far it was turned down (0 or more).
    void push (float input, float output, float reduction);

    // The line across: its parameter (the caller's handle, which must outlive this), named ("Ceiling"), or none.
    void setLine (ParameterHandle*, const juce::String& name);

    // The newest reading, as shown.
    float getNewestReduction() const { return reductions.get (reductions.size() - 1); }

    float getYForLevel (float decibels) const; // in its own coordinates

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    const float lowest, reductionRange;
    graph::LevelHistory inputs, outputs, reductions;
    ParameterHandle* line = nullptr;
    juce::String lineName;
    bool dragging = false;
    float startValue = 0.0f;

    graph::LevelAxis getLevels() const;
};
} // namespace phazite::ui

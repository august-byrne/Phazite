// SPDX-License-Identifier: AGPL-3.0-or-later
// The exciter's editor (devices/saturation/Exciter.h): the spectrum of what comes in (pale) and of what goes out (in its
// colour), and over them what the exciter adds, filled, so that the band it takes and the harmonics it makes above it show;
// its lift as a curve, how far it raises the sound at each frequency while the curve inside leaves the band straight, with a
// dot where the band starts, as high as the top is lifted: a drag moves the frequency sideways and the amount up and down,
// as one undo step, a press anywhere puts it under the pointer (GraphDot), and the wheel turns the drive. Beside it a meter of how far the band reaches into the curve, with a line
// where the curve bends: under the line the band is only lifted, over it it turns into harmonics. Then its Frequency and
// Amount, large, its Drive, Colour and High Cut, and Listen, which plays what is added alone while it is on and the editor
// shows.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class ExciterEditor : public DeviceEditorBase,
                      private DisplayTicker::Display
{
public:
    explicit ExciterEditor (const DeviceHandle&);
    ~ExciterEditor() override;

    juce::Component& getGraph() const noexcept;
    juce::Button& getListenButton() const noexcept; // plays what is added alone, while on and showing

    // Where the graph draws its dot (the frequency, as high as the top's lift), in its own coordinates.
    juce::Point<float> getDotPosition() const;

    // What the meter shows: how far the driven band reaches into the curve, in decibels against where it bends.
    float getShownReach() const;

private:
    class Graph;
    class HarmonicsPart;
    Graph* graph = nullptr;
    HarmonicsPart* harmonics = nullptr;

    void tick() override;
};
} // namespace phazite::ui

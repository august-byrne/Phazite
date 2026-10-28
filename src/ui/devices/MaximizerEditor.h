// SPDX-License-Identifier: AGPL-3.0-or-later
// The maximizer's editor (devices/dynamics/Maximizer.h), in the style of FL Studio's Maximus: its three bands, low in red,
// mid in orange and high in yellow, each a panel with its squeeze's curve (how loud a level comes out), a dot riding it at
// the band's level and how far it is squeezed, with its Squeeze and Level knobs under it; a drag up or down a panel sets its
// level (a double-click puts it back at 0 dB), and one across the strip above moves the split nearest, each one undo step.
// Then its limiter (LimiterDisplay), with the ceiling a line to drag; then its Gain, large, its Ceiling and Release, and
// where the bands split.
#pragma once

#include "DeviceEditorBase.h"
#include "LimiterEditor.h"

namespace phazite::ui
{
class MaximizerEditor : public DeviceEditorBase,
                        private DisplayTicker::Display
{
public:
    explicit MaximizerEditor (const DeviceHandle&);
    ~MaximizerEditor() override;

    juce::Component& getBands() const noexcept;
    LimiterDisplay& getLimiterDisplay() const noexcept { return *limiterDisplay; }

    // What a band's panel shows now, in decibels: its level coming in, and how far it is squeezed (0 or more).
    float getShownLevel (int band) const;
    float getShownReduction (int band) const;
    juce::Rectangle<int> getBandArea (int band) const; // its curve, in the bands' coordinates

    // Takes the meters into the displays; the ticker does it.
    void refresh();

private:
    class Bands;
    Bands* bands = nullptr;
    LimiterDisplay* limiterDisplay = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui

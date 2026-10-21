// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (devices/dynamics/Multiband.h), the squeeze it is known for drawn as it happens: its three bands as
// columns under the spectrum of what it plays, cut where the bands split. Each column shows the zone where its band is
// turned down (above -30 dB) and the one where it is brought up (below -40 dB), how loud the band comes in (a thin bar), how
// far it is pushed (an arrow, down in red or up in green, with its decibels) and how loud it goes out (a wide bar). A drag
// up or down a column sets its band's gain (a double-click puts it back at 0 dB), and one across the spectrum moves the
// crossover nearest, each one undo step. Beside it: Depth and Time, how much of each squeeze, and the bands' gains.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class MultibandEditor : public DeviceEditorBase
{
public:
    explicit MultibandEditor (const DeviceHandle&);
    ~MultibandEditor() override;

    juce::Component& getDisplay() const noexcept;

    // What a band's column shows now, in decibels: its level coming in, and how far it is pushed (up above 0).
    float getShownLevel (int band) const;
    float getShownChange (int band) const;

    // Where the display draws a band's column, and a crossover's line, in its own coordinates (for tests).
    juce::Rectangle<int> getBandArea (int band) const;
    int getCrossoverX (int crossover) const;

private:
    class Display;
    Display* display = nullptr;
};
} // namespace phazite::ui

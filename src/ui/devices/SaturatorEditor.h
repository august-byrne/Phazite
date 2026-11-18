// SPDX-License-Identifier: AGPL-3.0-or-later
// The saturator's editor (devices/saturation/Saturator.h), as Ableton's Saturator shows its curve: the shape the sound is
// driven into, as it plays (lopsided with Colour, in steps with Crush), lit as far as the sound reaches into it, so turning
// the drive up shows the sound pushing into the bend; and beside it the wave itself, coming in (pale) and going out (in its
// colour), held still on the wave's start, so the rounded or folded tops show. Six buttons, each drawn with its curve, pick
// the shape; a drag up or down the curve turns the drive, as one undo step, the pointer pointing up and down over it and the
// whole curve lit. Then its Drive, large, its Colour, Tone and Output, and Auto Gain. Then the Band, switched at its title's
// right end: the spectrum of what comes in (pale) and of what goes out (in its colour) with the band lit between From and
// To, whose edges drag sideways, the nearer one lit, as one undo step each (Escape takes a drag back); From, To and Listen,
// which plays the band alone while it is on and the editor shows. Last, small, the Quality.
#pragma once

#include "DeviceEditorBase.h"
#include "ui/components/DisplayTicker.h"

namespace phazite::ui
{
class SaturatorEditor : public DeviceEditorBase,
                        private DisplayTicker::Display
{
public:
    explicit SaturatorEditor (const DeviceHandle&);
    ~SaturatorEditor() override;

    juce::Component& getCurve() const noexcept;
    juce::Button& getShapeButton (int shape) const;

    // How far the sound reaches into the curve now: its loudest coming in, after the drive (1 is the curve's knee).
    float getShownReach() const;

    // The Band's picture, where it draws an edge (From, or To), across it in its own coordinates; and Listen, which plays the
    // band alone while it is on and the editor shows.
    juce::Component& getBandView() const noexcept;
    float getEdgeX (bool high) const;
    juce::Button& getListenButton() const noexcept;

private:
    class CurveView;
    class ShapeButtons;
    class BandView;
    class BandPart;
    CurveView* display = nullptr;
    ShapeButtons* shapes = nullptr;
    BandView* bandView = nullptr;
    BandPart* bandPart = nullptr;

    void tick() override;
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// The saturator's editor (devices/saturation/Saturator.h), as Ableton's Saturator shows its curve: the shape the sound is
// driven into, lit as far as the sound reaches into it, so turning the drive up shows the sound pushing into the bend; and
// beside it the wave itself, coming in (pale) and going out (in its colour), held still on the wave's start, so the
// rounded or folded tops show. Five buttons, each drawn with its curve, pick the shape; a drag up or down the curve turns the
// drive, as one undo step. Then its Drive, large, its Tone and Output, and Auto Gain.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class SaturatorEditor : public DeviceEditorBase
{
public:
    explicit SaturatorEditor (const DeviceHandle&);
    ~SaturatorEditor() override;

    juce::Component& getCurve() const noexcept;
    juce::Button& getShapeButton (int shape) const;

    // How far the sound reaches into the curve now: its loudest coming in, after the drive (1 is the curve's knee).
    float getShownReach() const;

private:
    class Display;
    class ShapeButtons;
    Display* display = nullptr;
    ShapeButtons* shapes = nullptr;
};
} // namespace phazite::ui

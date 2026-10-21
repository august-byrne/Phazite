// SPDX-License-Identifier: AGPL-3.0-or-later
// The stereo expander's editor (devices/stereo/StereoExpander.h), as iZotope's Imager shows a stereo image: a half disc of
// what it plays, each moment a dot, mono standing straight up and the two sides out of step lying flat, over the faint dots
// of what comes in, with the L and R lines that are safe to stay within, and a fan opened as wide as its width; under it,
// how alike the two sides are, from -1 to +1, red below 0, where the mono sum loses sound, with a mark for what comes in.
// Then its Width and Spread, large, and its bass mono, polarity and gain.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class StereoExpanderEditor : public DeviceEditorBase
{
public:
    explicit StereoExpanderEditor (const DeviceHandle&);
    ~StereoExpanderEditor() override;

    juce::Component& getImage() const noexcept;

    // What the image shows now: how alike the sides are going out and coming in (StereoExpander::noReading for none), and
    // how many of its dots are lit.
    float getShownCorrelation() const;
    float getShownInputCorrelation() const;
    int getNumDots() const;

private:
    class Image;
    Image* image = nullptr;
};
} // namespace phazite::ui

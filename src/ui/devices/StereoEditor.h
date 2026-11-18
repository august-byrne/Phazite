// SPDX-License-Identifier: AGPL-3.0-or-later
// Stereo's editor (devices/stereo/Stereo.h), as iZotope's Imager shows a stereo image: a half disc of what it plays, each
// moment a dot, mono standing straight up and the two sides out of step lying flat, over the faint dots of what comes in,
// with the L and R lines that are safe to stay within, and a fan opened as wide as its width; under it, how alike the two
// sides are, from -1 to +1, red below 0, where the mono sum loses sound, with a mark for what comes in. Then its parts, in
// the order the sound goes through them: Width (Width and Spread, large) over Bass (Bass Mono and its frequency), and Sides
// (Balance, Mono, Swap and the two flips) over Output (the DC filter and the gain).
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class StereoEditor : public DeviceEditorBase
{
public:
    explicit StereoEditor (const DeviceHandle&);
    ~StereoEditor() override;

    juce::Component& getImage() const noexcept;

    // What the image shows now: how alike the sides are going out and coming in (Stereo::noReading for none), and how many
    // of its dots are lit.
    float getShownCorrelation() const;
    float getShownInputCorrelation() const;
    int getNumDots() const;

    // Where the dots of what goes out lie across the disc on average, from -1 (along the L line) to 1 (along the R line).
    float getDotsAcross() const;

    // Where the half disc's centre is in the image, at its bottom: the fan opens from there, as wide as the pointer's angle
    // from straight up when it is dragged.
    juce::Point<float> getDiscCentre() const;

private:
    class Image;
    Image* image = nullptr;
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// The flanger's editor (devices/motion/Flanger.h): its comb over the spectrum of what it plays, a tooth at each of its
// notches, sliding as the LFO sweeps the copy's delay, the left side's response lit and the right side's fainter; under
// it, a strip of the delay in milliseconds, as a tape's heads: how far the sweep reaches, and where each side's copy lags
// now; then the LFO (SweepPicture). A dot sits at the delay's frequency, 1 / the longest delay (the comb's first peak, or
// with the feedback below 0 its first notch), as high as the feedback (0 in the middle): a drag moves both, as one undo
// step, and the wheel turns the depth. Then its Delay and Depth, large, the LFO's controls, its Feedback, Stereo and Safe
// Bass.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class FlangerEditor : public DeviceEditorBase
{
public:
    explicit FlangerEditor (const DeviceHandle&);
    ~FlangerEditor() override;

    juce::Component& getPicture() const noexcept;

    // Where the picture draws its dot (the delay's frequency, as high as the feedback), in its own coordinates; and how far it
    // shows a side's copy lagging now, in milliseconds.
    juce::Point<float> getDotPosition() const;
    double getShownDelay (int channel) const;

private:
    class Picture;
    Picture* picture = nullptr;
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// The phaser's editor (devices/motion/Phaser.h): its notches over the spectrum of what it plays, moving as the LFO sweeps
// them, the left side's response lit and glowing down to the bottom, so that each notch reads as a valley carved in the
// sound, and the right side's fainter; along the bottom, how far the sweep reaches; under it, the LFO (SweepPicture). A dot
// sits at the frequency, as high as the feedback (0 in the middle): a drag moves both, as one undo step, and the wheel turns
// the depth. Then its Frequency and Depth, large, the LFO's controls, and the notches' own.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class PhaserEditor : public DeviceEditorBase
{
public:
    explicit PhaserEditor (const DeviceHandle&);
    ~PhaserEditor() override;

    juce::Component& getPicture() const noexcept;

    // Where the picture draws its dot (the frequency, as high as the feedback), in its own coordinates; and where it shows
    // a side's notches swept to now: their centre, in hertz.
    juce::Point<float> getDotPosition() const;
    double getShownCentre (int channel) const;

private:
    class Picture;
    Picture* picture = nullptr;
};
} // namespace phazite::ui

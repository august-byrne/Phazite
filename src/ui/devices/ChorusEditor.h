// SPDX-License-Identifier: AGPL-3.0-or-later
// The chorus's editor (devices/motion/Chorus.h): the comb its copies make with the sound now, over the spectrum of what it
// plays, the left side's lit and the right side's fainter, sliding as the copies sweep; under it, a strip of the delay in
// milliseconds, as a tape's heads: how far the sweep reaches, and where each copy lags now (one a side, or three in
// Ensemble); then the LFO (SweepPicture), with a dot for each copy. A dot sits at the delay's frequency (1 / the delay, the
// middle of the sweep), as high as the depth: a drag moves both, as one undo step, and the wheel turns the rate (in time
// with the song, the note value). Then its Delay and Depth, large, the LFO's controls, its copies' Mode, Stereo and
// Feedback, and its High Cut and Safe Bass.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class ChorusEditor : public DeviceEditorBase
{
public:
    explicit ChorusEditor (const DeviceHandle&);
    ~ChorusEditor() override;

    juce::Component& getPicture() const noexcept;

    // Where the picture draws its dot (the delay's frequency, as high as the depth), in its own coordinates; how many copies
    // it shows each side; and how far it shows a side's copy lagging now, in milliseconds.
    juce::Point<float> getDotPosition() const;
    int getShownVoices() const;
    double getShownDelay (int channel, int voice) const;

private:
    class Picture;
    Picture* picture = nullptr;
};
} // namespace phazite::ui

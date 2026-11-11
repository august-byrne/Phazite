// SPDX-License-Identifier: AGPL-3.0-or-later
// The chorus's editor (devices/motion/Chorus.h): the comb its copies make with the sound now, over the spectrum of what it
// plays, the left side's lit and the right side's fainter, sliding as the copies sweep; under it, a strip of the delay in
// milliseconds, as a tape's heads: how far the sweep reaches, and where each copy lags now (one a side, or as many as it
// has voices, each on a line of its own with the part of the sweep it has to itself once Spread gives it one); then the LFO
// (SweepPicture), with a dot for each copy where Voice Phase puts it in the cycle, and Shimmer's fast sine on its curve. A
// dot sits at the delay's frequency (1 / the delay, the middle of the sweep), as high as the depth: a drag moves both, as
// one undo step, and the wheel turns the rate (in time with the song, the note value). Then its Delay and Depth, large,
// the LFO's controls, its Voices with their Phase, Spread and Shimmer (Phase and Spread faint while it has one voice a
// side, which has nothing to keep apart), and its Stereo, Feedback, High Cut and Safe Bass.
#pragma once

#include "DeviceEditorBase.h"
#include "ui/components/DisplayTicker.h"

namespace phazite::ui
{
class ChorusEditor : public DeviceEditorBase,
                     private DisplayTicker::Display
{
public:
    explicit ChorusEditor (const DeviceHandle&);
    ~ChorusEditor() override;

    juce::Component& getPicture() const noexcept;

    // Where the picture draws its dot (the delay's frequency, as high as the depth), in its own coordinates; how many copies
    // it shows each side; how far it shows a side's copy lagging now, in milliseconds; and where on its strip of the delay
    // it draws that copy, in its own coordinates.
    juce::Point<float> getDotPosition() const;
    int getShownVoices() const;
    double getShownDelay (int channel, int voice) const;
    juce::Point<float> getHeadPosition (int channel, int voice) const;

    // Shows what changed elsewhere: the controls that have nothing to act on faint. The ticker does it.
    void refresh();

private:
    class Picture;
    Picture* picture = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui

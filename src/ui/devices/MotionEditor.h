// SPDX-License-Identifier: AGPL-3.0-or-later
// Motion's editor (devices/motion/Motion.h): its three modes, Chorus, Flanger and Phaser, as tabs along its top, each a page
// of parts (DeviceEditorBase::setModes()), the LFO and the Stereo and Bass parts the same in all three. Each mode's page
// starts with its picture (SweepPicture), which reads Motion's values through the mapping to its engine's own:
// - Chorus: the comb its copies make with the sound now, over the spectrum of what it plays, the left side's lit and the
//   right side's fainter, sliding as the copies sweep; under it, a strip of the delay in milliseconds, as a tape's heads:
//   how far the sweep reaches, and where each copy lags now (one a side, or as many as it has voices, each on a line of its
//   own with the part of the sweep it has to itself once Spread gives it one); then the LFO, with a dot for each copy where
//   Voice Phase puts it in the cycle, and Shimmer's fast sine on its curve. A dot sits at the delay's frequency (1 / the
//   delay, the middle of the sweep), as high as the depth: a drag moves both, and the wheel turns the rate (in time with the
//   song, the note value). Then its Delay and Depth, large, the LFO, its Voices with their Phase, Spread and Shimmer (Phase
//   and Spread faint while it has one voice a side, which has nothing to keep apart), its Feedback and High Cut, and Stereo
//   and Safe Bass.
// - Flanger: its comb, a tooth at each of its notches, sliding as the LFO sweeps the copy's delay; under it the strip of the
//   delay, how far the sweep reaches and where each side's copy lags now. A dot sits at the delay's frequency (the comb's
//   first peak, or with the feedback below 0 its first notch), as high as the feedback (0 in the middle): a drag moves both,
//   and the wheel turns the depth. Then its Delay and Depth, large, the LFO, its Feedback, and Stereo and Safe Bass.
// - Phaser: its notches, moving as the LFO sweeps them, the left side's response glowing down to the bottom; along the
//   bottom, how far the sweep reaches. A dot sits at the frequency, as high as the feedback (0 in the middle): a drag moves
//   both, and the wheel turns the depth. Then its Frequency and Depth, large, the LFO, its Notches, Feedback and Spacing,
//   and Stereo and Safe Bass.
// Each drag of a dot is one undo step.
#pragma once

#include "DeviceEditorBase.h"
#include "ui/components/DisplayTicker.h"

namespace phazite::ui
{
class MotionEditor : public DeviceEditorBase,
                     private DisplayTicker::Display
{
public:
    explicit MotionEditor (const DeviceHandle&);
    ~MotionEditor() override;

    // The picture of the mode whose page shows (each mode has its own); and where it draws its dot, in its own coordinates.
    juce::Component& getPicture() const noexcept;
    juce::Point<float> getDotPosition() const;

    // The chorus's picture: how many copies it shows each side; how far it shows a side's copy lagging now, in milliseconds
    // (the flanger's picture's copy, while the Flanger's page shows); and where on its strip of the delay it draws that
    // copy, in its own coordinates.
    int getShownVoices() const;
    double getShownDelay (int channel, int voice = 0) const;
    juce::Point<float> getHeadPosition (int channel, int voice) const;

    // The phaser's picture: where it shows a side's notches swept to now, their centre, in hertz.
    double getShownCentre (int channel) const;

    // Shows what changed elsewhere: the controls that have nothing to act on faint. The ticker does it.
    void refresh();

private:
    class ChorusPicture;
    class FlangerPicture;
    class PhaserPicture;
    ChorusPicture* chorus = nullptr;
    FlangerPicture* flanger = nullptr;
    PhaserPicture* phaser = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui

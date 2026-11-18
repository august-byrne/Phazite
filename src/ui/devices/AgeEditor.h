// SPDX-License-Identifier: AGPL-3.0-or-later
// Age's editor (devices/age/Age.h): two tabs along its top, Record and Tape, its Medium, each a page of its parts. Its picture:
// on the record's page, the record turning at its speed, as far off its centre as its wow swings it, the stylus on it, and a
// speck where each crackle comes, which turns away with the record and fades; on the tape's, two reels turning at the Wow Rate,
// the tape running between them over the head, its line breaking for a moment where a dropout comes. Beside either, the pitch
// it played over the last three seconds; under either, the band it keeps, between its Low Cut and High Cut, whose ends drag
// (and whose knobs are at its ends), with the Band's switch. A drag up or down on the record, the reels or the pitch is the
// wow, as one undo step, and the wheel turns its rate. The pointer moves up and down over the record and the pitch, which
// ring the record, and sideways over the band, whose nearer end, which a press takes, lights up (GraphDot.h). Then its parts,
// each with its switch at the right end of its title, faint while off: Wobble (Wow, Rate, Flutter), Noise (Hiss, and Crackle
// and Density on the record, Dropouts on the tape), Drive (Distortion, and Head Bump on the tape) and Stereo (Width, Bass
// Mono).
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class AgeEditor : public DeviceEditorBase
{
public:
    explicit AgeEditor (const DeviceHandle&);
    ~AgeEditor() override;

    // The picture: the record, or the tape's reels, with the pitch and the band.
    juce::Component& getRecord() const noexcept;

    // In the picture's own coordinates: the record's middle (or the middle between the reels), as it is drawn now, and the
    // band under it.
    juce::Point<float> getRecordCentre() const;
    juce::Rectangle<int> getBandArea() const;

    // What it shows now: how far through its turn the record or the reels are (0 to 1), the pitch it plays (in cents), the
    // readings of it that its history holds, the crackles' specks on the record, and whether the tape's line is broken by a
    // dropout.
    float getShownTurn() const;
    float getShownPitch() const;
    int getNumReadings() const;
    int getNumSpecks() const;
    bool isTapeBroken() const;

    // The medium the picture draws.
    bool showsTape() const;

private:
    class Picture;
    Picture* picture = nullptr;
};
} // namespace phazite::ui

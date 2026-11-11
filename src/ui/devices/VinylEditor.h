// SPDX-License-Identifier: AGPL-3.0-or-later
// The vinyl's editor (devices/vinyl/Vinyl.h): the record turning at its speed, as far off its centre as its wow swings it,
// the stylus on it, and a speck where each crackle comes, which turns away with the record and fades; beside it, the pitch it
// played over the last three seconds, the wow's swing once a turn and the flutter's wobble on it; under them, the band the
// player keeps, between its Low Cut and High Cut, whose ends drag (and whose knobs are at its ends). A drag up or down on the
// record or the pitch is the wow, as one undo step, and the wheel turns its rate. The pointer moves up and down over the
// record and the pitch, which ring the record, and sideways over the band, whose nearer end, which a press takes, lights up
// (GraphDot.h). Then its Wow, Rate and Flutter, the groove's Distortion, Width and Bass Mono, and the surface's Hiss, Crackle
// and Density.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class VinylEditor : public DeviceEditorBase
{
public:
    explicit VinylEditor (const DeviceHandle&);
    ~VinylEditor() override;

    juce::Component& getRecord() const noexcept;

    // In the record's own coordinates: the record's middle, as it is drawn now, and the band under it.
    juce::Point<float> getRecordCentre() const;
    juce::Rectangle<int> getBandArea() const;

    // What it shows now: how far through its turn the record is (0 to 1), the pitch it plays (in cents), the readings of it
    // that its history holds, and the crackles' specks on the record.
    float getShownTurn() const;
    float getShownPitch() const;
    int getNumReadings() const;
    int getNumSpecks() const;

private:
    class Record;
    Record* record = nullptr;
};
} // namespace phazite::ui

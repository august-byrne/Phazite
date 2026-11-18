// SPDX-License-Identifier: AGPL-3.0-or-later
// The delay's editor (devices/delay/Delay.h), as FabFilter's Timeless shows its echoes: the sound coming in as a bar, then
// each echo a bar at its time after it, on a scale where the left's first echo is a step along (on the song's beats, or in
// milliseconds), fading as the feedback takes it down. While the sides differ (Link off) or bounce (Ping-Pong), the left's
// echoes stand above the middle line and the right's hang below it, each side's at its own time. Frozen, every echo is as
// tall as the first, and the word Frozen shows; ducked, they are as much lower as the duck takes them down now. What plays
// lights the bar coming in, then each echo in its turn. A drag puts the first echo under the pointer, sideways for the time
// (a note value while it follows the tempo) and up or down for the feedback, as one undo step: the left's, or with Link off
// the right's where the press is under the middle line. Under them, the band its echoes keep, between the low and the high
// cut, whose ends drag. Over the echoes the pointer is the grabbing hand and the first echo a press takes lights up; over
// the band it points sideways and the end a press takes, the nearer, lights up (GraphDot.h). Then the Time (Sync, the notes
// and times, Link, which shows the right's faint while on, and the Transition), the Feedback, large, with Ping-Pong and
// Freeze, the Character with the modulation, and the Duck.
#pragma once

#include "DeviceEditorBase.h"
#include "ui/components/DisplayTicker.h"

namespace phazite::ui
{
class DelayEditor : public DeviceEditorBase,
                    private DisplayTicker::Display
{
public:
    explicit DelayEditor (const DeviceHandle&);
    ~DelayEditor() override;

    juce::Component& getEchoes() const noexcept;

    // Where the echoes' display draws a side's echo number n (0 for the sound coming in), in its own coordinates, its bar's
    // foot; how tall it draws it, as a share of the sound coming in's; and the band its echoes keep.
    juce::Point<float> getEchoPosition (int n, int side = 0) const;
    float getEchoLevel (int n, int side = 0) const;
    juce::Rectangle<int> getBandArea() const;

    // How lit it draws the sound coming in now (0 to 1), and an echo of what came in a time ago, as if at full level: the peak
    // of what came in over the time that covers that moment, each peak taken with its moment. Afresh when it shows again.
    float getInputLight() const;
    float getLightAgo (double seconds) const;

    // What its readout says: the times, the feedback, and Frozen while it is.
    juce::String getReadout() const;

    // Shows what changed elsewhere: the right's time faint while Link is on. The ticker does it.
    void refresh();

private:
    class Echoes;
    Echoes* echoes = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's arm switch (docs/design.md, section 17; roadmap step 3.4), beside its mute and solo in its playlist header and
// its mixer strip: one switch, shown in both places, as FL Studio shows its arm on the mixer insert and the playlist track,
// and on every track that records, as Ableton Live shows one on every track. Lit, the track records when Record is pressed:
// - a track without an instrument records an input of the audio device. A click arms the track, or disarms it; on a track
//   without an input it opens the input list instead, where choosing one arms it. Right-click: the inputs (each alone, then
//   the pairs, the one chosen last first), when the input is heard (while armed, always, never), and Arm Only This Track;
// - a track with an instrument records what is played on MIDI keyboards and the computer keyboard, which play the selected
//   one with an instrument: its switch is lit on the track they play, as Ableton Live arms the selected MIDI track by itself.
//   A click on another's sends them there, armed; on the lit one, it stops recording onto it, until it is clicked again or
//   they play another (model/transport/RecordingModel.h: setMidiArmed()).
// Arming elsewhere (the other place, the Arm Track command, an undone input, another track selected) shows at its next tick
// (DisplayTicker.h).
#pragma once

#include "DisplayTicker.h"
#include "model/base/IDs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class ArmButton : public juce::Button,
                  private DisplayTicker::Display
{
public:
    explicit ArmButton (ProjectContext&);

    // The track it arms (an empty ID for none), and what it shows: whether the track records (an input, or MIDI), whether it
    // is armed, and whether Record records onto it now.
    void setTrack (TrackID);
    void update();
    bool isShown() const noexcept { return shown; } // the track can record
    bool recordsMidi() const noexcept { return midi; }

    // Called when what it shows changes (shown or not, armed or not, its input, recording or not), for what lays it out and
    // shows what the track records.
    std::function<void()> onChange;

    // The input list, as a click on a track without an input or a right-click opens it (a track that records MIDI has none).
    void showMenu();

    void clicked (const juce::ModifierKeys&) override;
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    ProjectContext& context;
    TrackID track;
    bool shown = false, midi = false, recording = false;
    juce::String shownInput;

    using juce::Button::clicked;
    void tick() override;
};

// What a track records from while it is armed, for its header and its strip: "In 1", "In 1+2" or "MIDI", and "Recording In 1"
// while Record records onto it; empty while it isn't armed.
juce::String describeArmedInput (const ProjectContext&, TrackID);
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's arm switch (docs/design.md, section 17; roadmap step 3.4), beside its mute and solo in its playlist header and
// its mixer strip: one switch, shown in both places, as FL Studio shows its arm on the mixer insert and the playlist track.
// Lit, the track records its input when Record is pressed. A click arms the track, or disarms it; on a track without an
// input it opens the input list instead, where choosing one arms it. Right-click: the inputs (each alone, then the pairs,
// the one chosen last first), when the input is heard (while armed, always, never), and Arm Only This Track. It shows
// only for a track that can record audio: a track without an instrument (model/transport/RecordingModel.h). Arming
// elsewhere (the other place, the Arm Track command, an undone input) shows at its next tick (DisplayTicker.h).
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

    // The track it arms (an empty ID for none), and what it shows: whether the track can record audio, and is armed.
    void setTrack (TrackID);
    void update();
    bool isShown() const noexcept { return shown; } // the track can record audio

    // Called when what it shows changes (shown or not, armed or not, its input), for what lays it out and shows the input.
    std::function<void()> onChange;

    // The input list, as a click on a track without an input or a right-click opens it.
    void showMenu();

    void clicked (const juce::ModifierKeys&) override;
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    ProjectContext& context;
    TrackID track;
    bool shown = false;
    juce::String shownInput;

    using juce::Button::clicked;
    void tick() override;
};

// What a track records from, for its header and its strip while it is armed: "In 1", or "In 1+2"; empty while it isn't.
juce::String describeArmedInput (const ProjectContext&, TrackID);
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// The tempo lane in the playlist (docs/design.md, section 4): under the time bar while the song's tempo changes, the tempo
// along the song as a line, as the engine plays it, with a point where each tempo starts and its value beside it. Its line
// to the next tempo holds and then jumps, or ramps, straight or bent, as an automation lane's does, and the lane takes the
// automation lanes' gestures: a click adds a tempo change, a press on the line pulls one from it, a point drags (on the grid
// unless Alt is held, Shift for fine steps), the handle in a ramp's middle bends it, and a double-click deletes a point or
// straightens a ramp. Right-click for a line's jump or ramp. Its header, over the track headers, offers Add Tempo Change and
// Remove Tempo Changes, which takes the lane away. It reaches the tempos through the model (model/transport/TempoModel.h).
#pragma once

#include "model/transport/TempoModel.h"
#include "ui/theme/DesignTokens.h"
#include "ui/timeline/TimelineEditor.h"
#include "ui/timeline/TimelineGesture.h"

#include <optional>
#include <vector>

namespace phazite::ui
{
class TempoLane : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    static constexpr int preferredHeight = TempoTint::laneHeight;

    // In an editor, with the tempos of the project it shows (TimelineEditor::getContext()).
    explicit TempoLane (TimelineEditor&);

    // Another project is open, or none: a drag in progress stops. The editor calls this, and tempoChanged() when the song's
    // tempos change.
    void showProject();
    void tempoChanged();

    // Where each tempo's point is, and the bend handle of the ramp from one to the next (nothing for a step, or a ramp too
    // narrow or flat to bend); the point and the handle at a position, if any; and the menu a right-click there shows
    // (tests run its items).
    juce::Point<float> getPointPosition (int index) const;
    std::optional<juce::Point<float>> getHandle (int line) const;
    std::optional<int> getPointAt (juce::Point<float>) const;
    std::optional<int> getHandleAt (juce::Point<float>) const;
    juce::PopupMenu getMenuAt (juce::Point<float>);

    // The tempos it shows, in beats per minute, from the bottom of its line's area to its top.
    juce::Range<double> getShownTempos() const noexcept { return shown; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    enum class Drag
    {
        none,
        pending, // until the mouse moves past the drag threshold
        add,     // a click on empty space adds a tempo change where it goes up
        move,
        bend
    };

    TimelineEditor& editor;
    ProjectContext& context;
    std::vector<TempoChange> tempos, before;
    juce::Range<double> shown { 110.0, 130.0 }; // kept while a drag goes on, so that the lane doesn't rescale under it
    TimelineGesture gesture;
    Drag drag = Drag::none, pendingDrag = Drag::none;
    juce::Point<float> start;
    int pressed = -1;  // the point pressed, or the line a bend bends
    int selected = -1; // the point clicked last, shown larger
    int hovered = -1;  // the line under the pointer, whose handle shows

    void refresh(); // reads the tempos, and fits the tempos shown to them unless a drag goes on

    juce::Rectangle<float> getLineArea() const;
    float tempoToY (double bpm) const;
    double yToTempo (float y) const; // past the lane's edges, on as far as the tempo's limits
    float beatToX (BeatPosition) const;
    BeatPosition xToBeat (float x) const;

    int getLineUnder (float x) const; // the tempo whose line to the next is across x, or -1
    bool isOnLine (juce::Point<float>) const;
    juce::String describeHere (juce::Point<float>) const;

    void pressPoint (int index);
    void pullPointFrom (const juce::MouseEvent&);
    void movePoint (const juce::MouseEvent&);
    void bendLine (const juce::MouseEvent&);
    void addLineMenu (juce::PopupMenu&, int line);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoLane)
};

//==============================================================================
// Left of the tempo lane, over the track headers: its name, and a menu with Add Tempo Change and Remove Tempo Changes.
class TempoLaneHeader : public juce::Component
{
public:
    explicit TempoLaneHeader (TimelineEditor&);

    void update(); // after the project changed

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::PopupMenu getMoreMenu(); // what its button shows (tests run its items)

private:
    TimelineEditor& editor;
    ProjectContext& context;
    juce::Label nameLabel { {}, "Tempo" };
    juce::TextButton moreButton { "..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoLaneHeader)
};
} // namespace phazite::ui

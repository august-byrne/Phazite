// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane in the playlist (docs/design.md, section 14; roadmap step 2.7): under the time bar, the song's chords
// over their time, each with its name and its numeral in the key there, and a line where the key changes. A click on a
// chord offers the key's chords and others to put in its place, its kind and inversion, and Delete; a click on empty
// space adds a chord there, a bar long. Dragging a chord's right edge makes it longer or shorter. Its header, over the
// track headers, shows the song's key and offers the progressions and the parts made from the chords. It reaches the
// chords through the model (model/chords/ChordModel.h).
#pragma once

#include "model/chords/ChordModel.h"
#include "ui/theme/DesignTokens.h"
#include "ui/timeline/TimelineEditor.h"

#include <optional>

namespace phazite::ui
{
class ChordLane : public juce::Component
{
public:
    static constexpr int preferredHeight = ChordTint::laneHeight;

    // In an editor, with the chords of the project it shows (TimelineEditor::getContext()).
    explicit ChordLane (TimelineEditor&);

    // Another project is open, or none: a drag in progress stops. The editor calls this, and repaints the lane when the
    // chords change.
    void showProject();

    // The menu a click at a place shows: the chord there's, or that of the empty space there. Shown at once by clicks;
    // tests run its items.
    juce::PopupMenu getMenuAt (juce::Point<int>);

    // Where a chord's dragged edge would go, while it is dragged.
    std::optional<BeatPosition> getDragTarget() const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Drag
    {
        BeatPosition chordStart;
        bool onEdge = false;
        bool dragging = false;
        std::optional<BeatPosition> target; // the new end
    };

    TimelineEditor& editor;
    ProjectContext& context;
    std::optional<Drag> drag;
    std::optional<BeatPosition> hovered; // the start of the chord under the pointer

    juce::Rectangle<int> getBlockArea (const LaneChord&) const;
    std::optional<LaneChord> getChordAt (juce::Point<int>) const;
    bool isOnRightEdge (const LaneChord&, juce::Point<int>) const;

    void addChordMenu (juce::PopupMenu&, const LaneChord&);
    void addEmptyMenu (juce::PopupMenu&, BeatPosition);
    BeatRange getNewChordRange (BeatPosition) const; // a bar from the bar's start, up to the next chord

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordLane)
};

//==============================================================================
// Left of the chord lane, over the track headers: its name, the song's key (a menu of keys), and a menu with the
// progressions and the parts made from the chords.
class ChordLaneHeader : public juce::Component
{
public:
    explicit ChordLaneHeader (TimelineEditor&);

    void update(); // the key shown, after the chords or the project changed

    void paint (juce::Graphics&) override;
    void resized() override;

    // For tests.
    juce::TextButton& getKeyButton() noexcept { return keyButton; }
    juce::PopupMenu getKeyMenu();
    juce::PopupMenu getMoreMenu();

private:
    TimelineEditor& editor;
    ProjectContext& context;
    juce::Label nameLabel { {}, "Chords" };
    juce::TextButton keyButton, moreButton { "..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordLaneHeader)
};

// The parts made from the chords, by kind and style, as a menu of submenus: they go where getChordPartRange() says.
juce::PopupMenu getMakePartMenu (ProjectContext&);

// A menu of keys, a submenu of scales for each root, with the key ticked; choosing one calls back.
juce::PopupMenu getKeyChoiceMenu (const Key& current, std::function<void (Key)> choose);
} // namespace phazite::ui

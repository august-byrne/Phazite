// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections in the playlist's time bar (docs/design.md, section 6): a thin band above the bar
// numbers, with each section's name over its time and the energy curve faintly behind. The playlist draws
// the sections as ghosts behind the tracks too. Dragging a section moves it with everything in it, across
// every track; Ctrl copies it and Ctrl+Shift copies it as aliases. Dragging its right edge makes it longer or
// shorter, and the rest of the song moves along. Double-clicking adds or renames one, and right-clicking here
// or on the bar numbers offers the song outlines. The hint bar shows each section's tip.
#pragma once

#include "model/arrangement/PlaylistModel.h"
#include "ui/timeline/TimelineEditor.h"

#include <optional>

namespace phazite::ui
{
class SectionsStrip : public juce::Component,
                      private juce::ChangeListener
{
public:
    static constexpr int preferredHeight = 18;

    // In an editor's time bar, with the sections of the project it shows (TimelineEditor::getContext()).
    explicit SectionsStrip (TimelineEditor&);
    ~SectionsStrip() override;

    // Another project is open, or none: a drag or a rename in progress stops. The editor calls this, and repaints the
    // strip when the sections or the tempo change.
    void showProject();

    // Shows the section's name in a text field, to rename it.
    void startRenaming (SectionID);

    // The menu of the section at a place in the song, which it selects, or of none there: renaming and coloring it,
    // adding one there, and the song outlines. The time bar's bar numbers show it too.
    void showMenuAt (BeatPosition);

    // While a section is dragged: where it would go (moving), or where its end would be (resizing). The
    // playlist draws it across the tracks too; onDragChanged says when to repaint.
    std::optional<BeatPosition> getDragTarget() const;
    std::function<void()> onDragChanged;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    // What the mouse is doing, from the moment it goes down until it goes up.
    struct Drag
    {
        enum class Kind
        {
            pending, // not dragging yet
            move,    // the section, to a boundary; with Ctrl, a copy
            resize   // its right edge
        };

        Kind kind = Kind::pending;
        SectionID section;
        bool onEdge = false;
        std::optional<BeatPosition> target; // move: the boundary it would go to; resize: the new end
    };

    TimelineEditor& editor;
    ProjectContext& context;
    SongSelection& selection;
    SongRange& range;
    std::optional<Drag> drag;
    juce::Label nameLabel; // shown over a section while it is renamed
    SectionID renaming;

    juce::Rectangle<int> getBlockArea (const SongSection&) const; // where the section's block is drawn
    std::optional<SongSection> getSectionAt (juce::Point<int>) const;
    bool isOnRightEdge (const SongSection&, juce::Point<int>) const;
    BeatPosition findNearestBoundary (const SongSection&, double x) const;
    void paintEnergyCurve (juce::Graphics&, const std::vector<SongSection>&);
    void showMenu (const std::optional<SongSection>&, BeatPosition where);
    void addSectionAt (BeatPosition);
    void renamed(); // the name field closed
    void dragChanged();

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); } // the selection changed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionsStrip)
};
} // namespace phazite::ui

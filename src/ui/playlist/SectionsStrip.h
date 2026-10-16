// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections in the playlist's time bar (docs/design.md, section 6): a thin band above the bar
// numbers, with each section's name over its time and the energy curve faintly behind. The playlist draws
// the sections as ghosts behind the tracks too. Dragging a section moves it with everything in it, across
// every track; Ctrl copies it and Ctrl+Shift copies it as aliases. Dragging its right edge makes it longer or
// shorter, and the rest of the song moves along. Double-clicking adds or renames one, and right-clicking here
// or on the bar numbers offers the song outlines. The hint bar shows each section's tip.
#pragma once

#include "model/arrangement/Selection.h"
#include "ui/timeline/TimelineEditor.h"

#include <optional>

namespace phazite::ui
{
class SectionsStrip : public juce::Component,
                      private juce::ValueTree::Listener,
                      private juce::ChangeListener
{
public:
    static constexpr int preferredHeight = 18;

    SectionsStrip (TimelineEditor&, te::SelectionManager&, RangeSelection&);
    ~SectionsStrip() override;

    // Follows the editor's Edit; call after the editor's changed.
    void setEdit (te::Edit*);

    // Shows the section's name in a text field, to rename it.
    void startRenaming (SectionID);
    void startRenaming (te::ArrangerClip&);

    // The menu for a section (or none) at a place in the song: renaming and coloring it, adding one there,
    // and the song outlines. The time bar's bar numbers show it too.
    void showMenu (te::ArrangerClip*, te::BeatPosition where);
    void showMenuAt (BeatPosition); // the menu of the section there, which it selects, or of none

    // While a section is dragged: where it would go (moving), or where its end would be (resizing). The
    // playlist draws it across the tracks too; onDragChanged says when to repaint.
    std::optional<te::BeatPosition> getDragTarget() const;
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
        te::SafeSelectable<te::ArrangerClip> section;
        bool onEdge = false;
        std::optional<te::BeatPosition> target; // move: the boundary it would go to; resize: the new end
    };

    TimelineEditor& editor;
    te::SelectionManager& selection;
    RangeSelection& range;
    te::SafeSelectable<te::Edit> edit;
    juce::ValueTree editState;
    std::optional<Drag> drag;
    juce::Label nameLabel; // shown over a section while it is renamed
    te::SafeSelectable<te::ArrangerClip> renaming;

    juce::Rectangle<int> getBlockArea (const te::Clip&) const; // where the section's block is drawn
    te::ArrangerClip* getSectionAt (juce::Point<int>) const;
    bool isOnRightEdge (const te::Clip&, juce::Point<int>) const;
    te::BeatPosition findNearestBoundary (const te::ArrangerClip&, double x) const;
    void paintEnergyCurve (juce::Graphics&, const juce::Array<te::ArrangerClip*>&);
    void addSectionAt (te::BeatPosition);
    void renamed(); // the name field closed
    void dragChanged();

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); } // the selection changed
    void sectionsChanged (const juce::ValueTree&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionsStrip)
};
} // namespace phazite::ui

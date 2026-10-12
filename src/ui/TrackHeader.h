// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's header in the playlist: its color, name, mute and solo. Clicking selects the track,
// double-clicking the name renames it, dragging moves the track, and dragging the bottom edge resizes it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
namespace te = tracktion;

class TrackHeader : public juce::Component
{
public:
    // What a header leaves to the playlist.
    struct Owner
    {
        virtual ~Owner() = default;

        virtual void trackClicked (te::Track&, const juce::ModifierKeys&) = 0; // selects it
        virtual void showTrackMenu (te::Track&) = 0;
        virtual void showColourMenu (te::Track&, juce::Component& target) = 0;
        virtual void trackDragged (TrackHeader&, const juce::MouseEvent&) = 0; // moving it to another row
        virtual void trackDragEnded (TrackHeader&, const juce::MouseEvent&) = 0;
        virtual void trackHeightChanged() = 0; // while its edge is dragged
    };

    TrackHeader (Owner&, te::Track&);

    te::Track* getTrack() const { return track.get(); }

    // Shows the track as it is now: its name, color, mute and solo, whether it is selected, and how deep
    // in folders it is (its name is indented as far). A folder has a triangle that collapses it.
    void update (juce::Colour, bool isSelected, int depth);

    void startRenaming(); // shows the name in a text field

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override { updateCollapseButton(); } // the theme's colors
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    enum class Drag
    {
        none,
        pending, // the mouse is down, but hasn't moved far enough to drag yet
        move,
        resize
    };

    Owner& owner;
    te::SafeSelectable<te::Track> track;
    juce::Label nameLabel;
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    juce::ShapeButton collapseButton { "Collapse folder", juce::Colours::grey, juce::Colours::lightgrey, juce::Colours::white };
    juce::Colour colour;
    bool selected = false, isFolder = false, collapsed = false;
    int depth = 0;

    void updateCollapseButton();

    Drag drag = Drag::none;
    bool selectOnMouseUp = false; // a click on a selected track selects it alone, unless it starts a drag
    int heightAtDragStart = 0;

    juce::Rectangle<int> getColourArea() const;
    bool isOnResizeEdge (juce::Point<int>) const;
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's header in the playlist: its color, a handle to move it by, a chevron that collapses it, its name,
// mute and solo. Clicking selects the track, double-clicking the name renames it, dragging the handle moves the
// track, dragging the bottom edge resizes it, and dragging elsewhere selects the tracks the mouse passes.
#pragma once

#include "ui/components/Controls.h"
#include "ui/theme/Icons.h"

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
        virtual void trackSelectionDragged (TrackHeader&, const juce::MouseEvent&) = 0; // selecting the tracks it passes
        virtual void trackHeightChanged() = 0;                                          // while its edge is dragged
    };

    TrackHeader (Owner&, te::Track&);

    te::Track* getTrack() const { return track.get(); }

    // Shows the track as it is now: its name, color, mute and solo, whether it is selected and collapsed, and how
    // deep in folders it is (its name is indented as far).
    void update (juce::Colour, bool isSelected, int depth);

    void startRenaming(); // shows the name in a text field

    // The handle that moves the track, at its left.
    juce::Rectangle<int> getGripArea() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override { updateColours(); } // the theme's colors
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    enum class Drag
    {
        none,
        pending, // the mouse went down on the handle, but hasn't moved far enough to drag yet
        move,
        resize,
        selectPending, // elsewhere on the header: a drag selects the tracks it passes
        select
    };

    Owner& owner;
    te::SafeSelectable<te::Track> track;
    juce::Label nameLabel;
    MuteSoloButtons muteSolo;
    IconButton collapseButton { "Collapse track", Icon::expanded };
    std::unique_ptr<juce::Drawable> gripIcon;
    juce::Colour gripIconColour { juce::Colours::black }; // the color the drawing is in now
    juce::Colour colour;
    bool selected = false, isFolder = false, collapsed = false;
    int depth = 0;

    juce::Colour getBackground() const; // the surface, tinted with the track's color
    void updateColours();
    void updateCollapseButton();

    Drag drag = Drag::none;
    bool selectOnMouseUp = false; // a click on a selected track selects it alone, unless it starts a drag
    int heightAtDragStart = 0;

    juce::Rectangle<int> getColourArea() const;
    bool isOnResizeEdge (juce::Point<int>) const;
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's header in the playlist: its color, a handle to move it by, a chevron that collapses it (and its lanes), its name,
// mute and solo. Clicking selects the track, double-clicking the name renames it, dragging the handle moves the
// track, dragging the bottom edge resizes it, and dragging elsewhere selects the tracks the mouse passes.
#pragma once

#include "model/arrangement/PlaylistModel.h"
#include "ui/components/Controls.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class TrackHeader : public juce::Component
{
public:
    // What a header leaves to the playlist.
    struct Owner
    {
        virtual ~Owner() = default;

        virtual void trackClicked (TrackID, const juce::ModifierKeys&) = 0; // selects it
        virtual void showTrackMenu (TrackID) = 0;
        virtual void showColourMenu (TrackID, juce::Component& target) = 0;
        virtual void trackDragged (TrackHeader&, const juce::MouseEvent&) = 0; // moving it to another row
        // While moving it: true if it went out of the playlist and is now carried elsewhere (into the browser, to save it).
        virtual bool trackDraggedOut (TrackHeader&, const juce::MouseEvent&) = 0;
        virtual void trackDragEnded (TrackHeader&, const juce::MouseEvent&) = 0;
        virtual void trackSelectionDragged (TrackHeader&, const juce::MouseEvent&) = 0; // selecting the tracks it passes
        virtual void trackHeightChanged() = 0;                                          // while its edge is dragged
    };

    TrackHeader (Owner&, ProjectContext&, const TrackRow&);

    TrackID getTrack() const noexcept { return row.id; }

    // Shows the track as it is now (getPlaylistRows() in model/arrangement/PlaylistModel.h): its name, color, mute and
    // solo, whether it is collapsed and how deep in folders it is (its name is indented as far), and whether it is
    // selected.
    void update (const TrackRow&, bool isSelected);

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
        select,
        out // moved out of the playlist, carried elsewhere
    };

    Owner& owner;
    ProjectContext& context;
    TrackRow row; // as it was shown last
    juce::Label nameLabel;
    MuteSoloButtons muteSolo;
    IconButton collapseButton { "Collapse track", Icon::expanded };
    std::unique_ptr<juce::Drawable> gripIcon;
    juce::Colour gripIconColour { juce::Colours::black }; // the color the drawing is in now
    bool selected = false;

    bool isFolder() const noexcept { return row.kind == TrackType::folder; }
    bool isMaster() const noexcept { return row.kind == TrackType::master; }
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

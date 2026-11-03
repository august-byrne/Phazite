// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's header in the playlist: a rail in the color of each folder it is in (paintTrackStrips()), its own color, a
// folder's or a return's icon (a folder, open while its tracks show, as most DAWs mark their folders and groups, or the
// Return key's arrow), its name, its switch for mute and solo, an arm switch for a track that records audio (with what it
// records under its name while armed), and a snowflake while it is frozen (a click unfreezes it). Clicking selects the
// track, dragging it anywhere moves the selected tracks, as Bitwig, Logic Pro and Studio One move tracks by their headers,
// double-clicking collapses it to a thin row or expands it (a folder hides its tracks or shows them), and dragging the
// bottom edge resizes it.
#pragma once

#include "model/arrangement/PlaylistModel.h"
#include "ui/components/ArmButton.h"
#include "ui/components/TrackSwitch.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// A header's left edge, as a track's and its automation lanes' show it: for each folder the track is in, a rail in the
// folder's color, which runs on down the folder's tracks, beside the folder's tint; then the track's own color, which runs
// on down its lanes (and a folder's down its tracks' rails). And how wide that is.
int getTrackStripsWidth (const TrackRow&);
void paintTrackStrips (juce::Graphics&, const juce::Component&, const TrackRow&);

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
        virtual void trackDragged (TrackHeader&, const juce::MouseEvent&) = 0; // moving the selected tracks to another place
        // While moving them: true if it went out of the playlist and is now carried elsewhere (into the browser, to save it).
        virtual bool trackDraggedOut (TrackHeader&, const juce::MouseEvent&) = 0;
        virtual void trackDragEnded (TrackHeader&, const juce::MouseEvent&) = 0;
        virtual void trackHeightChanged() = 0; // while its edge is dragged
    };

    TrackHeader (Owner&, ProjectContext&, const TrackRow&);

    TrackID getTrack() const noexcept { return row.id; }

    // Shows the track as it is now (getPlaylistRows() in model/arrangement/PlaylistModel.h): its name, color, mute and
    // solo, whether it is collapsed and the folders it is in (it is indented as far, beside their rails), and whether it is
    // selected.
    void update (const TrackRow&, bool isSelected);

    void startRenaming(); // shows the name in a text field

    // The snowflake, which shows while the track is frozen (for tests).
    IconButton& getFrozenButton() noexcept { return frozenButton; }

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
        pending, // the mouse went down, but hasn't moved far enough to move the tracks yet
        move,
        resize,
        out // moved out of the playlist, carried elsewhere
    };

    Owner& owner;
    ProjectContext& context;
    TrackRow row; // as it was shown last
    juce::Label nameLabel;
    TrackSwitch muteSolo;                                       // mute (a click) and solo (a right-click)
    ArmButton arm;                                              // a track that records audio's
    juce::Label inputLabel;                                     // what it records, while it is armed
    IconButton frozenButton { "Unfreeze track", Icon::frozen }; // while it is frozen
    std::unique_ptr<juce::Drawable> kindIcon;                   // a folder's or a return's, before its name
    juce::Colour kindIconColour { juce::Colours::black };       // the color the drawing is in now
    juce::Rectangle<int> kindIconArea;
    bool selected = false;

    bool isFolder() const noexcept { return row.kind == TrackType::folder; }
    bool isMaster() const noexcept { return row.kind == TrackType::master; }
    juce::String getAccessibleName() const; // its name, and what it is if it isn't a track
    juce::Colour getBackground() const;     // the surface, tinted with the track's color
    void updateColours();
    void updateKindIcon(); // a folder's, open or closed
    void updateFrozenButton();
    void showArmedInput();

    Drag drag = Drag::none;
    bool selectOnMouseUp = false; // a click on a selected track selects it alone, unless it starts a drag
    int heightAtDragStart = 0;

    juce::Rectangle<int> getColourArea() const;
    bool isOnResizeEdge (juce::Point<int>) const;
};
} // namespace phazite::ui

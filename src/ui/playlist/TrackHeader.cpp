// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackHeader.h"
#include "TrackHeights.h"
#include "model/arrangement/Tracks.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int colourStripWidth = 8;
    constexpr int gripWidth = 14;       // the handle that moves the track, right of the color strip
    constexpr int resizeEdgeHeight = 5; // the bottom edge, which resizes the track when dragged
    constexpr int buttonSize = 24;
    constexpr int minButtonSize = 16;  // mute and solo shrink to fit a short row, down to this
    constexpr int dragThreshold = 4;   // pixels the mouse moves before a press on the handle becomes a drag
    constexpr int indentPerLevel = 12; // a track in a folder is indented this much more than the folder
    constexpr int collapseButtonSize = 20;
    constexpr float gripAlpha = 0.6f; // the handle is drawn in the header's ink, a little muted
} // namespace

TrackHeader::TrackHeader (Owner& o, ProjectContext& context, te::Track& trackToShow)
    : owner (o),
      track (trackToShow),
      muteSolo (context),
      gripIcon (createIcon (Icon::grip))
{
    setDescription ("Click to select the track, Ctrl+click to add it to the selection, Shift+click to select up to it. "
                    "Drag the handle at the left to move the track, and the bottom edge to resize it. The arrow "
                    "collapses it. Double-click the name to rename it, and right-click for more.");

    // The header handles clicks on the name; the text field gets them while renaming.
    nameLabel.setTitle ("Track name");
    nameLabel.setFont (juce::FontOptions (TextSize::body));
    nameLabel.setInterceptsMouseClicks (false, true);
    nameLabel.onTextChange = [this]
    {
        if (auto* t = track.get())
            renameTrack (*t, nameLabel.getText()); // an empty name gives the track its number back
    };

    muteSolo.setTrack (getTrackID (trackToShow));

    // The chevron collapses a track to a thin row, and hides a folder's tracks.
    isFolder = trackToShow.isFolderTrack();
    collapseButton.setQuiet (true);
    collapseButton.setMouseClickGrabsKeyboardFocus (false);
    collapseButton.onClick = [this]
    {
        auto* t = track.get();

        if (t == nullptr)
            return;

        // Alt+click: every track, as in Ableton Live.
        const auto collapse = ! isCollapsed (*t);

        if (juce::ModifierKeys::currentModifiers.isAltDown())
            for (auto* other : getPlaylistTracks (t->edit))
                setCollapsed (*other, collapse);
        else
            setCollapsed (*t, collapse);
    };

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &muteSolo, &collapseButton })
        addAndMakeVisible (c);

    updateCollapseButton();
    updateColours();
}

void TrackHeader::update (juce::Colour newColour, bool isSelected, int newDepth)
{
    auto* t = track.get();

    if (t == nullptr)
        return;

    const auto name = t->getName();
    setTitle (isFolder ? name + " (folder)" : name); // what screen readers call the header

    if (! nameLabel.isBeingEdited())
        nameLabel.setText (name, juce::dontSendNotification);

    muteSolo.update(); // and whether other tracks' solos silence it

    if (collapsed != isCollapsed (*t))
    {
        collapsed = ! collapsed;
        updateCollapseButton();
    }

    if (colour != newColour || selected != isSelected || depth != newDepth)
    {
        colour = newColour;
        selected = isSelected;
        depth = newDepth;
        updateColours();
        resized();
        repaint();
    }
}

void TrackHeader::updateCollapseButton()
{
    // Pointing down while open, right while collapsed.
    collapseButton.setIcon (collapsed ? Icon::folded : Icon::expanded);

    if (isFolder)
    {
        collapseButton.setTitle (collapsed ? "Expand folder" : "Collapse folder");
        collapseButton.setTooltip (collapsed ? "Shows the folder's tracks. Alt+click: every track"
                                             : "Hides the folder's tracks. Alt+click: every track");
    }
    else
    {
        collapseButton.setTitle (collapsed ? "Expand track" : "Collapse track");
        collapseButton.setTooltip (collapsed ? "Shows the track at its full height. Alt+click: every track"
                                             : "Makes the track a thin row that shows its name. Alt+click: every track");
    }
}

juce::Colour TrackHeader::getBackground() const
{
    // In the track's color, which runs on through its row in the playlist; more of it while selected.
    return getPalette (*this).surface.interpolatedWith (colour, selected ? TrackTint::selectedHeader : TrackTint::header);
}

void TrackHeader::updateColours()
{
    // Black or white, whichever reads better on the track's color.
    nameLabel.setColour (juce::Label::textColourId, getInkFor (getBackground()));
    repaint();
}

void TrackHeader::startRenaming()
{
    nameLabel.showEditor();
}

juce::Rectangle<int> TrackHeader::getColourArea() const
{
    return getLocalBounds().removeFromLeft (colourStripWidth).withTrimmedBottom (1);
}

juce::Rectangle<int> TrackHeader::getGripArea() const
{
    return getLocalBounds().withTrimmedLeft (colourStripWidth).withWidth (gripWidth).withTrimmedBottom (resizeEdgeHeight);
}

bool TrackHeader::isOnResizeEdge (juce::Point<int> position) const
{
    return position.y >= getHeight() - resizeEdgeHeight;
}

//==============================================================================
void TrackHeader::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto background = getBackground();
    auto area = getLocalBounds();

    g.setColour (background);
    g.fillRect (area);

    g.setColour (colour);
    g.fillRect (getColourArea());

    // The handle, lined up with the name.
    if (gripIcon != nullptr)
    {
        if (const auto ink = getInkFor (background).withAlpha (gripAlpha); ink != gripIconColour)
        {
            gripIcon->replaceColour (gripIconColour, ink);
            gripIconColour = ink;
        }

        const auto topRow = juce::jmin (getHeight() - 1, controlHeight + Space::xs);
        gripIcon->drawWithin (g, getGripArea().withHeight (topRow).toFloat().withSizeKeepingCentre ((float) gripWidth, 16.0f),
                              juce::RectanglePlacement::centred, 1.0f);
    }

    g.setColour (p.border);
    g.fillRect (area.removeFromBottom (1));
}

void TrackHeader::resized()
{
    auto area = getLocalBounds()
                    .withTrimmedLeft (colourStripWidth + gripWidth + depth * indentPerLevel)
                    .withTrimmedRight (Space::xs)
                    .withTrimmedBottom (1);
    auto top = area.removeFromTop (juce::jmin (area.getHeight(), controlHeight + Space::xs));

    // Mute and solo shrink to fit a collapsed row. A row shorter than that has room only for the name.
    const auto size = juce::jmin (buttonSize, top.getHeight() - 2);
    const auto roomForButtons = size >= minButtonSize;
    muteSolo.setVisible (roomForButtons);
    collapseButton.setVisible (roomForButtons);

    if (roomForButtons)
    {
        const auto buttonsWidth = 2 * size + Space::xs / 2;
        muteSolo.setBounds (top.removeFromRight (buttonsWidth).withSizeKeepingCentre (buttonsWidth, size));
        top.removeFromRight (Space::xs);
        collapseButton.setBounds (top.removeFromLeft (collapseButtonSize).withSizeKeepingCentre (collapseButtonSize, size));
    }

    nameLabel.setBounds (top);
}

void TrackHeader::mouseMove (const juce::MouseEvent& e)
{
    if (isOnResizeEdge (e.getPosition()))
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else if (getGripArea().contains (e.getPosition()))
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    auto* t = track.get();
    drag = Drag::none;
    selectOnMouseUp = false;

    if (t == nullptr)
        return;

    if (isOnResizeEdge (e.getPosition()) && ! e.mods.isPopupMenu())
    {
        // Alt-click resets the height, as in FL Studio.
        if (e.mods.isAltDown())
        {
            setShownTrackHeight (*t, TrackHeight::standard);
            owner.trackHeightChanged();
            return;
        }

        drag = Drag::resize;
        heightAtDragStart = getShownTrackHeight (*t);
        return;
    }

    if (e.mods.isPopupMenu())
    {
        if (! selected)
            owner.trackClicked (*t, {});

        owner.showTrackMenu (*t);
        return;
    }

    if (getColourArea().contains (e.getPosition()))
    {
        if (! selected)
            owner.trackClicked (*t, {});

        owner.showColourMenu (*t, *this);
        return;
    }

    // Clicking a selected track keeps the selection, so all of it can be dragged by the handle; without a drag,
    // the click selects this track alone when the mouse goes up.
    if (selected && ! (e.mods.isCommandDown() || e.mods.isShiftDown()))
        selectOnMouseUp = true;
    else
        owner.trackClicked (*t, e.mods);

    // Only the handle moves tracks, so that clicks elsewhere don't move them by accident: there, a drag selects.
    drag = getGripArea().contains (e.getPosition()) ? Drag::pending : Drag::selectPending;
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    auto* t = track.get();

    if (t == nullptr)
        return;

    if (drag == Drag::resize)
    {
        setShownTrackHeight (*t, heightAtDragStart + e.getDistanceFromDragStartY());
        owner.trackHeightChanged();
        return;
    }

    if (e.getDistanceFromDragStart() > dragThreshold)
    {
        if (drag == Drag::pending)
            drag = Drag::move;
        else if (drag == Drag::selectPending)
            drag = Drag::select;
    }

    if (drag == Drag::move)
    {
        owner.trackDragged (*this, e);
    }
    else if (drag == Drag::select)
    {
        selectOnMouseUp = false;
        owner.trackSelectionDragged (*this, e);
    }
}

void TrackHeader::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::move)
        owner.trackDragEnded (*this, e);
    else if (auto* t = track.get(); t != nullptr && selectOnMouseUp)
        owner.trackClicked (*t, {});

    drag = Drag::none;
    selectOnMouseUp = false;
}

void TrackHeader::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto* t = track.get();

    if (t == nullptr)
        return;

    if (isOnResizeEdge (e.getPosition()))
    {
        setShownTrackHeight (*t, TrackHeight::standard);
        owner.trackHeightChanged();
    }
    else if (nameLabel.getBounds().contains (e.getPosition()))
    {
        startRenaming();
    }
}
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackHeader.h"
#include "TrackHeights.h"
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

TrackHeader::TrackHeader (Owner& o, ProjectContext& projectContext, const TrackRow& shown)
    : owner (o),
      context (projectContext),
      row (shown),
      muteSolo (projectContext),
      gripIcon (createIcon (Icon::grip))
{
    setDescription ("Click to select the track, Ctrl+click to add it to the selection, Shift+click to select up to it. "
                    "Drag the handle at the left to move the track, and the bottom edge to resize it. The arrow "
                    "collapses it. Double-click the name to rename it, and right-click for more.");

    // The header handles clicks on the name; the text field gets them while renaming.
    nameLabel.setTitle ("Track name");
    nameLabel.setFont (juce::FontOptions (TextSize::body));
    nameLabel.setInterceptsMouseClicks (false, true);
    nameLabel.setText (row.name, juce::dontSendNotification);
    nameLabel.onTextChange = [this] { renameTrack (context, row.id, nameLabel.getText()); }; // an empty name gives its number back

    muteSolo.setTrack (row.id);

    // The chevron collapses a track to a thin row, and hides a folder's tracks.
    collapseButton.setQuiet (true);
    collapseButton.setMouseClickGrabsKeyboardFocus (false);
    collapseButton.onClick = [this]
    {
        // Alt+click: every track, as in Ableton Live.
        const auto collapse = ! row.collapsed;

        if (juce::ModifierKeys::currentModifiers.isAltDown())
            for (const auto id : getAllPlaylistTracks (context))
                setCollapsed (context, id, collapse);
        else
            setCollapsed (context, row.id, collapse);
    };

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &muteSolo, &collapseButton })
        addAndMakeVisible (c);

    setTitle (isFolder() ? row.name + " (folder)" : row.name); // what screen readers call the header

    if (isMaster())
        setDescription ("The master's automation lanes are under it. Right-click to hide them.");

    updateCollapseButton();
    updateColours();
}

void TrackHeader::update (const TrackRow& shown, bool isSelected)
{
    const auto before = row;
    row = shown;
    setTitle (isFolder() ? row.name + " (folder)" : row.name);

    if (! nameLabel.isBeingEdited())
        nameLabel.setText (row.name, juce::dontSendNotification);

    muteSolo.update(); // and whether other tracks' solos silence it

    if (row.collapsed != before.collapsed)
        updateCollapseButton();

    if (row.colour != before.colour || selected != isSelected || row.depth != before.depth)
    {
        selected = isSelected;
        updateColours();
        resized();
        repaint();
    }
}

void TrackHeader::updateCollapseButton()
{
    // Pointing down while open, right while collapsed.
    collapseButton.setIcon (row.collapsed ? Icon::folded : Icon::expanded);

    if (isFolder())
    {
        collapseButton.setTitle (row.collapsed ? "Expand folder" : "Collapse folder");
        collapseButton.setTooltip (row.collapsed ? "Shows the folder's tracks. Alt+click: every track"
                                                 : "Hides the folder's tracks. Alt+click: every track");
    }
    else
    {
        collapseButton.setTitle (row.collapsed ? "Expand track" : "Collapse track");
        collapseButton.setTooltip (row.collapsed ? "Shows the track at its full height. Alt+click: every track"
                                                 : "Makes the track a thin row that shows its name. Alt+click: every track");
    }
}

juce::Colour TrackHeader::getBackground() const
{
    // In the track's color, which runs on through its row in the playlist; more of it while selected.
    return getPalette (*this).surface.interpolatedWith (row.colour, selected ? TrackTint::selectedHeader : TrackTint::header);
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

    g.setColour (row.colour);
    g.fillRect (getColourArea());

    // The handle, lined up with the name.
    if (gripIcon != nullptr && ! isMaster())
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
                    .withTrimmedLeft (colourStripWidth + gripWidth + row.depth * indentPerLevel)
                    .withTrimmedRight (Space::xs)
                    .withTrimmedBottom (1);
    auto top = area.removeFromTop (juce::jmin (area.getHeight(), controlHeight + Space::xs));

    // Mute and solo shrink to fit a collapsed row. A row shorter than that has room only for the name.
    const auto size = juce::jmin (buttonSize, top.getHeight() - 2);
    const auto roomForButtons = size >= minButtonSize && ! isMaster(); // the master's row shows its name only
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
    if (isMaster())
        setMouseCursor (juce::MouseCursor::NormalCursor);
    else if (isOnResizeEdge (e.getPosition()))
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else if (getGripArea().contains (e.getPosition()))
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::none;
    selectOnMouseUp = false;

    // The master's row only names the lanes under it: no selecting, moving, resizing or renaming it.
    if (isMaster())
    {
        if (e.mods.isPopupMenu())
            owner.showTrackMenu (row.id);

        return;
    }

    if (isOnResizeEdge (e.getPosition()) && ! e.mods.isPopupMenu())
    {
        // Alt-click resets the height, as in FL Studio.
        if (e.mods.isAltDown())
        {
            setShownTrackHeight (context, row.id, TrackHeight::standard);
            owner.trackHeightChanged();
            return;
        }

        drag = Drag::resize;
        heightAtDragStart = getShownTrackHeight (row);
        return;
    }

    if (e.mods.isPopupMenu())
    {
        if (! selected)
            owner.trackClicked (row.id, {});

        owner.showTrackMenu (row.id);
        return;
    }

    if (getColourArea().contains (e.getPosition()))
    {
        if (! selected)
            owner.trackClicked (row.id, {});

        owner.showColourMenu (row.id, *this);
        return;
    }

    // Clicking a selected track keeps the selection, so all of it can be dragged by the handle; without a drag,
    // the click selects this track alone when the mouse goes up.
    if (selected && ! (e.mods.isCommandDown() || e.mods.isShiftDown()))
        selectOnMouseUp = true;
    else
        owner.trackClicked (row.id, e.mods);

    // Only the handle moves tracks, so that clicks elsewhere don't move them by accident: there, a drag selects.
    drag = getGripArea().contains (e.getPosition()) ? Drag::pending : Drag::selectPending;
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::resize)
    {
        setShownTrackHeight (context, row.id, heightAtDragStart + e.getDistanceFromDragStartY());
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
    else if (selectOnMouseUp)
        owner.trackClicked (row.id, {});

    drag = Drag::none;
    selectOnMouseUp = false;
}

void TrackHeader::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isMaster())
        return;

    if (isOnResizeEdge (e.getPosition()))
    {
        setShownTrackHeight (context, row.id, TrackHeight::standard);
        owner.trackHeightChanged();
    }
    else if (nameLabel.getBounds().contains (e.getPosition()))
    {
        startRenaming();
    }
}
} // namespace phazite::ui

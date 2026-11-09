// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackHeader.h"
#include "TrackHeights.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/transport/RecordingModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int resizeEdgeHeight = 5; // the bottom edge, which resizes the track when dragged
    constexpr int buttonSize = 24;
    constexpr int minButtonSize = 16;     // the switches shrink to fit a short row, down to this
    constexpr int dragThreshold = 4;      // pixels the mouse moves before a press becomes a drag that moves the tracks
    constexpr float kindIconAlpha = 0.6f; // a folder's or a return's icon is drawn in the header's ink, a little muted
    constexpr float kindIconSize = 14.0f;
    constexpr int kindIconRoom = 180;   // a header narrower than this gives its folder's or return's icon's room to its name
    constexpr float inputAlpha = 0.75f; // what an armed track records is in the header's ink, a little muted

    juce::Colour getHeaderTint (const juce::Component& component, juce::Colour colour, bool selected)
    {
        return getPalette (component).surface.interpolatedWith (colour, selected ? TrackTint::selectedHeader : TrackTint::header);
    }
} // namespace

int getTrackStripsWidth (const TrackRow& row)
{
    return row.depth * TrackStrip::indent + TrackStrip::width;
}

void paintTrackStrips (juce::Graphics& g, const juce::Component& component, const TrackRow& row)
{
    // Each folder's column, in its header's tint, with its rail down the left, as a bracket round its tracks: the rails meet
    // from row to row, so they run over the rows' bottom borders.
    const auto height = component.getHeight();
    auto x = 0;

    for (const auto colour : row.folderColours)
    {
        g.setColour (getHeaderTint (component, colour, false));
        g.fillRect (x, 0, TrackStrip::indent, height);
        g.setColour (colour);
        g.fillRect (x, 0, TrackStrip::width, height);
        x += TrackStrip::indent;
    }

    g.setColour (row.colour);
    g.fillRect (x, 0, TrackStrip::width, height);
}

//==============================================================================
TrackHeader::TrackHeader (Owner& o, ProjectContext& projectContext, const TrackRow& shown)
    : owner (o),
      context (projectContext),
      row (shown),
      muteSolo (projectContext),
      arm (projectContext)
{
    setDescription ("Click to select the track, Ctrl+click to add it to the selection, Shift+click to select up to it. "
                    "Drag it to move the selected tracks, onto a folder to put them in it. Double-click to collapse it "
                    "or expand it, drag the bottom edge to resize it, and right-click for more, such as Rename.");

    // The header handles clicks on the name; the text field gets them while renaming.
    nameLabel.setTitle ("Track name");
    nameLabel.setFont (juce::FontOptions (TextSize::body));
    nameLabel.setInterceptsMouseClicks (false, true);
    nameLabel.setText (row.name, juce::dontSendNotification);
    nameLabel.onTextChange = [this] { renameTrack (context, row.id, nameLabel.getText()); }; // an empty name gives its number back

    muteSolo.setTrack (row.id);

    // A track records an input, or with an instrument MIDI: its arm switch, and while it is armed, what it records under its
    // name.
    arm.setTrack (row.id);
    arm.onChange = [this] { showArmedInput(); };
    inputLabel.setFont (juce::FontOptions (TextSize::small));
    inputLabel.setInterceptsMouseClicks (false, false);
    showArmedInput();

    // While the track is frozen, a snowflake that plays it live again.
    frozenButton.setQuiet (true);
    frozenButton.setMouseClickGrabsKeyboardFocus (false);
    frozenButton.onClick = [this] { unfreezeTrack (context, row.id); };

    // How loud it plays now, beside its switch; the master's row only names its lanes, and a plain folder has no sound of its own.
    if (! isMaster())
        peak.setSource (getLevel (context, row.id));

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &muteSolo, &peak })
        addAndMakeVisible (c);

    addChildComponent (frozenButton);
    addChildComponent (arm);
    addChildComponent (inputLabel);
    updateFrozenButton();
    updateKindIcon();

    setTitle (getAccessibleName()); // what screen readers call the header

    if (isMaster())
        setDescription ("The master's automation lanes are under it. Double-click to collapse them to thin rows, or to expand them.");

    updateColours();
}

void TrackHeader::update (const TrackRow& shown, bool isSelected)
{
    const auto before = row;
    row = shown;
    setTitle (getAccessibleName());

    if (! nameLabel.isBeingEdited())
        nameLabel.setText (row.name, juce::dontSendNotification);

    muteSolo.update(); // and whether other tracks' solos silence it
    arm.update();      // and whether it can record audio, an instrument added or removed

    if (row.collapsed != before.collapsed)
    {
        updateKindIcon();
        repaint();
    }

    if (row.frozen != before.frozen || row.name != before.name)
    {
        updateFrozenButton();
        resized();
    }

    if (row.colour != before.colour || selected != isSelected || row.depth != before.depth || row.folderColours != before.folderColours)
    {
        selected = isSelected;
        updateColours();
        resized();
        repaint();
    }
}

void TrackHeader::updateKindIcon()
{
    // A folder's is open while its tracks show.
    if (isFolder())
        kindIcon = createIcon (row.collapsed ? Icon::folder : Icon::openFolder);
    else if (row.kind == TrackType::returnTrack)
        kindIcon = createIcon (Icon::returnTrack);

    kindIconColour = juce::Colours::black; // as it is drawn
}

void TrackHeader::updateFrozenButton()
{
    frozenButton.setTooltip (row.name
                             + " is frozen: it plays a file of its sound, so its instrument and effects take no CPU. "
                               "Click to play it live again. Changing its clips or notes unfreezes it too.");
}

juce::String TrackHeader::getAccessibleName() const
{
    return isFolder() ? row.name + " (folder)" : row.kind == TrackType::returnTrack ? row.name + " (return)" : row.name;
}

juce::Colour TrackHeader::getBackground() const
{
    // In the track's color, which runs on through its row in the playlist; more of it while selected.
    return getHeaderTint (*this, row.colour, selected);
}

void TrackHeader::updateColours()
{
    // Black or white, whichever reads better on the track's color; what it records a little muted, or in the danger colour
    // while Record records onto it.
    const auto ink = getInkFor (getBackground());
    nameLabel.setColour (juce::Label::textColourId, ink);
    inputLabel.setColour (juce::Label::textColourId,
                          isRecordingOnto (context, row.id) ? getPalette (*this).danger : ink.withAlpha (inputAlpha));
    peak.setFillColour (row.colour);
    repaint();
}

void TrackHeader::startRenaming()
{
    nameLabel.showEditor();
}

juce::Rectangle<int> TrackHeader::getColourArea() const
{
    return getLocalBounds().withTrimmedLeft (getTrackStripsWidth (row) - TrackStrip::width).withWidth (TrackStrip::width);
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
    auto area = getLocalBounds().withTrimmedLeft (getTrackStripsWidth (row));

    g.setColour (background);
    g.fillRect (area);
    paintTrackStrips (g, *this, row);

    // A folder's or a return's icon, before its name, in the header's ink.
    if (kindIcon != nullptr && ! kindIconArea.isEmpty())
    {
        if (const auto ink = getInkFor (background).withAlpha (kindIconAlpha); ink != kindIconColour)
        {
            kindIcon->replaceColour (kindIconColour, ink);
            kindIconColour = ink;
        }

        kindIcon->drawWithin (g, kindIconArea.toFloat().withSizeKeepingCentre (kindIconSize, kindIconSize),
                              juce::RectanglePlacement::centred, 1.0f);
    }

    // Right of the strips, so that the folders' rails and the track's own color run on into the rows below.
    g.setColour (p.border);
    g.fillRect (area.removeFromBottom (1));
}

void TrackHeader::resized()
{
    // Along the right edge, as tall as the row (above its border), how loud it plays now, as FL Studio shows it: on every row
    // but the master's, collapsed ones' too.
    peak.setVisible (! isMaster());
    peak.setBounds (getLocalBounds().withTrimmedBottom (1).removeFromRight (PeakBarTint::width));

    auto area = getLocalBounds()
                    .withTrimmedLeft (getTrackStripsWidth (row) + Space::s)
                    .withTrimmedRight (PeakBarTint::width + Space::xs)
                    .withTrimmedBottom (1);
    // The name and the switches in a row as tall as a control, which leaves what an armed track records room under it in a
    // track of TrackHeight::standard.
    auto top = area.removeFromTop (juce::jmin (area.getHeight(), controlHeight));

    // The switches shrink to fit a collapsed row. A row shorter than that has room only for the name. The master's row
    // has its name only, for its lanes.
    const auto size = juce::jmin (buttonSize, top.getHeight() - 2);
    const auto roomForButtons = size >= minButtonSize;
    muteSolo.setVisible (roomForButtons && ! isMaster());
    arm.setVisible (roomForButtons && ! isMaster() && arm.isShown());
    frozenButton.setVisible (roomForButtons && row.frozen);

    if (roomForButtons)
    {
        if (! isMaster())
        {
            // Mute and solo at the right end, small, as FL Studio's mute switch is.
            const auto switchSize = juce::jmin (SwitchTint::size, size);
            muteSolo.setBounds (top.removeFromRight (switchSize).withSizeKeepingCentre (switchSize, switchSize));
            top.removeFromRight (Space::xs);

            if (arm.isVisible())
            {
                arm.setBounds (top.removeFromRight (size).withSizeKeepingCentre (size, size));
                top.removeFromRight (Space::xs);
            }
        }

        if (row.frozen)
        {
            frozenButton.setBounds (top.removeFromRight (size).withSizeKeepingCentre (size, size));
            top.removeFromRight (Space::xs);
        }
    }

    kindIconArea =
        kindIcon != nullptr && getWidth() >= kindIconRoom ? top.removeFromLeft ((int) kindIconSize + Space::xs) : juce::Rectangle<int>();
    nameLabel.setBounds (top);

    // Under the name, while the track is armed and has room for it: what it records.
    const auto inputHeight = juce::roundToInt (TextSize::small) + Space::xs;
    inputLabel.setVisible (inputLabel.getText().isNotEmpty() && area.getHeight() >= inputHeight);
    inputLabel.setBounds (area.removeFromTop (inputHeight));
}

void TrackHeader::showArmedInput()
{
    inputLabel.setText (describeArmedInput (context, row.id), juce::dontSendNotification);
    updateColours(); // in the danger colour while Record records onto it
    resized();
}

void TrackHeader::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (! isMaster() && isOnResizeEdge (e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor
                                                                     : juce::MouseCursor::NormalCursor);
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

    // Clicking a selected track keeps the selection, so that all of it can be dragged; without a drag, the click selects
    // this track alone when the mouse goes up.
    if (selected && ! (e.mods.isCommandDown() || e.mods.isShiftDown()))
        selectOnMouseUp = true;
    else
        owner.trackClicked (row.id, e.mods);

    drag = Drag::pending;
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::resize)
    {
        setShownTrackHeight (context, row.id, heightAtDragStart + e.getDistanceFromDragStartY());
        owner.trackHeightChanged();
        return;
    }

    if (drag == Drag::pending && e.getDistanceFromDragStart() > dragThreshold)
    {
        drag = Drag::move;
        selectOnMouseUp = false;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    if (drag == Drag::move)
    {
        if (owner.trackDraggedOut (*this, e))
            drag = Drag::out; // the drag-and-drop container has it now
        else
            owner.trackDragged (*this, e);
    }
}

void TrackHeader::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::move)
        owner.trackDragEnded (*this, e);
    else if (selectOnMouseUp)
        owner.trackClicked (row.id, {});

    if (drag == Drag::move || drag == Drag::out)
        setMouseCursor (juce::MouseCursor::NormalCursor);

    drag = Drag::none;
    selectOnMouseUp = false;
}

void TrackHeader::mouseDoubleClick (const juce::MouseEvent& e)
{
    // The bottom edge resets the height; anywhere else, the track collapses or expands (the master's lanes do).
    if (! isMaster() && isOnResizeEdge (e.getPosition()))
    {
        setShownTrackHeight (context, row.id, TrackHeight::standard);
        owner.trackHeightChanged();
        return;
    }

    if (! nameLabel.isBeingEdited())
        setCollapsed (context, row.id, ! row.collapsed);
}
} // namespace phazite::ui

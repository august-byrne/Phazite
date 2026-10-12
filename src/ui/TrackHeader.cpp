// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackHeader.h"
#include "LookAndFeel.h"
#include "model/Tracks.h"

namespace phazite::ui
{
namespace
{
    constexpr int colourStripWidth = 8;
    constexpr int resizeEdgeHeight = 5; // the bottom edge, which resizes the track when dragged
    constexpr int buttonSize = 24;
    constexpr int dragThreshold = 4; // pixels the mouse moves before a click becomes a drag
} // namespace

TrackHeader::TrackHeader (Owner& o, te::Track& t)
    : owner (o),
      track (t)
{
    setDescription ("Click to select the track, Ctrl+click to add it to the selection, Shift+click to select up to it. "
                    "Double-click the name to rename it, drag the track to move it, drag its bottom edge to resize it, "
                    "and right-click for more.");

    // The header handles clicks on the name; the text field gets them while renaming.
    nameLabel.setTitle ("Track name");
    nameLabel.setFont (juce::FontOptions (TextSize::body));
    nameLabel.setInterceptsMouseClicks (false, true);
    nameLabel.onTextChange = [this]
    {
        if (auto* t = track.get(); t != nullptr && nameLabel.getText().trim() != t->getName())
        {
            t->edit.getUndoManager().beginNewTransaction ("Rename Track");
            t->setName (nameLabel.getText().trim()); // an empty name gives the track its number back
        }
    };

    muteButton.setTitle ("Mute");
    muteButton.setTooltip ("Mute: silences this track");
    soloButton.setTitle ("Solo");
    soloButton.setTooltip ("Solo: plays only this track, and any others that are soloed");

    for (auto* button : { &muteButton, &soloButton })
        button->setClickingTogglesState (true);

    // Tracktion keeps mute and solo out of the undo history, as many DAWs do.
    muteButton.onClick = [this]
    {
        if (auto* t = track.get())
            t->setMute (muteButton.getToggleState());
    };

    soloButton.onClick = [this]
    {
        if (auto* t = track.get())
            t->setSolo (soloButton.getToggleState());
    };

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &muteButton, &soloButton })
        addAndMakeVisible (c);
}

void TrackHeader::update (juce::Colour newColour, bool isSelected)
{
    auto* t = track.get();

    if (t == nullptr)
        return;

    const auto name = t->getName();
    setTitle (name); // what screen readers call the header

    if (! nameLabel.isBeingEdited())
        nameLabel.setText (name, juce::dontSendNotification);

    muteButton.setToggleState (t->isMuted (false), juce::dontSendNotification);
    soloButton.setToggleState (t->isSolo (false), juce::dontSendNotification);

    if (colour != newColour || selected != isSelected)
    {
        colour = newColour;
        selected = isSelected;
        repaint();
    }
}

void TrackHeader::startRenaming()
{
    nameLabel.showEditor();
}

juce::Rectangle<int> TrackHeader::getColourArea() const
{
    return getLocalBounds().removeFromLeft (colourStripWidth).withTrimmedBottom (1);
}

bool TrackHeader::isOnResizeEdge (juce::Point<int> position) const
{
    return position.y >= getHeight() - resizeEdgeHeight;
}

//==============================================================================
void TrackHeader::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    auto area = getLocalBounds();

    g.setColour (p.surface);
    g.fillRect (area);

    if (selected)
    {
        g.setColour (p.accent.withAlpha (0.22f));
        g.fillRect (area);
    }

    g.setColour (colour);
    g.fillRect (getColourArea());

    g.setColour (p.border);
    g.fillRect (area.removeFromBottom (1));
}

void TrackHeader::resized()
{
    auto area = getLocalBounds().withTrimmedLeft (colourStripWidth + Space::xs).withTrimmedRight (Space::xs).withTrimmedBottom (1);
    auto top = area.removeFromTop (juce::jmin (area.getHeight(), controlHeight + Space::xs));

    soloButton.setBounds (top.removeFromRight (buttonSize).withSizeKeepingCentre (buttonSize, buttonSize));
    top.removeFromRight (Space::xs / 2);
    muteButton.setBounds (top.removeFromRight (buttonSize).withSizeKeepingCentre (buttonSize, buttonSize));
    top.removeFromRight (Space::xs);
    nameLabel.setBounds (top);
}

void TrackHeader::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isOnResizeEdge (e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
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
            setTrackHeight (*t, TrackHeight::standard);
            owner.trackHeightChanged();
            return;
        }

        drag = Drag::resize;
        heightAtDragStart = getTrackHeight (*t);
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

    // Clicking a selected track keeps the selection, so all of it can be dragged; without a drag, the
    // click selects this track alone when the mouse goes up.
    if (selected && ! (e.mods.isCommandDown() || e.mods.isShiftDown()))
        selectOnMouseUp = true;
    else
        owner.trackClicked (*t, e.mods);

    drag = Drag::pending;
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    auto* t = track.get();

    if (t == nullptr)
        return;

    if (drag == Drag::resize)
    {
        setTrackHeight (*t, heightAtDragStart + e.getDistanceFromDragStartY());
        owner.trackHeightChanged();
        return;
    }

    if (drag == Drag::pending && e.getDistanceFromDragStart() > dragThreshold)
        drag = Drag::move;

    if (drag == Drag::move)
        owner.trackDragged (*this, e);
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
        setTrackHeight (*t, TrackHeight::standard);
        owner.trackHeightChanged();
    }
    else if (nameLabel.getBounds().contains (e.getPosition()))
    {
        startRenaming();
    }
}
} // namespace phazite::ui

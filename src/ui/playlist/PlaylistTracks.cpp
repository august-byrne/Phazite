// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's track headers: clicking, dragging (into the browser too) and their menus; adding tracks at the end; and the
// playlist's commands.
#include "PlaylistParts.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
void Playlist::addTrackAtEnd (bool folder)
{
    if (! context.hasProject())
        return;

    const auto added = phazite::addTrackAtEnd (context, folder);
    rebuild(); // its row is there now
    scrollToShow (getRowOf (added));
}

void Playlist::showAddTrackMenu()
{
    const juce::Component::SafePointer<Playlist> safeThis (this);
    juce::PopupMenu menu;
    menu.addSectionHeader ("Add at the end");

    menu.addItem ("Track",
                  [safeThis]
                  {
                      if (safeThis != nullptr)
                          safeThis->addTrackAtEnd (false);
                  });

    menu.addItem ("Folder",
                  [safeThis]
                  {
                      if (safeThis != nullptr)
                          safeThis->addTrackAtEnd (true);
                  });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showRulerMenu (BeatPosition where)
{
    // As a right-click on the section there would.
    if (context.hasProject())
        sectionsStrip.showMenuAt (where);
}

void Playlist::setColourOfSelectedTracks (juce::Colour colour)
{
    setTrackColours (context, selection.getTracks(), colour);
}

//==============================================================================
void Playlist::trackClicked (TrackID track, const juce::ModifierKeys& mods)
{
    // The arrow keys then step through the tracks, rather than move a slider that had the focus.
    lanes->grabKeyboardFocus();

    // Shift selects the tracks from the last one clicked to this one.
    if (const auto anchorRow = getRowOf (selectionAnchor); mods.isShiftDown() && anchorRow >= 0)
    {
        const auto row = getRowOf (track);
        selection.clear();

        for (auto i = std::min (anchorRow, row); i <= std::max (anchorRow, row); ++i)
            if (juce::isPositiveAndBelow (i, (int) rows.size()))
                selection.add (rows[(size_t) i].track.id);

        return;
    }

    if (! mods.isCommandDown())
        selection.selectOnly (track);
    else if (selection.isSelected (track))
        selection.remove (track);
    else
        selection.add (track);

    selectionAnchor = track;
}

void Playlist::showTrackMenu (TrackID track)
{
    auto& bridge = getCommands();
    juce::PopupMenu menu;

    // The master's row is there for its lanes, which a double-click on it collapses: it has no menu.
    const auto row = getRowOf (track);

    if (row >= 0 && rows[(size_t) row].track.kind == TrackType::master)
        return;

    addCommandItems (menu, bridge, { "edit.rename", "track.color" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.mute", "track.solo", "track.arm", "track.pin" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.moveUp", "track.moveDown" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.groupIntoFolder", "track.ungroup", "track.collapse" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "automation.showLanes" });
    juce::PopupMenu modes;
    addCommandItems (modes, bridge, { "automation.read", "automation.touch", "automation.latch", "automation.write" });
    menu.addSubMenu ("Automation Mode", modes);
    menu.addSeparator();
    addCommandItems (menu, bridge, { "edit.paste", "edit.duplicate", "edit.delete" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.freeze", "track.bounceInPlace" });

    // A track's sound ducking under another's, as one step: a compressor listening to that track (SidechainMenus.h).
    if (row >= 0 && rows[(size_t) row].track.kind == TrackType::track)
    {
        const auto duck = makeDuckUnderMenu (context, track);
        menu.addSubMenu ("Duck Under", duck, duck.getNumItems() > 0);
    }

    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.savePreset" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.add", "track.addFolder" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showColourMenu (TrackID track, juce::Component& target)
{
    const auto row = getRowOf (track);
    ui::showColourMenu (target, row >= 0 ? rows[(size_t) row].track.colour : juce::Colours::grey,
                        [safeThis = juce::Component::SafePointer<Playlist> (this)] (juce::Colour colour)
                        {
                            if (safeThis != nullptr)
                                safeThis->setColourOfSelectedTracks (colour);
                        });
}

void Playlist::trackDragged (TrackHeader&, const juce::MouseEvent& e)
{
    // Past the top or bottom, the tracks scroll along.
    const auto position = e.getEventRelativeTo (lanes.get()).getPosition();
    beginDragAutoRepeat (autoRepeatInterval);
    autoScroll (position, lanes->getLocalBounds().withTrimmedTop (pinnedHeight), ScrollDirections::upAndDown);

    fadeMovingHeaders (true);
    trackDrop = findTrackDrop (position.y);
    repaint();
}

bool Playlist::trackDraggedOut (TrackHeader& header, const juce::MouseEvent& e)
{
    // Past the playlist's left edge, toward the browser: no longer moving among the tracks, but carried there, which saves
    // it as a preset (ui/browser/Browser.h).
    if (e.getEventRelativeTo (this).position.x >= 0.0f || isDragAndDropActive())
        return false;

    trackDrop.reset();
    fadeMovingHeaders (false);
    repaint();
    startDragging (makeTrackDrag (header.getTrack()), &header, juce::ScaledImage (header.createComponentSnapshot (header.getLocalBounds())),
                   true);
    return true;
}

void Playlist::trackDragEnded (TrackHeader&, const juce::MouseEvent&)
{
    const auto place = trackDrop;
    trackDrop.reset();
    fadeMovingHeaders (false);
    repaint();

    if (context.hasProject() && place.has_value() && juce::isPositiveAndBelow (place->row, (int) rows.size()))
        moveTracksTo (context, selection.getTracks(), rows[(size_t) place->row].track.id, place->drop);
}

std::optional<Playlist::TrackDropPlace> Playlist::findTrackDrop (int y) const
{
    // Among the tracks and folders: the returns' and the master's rows come after them, wherever the returns are in the
    // song's list of tracks.
    const auto isTrackRow = [] (const Row& row) { return row.track.kind == TrackType::track || row.track.kind == TrackType::folder; };
    auto last = -1;

    for (int i = 0; i < (int) rows.size(); ++i)
        if (isTrackRow (rows[(size_t) i]))
            last = i;

    if (last < 0)
        return {};

    const auto index = getRowAt (y);
    TrackDropPlace place;

    if (index < 0 || index > last)
    {
        // Under them: at the end, after the last top-level one.
        for (auto i = last; i >= 0 && place.row < 0; --i)
            if (rows[(size_t) i].track.depth == 0)
                place = { i, TrackDrop::after };
    }
    else if (const auto& row = rows[(size_t) index]; row.track.kind == TrackType::folder)
    {
        // Its lanes count as its bottom quarter.
        const auto offset = y - getRowTop (row);
        const auto firstInside = index < last && rows[(size_t) index + 1].track.depth > row.track.depth;

        if (offset < row.height / 4)
            place = { index, TrackDrop::before };
        else if (offset < row.height - row.height / 4)
            place = { index, TrackDrop::into };
        else
            place = firstInside ? TrackDropPlace { index + 1, TrackDrop::before } : TrackDropPlace { index, TrackDrop::after };
    }
    else
    {
        place = { index, y - getRowTop (row) < row.height / 2 ? TrackDrop::before : TrackDrop::after };
    }

    if (place.row < 0 || ! canMoveTracksTo (context, selection.getTracks(), rows[(size_t) place.row].track.id, place.drop))
        return {};

    return place;
}

void Playlist::fadeMovingHeaders (bool fade)
{
    if (std::exchange (headersFaded, fade) == fade)
        return;

    // The selected tracks, and those inside the selected folders, which move with them.
    const auto selected = selection.getTracks();

    for (auto* header : headers)
    {
        const auto id = header->getTrack();
        const auto moves = fade
                           && std::any_of (selected.begin(), selected.end(),
                                           [this, id] (TrackID track) { return track == id || isInside (context, id, track); });
        header->setAlpha (moves ? TrackDropTint::moving : 1.0f);
    }
}

void Playlist::paintTrackDrop (juce::Graphics& g) const
{
    if (! trackDrop.has_value() || ! juce::isPositiveAndBelow (trackDrop->row, (int) rows.size()))
        return;

    const auto& p = getPalette (*this);
    const auto& row = rows[(size_t) trackDrop->row];
    const auto left = (float) headerColumn.getX();
    const auto right = (float) lanes->getRight();
    const auto top = (float) (lanes->getY() + getRowTop (row));
    g.reduceClipRegion (headerColumn.getBounds().getUnion (lanes->getBounds()));

    // Into a folder: its row outlined, over a wash.
    if (trackDrop->drop == TrackDrop::into)
    {
        const juce::Rectangle<float> area (left, top, right - left, (float) row.height);
        g.setColour (p.accent.withAlpha (TrackDropTint::wash));
        g.fillRect (area);
        g.setColour (p.accent);
        g.drawRect (area, TrackDropTint::outline);
        return;
    }

    // Before or after a track: a line from the depth in folders the tracks go to. After a folder, past its tracks.
    auto bottom = (size_t) trackDrop->row;

    if (trackDrop->drop == TrackDrop::after)
        while (bottom + 1 < rows.size() && rows[bottom + 1].track.depth > row.track.depth)
            ++bottom;

    const auto y =
        trackDrop->drop == TrackDrop::before ? top : (float) (lanes->getY() + getRowTop (rows[bottom]) + rows[bottom].getTotalHeight());
    const auto x = left + (float) (row.track.depth * TrackStrip::indent);
    const auto dot = TrackDropTint::dot;
    g.setColour (p.accent);
    g.fillRect (juce::Rectangle<float> (x, y - TrackDropTint::line / 2.0f, right - x, TrackDropTint::line));
    g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre ({ x + dot / 2.0f, y }));
}

void Playlist::updateCollapseAllButton()
{
    const auto expanded = isAnyRowExpanded (context);
    collapseAllButton.setIcon (expanded ? Icon::collapseAll : Icon::expandAll);
    collapseAllButton.setTitle (expanded ? "Collapse all" : "Expand all");
    collapseAllButton.setTooltip (withShortcut (getCommands(),
                                                expanded ? "Makes every track, folder and automation lane a thin row"
                                                         : "Shows every track, folder and automation lane at its full height",
                                                "track.collapseAll"));
}

//==============================================================================
void Playlist::addCommands (CommandRegistry& registry)
{
    const auto hasProject = [this] { return context.hasProject(); };
    const auto hasSelectedTracks = [this] { return ! selection.getTracks().empty(); };
    const auto hasSelectedSections = [this] { return ! selection.getSections().empty(); };

    registry.add ({ .id = "edit.rename",
                    .name = "Rename",
                    .category = "Edit",
                    .description = "Renames the selected section or track.",
                    .synonyms = { "name", "label", "title" },
                    .perform =
                        [this]
                    {
                        if (const auto sections = selection.getSections(); ! sections.empty())
                        {
                            sectionsStrip.startRenaming (sections.front());
                            return;
                        }

                        rebuild(); // a track added a moment ago has its header now

                        if (const auto tracks = selection.getTracks(); ! tracks.empty())
                        {
                            scrollToShow (getRowOf (tracks.front()));

                            if (auto* header = getHeaderFor (tracks.front()))
                                header->startRenaming();
                        }
                    },
                    .isEnabled = [hasSelectedTracks, hasSelectedSections] { return hasSelectedSections() || hasSelectedTracks(); },
                    .currentName =
                        [hasSelectedTracks, hasSelectedSections]
                    {
                        if (hasSelectedSections())
                            return juce::String ("Rename Section");

                        return hasSelectedTracks() ? juce::String ("Rename Track") : juce::String();
                    } });

    registry.add ({ .id = "track.color",
                    .name = "Track Color",
                    .category = "Track",
                    .description = "Changes the color of the selected tracks.",
                    .synonyms = { "colour", "recolor", "paint" },
                    .perform =
                        [this]
                    {
                        rebuild();

                        if (const auto tracks = selection.getTracks(); ! tracks.empty())
                        {
                            scrollToShow (getRowOf (tracks.front()));

                            if (auto* header = getHeaderFor (tracks.front()))
                                showColourMenu (tracks.front(), *header);
                        }
                    },
                    .isEnabled = hasSelectedTracks });

    registry.add ({ .id = "view.focusMode",
                    .name = "Focus Mode",
                    .category = "View",
                    .description = "Makes every track small but the selected ones, and the folders they are in.",
                    .synonyms = { "solo view", "collapse others", "zoom tracks", "concentrate" },
                    .perform =
                        [this]
                    {
                        focusMode = ! focusMode;
                        layOutRows();
                    },
                    .isEnabled = hasProject,
                    .isTicked = [this] { return focusMode; } });
}

void Playlist::refreshShortcutTexts()
{
    TimelineEditor::refreshShortcutTexts();
    addTrackButton.setTooltip (withShortcut (getCommands(), "Adds an empty track below the selected one", "track.add"));
    updateCollapseAllButton();
}
} // namespace phazite::ui

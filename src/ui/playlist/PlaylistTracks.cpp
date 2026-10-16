// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's track headers: clicking, dragging and their menus; adding tracks at the end; and the playlist's commands.
#include "PlaylistParts.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/Tracks.h"
#include "ui/input/CommandUI.h"

namespace phazite::ui
{
void Playlist::addTrackAtEnd (bool folder)
{
    auto* e = getEdit();

    if (e == nullptr)
        return;

    auto* added = phazite::addTrackAtEnd (*e, folder, &selection);
    handleUpdateNowIfNeeded(); // its row is there now
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

    addCommandItem (menu, getCommands(), "track.addReturn"); // returns go at the end anyway
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showRulerMenu (BeatPosition where)
{
    auto* e = getEdit();

    if (e == nullptr)
        return;

    // As a right-click on the section there would.
    const auto beat = toTracktion (where);
    auto* section = getSectionAt (*e, beat);

    if (section != nullptr && ! selection.isSelected (section))
        selection.selectOnly (section);

    sectionsStrip.showMenu (section, beat);
}

void Playlist::setColourOfSelectedTracks (juce::Colour colour)
{
    setTrackColours (getSelectedTracks (selection), colour);
}

//==============================================================================
void Playlist::trackClicked (te::Track& track, const juce::ModifierKeys& mods)
{
    // The arrow keys then step through the tracks, rather than move a slider that had the focus.
    lanes->grabKeyboardFocus();

    // Shift selects the tracks from the last one clicked to this one.
    if (const auto anchorRow = getRowOf (selectionAnchor.get()); mods.isShiftDown() && anchorRow >= 0)
    {
        const auto row = getRowOf (&track);
        selection.deselectAll();

        for (auto i = std::min (anchorRow, row); i <= std::max (anchorRow, row); ++i)
            if (auto* t = rows[(size_t) i].track.get())
                selection.addToSelection (t);

        return;
    }

    if (! mods.isCommandDown())
        selection.selectOnly (track);
    else if (selection.isSelected (track))
        selection.deselect (&track);
    else
        selection.addToSelection (track);

    selectionAnchor = &track;
}

void Playlist::showTrackMenu (te::Track&)
{
    auto& bridge = getCommands();
    juce::PopupMenu menu;
    addCommandItems (menu, bridge, { "edit.rename", "track.color" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.mute", "track.solo", "track.pin" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.moveUp", "track.moveDown" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.groupIntoFolder", "track.ungroup", "track.collapse" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "edit.duplicate", "edit.delete" });
    menu.addSeparator();
    addCommandItems (menu, bridge, { "track.add", "track.addFolder" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showColourMenu (te::Track& track, juce::Component& target)
{
    ui::showColourMenu (target, getTrackColour (track, getRowOf (&track)),
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
    const auto y = position.y;

    // The track would go to the nearest boundary between rows, as they are on screen.
    const auto boundary = [this] (size_t i) { return i < rows.size() ? getRowTop (rows[i]) : pinnedHeight + contentHeight - scrollY; };
    size_t nearest = 0;

    for (size_t i = 1; i <= rows.size(); ++i)
        if (std::abs (y - boundary (i)) < std::abs (y - boundary (nearest)))
            nearest = i;

    dropRow = (int) nearest;
    repaint();
}

void Playlist::trackSelectionDragged (TrackHeader& header, const juce::MouseEvent& e)
{
    // From the track the drag started on to the one under the pointer, as a Shift+click would.
    const auto position = e.getEventRelativeTo (lanes.get()).getPosition();
    beginDragAutoRepeat (autoRepeatInterval);
    autoScroll (position, lanes->getLocalBounds().withTrimmedTop (pinnedHeight), ScrollDirections::upAndDown);

    const auto from = getRowOf (header.getTrack());
    auto to = getRowAt (juce::jlimit (0, lanes->getHeight() - 1, position.y));

    if (from < 0)
        return;

    if (to < 0) // below the last row
        to = (int) rows.size() - 1;

    selection.deselectAll();

    for (auto i = std::min (from, to); i <= std::max (from, to); ++i)
        if (auto* t = rows[(size_t) i].track.get())
            selection.addToSelection (t);

    selectionAnchor = header.getTrack();
}

void Playlist::trackDragEnded (TrackHeader&, const juce::MouseEvent&)
{
    const auto target = dropRow;
    dropRow = -1;
    repaint();

    auto* e = getEdit();
    const auto moving = getSelectedTracks (selection);

    if (e == nullptr || target < 0 || moving.isEmpty())
        return;

    // The tracks go, in order, after the nearest track above the drop point that isn't moving.
    te::Track* previous = nullptr;

    for (auto i = target - 1; i >= 0 && previous == nullptr; --i)
        if (auto* t = rows[(size_t) i].track.get(); t != nullptr && ! selection.isSelected (t))
            previous = t;

    moveTracksAfter (moving, previous);
}

//==============================================================================
void Playlist::addCommands (CommandRegistry& registry)
{
    const auto hasEdit = [this] { return getEdit() != nullptr; };
    const auto hasSelectedTracks = [this] { return ! getSelectedTracks (selection).isEmpty(); };
    const auto hasSelectedSections = [this] { return ! getSelectedSections (selection).isEmpty(); };

    registry.add ({ .id = "edit.rename",
                    .name = "Rename",
                    .category = "Edit",
                    .description = "Renames the selected section or track.",
                    .synonyms = { "name", "label", "title" },
                    .perform =
                        [this]
                    {
                        if (auto* section = getSelectedSections (selection).getFirst())
                        {
                            sectionsStrip.startRenaming (*section);
                            return;
                        }

                        handleUpdateNowIfNeeded(); // a track added a moment ago has its header now

                        if (auto* track = getSelectedTracks (selection).getFirst())
                        {
                            scrollToShow (getRowOf (track));

                            if (auto* header = getHeaderFor (track))
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
                        handleUpdateNowIfNeeded();

                        if (auto* track = getSelectedTracks (selection).getFirst())
                        {
                            scrollToShow (getRowOf (track));

                            if (auto* header = getHeaderFor (track))
                                showColourMenu (*track, *header);
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
                    .isEnabled = hasEdit,
                    .isTicked = [this] { return focusMode; } });
}

void Playlist::refreshShortcutTexts()
{
    TimelineEditor::refreshShortcutTexts();
    addTrackButton.setTooltip (withShortcut (getCommands(), "Adds an empty track below the selected one", "track.add"));
}
} // namespace phazite::ui

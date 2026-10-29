// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's track headers: clicking, dragging (into the browser too) and their menus; adding tracks at the end; and the
// playlist's commands.
#include "PlaylistParts.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/input/LibraryDrag.h"

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

    addCommandItem (menu, getCommands(), "track.addReturn"); // returns go at the end anyway
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

    // The master's row is there for its lanes, which its chevron collapses: it has no menu.
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

bool Playlist::trackDraggedOut (TrackHeader& header, const juce::MouseEvent& e)
{
    // Past the playlist's left edge, toward the browser: no longer moving among the tracks, but carried there, which saves
    // it as a preset (ui/browser/Browser.h).
    if (e.getEventRelativeTo (this).position.x >= 0.0f || isDragAndDropActive())
        return false;

    dropRow = -1;
    repaint();
    startDragging (makeTrackDrag (header.getTrack()), &header, juce::ScaledImage (header.createComponentSnapshot (header.getLocalBounds())),
                   true);
    return true;
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

    selection.clear();

    for (auto i = std::min (from, to); i <= std::max (from, to); ++i)
        selection.add (rows[(size_t) i].track.id);

    selectionAnchor = header.getTrack();
}

void Playlist::trackDragEnded (TrackHeader&, const juce::MouseEvent&)
{
    const auto target = dropRow;
    dropRow = -1;
    repaint();

    const auto moving = selection.getTracks();

    if (! context.hasProject() || target < 0 || moving.empty())
        return;

    // The tracks go, in order, after the nearest track above the drop point that isn't moving.
    TrackID previous;

    for (auto i = target - 1; i >= 0 && ! previous.isValid(); --i)
        if (const auto id = rows[(size_t) i].track.id; ! selection.isSelected (id))
            previous = id;

    moveTracksAfter (context, moving, previous);
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
}
} // namespace phazite::ui

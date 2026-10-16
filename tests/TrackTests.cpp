// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/Selection.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/Undo.h"
#include "model/project/ProjectContext.h"
#include "ui/playlist/TrackHeights.h"

namespace
{
// An Edit with tracks named A, B, C..., a selection, and the track commands, with the playlist's height commands.
struct Tracks
{
    explicit Tracks (int numTracks)
    {
        edit->ensureNumberOfAudioTracks (numTracks);

        for (int i = 0; i < numTracks; ++i)
            te::getAudioTracks (*edit)[i]->setName (juce::String::charToString ((juce::juce_wchar) ('A' + i)));

        edit->getUndoManager().clearUndoHistory();
        context.setEdit (edit.get());
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addTrackCommands (registry, selection);
        phazite::ui::addTrackViewCommands (registry, context);
    }

    ~Tracks() { selection.deselectAll(); }

    te::Track* operator[] (const char* name) const
    {
        for (auto* track : phazite::getPlaylistTracks (*edit))
            if (track->getName() == name)
                return track;

        return nullptr;
    }

    juce::String getOrder() const
    {
        juce::StringArray names;

        for (auto* track : phazite::getPlaylistTracks (*edit))
            names.add (track->getName());

        return names.joinIntoString (" ");
    }

    juce::String getSelectedNames() const
    {
        juce::StringArray names;

        for (auto* track : phazite::getSelectedTracks (selection))
            names.add (track->getName());

        return names.joinIntoString (" ");
    }

    void select (std::initializer_list<const char*> names)
    {
        selection.deselectAll();

        for (const auto* name : names)
            selection.addToSelection ((*this)[name]);
    }

    juce::String getUndoName() const { return edit->getUndoManager().getUndoDescription(); }

    // How the playlist shows a track's height, through the model's row of it.
    phazite::TrackRow rowOf (const te::Track& track) const
    {
        return phazite::getTrackRow (context, phazite::getTrackID (track)).value_or (phazite::TrackRow());
    }
    int heightOf (const te::Track& track) const { return phazite::ui::getTrackHeight (rowOf (track)); }
    int shownHeightOf (const te::Track& track) const { return phazite::ui::getShownTrackHeight (rowOf (track)); }
    void setShownHeight (const te::Track& track, int height)
    {
        phazite::ui::setShownTrackHeight (context, phazite::getTrackID (track), height);
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngine() }; // the open project, as the app has it
    te::SelectionManager& selection = context.getSelection();
    phazite::RangeSelection& range = context.getRangeSelection();
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("The track palette has eight distinct, named colors")
{
    const auto& colours = phazite::getTrackColours();

    for (size_t i = 0; i < colours.size(); ++i)
    {
        CHECK (juce::String (colours[i].name).isNotEmpty());
        CHECK (colours[i].colour.isOpaque());

        for (auto j = i + 1; j < colours.size(); ++j)
            CHECK (colours[i].colour != colours[j].colour);
    }
}

TEST_CASE ("The playlist shows audio tracks and folders, but not the global tracks")
{
    Tracks tracks (1);
    auto& edit = *tracks.edit;
    edit.ensureTempoTrack();
    edit.ensureArrangerTrack();

    auto folder = edit.insertNewFolderTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr, false);
    REQUIRE (folder != nullptr);
    folder->setName ("Folder");
    auto inFolder = edit.insertNewAudioTrack (te::TrackInsertPoint (folder.get(), nullptr), nullptr);
    REQUIRE (inFolder != nullptr);
    inFolder->setName ("Inside");

    CHECK (tracks.getOrder() == juce::String ("A Folder Inside"));

    // A track without a color of its own shows its row's color from the palette.
    const auto& colours = phazite::getTrackColours();
    CHECK (phazite::getTrackColour (*tracks["A"], 0) == colours[0].colour);
    CHECK (phazite::getTrackColour (*tracks["A"], 9) == colours[1].colour);
    tracks["A"]->setColour (colours[4].colour);
    CHECK (phazite::getTrackColour (*tracks["A"], 0) == colours[4].colour);
}

TEST_CASE ("A new track goes below the selected one, in the next color, and is selected")
{
    Tracks tracks (2);
    const auto& colours = phazite::getTrackColours();

    // With nothing selected, at the bottom.
    REQUIRE (tracks.registry.invoke ("track.add"));
    CHECK (tracks.getUndoName() == juce::String ("Add Track"));
    auto added = phazite::getPlaylistTracks (*tracks.edit).getLast();
    added->setName ("New");
    CHECK (tracks.getOrder() == juce::String ("A B New"));
    CHECK (tracks.getSelectedNames() == juce::String ("New"));
    CHECK (added->getColour() == colours[2].colour); // B shows the palette's second color

    // Below the selected track.
    tracks.select ({ "A" });
    REQUIRE (tracks.registry.invoke ("track.add"));
    CHECK (phazite::getPlaylistTracks (*tracks.edit)[1]->getColour() == colours[1].colour);
    CHECK (phazite::getSelectedTracks (tracks.selection).getFirst() == phazite::getPlaylistTracks (*tracks.edit)[1]);

    phazite::undo (*tracks.edit);
    CHECK (tracks.getOrder() == juce::String ("A B New"));
}

TEST_CASE ("Deleting tracks is one undo step, and selects the track that takes their place")
{
    Tracks tracks (4);
    tracks.select ({ "B", "C" });
    CHECK (phazite::getCurrentName (*tracks.registry.find ("edit.delete")) == juce::String ("Delete 2 Tracks"));

    REQUIRE (tracks.registry.invoke ("edit.delete"));
    CHECK (tracks.getOrder() == juce::String ("A D"));
    CHECK (tracks.getSelectedNames() == juce::String ("D"));
    CHECK (tracks.getUndoName() == juce::String ("Delete 2 Tracks"));

    phazite::undo (*tracks.edit);
    CHECK (tracks.getOrder() == juce::String ("A B C D"));

    // The last track: the one above it is selected.
    tracks.select ({ "D" });
    REQUIRE (tracks.registry.invoke ("edit.delete"));
    CHECK (tracks.getSelectedNames() == juce::String ("C"));

    // Nothing selected: nothing to delete.
    tracks.selection.deselectAll();
    CHECK_FALSE (tracks.registry.invoke ("edit.delete"));
}

TEST_CASE ("Duplicating a track puts the copy right after it")
{
    Tracks tracks (2);
    tracks.select ({ "A" });

    REQUIRE (tracks.registry.invoke ("edit.duplicate"));
    CHECK (tracks.getOrder() == juce::String ("A A B"));
    CHECK (tracks.getUndoName() == juce::String ("Duplicate Track"));

    const auto copy = phazite::getPlaylistTracks (*tracks.edit)[1];
    CHECK (phazite::getSelectedTracks (tracks.selection) == juce::Array<te::Track*> { copy });
}

TEST_CASE ("Tracks move up and down, and selected neighbours move together")
{
    Tracks tracks (4);
    tracks.select ({ "B", "C" });

    REQUIRE (tracks.registry.invoke ("track.moveUp"));
    CHECK (tracks.getOrder() == juce::String ("B C A D"));
    CHECK (tracks.getUndoName() == juce::String ("Move Track Up"));

    REQUIRE (tracks.registry.invoke ("track.moveUp")); // already at the top
    CHECK (tracks.getOrder() == juce::String ("B C A D"));

    REQUIRE (tracks.registry.invoke ("track.moveDown"));
    CHECK (tracks.getOrder() == juce::String ("A B C D"));

    tracks.select ({ "D" });
    REQUIRE (tracks.registry.invoke ("track.moveDown")); // already at the bottom
    CHECK (tracks.getOrder() == juce::String ("A B C D"));
}

TEST_CASE ("The arrow-key commands step through the tracks")
{
    Tracks tracks (3);

    REQUIRE (tracks.registry.invoke ("track.selectNext")); // nothing selected: the first
    CHECK (tracks.getSelectedNames() == juce::String ("A"));
    REQUIRE (tracks.registry.invoke ("track.selectNext"));
    CHECK (tracks.getSelectedNames() == juce::String ("B"));
    REQUIRE (tracks.registry.invoke ("track.selectPrevious"));
    REQUIRE (tracks.registry.invoke ("track.selectPrevious"));
    CHECK (tracks.getSelectedNames() == juce::String ("A")); // stays at the top

    REQUIRE (tracks.registry.invoke ("edit.selectAll"));
    CHECK (tracks.getSelectedNames() == juce::String ("A B C"));
    REQUIRE (tracks.registry.invoke ("edit.deselectAll"));
    CHECK (tracks.getSelectedNames().isEmpty());
}

TEST_CASE ("Mute and solo switch the selected tracks on together, then off")
{
    Tracks tracks (3);
    const auto* mute = tracks.registry.find ("track.mute");
    REQUIRE (mute != nullptr);

    tracks["A"]->setMute (true);
    tracks.select ({ "A", "B" });
    CHECK_FALSE (mute->isTicked());

    REQUIRE (tracks.registry.invoke ("track.mute")); // not all were muted: now all are
    CHECK (tracks["A"]->isMuted (false));
    CHECK (tracks["B"]->isMuted (false));
    CHECK (mute->isTicked());

    REQUIRE (tracks.registry.invoke ("track.mute"));
    CHECK_FALSE (tracks["A"]->isMuted (false));
    CHECK_FALSE (tracks["B"]->isMuted (false));

    REQUIRE (tracks.registry.invoke ("track.solo"));
    CHECK (tracks["A"]->isSolo (false));
    CHECK (tracks["B"]->isSolo (false));
    CHECK_FALSE (tracks["C"]->isSolo (false));
}

TEST_CASE ("Track heights stay within limits, and change for the selection or all tracks")
{
    Tracks tracks (2);
    auto& a = *tracks["A"];
    auto& b = *tracks["B"];

    CHECK_FALSE (phazite::getSavedTrackHeight (a).has_value()); // a new track has none, and shows the standard height
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::standard);
    phazite::setSavedTrackHeight (a, 100000);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::maximum);

    // The project keeps a height as given, and the playlist shows it within its limits.
    phazite::setSavedTrackHeight (a, 100000);
    CHECK (phazite::getSavedTrackHeight (a).value_or (0) == 100000);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::maximum);

    phazite::setSavedTrackHeight (a, 0);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::minimum);
    CHECK_FALSE (tracks.edit->getUndoManager().canUndo()); // heights aren't undo steps

    tracks.select ({ "B" });
    REQUIRE (tracks.registry.invoke ("track.taller"));
    CHECK (tracks.heightOf (b) == phazite::ui::TrackHeight::standard + phazite::ui::TrackHeight::step);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::minimum);

    tracks.selection.deselectAll();
    REQUIRE (tracks.registry.invoke ("track.shorter"));
    CHECK (tracks.heightOf (b) == phazite::ui::TrackHeight::standard);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::minimum);
    CHECK (phazite::isCollapsed (a)); // past the minimum, a track collapses
    CHECK_FALSE (phazite::isCollapsed (b));
}

TEST_CASE ("A track collapses to a thin row by its command, or by getting shorter than the minimum")
{
    Tracks tracks (2);
    auto& a = *tracks["A"];
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::standard);

    tracks.select ({ "A" });
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK (phazite::isCollapsed (a));
    CHECK (tracks.registry.find ("track.collapse")->isTicked());
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::collapsed);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::standard); // kept for when it expands
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK_FALSE (phazite::isCollapsed (a));
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::standard);

    // Shorter and shorter: 48, 32, then collapsed. Taller expands it again, from the thin row.
    for (int i = 0; i < 3; ++i)
        REQUIRE (tracks.registry.invoke ("track.shorter"));

    CHECK (phazite::isCollapsed (a));
    REQUIRE (tracks.registry.invoke ("track.taller"));
    CHECK_FALSE (phazite::isCollapsed (a));
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::collapsed + phazite::ui::TrackHeight::step);

    // Dragging the edge: halfway between the thin row and the minimum, it snaps to one or the other.
    tracks.setShownHeight (a, 27);
    CHECK (phazite::isCollapsed (a));
    tracks.setShownHeight (a, 29);
    CHECK_FALSE (phazite::isCollapsed (a));
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::minimum);
    CHECK_FALSE (tracks.edit->getUndoManager().canUndo()); // collapsing changes the view, as heights do

    // A collapsed folder keeps its height (its tracks are hidden instead), and doesn't collapse by resizing.
    tracks.select ({ "B" });
    REQUIRE (tracks.registry.invoke ("track.groupIntoFolder"));
    auto& folder = *tracks["Folder"];
    phazite::setCollapsed (folder, true);
    CHECK (tracks.shownHeightOf (folder) == phazite::ui::TrackHeight::standard);
    phazite::setCollapsed (folder, false);
    tracks.setShownHeight (folder, 0);
    CHECK_FALSE (phazite::isCollapsed (folder));
    CHECK (tracks.shownHeightOf (folder) == phazite::ui::TrackHeight::minimum);
}

TEST_CASE ("Tracks group into a folder, which collapses, takes more tracks and ungroups")
{
    Tracks tracks (3);

    // An empty folder goes at the bottom when nothing is selected.
    REQUIRE (tracks.registry.invoke ("track.addFolder"));
    CHECK (tracks.getOrder() == juce::String ("A B C Folder"));
    phazite::undo (*tracks.edit);
    CHECK (tracks.getOrder() == juce::String ("A B C"));

    tracks.select ({ "A", "B" });
    REQUIRE (tracks.registry.invoke ("track.groupIntoFolder"));
    CHECK (tracks.getUndoName() == juce::String ("Group into Folder"));
    CHECK (tracks.getOrder() == juce::String ("Folder A B C"));
    CHECK (tracks.getSelectedNames() == juce::String ("Folder"));

    auto* folder = dynamic_cast<te::FolderTrack*> (tracks["Folder"]);
    REQUIRE (folder != nullptr);
    CHECK (tracks["A"]->getParentFolderTrack() == folder);
    CHECK (tracks["B"]->getParentFolderTrack() == folder);
    CHECK (tracks["C"]->getParentFolderTrack() == nullptr);

    // Collapsing hides its tracks, and isn't an undo step.
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK (phazite::isCollapsed (*folder));
    CHECK (phazite::isHiddenInCollapsedFolder (*tracks["A"]));
    CHECK_FALSE (phazite::isHiddenInCollapsedFolder (*tracks["C"]));
    CHECK (tracks.getUndoName() == juce::String ("Group into Folder"));
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK_FALSE (phazite::isCollapsed (*folder));

    // Another track goes in after its last track.
    phazite::moveTrackIntoFolder (*tracks["C"], *folder);
    CHECK (tracks.getOrder() == juce::String ("Folder A B C"));
    CHECK (tracks["C"]->getParentFolderTrack() == folder);

    // Ungrouping takes the tracks out to where the folder was.
    tracks.select ({ "Folder" });
    REQUIRE (tracks.registry.invoke ("track.ungroup"));
    CHECK (tracks.getOrder() == juce::String ("A B C"));
    CHECK (tracks["A"]->getParentFolderTrack() == nullptr);

    phazite::undo (*tracks.edit);
    CHECK (tracks.getOrder() == juce::String ("Folder A B C"));
}

TEST_CASE ("Pinning a track is kept in the project, but isn't an undo step")
{
    Tracks tracks (2);
    tracks.select ({ "B" });

    REQUIRE (tracks.registry.invoke ("track.pin"));
    CHECK (phazite::isPinned (*tracks["B"]));
    CHECK (tracks.registry.find ("track.pin")->isTicked());
    CHECK_FALSE (tracks.edit->getUndoManager().canUndo());

    REQUIRE (tracks.registry.invoke ("track.pin"));
    CHECK_FALSE (phazite::isPinned (*tracks["B"]));
}

TEST_CASE ("Solo leaves the mute switches alone, and a track can be soloed alone")
{
    Tracks tracks (3);
    auto& a = *tracks["A"];
    auto& b = *tracks["B"];
    auto& c = *tracks["C"];
    c.setMute (true);

    a.setSolo (true);
    CHECK (phazite::isSilencedBySolo (b));
    CHECK_FALSE (phazite::isSilencedBySolo (a));
    CHECK_FALSE (phazite::isSilencedBySolo (c)); // muted by its own switch
    b.setSolo (true);

    // Alone: the other solos go, and the mute switches stay as they were.
    phazite::toggleSoloAlone (a);
    CHECK (a.isSolo (false));
    CHECK_FALSE (b.isSolo (false));
    CHECK (c.isMuted (false));
    CHECK (phazite::isSilencedBySolo (b));

    // Again: no solo at all, and C is still muted.
    phazite::toggleSoloAlone (a);
    CHECK_FALSE (a.isSolo (false));
    CHECK_FALSE (tracks.edit->areAnyTracksSolo());
    CHECK (c.isMuted (false));
    CHECK_FALSE (phazite::isSilencedBySolo (b));
}

TEST_CASE ("The track switches work on lists of tracks")
{
    Tracks tracks (3);
    auto* a = tracks["A"];
    auto* b = tracks["B"];
    const juce::Array<te::Track*> both { a, b }, none;

    struct Switch
    {
        const char* name;
        bool (*allOn) (const juce::Array<te::Track*>&);
        void (*toggle) (const juce::Array<te::Track*>&);
    };

    // Each switch: on for all of them unless all are on, then off; an empty list is never all on.
    for (const auto& s : { Switch { "mute", phazite::areAllTracksMuted, phazite::toggleTracksMuted },
                           Switch { "solo", phazite::areAllTracksSoloed, phazite::toggleTracksSoloed },
                           Switch { "collapse", phazite::areAllTracksCollapsed, phazite::toggleTracksCollapsed },
                           Switch { "pin", phazite::areAllTracksPinned, phazite::toggleTracksPinned } })
    {
        CAPTURE (s.name);
        CHECK_FALSE (s.allOn (none));
        s.toggle (none);

        s.toggle ({ a }); // one of the two on
        CHECK (s.allOn ({ a }));
        CHECK_FALSE (s.allOn (both));
        s.toggle (both); // so both go on
        CHECK (s.allOn (both));
        s.toggle (both); // and then both off
        CHECK_FALSE (s.allOn ({ a }));
        CHECK_FALSE (s.allOn ({ b }));
    }

    // Collapsing and pinning change the view, so they aren't undo steps.
    tracks.edit->getUndoManager().clearUndoHistory();
    phazite::toggleTracksCollapsed (both);
    phazite::toggleTracksPinned (both);
    CHECK_FALSE (tracks.edit->getUndoManager().canUndo());

    SUBCASE ("A track in a muted folder isn't muted itself, and Mute switches its own switch")
    {
        auto* folder = phazite::groupIntoFolder ({ b }, nullptr);
        REQUIRE (folder != nullptr);
        folder->setMute (true);
        CHECK (b->isMuted (true)); // silent, through its folder
        CHECK_FALSE (phazite::areAllTracksMuted ({ b }));

        phazite::toggleTracksMuted ({ b });
        CHECK (b->isMuted (false));
        phazite::toggleTracksMuted ({ b });
        CHECK_FALSE (b->isMuted (false));
        CHECK (folder->isMuted (false)); // the folder's own switch stays
    }
}

TEST_CASE ("Tracks and folders go in after the selection")
{
    Tracks tracks (3);

    // After the last selected track, and selected alone; at the end while nothing is selected.
    tracks.select ({ "A", "B" });
    auto* track = phazite::insertTrackAfterSelection (tracks.selection);
    REQUIRE (track != nullptr);
    track->setName ("New");
    CHECK (tracks.getOrder() == juce::String ("A B New C"));
    CHECK (tracks.getSelectedNames() == juce::String ("New"));

    tracks.selection.deselectAll();
    auto* folder = phazite::insertFolderAfterSelection (tracks.selection);
    REQUIRE (folder != nullptr);
    CHECK (tracks.getOrder() == juce::String ("A B New C Folder"));
    CHECK (tracks.getSelectedNames() == juce::String ("Folder"));

    // Without an Edit, nothing goes in.
    tracks.selection.deselectAll();
    tracks.selection.edit = nullptr;
    CHECK (phazite::insertTrackAfterSelection (tracks.selection) == nullptr);
    CHECK (phazite::insertFolderAfterSelection (tracks.selection) == nullptr);
    tracks.selection.edit = tracks.edit.get();
    CHECK (phazite::getPlaylistTracks (*tracks.edit).size() == 5);
}

TEST_CASE ("Selected folders ungroup together")
{
    Tracks tracks (4);
    auto* first = phazite::groupIntoFolder ({ tracks["A"], tracks["B"] }, nullptr);
    REQUIRE (first != nullptr);
    first->setName ("F1");
    auto* second = phazite::groupIntoFolder ({ tracks["C"] }, nullptr);
    REQUIRE (second != nullptr);
    second->setName ("F2");
    CHECK (tracks.getOrder() == juce::String ("F1 A B F2 C D"));

    tracks.select ({ "F1", "F2", "D" });
    CHECK (phazite::getSelectedFolders (tracks.selection).size() == 2);
    CHECK (phazite::hasSelectedFolders (tracks.selection));

    phazite::ungroupFolders (phazite::getSelectedFolders (tracks.selection));
    CHECK (tracks.getOrder() == juce::String ("A B C D"));
    CHECK_FALSE (phazite::hasSelectedFolders (tracks.selection));
}

TEST_CASE ("Neighbour selection starts at either end")
{
    Tracks tracks (3);

    // With nothing selected, up starts at the bottom and down at the top; neither goes past the end.
    phazite::selectNeighbourTrack (tracks.selection, -1);
    CHECK (tracks.getSelectedNames() == juce::String ("C"));
    phazite::selectNeighbourTrack (tracks.selection, 1);
    CHECK (tracks.getSelectedNames() == juce::String ("C"));

    tracks.selection.deselectAll();
    phazite::selectNeighbourTrack (tracks.selection, 1);
    CHECK (tracks.getSelectedNames() == juce::String ("A"));
    phazite::selectNeighbourTrack (tracks.selection, -1);
    CHECK (tracks.getSelectedNames() == juce::String ("A"));

    // From several selected tracks: down from the last one.
    tracks.select ({ "A", "B" });
    phazite::selectNeighbourTrack (tracks.selection, 1);
    CHECK (tracks.getSelectedNames() == juce::String ("C"));
}

TEST_CASE ("Tracks move within their folders")
{
    Tracks tracks (4);
    REQUIRE (phazite::groupIntoFolder ({ tracks["B"], tracks["C"] }, nullptr) != nullptr);
    CHECK (tracks.getOrder() == juce::String ("A Folder B C D"));

    tracks.select ({ "C" });
    phazite::moveSelectedTracks (tracks.selection, -1);
    CHECK (tracks.getOrder() == juce::String ("A Folder C B D"));
    phazite::moveSelectedTracks (tracks.selection, -1); // the first in its folder stays inside it
    CHECK (tracks.getOrder() == juce::String ("A Folder C B D"));

    // A folder moves with its tracks, past the top-level tracks.
    tracks.select ({ "Folder" });
    phazite::moveSelectedTracks (tracks.selection, 1);
    CHECK (tracks.getOrder() == juce::String ("A D Folder C B"));
}

TEST_CASE ("Deleting and duplicating tracks directly")
{
    Tracks tracks (3);

    // Each copy goes right after its original. Tracktion adds the copies to what is selected.
    tracks.select ({ "B" });
    auto copies = phazite::duplicateTracks ({ tracks["A"], tracks["C"] }, nullptr);
    REQUIRE (copies.size() == 2);
    CHECK (tracks.getOrder() == juce::String ("A A B C C"));
    CHECK (tracks.getSelectedNames() == juce::String ("A B C"));
    CHECK (tracks.selection.isSelected (copies.getFirst()));

    // With one, the copies are selected instead.
    copies = phazite::duplicateTracks ({ tracks["B"] }, &tracks.selection);
    REQUIRE (copies.size() == 1);
    CHECK (tracks.getOrder() == juce::String ("A A B B C C"));
    CHECK (phazite::getSelectedTracks (tracks.selection).size() == 1);
    CHECK (phazite::getSelectedTracks (tracks.selection).getFirst() == copies.getFirst());

    phazite::deleteTracks ({ copies.getFirst() }, nullptr);
    CHECK (tracks.getOrder() == juce::String ("A A B C C"));

    // A folder goes with its tracks; with a selection manager, the track that takes the first one's place is selected.
    auto* folder = phazite::groupIntoFolder ({ tracks["B"] }, nullptr);
    REQUIRE (folder != nullptr);
    CHECK (tracks.getOrder() == juce::String ("A A Folder B C C"));
    phazite::deleteTracks ({ folder, tracks["B"] }, &tracks.selection);
    CHECK (tracks.getOrder() == juce::String ("A A C C"));
    CHECK (tracks.getSelectedNames() == juce::String ("C"));

    phazite::deleteTracks ({}, &tracks.selection); // nothing to delete
    CHECK (tracks.getOrder() == juce::String ("A A C C"));
}

TEST_CASE ("The selected tracks, or all of them")
{
    Tracks tracks (3);
    CHECK_FALSE (phazite::hasSelectedTracks (tracks.selection));
    CHECK (phazite::getSelectedTracksOrAll (tracks.selection).size() == 3);

    tracks.select ({ "B" });
    CHECK (phazite::hasSelectedTracks (tracks.selection));
    const auto selected = phazite::getSelectedTracksOrAll (tracks.selection);
    REQUIRE (selected.size() == 1);
    CHECK (selected.getFirst() == tracks["B"]);

    // Without an Edit, there are none.
    tracks.selection.deselectAll();
    tracks.selection.edit = nullptr;
    CHECK (phazite::getSelectedTracksOrAll (tracks.selection).isEmpty());
    tracks.selection.edit = tracks.edit.get();
}

TEST_CASE ("Palette colours are found by name, whatever the case")
{
    const auto& colours = phazite::getTrackColours();
    CHECK (phazite::findTrackColour ("Sky blue") == colours[1].colour);
    CHECK (phazite::findTrackColour ("sky BLUE") == colours[1].colour);
    CHECK (phazite::findTrackColour ("GRAY") == colours[7].colour);
    CHECK_FALSE (phazite::findTrackColour ("Magenta").has_value());
    CHECK_FALSE (phazite::findTrackColour ("").has_value());
}

TEST_CASE ("Renaming a track is an undo step, and an empty name gives it its number back")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    edit->getUndoManager().clearUndoHistory();

    phazite::renameTrack (*track, "  Bass ");
    CHECK (track->getName() == juce::String ("Bass"));
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Rename Track"));

    edit->getUndoManager().clearUndoHistory();
    phazite::renameTrack (*track, "Bass");
    CHECK_FALSE (edit->getUndoManager().canUndo());

    phazite::renameTrack (*track, "");
    CHECK (track->getName() != juce::String ("Bass"));
    CHECK (track->getName().isNotEmpty());
}

TEST_CASE ("Adding a track at the end, coloring tracks and moving them are undo steps of their own")
{
    Tracks t (3);

    auto* added = phazite::addTrackAtEnd (*t.edit, false, &t.selection);
    REQUIRE (added != nullptr);
    CHECK (phazite::getPlaylistTracks (*t.edit).getLast() == added);
    CHECK (t.selection.isSelected (added));
    CHECK (t.getUndoName() == juce::String ("Add Track"));
    added->setName ("D");

    auto* folder = phazite::addTrackAtEnd (*t.edit, true, nullptr);
    REQUIRE (folder != nullptr);
    CHECK (folder->isFolderTrack());
    CHECK (t.getUndoName() == juce::String ("Add Folder"));
    folder->setName ("F");

    phazite::setTrackColours ({ t["A"], t["B"] }, juce::Colours::red);
    CHECK (t["A"]->getColour() == juce::Colours::red);
    CHECK (t["B"]->getColour() == juce::Colours::red);
    CHECK (t.getUndoName() == juce::String ("Change Track Colors"));
    phazite::setTrackColours ({ t["C"] }, juce::Colours::red);
    CHECK (t.getUndoName() == juce::String ("Change Track Color"));

    // To the start, in the order given; then after another track.
    phazite::moveTracksAfter ({ t["C"], t["A"] }, nullptr);
    CHECK (t.getOrder() == juce::String ("C A B D F"));
    CHECK (t.getUndoName() == juce::String ("Move Tracks"));
    phazite::moveTracksAfter ({ t["C"] }, t["D"]);
    CHECK (t.getOrder() == juce::String ("A B D C F"));
    CHECK (t.getUndoName() == juce::String ("Move Track"));

    phazite::undo (*t.edit);
    CHECK (t.getOrder() == juce::String ("C A B D F"));
}

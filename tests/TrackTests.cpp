// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/playlist/TrackHeights.h"

namespace
{
// A song with tracks named A, B, C..., a selection, and the track commands, with the playlist's height commands.
struct Tracks
{
    explicit Tracks (int numTracks)
    {
        const auto ids = phazite::ensureTracks (*song, numTracks);

        for (int i = 0; i < numTracks; ++i)
            phazite::setTrackName (*song, ids[(size_t) i], juce::String::charToString ((juce::juce_wchar) ('A' + i)));

        song->getUndoManager().clearUndoHistory();
        setSong (context, song.get());
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addTrackCommands (registry, selection);
        phazite::ui::addTrackViewCommands (registry, context);
    }

    ~Tracks()
    {
        selection.deselectAll();
        setSong (context, nullptr);
    }

    phazite::TrackID operator[] (const char* name) const
    {
        for (const auto track : phazite::getTrackIDs (*song))
            if (phazite::getTrackName (*song, track) == name)
                return track;

        return {};
    }

    juce::String getOrder() const
    {
        juce::StringArray names;

        for (const auto track : phazite::getTrackIDs (*song))
            names.add (phazite::getTrackName (*song, track));

        return names.joinIntoString (" ");
    }

    juce::String getSelectedNames() const
    {
        juce::StringArray names;

        for (const auto track : phazite::getSelectedTracks (selection))
            names.add (phazite::getTrackName (*song, track));

        return names.joinIntoString (" ");
    }

    void select (std::initializer_list<const char*> names)
    {
        selection.deselectAll();

        for (const auto* name : names)
            selection.addToSelection ((*this)[name]);
    }

    juce::String getUndoName() const { return song->getUndoManager().getUndoDescription(); }

    // A track's own color, as the song keeps it: transparent while it has none.
    juce::Colour getOwnColour (phazite::TrackID track) const
    {
        return juce::Colour::fromString (phazite::findTrackState (*song, track)[phazite::schema::trackColour].toString());
    }

    // How the playlist shows a track's height, through the model's row of it.
    phazite::TrackRow rowOf (phazite::TrackID track) const { return phazite::getTrackRow (context, track).value_or (phazite::TrackRow()); }
    int heightOf (phazite::TrackID track) const { return phazite::ui::getTrackHeight (rowOf (track)); }
    int shownHeightOf (phazite::TrackID track) const { return phazite::ui::getShownTrackHeight (rowOf (track)); }
    void setShownHeight (phazite::TrackID track, int height) { phazite::ui::setShownTrackHeight (context, track, height); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::Selection& selection = getSelection (context);
    phazite::RangeSelection& range = getRangeSelection (context);
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

TEST_CASE ("The playlist shows tracks and folders, each folder before its tracks, in their rows' colors")
{
    Tracks tracks (1);
    auto& song = *tracks.song;

    const auto folder = phazite::insertFolder (song, {}, nullptr);
    REQUIRE (folder.isValid());
    phazite::setTrackName (song, folder, "Folder");
    const auto inFolder = phazite::insertPlainTrack (song, {});
    REQUIRE (inFolder.isValid());
    phazite::moveTrackIntoFolder (song, inFolder, folder);
    phazite::setTrackName (song, inFolder, "Inside");

    CHECK (tracks.getOrder() == juce::String ("A Folder Inside"));

    // A track without a color of its own shows its row's color from the palette.
    const auto& colours = phazite::getTrackColours();
    CHECK (phazite::getTrackColour (song, tracks["A"], 0) == colours[0].colour);
    CHECK (phazite::getTrackColour (song, tracks["A"], 9) == colours[1].colour);
    phazite::setTrackColour (song, tracks["A"], colours[4].colour);
    CHECK (phazite::getTrackColour (song, tracks["A"], 0) == colours[4].colour);
}

TEST_CASE ("A new track goes below the selected one, in the next color, and is selected")
{
    Tracks tracks (2);
    auto& song = *tracks.song;
    const auto& colours = phazite::getTrackColours();

    // With nothing selected, at the bottom.
    REQUIRE (tracks.registry.invoke ("track.add"));
    CHECK (tracks.getUndoName() == juce::String ("Add Track"));
    const auto added = phazite::getTrackIDs (song).back();
    phazite::setTrackName (song, added, "New");
    CHECK (tracks.getOrder() == juce::String ("A B New"));
    CHECK (tracks.getSelectedNames() == juce::String ("New"));
    CHECK (tracks.getOwnColour (added) == colours[2].colour); // B shows the palette's second color

    // Below the selected track.
    tracks.select ({ "A" });
    REQUIRE (tracks.registry.invoke ("track.add"));
    CHECK (tracks.getOwnColour (phazite::getTrackIDs (song)[1]) == colours[1].colour);
    CHECK (phazite::getSelectedTracks (tracks.selection).front() == phazite::getTrackIDs (song)[1]);

    phazite::undo (song);
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

    phazite::undo (*tracks.song);
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

    const auto copy = phazite::getTrackIDs (*tracks.song)[1];
    CHECK (phazite::getSelectedTracks (tracks.selection) == std::vector { copy });
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
    auto& song = *tracks.song;
    const auto* mute = tracks.registry.find ("track.mute");
    REQUIRE (mute != nullptr);

    phazite::setTrackMuted (song, tracks["A"], true);
    tracks.select ({ "A", "B" });
    CHECK_FALSE (mute->isTicked());

    REQUIRE (tracks.registry.invoke ("track.mute")); // not all were muted: now all are
    CHECK (phazite::isTrackMuted (song, tracks["A"], false));
    CHECK (phazite::isTrackMuted (song, tracks["B"], false));
    CHECK (mute->isTicked());

    REQUIRE (tracks.registry.invoke ("track.mute"));
    CHECK_FALSE (phazite::isTrackMuted (song, tracks["A"], false));
    CHECK_FALSE (phazite::isTrackMuted (song, tracks["B"], false));

    REQUIRE (tracks.registry.invoke ("track.solo"));
    CHECK (phazite::isTrackSoloed (song, tracks["A"]));
    CHECK (phazite::isTrackSoloed (song, tracks["B"]));
    CHECK_FALSE (phazite::isTrackSoloed (song, tracks["C"]));
}

TEST_CASE ("Track heights stay within limits, and change for the selection or all tracks")
{
    Tracks tracks (2);
    auto& song = *tracks.song;
    const auto a = tracks["A"];
    const auto b = tracks["B"];

    CHECK_FALSE (phazite::getSavedTrackHeight (song, a).has_value()); // a new track has none, and shows the standard height
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::standard);
    phazite::setSavedTrackHeight (song, a, 100000);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::maximum);

    // The project keeps a height as given, and the playlist shows it within its limits.
    phazite::setSavedTrackHeight (song, a, 100000);
    CHECK (phazite::getSavedTrackHeight (song, a).value_or (0) == 100000);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::maximum);

    phazite::setSavedTrackHeight (song, a, 0);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::minimum);
    CHECK_FALSE (song.getUndoManager().canUndo()); // heights aren't undo steps

    tracks.select ({ "B" });
    REQUIRE (tracks.registry.invoke ("track.taller"));
    CHECK (tracks.heightOf (b) == phazite::ui::TrackHeight::standard + phazite::ui::TrackHeight::step);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::minimum);

    tracks.selection.deselectAll();
    REQUIRE (tracks.registry.invoke ("track.shorter"));
    CHECK (tracks.heightOf (b) == phazite::ui::TrackHeight::standard);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::minimum);
    CHECK (phazite::isCollapsed (song, a)); // past the minimum, a track collapses
    CHECK_FALSE (phazite::isCollapsed (song, b));
}

TEST_CASE ("A track collapses to a thin row by its command, or by getting shorter than the minimum")
{
    Tracks tracks (2);
    auto& song = *tracks.song;
    const auto a = tracks["A"];
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::standard);

    tracks.select ({ "A" });
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK (phazite::isCollapsed (song, a));
    CHECK (tracks.registry.find ("track.collapse")->isTicked());
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::collapsed);
    CHECK (tracks.heightOf (a) == phazite::ui::TrackHeight::standard); // kept for when it expands
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK_FALSE (phazite::isCollapsed (song, a));
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::standard);

    // Shorter and shorter: 48, 32, then collapsed. Taller expands it again, from the thin row.
    for (int i = 0; i < 3; ++i)
        REQUIRE (tracks.registry.invoke ("track.shorter"));

    CHECK (phazite::isCollapsed (song, a));
    REQUIRE (tracks.registry.invoke ("track.taller"));
    CHECK_FALSE (phazite::isCollapsed (song, a));
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::collapsed + phazite::ui::TrackHeight::step);

    // Dragging the edge: halfway between the thin row and the minimum, it snaps to one or the other.
    tracks.setShownHeight (a, 27);
    CHECK (phazite::isCollapsed (song, a));
    tracks.setShownHeight (a, 29);
    CHECK_FALSE (phazite::isCollapsed (song, a));
    CHECK (tracks.shownHeightOf (a) == phazite::ui::TrackHeight::minimum);
    CHECK_FALSE (song.getUndoManager().canUndo()); // collapsing changes the view, as heights do

    // A collapsed folder keeps its height, shorter than a track's at first (its tracks are hidden instead), and doesn't collapse
    // by resizing.
    tracks.select ({ "B" });
    REQUIRE (tracks.registry.invoke ("track.groupIntoFolder"));
    const auto folder = tracks["Folder"];
    REQUIRE (folder.isValid());
    phazite::setCollapsed (song, folder, true);
    CHECK (tracks.shownHeightOf (folder) == phazite::ui::TrackHeight::folder);
    phazite::setCollapsed (song, folder, false);
    tracks.setShownHeight (folder, 0);
    CHECK_FALSE (phazite::isCollapsed (song, folder));
    CHECK (tracks.shownHeightOf (folder) == phazite::ui::TrackHeight::minimum);
}

TEST_CASE ("Tracks group into a folder, which collapses, takes more tracks and ungroups")
{
    Tracks tracks (3);
    auto& song = *tracks.song;

    // An empty folder goes at the bottom when nothing is selected.
    REQUIRE (tracks.registry.invoke ("track.addFolder"));
    CHECK (tracks.getOrder() == juce::String ("A B C Folder"));
    phazite::undo (song);
    CHECK (tracks.getOrder() == juce::String ("A B C"));

    tracks.select ({ "A", "B" });
    REQUIRE (tracks.registry.invoke ("track.groupIntoFolder"));
    CHECK (tracks.getUndoName() == juce::String ("Group into Folder"));
    CHECK (tracks.getOrder() == juce::String ("Folder A B C"));
    CHECK (tracks.getSelectedNames() == juce::String ("Folder"));

    const auto folder = tracks["Folder"];
    REQUIRE (phazite::isFolder (song, folder));
    CHECK (phazite::getParentFolder (song, tracks["A"]) == folder);
    CHECK (phazite::getParentFolder (song, tracks["B"]) == folder);
    CHECK_FALSE (phazite::getParentFolder (song, tracks["C"]).isValid());

    // Collapsing hides its tracks, and isn't an undo step.
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK (phazite::isCollapsed (song, folder));
    CHECK (phazite::isHiddenInCollapsedFolder (song, tracks["A"]));
    CHECK_FALSE (phazite::isHiddenInCollapsedFolder (song, tracks["C"]));
    CHECK (tracks.getUndoName() == juce::String ("Group into Folder"));
    REQUIRE (tracks.registry.invoke ("track.collapse"));
    CHECK_FALSE (phazite::isCollapsed (song, folder));

    // Another track goes in after its last track.
    phazite::moveTrackIntoFolder (song, tracks["C"], folder);
    CHECK (tracks.getOrder() == juce::String ("Folder A B C"));
    CHECK (phazite::getParentFolder (song, tracks["C"]) == folder);

    // Ungrouping takes the tracks out to where the folder was.
    tracks.select ({ "Folder" });
    REQUIRE (tracks.registry.invoke ("track.ungroup"));
    CHECK (tracks.getOrder() == juce::String ("A B C"));
    CHECK_FALSE (phazite::getParentFolder (song, tracks["A"]).isValid());

    phazite::undo (song);
    CHECK (tracks.getOrder() == juce::String ("Folder A B C"));
}

TEST_CASE ("Pinning a track is kept in the project, but isn't an undo step")
{
    Tracks tracks (2);
    auto& song = *tracks.song;
    tracks.select ({ "B" });

    REQUIRE (tracks.registry.invoke ("track.pin"));
    CHECK (phazite::isPinned (song, tracks["B"]));
    CHECK (tracks.registry.find ("track.pin")->isTicked());
    CHECK_FALSE (song.getUndoManager().canUndo());

    REQUIRE (tracks.registry.invoke ("track.pin"));
    CHECK_FALSE (phazite::isPinned (song, tracks["B"]));
}

TEST_CASE ("Solo leaves the mute switches alone, and a track can be soloed alone")
{
    Tracks tracks (3);
    auto& song = *tracks.song;
    const auto a = tracks["A"];
    const auto b = tracks["B"];
    const auto c = tracks["C"];
    phazite::setTrackMuted (song, c, true);

    phazite::setTrackSoloed (song, a, true);
    CHECK (phazite::isSilencedBySolo (song, b));
    CHECK_FALSE (phazite::isSilencedBySolo (song, a));
    CHECK_FALSE (phazite::isSilencedBySolo (song, c)); // muted by its own switch
    phazite::setTrackSoloed (song, b, true);

    // Alone: the other solos go, and the mute switches stay as they were.
    phazite::toggleSoloAlone (song, a);
    CHECK (phazite::isTrackSoloed (song, a));
    CHECK_FALSE (phazite::isTrackSoloed (song, b));
    CHECK (phazite::isTrackMuted (song, c, false));
    CHECK (phazite::isSilencedBySolo (song, b));

    // Again: no solo at all, and C is still muted.
    phazite::toggleSoloAlone (song, a);
    CHECK_FALSE (phazite::isTrackSoloed (song, a));
    CHECK_FALSE (phazite::areAnyTracksSoloed (song));
    CHECK (phazite::isTrackMuted (song, c, false));
    CHECK_FALSE (phazite::isSilencedBySolo (song, b));
}

TEST_CASE ("The track switches work on lists of tracks")
{
    Tracks tracks (3);
    const auto a = tracks["A"];
    const auto b = tracks["B"];
    auto& song = *tracks.song;
    const std::vector<phazite::TrackID> both { a, b }, onlyA { a }, onlyB { b };
    const std::vector<phazite::TrackID> none;

    struct Switch
    {
        const char* name;
        bool (*allOn) (const phazite::Song&, const std::vector<phazite::TrackID>&);
        void (*toggle) (phazite::Song&, const std::vector<phazite::TrackID>&);
    };

    // Each switch: on for all of them unless all are on, then off; an empty list is never all on.
    for (const auto& s : { Switch { "mute", phazite::areAllTracksMuted, phazite::toggleTracksMuted },
                           Switch { "solo", phazite::areAllTracksSoloed, phazite::toggleTracksSoloed },
                           Switch { "collapse", phazite::areAllTracksCollapsed, phazite::toggleTracksCollapsed },
                           Switch { "pin", phazite::areAllTracksPinned, phazite::toggleTracksPinned } })
    {
        CAPTURE (s.name);
        CHECK_FALSE (s.allOn (song, none));
        s.toggle (song, none);

        s.toggle (song, onlyA); // one of the two on
        CHECK (s.allOn (song, onlyA));
        CHECK_FALSE (s.allOn (song, both));
        s.toggle (song, both); // so both go on
        CHECK (s.allOn (song, both));
        s.toggle (song, both); // and then both off
        CHECK_FALSE (s.allOn (song, onlyA));
        CHECK_FALSE (s.allOn (song, onlyB));
    }

    // Collapsing and pinning change the view, so they aren't undo steps.
    song.getUndoManager().clearUndoHistory();
    phazite::toggleTracksCollapsed (song, both);
    phazite::toggleTracksPinned (song, both);
    CHECK_FALSE (song.getUndoManager().canUndo());

    SUBCASE ("A track in a muted folder isn't muted itself, and Mute switches its own switch")
    {
        const auto folder = phazite::groupIntoFolder (song, { b }, nullptr);
        REQUIRE (folder.isValid());
        phazite::setTrackMuted (song, folder, true);
        CHECK (phazite::isTrackMuted (song, b, true)); // silent, through its folder
        CHECK_FALSE (phazite::areAllTracksMuted (song, onlyB));

        phazite::toggleTracksMuted (song, onlyB);
        CHECK (phazite::isTrackMuted (song, b, false));
        phazite::toggleTracksMuted (song, onlyB);
        CHECK_FALSE (phazite::isTrackMuted (song, b, false));
        CHECK (phazite::isTrackMuted (song, folder, false)); // the folder's own switch stays
    }
}

TEST_CASE ("Tracks and folders go in after the selection")
{
    Tracks tracks (3);
    auto& song = *tracks.song;

    // After the last selected track, and selected alone; at the end while nothing is selected.
    tracks.select ({ "A", "B" });
    const auto track = phazite::insertTrackAfterSelection (tracks.selection);
    REQUIRE (track.isValid());
    phazite::setTrackName (song, track, "New");
    CHECK (tracks.getOrder() == juce::String ("A B New C"));
    CHECK (tracks.getSelectedNames() == juce::String ("New"));

    tracks.selection.deselectAll();
    const auto folder = phazite::insertFolderAfterSelection (tracks.selection);
    REQUIRE (folder.isValid());
    CHECK (tracks.getOrder() == juce::String ("A B New C Folder"));
    CHECK (tracks.getSelectedNames() == juce::String ("Folder"));

    // Without a song, nothing goes in.
    tracks.selection.deselectAll();
    tracks.selection.setSong (nullptr);
    CHECK_FALSE (phazite::insertTrackAfterSelection (tracks.selection).isValid());
    CHECK_FALSE (phazite::insertFolderAfterSelection (tracks.selection).isValid());
    tracks.selection.setSong (&song);
    CHECK (phazite::getTrackIDs (song).size() == 5);
}

TEST_CASE ("Selected folders ungroup together")
{
    Tracks tracks (4);
    auto& song = *tracks.song;
    const auto first = phazite::groupIntoFolder (song, { tracks["A"], tracks["B"] }, nullptr);
    REQUIRE (first.isValid());
    phazite::setTrackName (song, first, "F1");
    const auto second = phazite::groupIntoFolder (song, { tracks["C"] }, nullptr);
    REQUIRE (second.isValid());
    phazite::setTrackName (song, second, "F2");
    CHECK (tracks.getOrder() == juce::String ("F1 A B F2 C D"));

    tracks.select ({ "F1", "F2", "D" });
    CHECK (phazite::getSelectedFolders (tracks.selection).size() == 2);
    CHECK (phazite::hasSelectedFolders (tracks.selection));

    phazite::ungroupFolders (song, phazite::getSelectedFolders (tracks.selection));
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
    REQUIRE (phazite::groupIntoFolder (*tracks.song, { tracks["B"], tracks["C"] }, nullptr).isValid());
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
    auto& song = *tracks.song;

    // Each copy goes right after its original. Without a selection, what is selected stays as it was.
    tracks.select ({ "B" });
    auto copies = phazite::duplicateTracks (song, { tracks["A"], tracks["C"] }, nullptr);
    REQUIRE (copies.size() == 2);
    CHECK (tracks.getOrder() == juce::String ("A A B C C"));
    CHECK (tracks.getSelectedNames() == juce::String ("B"));
    CHECK_FALSE (tracks.selection.isSelected (copies.front()));

    // With one, the copies are selected instead.
    copies = phazite::duplicateTracks (song, { tracks["B"] }, &tracks.selection);
    REQUIRE (copies.size() == 1);
    CHECK (tracks.getOrder() == juce::String ("A A B B C C"));
    CHECK (phazite::getSelectedTracks (tracks.selection).size() == 1);
    CHECK (phazite::getSelectedTracks (tracks.selection).front() == copies.front());

    phazite::deleteTracks (song, { copies.front() }, nullptr);
    CHECK (tracks.getOrder() == juce::String ("A A B C C"));

    // A folder goes with its tracks; with a selection, the track that takes the first one's place is selected.
    const auto folder = phazite::groupIntoFolder (song, { tracks["B"] }, nullptr);
    REQUIRE (folder.isValid());
    CHECK (tracks.getOrder() == juce::String ("A A Folder B C C"));
    phazite::deleteTracks (song, { folder, tracks["B"] }, &tracks.selection);
    CHECK (tracks.getOrder() == juce::String ("A A C C"));
    CHECK (tracks.getSelectedNames() == juce::String ("C"));

    phazite::deleteTracks (song, {}, &tracks.selection); // nothing to delete
    CHECK (tracks.getOrder() == juce::String ("A A C C"));
}

TEST_CASE ("A duplicated track's clips are its own, the song's, and the original keeps its")
{
    Tracks tracks (1);
    auto& context = tracks.context;
    const auto original = tracks["A"];
    const auto clip = phazite::addMidiClipAt (context, original, phazite::BeatPosition());
    REQUIRE (clip.isValid());
    phazite::addNote (context, clip, 60, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (1.0), 100);

    const auto copies = phazite::duplicateTracks (*tracks.song, { original }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = copies.front();

    // Each has its clip, the copy's with an ID of its own, which plays its own copy of the note.
    const auto originalClips = phazite::getClips (context, original), copiedClips = phazite::getClips (context, copy);
    REQUIRE (originalClips.size() == 1);
    REQUIRE (copiedClips.size() == 1);
    CHECK (originalClips[0].id == clip);
    CHECK (copiedClips[0].id != clip);
    CHECK (phazite::getNotes (context, { copiedClips[0].id }).size() == 1);
    CHECK (phazite::getNotes (context, { clip }).size() == 1);
}

TEST_CASE ("A duplicated track's devices and sends are its own, the song's, and the original keeps its")
{
    Tracks tracks (1);
    auto& context = tracks.context;
    const auto original = tracks["A"];

    for (const auto& type : phazite::getDeviceTypes (context))
        if (type.description.fileOrIdentifier == juce::String ("phaziteEq"))
            phazite::addEffect (context, original, type);

    phazite::addSendToNewReturn (context, original);
    const auto copies = phazite::duplicateTracks (*tracks.song, { original }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = copies.front();

    // Each has its effect and its send, the copy's with IDs of their own.
    const auto originalEffects = phazite::getEffects (context, original), copiedEffects = phazite::getEffects (context, copy);
    const auto originalSends = phazite::getSends (context, original), copiedSends = phazite::getSends (context, copy);
    REQUIRE (originalEffects.size() == 1);
    REQUIRE (copiedEffects.size() == 1);
    REQUIRE (originalSends.size() == 1);
    REQUIRE (copiedSends.size() == 1);
    CHECK (copiedEffects[0].id != originalEffects[0].id);
    CHECK (copiedSends[0].id != originalSends[0].id);
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
    CHECK (selected.front() == tracks["B"]);

    // Without a song, there are none.
    tracks.selection.deselectAll();
    tracks.selection.setSong (nullptr);
    CHECK (phazite::getSelectedTracksOrAll (tracks.selection).empty());
    tracks.selection.setSong (tracks.song.get());
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
    auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song)[0];
    song->getUndoManager().clearUndoHistory();

    phazite::renameTrack (*song, track, "  Bass ");
    CHECK (phazite::getTrackName (*song, track) == juce::String ("Bass"));
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Rename Track"));

    song->getUndoManager().clearUndoHistory();
    phazite::renameTrack (*song, track, "Bass");
    CHECK_FALSE (song->getUndoManager().canUndo());

    phazite::renameTrack (*song, track, "");
    CHECK (phazite::getTrackName (*song, track) != juce::String ("Bass"));
    CHECK (phazite::getTrackName (*song, track).isNotEmpty());
}

TEST_CASE ("Adding a track at the end, coloring tracks and moving them are undo steps of their own")
{
    Tracks t (3);
    auto& song = *t.song;

    const auto added = phazite::addTrackAtEnd (song, false, &t.selection);
    REQUIRE (added.isValid());
    CHECK (phazite::getTrackIDs (song).back() == added);
    CHECK (t.selection.isSelected (added));
    CHECK (t.getUndoName() == juce::String ("Add Track"));
    phazite::setTrackName (song, added, "D");

    const auto folder = phazite::addTrackAtEnd (song, true, nullptr);
    REQUIRE (folder.isValid());
    CHECK (phazite::isFolder (song, folder));
    CHECK (t.getUndoName() == juce::String ("Add Folder"));
    phazite::setTrackName (song, folder, "F");

    phazite::setTrackColours (song, { t["A"], t["B"] }, juce::Colours::red);
    CHECK (t.getOwnColour (t["A"]) == juce::Colours::red);
    CHECK (t.getOwnColour (t["B"]) == juce::Colours::red);
    CHECK (t.getUndoName() == juce::String ("Change Track Colors"));
    phazite::setTrackColours (song, { t["C"] }, juce::Colours::red);
    CHECK (t.getUndoName() == juce::String ("Change Track Color"));

    // To the start, in the order given; then after another track.
    phazite::moveTracksAfter (song, { t["C"], t["A"] }, {});
    CHECK (t.getOrder() == juce::String ("C A B D F"));
    CHECK (t.getUndoName() == juce::String ("Move Tracks"));
    phazite::moveTracksAfter (song, { t["C"] }, t["D"]);
    CHECK (t.getOrder() == juce::String ("A B D C F"));
    CHECK (t.getUndoName() == juce::String ("Move Track"));

    phazite::undo (song);
    CHECK (t.getOrder() == juce::String ("C A B D F"));
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist as the UI reaches it, by IDs and plain values (src/model/arrangement/PlaylistModel.h), and the time range
// selected across tracks by IDs (src/model/arrangement/SongRange.h).
#include "TestClips.h"
#include "TestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongRange.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

namespace
{
phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

phazite::BeatDuration beats (double length)
{
    return phazite::BeatDuration::fromBeats (length);
}

// A clip as the playlist is told of it, made in the order of its ID.
phazite::ClipInfo clipAt (std::uint64_t id, double start, double length, phazite::ClipType kind = phazite::ClipType::midi)
{
    phazite::ClipInfo clip;
    clip.id = phazite::ClipID::fromRaw (id);
    clip.kind = kind;
    clip.range = { beat (start), beats (length) };
    return clip;
}

// An open project at 120 BPM (a beat is half a second) with tracks A and B, A with a MIDI clip from beat 4 to 8 that
// holds two notes.
struct Song
{
    Song()
    {
        const auto tracks = phazite::ensureTracks (*song, 2);
        a = tracks[0];
        b = tracks[1];
        phazite::setTrackName (*song, a, "A");
        phazite::setTrackName (*song, b, "B");

        clip = phazite::insertMidiClip (*song, a, { beat (4.0), beat (8.0) }, nullptr);
        REQUIRE (clip.isValid());
        phazite::test::addMidiNote (*song, clip, 60, beat (0.0), beats (1.0), 100);
        phazite::test::addMidiNote (*song, clip, 64, beat (2.0), beats (1.0), 100);
        setSong (context, song.get());
    }

    ~Song() { setSong (context, nullptr); }

    phazite::Song& getSong() const { return *song; }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::TrackID a, b;
    phazite::ClipID clip;
};
} // namespace

TEST_CASE ("The playlist's rows are its tracks top to bottom, without those in collapsed folders")
{
    Song song;
    auto& context = song.context;
    const auto folder = phazite::groupIntoFolder (song.getSong(), { song.b }, nullptr);
    REQUIRE (folder.isValid());

    auto rows = phazite::getPlaylistRows (context);
    REQUIRE (rows.size() == 3);
    CHECK (rows[0].id == song.a);
    CHECK (rows[0].name == juce::String ("A"));
    CHECK (rows[0].kind == phazite::TrackType::track);
    CHECK (rows[0].colour == phazite::getTrackColour (song.getSong(), song.a, 0));
    CHECK (rows[0].depth == 0);
    CHECK (rows[1].id == folder);
    CHECK (rows[1].kind == phazite::TrackType::folder);
    CHECK (rows[2].id == song.b);
    CHECK (rows[2].depth == 1);
    CHECK (phazite::isInside (context, song.b, folder));
    CHECK_FALSE (phazite::isInside (context, song.a, folder));
    CHECK_FALSE (phazite::isInside (context, folder, folder));
    CHECK (phazite::canHoldClips (context, song.a));
    CHECK_FALSE (phazite::canHoldClips (context, folder));

    // A collapsed folder hides its tracks from the rows; they are still the playlist's.
    phazite::setCollapsed (context, folder, true);
    rows = phazite::getPlaylistRows (context);
    REQUIRE (rows.size() == 2);
    CHECK (rows[1].collapsed);
    CHECK (phazite::getAllPlaylistTracks (context) == std::vector { song.a, folder, song.b });
    REQUIRE (phazite::getTrackRow (context, song.b).has_value());
    CHECK (phazite::getTrackRow (context, song.b)->depth == 1);

    // Heights are view state: no undo steps.
    song.getSong().getUndoManager().clearUndoHistory();
    CHECK_FALSE (phazite::getTrackRow (context, song.a)->savedHeight.has_value());
    phazite::setSavedTrackHeight (context, song.a, 120);
    CHECK (phazite::getTrackRow (context, song.a)->savedHeight.value_or (0) == 120);
    CHECK_FALSE (song.getSong().getUndoManager().canUndo());

    // A track with an instrument takes a double-click's MIDI clip.
    CHECK_FALSE (phazite::getTrackRow (context, song.a)->hasInstrument);
    phazite::chooseInstrument (context, song.a, song.find ("phaziteSynth"));
    CHECK (phazite::getTrackRow (context, song.a)->hasInstrument);

    // Nothing for what is gone, or without a project.
    const auto gone = song.b;
    phazite::deleteTracks (song.getSong(), { song.b }, nullptr);
    CHECK_FALSE (phazite::getTrackRow (context, gone).has_value());
    CHECK (phazite::getClips (context, gone).empty());
    setSong (context, nullptr);
    CHECK (phazite::getPlaylistRows (context).empty());
    CHECK (phazite::getAllPlaylistTracks (context).empty());
}

TEST_CASE ("A folder's row sums up its tracks' clips, a lane for each track that holds clips, in its color")
{
    Song song;
    auto& context = song.context;
    const auto folder = phazite::groupIntoFolder (song.getSong(), { song.a, song.b }, nullptr);
    REQUIRE (folder.isValid());

    // A's clip, on the first lane; B has none, but holds clips, so it has the second lane.
    auto summary = phazite::getFolderSummary (context, folder);
    CHECK (summary.lanes == 2);
    REQUIRE (summary.clips.size() == 1);
    CHECK (summary.clips[0].range == phazite::BeatRange { beat (4.0), beats (4.0) });
    CHECK (summary.clips[0].lane == 0);
    CHECK (summary.clips[0].colour == phazite::getTrackRow (context, song.a)->colour);
    CHECK_FALSE (summary.clips[0].muted);

    // B's clip on the second lane, and a muted clip fainter.
    const auto second = phazite::addMidiClipAt (context, song.b, beat (8.0));
    REQUIRE (second.isValid());
    phazite::toggleClipsMuted (context, { second });
    summary = phazite::getFolderSummary (context, folder);
    REQUIRE (summary.clips.size() == 2);
    CHECK (summary.clips[1].lane == 1);
    CHECK (summary.clips[1].muted);

    // Collapsed, it still sums them up; a track, or what is gone, has nothing to sum up.
    phazite::setCollapsed (context, folder, true);
    CHECK (phazite::getFolderSummary (context, folder).clips.size() == 2);
    CHECK (phazite::getFolderSummary (context, song.a).lanes == 0);
    CHECK (phazite::getFolderSummary (context, {}).clips.empty());
}

TEST_CASE ("Tracks by ID are renamed, colored, moved and added, each as one undo step")
{
    Song song;
    auto& context = song.context;
    auto& undo = song.getSong().getUndoManager();
    undo.clearUndoHistory();

    phazite::renameTrack (context, song.a, "Lead");
    CHECK (phazite::getTrackName (song.getSong(), song.a) == juce::String ("Lead"));
    CHECK (undo.getUndoDescription() == juce::String ("Rename Track"));

    phazite::setTrackColours (context, { song.a, song.b }, juce::Colours::red);
    CHECK (phazite::getPlaylistRows (context)[1].colour == juce::Colours::red);
    CHECK (undo.getUndoDescription() == juce::String ("Change Track Colors"));

    phazite::moveTracksTo (context, { song.a }, song.b, phazite::TrackDrop::after);
    CHECK (phazite::getAllPlaylistTracks (context) == std::vector { song.b, song.a });
    phazite::moveTracksTo (context, { song.a }, song.b, phazite::TrackDrop::before);
    CHECK (phazite::getAllPlaylistTracks (context) == std::vector { song.a, song.b });
    CHECK (undo.getUndoDescription() == juce::String ("Move Track"));

    const auto added = phazite::addTrackAtEnd (context, true);
    REQUIRE (added.isValid());
    CHECK (phazite::getPlaylistRows (context).back().id == added);
    CHECK (phazite::getPlaylistRows (context).back().kind == phazite::TrackType::folder);
    CHECK (context.getSongSelection().isSelected (added));
    CHECK (undo.getUndoDescription() == juce::String ("Add Folder"));
}

TEST_CASE ("Dragged tracks go before or after a track, into a folder after its last, and never a folder into itself")
{
    Song song;
    auto& context = song.context;
    const auto tracks = phazite::ensureTracks (song.getSong(), 4);
    const auto c = tracks[2], d = tracks[3];
    const auto a = song.a, b = song.b;
    const auto folder = phazite::groupIntoFolder (song.getSong(), { c }, nullptr); // A, B, Folder (C), D
    REQUIRE (folder.isValid());
    const auto order = [&context] { return phazite::getAllPlaylistTracks (context); };
    REQUIRE (order() == std::vector { a, b, folder, c, d });

    // Into the folder, after its last track; and before a track in it, which puts it in the folder too.
    phazite::moveTracksTo (context, { a }, folder, phazite::TrackDrop::into);
    CHECK (order() == std::vector { b, folder, c, a, d });
    CHECK (phazite::isInside (context, a, folder));
    phazite::moveTracksTo (context, { d }, c, phazite::TrackDrop::before);
    CHECK (order() == std::vector { b, folder, d, c, a });
    CHECK (phazite::getTrackRow (context, d)->depth == 1);

    // After the folder: out of it, past its tracks. Several go in their order in the playlist, whatever order they are given
    // in, and a track next to the place that moves itself counts as not there.
    phazite::moveTracksTo (context, { a, d }, folder, phazite::TrackDrop::after);
    CHECK (order() == std::vector { b, folder, c, d, a });
    CHECK_FALSE (phazite::isInside (context, d, folder));
    phazite::moveTracksTo (context, { b, d }, d, phazite::TrackDrop::after);
    CHECK (order() == std::vector { folder, c, b, d, a });

    // A folder takes its tracks along, also when they are given too; it can't go into itself or into a track of its own.
    CHECK_FALSE (phazite::canMoveTracksTo (context, { folder }, folder, phazite::TrackDrop::into));
    CHECK_FALSE (phazite::canMoveTracksTo (context, { folder }, c, phazite::TrackDrop::after));
    CHECK_FALSE (phazite::canMoveTracksTo (context, { a }, b, phazite::TrackDrop::into)); // not a folder
    CHECK (phazite::canMoveTracksTo (context, { folder, c }, a, phazite::TrackDrop::after));
    phazite::moveTracksTo (context, { folder, c }, a, phazite::TrackDrop::after);
    CHECK (order() == std::vector { b, d, a, folder, c });
    CHECK (phazite::isInside (context, c, folder));

    // Each move is one undo step.
    phazite::undo (song.getSong());
    CHECK (order() == std::vector { folder, c, b, d, a });
}

TEST_CASE ("Each row knows the colors of the folders it is in, outermost first")
{
    Song song;
    auto& context = song.context;
    const auto inner = phazite::groupIntoFolder (song.getSong(), { song.b }, nullptr);
    REQUIRE (inner.isValid());
    const auto outer = phazite::groupIntoFolder (song.getSong(), { inner }, nullptr);
    REQUIRE (outer.isValid());
    phazite::setTrackColours (context, { outer }, juce::Colours::red);
    phazite::setTrackColours (context, { inner }, juce::Colours::blue);

    const auto rows = phazite::getPlaylistRows (context); // A, outer, inner, B
    REQUIRE (rows.size() == 4);
    CHECK (rows[0].folderColours.empty());
    CHECK (rows[1].folderColours.empty());
    CHECK (rows[2].folderColours == std::vector { juce::Colours::red });
    CHECK (rows[3].folderColours == std::vector { juce::Colours::red, juce::Colours::blue });
    CHECK (phazite::getTrackRow (context, song.b)->folderColours == rows[3].folderColours);
}

TEST_CASE ("Collapse All collapses every row and lane while any is expanded, and expands them all otherwise, without undo steps")
{
    Song song;
    auto& context = song.context;
    const auto folder = phazite::groupIntoFolder (song.getSong(), { song.b }, nullptr);
    REQUIRE (folder.isValid());
    phazite::showAutomationLane (context, phazite::getVolume (context, song.a).getRef());
    song.getSong().getUndoManager().clearUndoHistory();
    REQUIRE (phazite::isAnyRowExpanded (context));

    phazite::toggleAllRowsCollapsed (song.getSong());
    CHECK_FALSE (phazite::isAnyRowExpanded (context));
    CHECK (phazite::getTrackRow (context, song.a)->collapsed);
    CHECK (phazite::getTrackRow (context, folder)->collapsed);
    CHECK (phazite::getTrackRow (context, song.b)->collapsed); // hidden in its folder, collapsed too
    CHECK (phazite::getAutomationLanes (context, song.a)[0].collapsed);

    // A lane expanded again counts: the next Collapse All collapses it.
    phazite::setAutomationLaneCollapsed (context, phazite::getVolume (context, song.a).getRef(), false);
    CHECK (phazite::isAnyRowExpanded (context));
    phazite::toggleAllRowsCollapsed (song.getSong());
    CHECK_FALSE (phazite::isAnyRowExpanded (context));

    phazite::toggleAllRowsCollapsed (song.getSong());
    CHECK_FALSE (phazite::getTrackRow (context, song.a)->collapsed);
    CHECK_FALSE (phazite::getTrackRow (context, folder)->collapsed);
    CHECK_FALSE (phazite::getAutomationLanes (context, song.a)[0].collapsed);
    CHECK_FALSE (song.getSong().getUndoManager().canUndo());
}

TEST_CASE ("Clips by ID: what the playlist draws of them, and the tools' edits")
{
    Song song;
    auto& context = song.context;

    const auto clips = phazite::getClips (context, song.a);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].id == song.clip);
    CHECK (clips[0].track == song.a);
    CHECK (clips[0].kind == phazite::ClipType::midi);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK_FALSE (clips[0].muted);
    CHECK_FALSE (clips[0].linked);

    // The notes, in beats from the clip's start.
    const auto notes = phazite::getNoteShapes (context, song.clip);
    REQUIRE (notes.size() == 2);
    CHECK (notes[1].pitch == 64);
    CHECK (notes[1].start == doctest::Approx (2.0));
    CHECK (notes[1].end == doctest::Approx (3.0));

    // Moved to the other track, trimmed, split, copied as an alias, muted, nudged and deleted.
    phazite::moveClip (context, song.clip, beat (8.0), song.b);
    CHECK (phazite::getClip (context, song.clip)->track == song.b);
    phazite::trimClipEnd (context, song.clip, beat (14.0));
    CHECK (phazite::getClip (context, song.clip)->range.getEnd().inBeats() == doctest::Approx (14.0));
    phazite::trimClipStart (context, song.clip, beat (9.0));
    CHECK (phazite::getClip (context, song.clip)->range.getStart().inBeats() == doctest::Approx (9.0));
    CHECK (phazite::getNoteShapes (context, song.clip)[0].start == doctest::Approx (-1.0)); // the notes stay where they were

    const auto halves = phazite::splitClips (context, { song.clip }, beat (12.0));
    REQUIRE (halves.size() == 1);
    CHECK (phazite::getClip (context, halves[0])->range.getStart().inBeats() == doctest::Approx (12.0));
    CHECK (phazite::getClips (context, song.b).size() == 2);

    const auto aliases = phazite::copyClips (context, { song.clip }, true);
    REQUIRE (aliases.size() == 1);
    CHECK (phazite::getClip (context, aliases[0])->linked);
    CHECK (phazite::getClip (context, song.clip)->linked);

    phazite::toggleClipsMuted (context, { song.clip });
    CHECK (phazite::getClip (context, song.clip)->muted);
    phazite::nudgeClips (context, { song.clip }, beats (1.0));
    CHECK (phazite::getClip (context, song.clip)->range.getStart().inBeats() == doctest::Approx (10.0));

    phazite::deleteClips (context, { song.clip }, false);
    CHECK_FALSE (phazite::getClip (context, song.clip).has_value());
    CHECK (phazite::getNoteShapes (context, song.clip).empty());

    // Draw and Paint place a copy of the last clip used.
    const auto placed = phazite::placeClip (context, song.a, beat (16.0), halves[0]);
    REQUIRE (placed.isValid());
    CHECK (phazite::getClip (context, placed)->track == song.a);
    CHECK (phazite::getClip (context, placed)->range.getStart().inBeats() == doctest::Approx (16.0));

    // A double-click adds an empty MIDI clip, selected.
    const auto added = phazite::addMidiClipAt (context, song.a, beat (24.0));
    REQUIRE (added.isValid());
    CHECK (phazite::getClip (context, added)->range.getStart().inBeats() == doctest::Approx (24.0));
    CHECK (context.getSongSelection().isSelected (added));
}

TEST_CASE ("A clip is named like its track when its name is the track's but for capitals and separators and a tempo or key")
{
    Song song;
    auto& context = song.context;

    // A new MIDI clip is named after its track, whose header says it: the playlist shows no tag with it.
    auto info = phazite::getClip (context, song.clip);
    REQUIRE (info.has_value());
    CHECK (info->name == juce::String ("A"));
    CHECK (info->namedAfterTrack);
    CHECK (info->namedLikeTrack);

    // Names of their own, as samples' files give them (2026-11-07, the owner: the track header said "Shaker" while the clips
    // on it said "Shaker 85bmp"): alike where the track's header says what they do, but for a tempo or key it doesn't give.
    struct Names
    {
        const char *track, *clip;
        bool alike;
    };

    // clang-format off
    const Names names[] = {
        { "Shaker",        "Shaker 82 BPM",    true  },
        { "Shaker",        "shaker_85bpm",     true  }, // whatever the capitals and what is between the words
        { "Shaker Loop",   "ShakerLoop",       true  },
        { "Shaker 82 BPM", "Shaker",           true  }, // the track's name gives the tempo
        { "Bass",          "Bass Am",          true  }, // a key
        { "Pad",           "Pad C#min 120bpm", true  },
        { "Kick",          "Kick (F)",         true  },
        { "Vox Am",        "Vox Am 90 BPM",    true  }, // the same key in both
        { "Shaker",        "Shaker 2",         false }, // a word more: which of the shakers
        { "Shaker",        "Shaker Loop",      false },
        { "808",           "808 Long F",       false },
        { "Vox Am",        "Vox Em",           false }, // another key
        { "Shaker 120bpm", "Shaker 85bpm",     false }, // another tempo
        { "120 BPM",       "128 BPM",          false }, // a tempo and nothing else
        { "Bass",          "Am",               false },
    };
    // clang-format on

    for (const auto& each : names)
    {
        CAPTURE (each.track);
        CAPTURE (each.clip);
        phazite::renameTrack (context, song.a, each.track);
        phazite::renameClips (context, { song.clip }, each.clip);
        info = phazite::getClip (context, song.clip);
        REQUIRE (info.has_value());
        CHECK (info->name == juce::String (each.clip));
        CHECK_FALSE (info->namedAfterTrack); // a name of its own, which the inspector's Name shows
        CHECK (info->namedLikeTrack == each.alike);
    }
}

TEST_CASE ("Audio clips by ID are faded, crossfaded and slipped")
{
    Song song;
    auto& context = song.context;
    phazite::test::TemporaryFolder temp;
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 4.0); // 8 beats
    CHECK (phazite::canImport (context, file));
    CHECK_FALSE (phazite::canImport (context, temp.folder.getChildFile ("Notes.txt")));

    const auto imported = phazite::importFiles (context, { file.getFullPathName() }, song.b, beat (0.0));
    REQUIRE (imported.size() == 1);
    const auto first = imported[0];
    REQUIRE (phazite::getClip (context, first).has_value());
    CHECK (phazite::getClip (context, first)->kind == phazite::ClipType::audio);
    CHECK (phazite::getClip (context, first)->track == song.b);
    CHECK (phazite::getClip (context, first)->range.getEnd().inBeats() == doctest::Approx (8.0));

    // A fade in to a quarter of the clip, as a part of its length.
    phazite::fadeClipTo (context, first, true, beat (2.0));
    CHECK (phazite::getClip (context, first)->fadeIn == doctest::Approx (0.25));
    CHECK (phazite::getFadeGain (phazite::linearFade, 0.5f) == doctest::Approx (0.5f));
    CHECK (phazite::getFadeGain (phazite::convexFade, 0.5f) == doctest::Approx (std::sqrt (0.5f)));

    // Split in two, the halves meet in the middle, where a drag a beat to the right crossfades them over two beats.
    const auto halves = phazite::splitClips (context, { first }, beat (4.0));
    REQUIRE (halves.size() == 1);
    const auto second = halves[0];
    CHECK (phazite::getCrossfadeSpots (context, song.a, beats (0.01)).empty());
    const auto spots = phazite::getCrossfadeSpots (context, song.b, beats (0.01));
    REQUIRE (spots.size() == 1);
    CHECK (spots[0].first == first);
    CHECK (spots[0].second == second);
    CHECK (spots[0].boundary.inBeats() == doctest::Approx (4.0));

    phazite::setCrossfade (context, spots[0], beat (5.0));
    const auto left = phazite::getClip (context, first), right = phazite::getClip (context, second);
    CHECK (left->range.getEnd().inBeats() == doctest::Approx (5.0));
    CHECK (right->range.getStart().inBeats() == doctest::Approx (3.0));
    CHECK (left->fadeOut == doctest::Approx (0.4)); // 2 of its 5 beats
    CHECK (right->fadeIn == doctest::Approx (0.4));
    CHECK (phazite::getClipSubRows (phazite::getClips (context, song.b)) == std::vector<phazite::ClipSubRow> (2)); // the whole row

    // Slipped: what plays inside moves a beat earlier, and the clip stays where it is.
    CHECK (phazite::getSlipOffset (context, first) == doctest::Approx (0.0));
    phazite::slipClip (context, first, 0.0, beat (1.0), beat (0.0));
    CHECK (phazite::getSlipOffset (context, first) == doctest::Approx (0.5)); // seconds
    CHECK (phazite::getClip (context, first)->range.getStart().inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("Clips that overlap on a track take sub-rows of its row, those made first above, and crossfaded clips share one")
{
    using phazite::ClipSubRow;
    const auto subRowsOf = [] (const std::vector<phazite::ClipInfo>& clips) { return phazite::getClipSubRows (clips); };

    // A two-bar sample every bar, as FL Studio users stagger them on rows: every other one in the lower sub-row. The one
    // after them only touches the last, so it has the whole row.
    CHECK (subRowsOf ({ clipAt (1, 0.0, 8.0), clipAt (2, 4.0, 8.0), clipAt (3, 8.0, 8.0), clipAt (4, 12.0, 8.0), clipAt (5, 20.0, 4.0) })
           == std::vector<ClipSubRow> { { 0, 2 }, { 1, 2 }, { 0, 2 }, { 1, 2 }, { 0, 1 } });

    // A loop whose length from its audio rounds a little past its beats only touches a copy placed on its end.
    CHECK (subRowsOf ({ clipAt (1, 0.0, 4.0 + 1.0e-9), clipAt (2, 4.0, 4.0) }) == std::vector<ClipSubRow> (2));

    // The one made first is above, wherever it is: a newer clip dragged over it never moves it.
    CHECK (subRowsOf ({ clipAt (2, 0.0, 6.0), clipAt (1, 4.0, 4.0) }) == std::vector<ClipSubRow> { { 1, 2 }, { 0, 2 } });

    // One over two in different sub-rows takes a third, and the clips it joins share the row out in three.
    CHECK (subRowsOf ({ clipAt (1, 0.0, 4.0), clipAt (2, 2.0, 4.0), clipAt (3, 3.0, 2.0), clipAt (4, 8.0, 4.0) })
           == std::vector<ClipSubRow> { { 0, 3 }, { 1, 3 }, { 2, 3 }, { 0, 1 } });

    // Two audio clips that overlap only where the first fades out and the second fades in crossfade: they share the row.
    // With a fade shorter than the overlap, each plays whole over the other for a while, so they don't.
    auto first = clipAt (1, 0.0, 5.0, phazite::ClipType::audio), second = clipAt (2, 3.0, 5.0, phazite::ClipType::audio);
    first.fadeOut = 0.4; // 2 of its 5 beats
    second.fadeIn = 0.4;
    CHECK (subRowsOf ({ first, second }) == std::vector<ClipSubRow> { { 0, 1 }, { 0, 1 } });
    second.fadeIn = 0.2;
    CHECK (subRowsOf ({ first, second }) == std::vector<ClipSubRow> { { 0, 2 }, { 1, 2 } });

    // MIDI clips have no fades: overlapping, they take sub-rows.
    CHECK (subRowsOf ({ clipAt (1, 0.0, 5.0), clipAt (2, 3.0, 5.0) }) == std::vector<ClipSubRow> { { 0, 2 }, { 1, 2 } });
    CHECK (subRowsOf ({}).empty());
}

TEST_CASE ("The playlist's sections, play start and time line by plain values")
{
    Song song;
    auto& context = song.context;
    CHECK (phazite::getSongSections (context).empty());
    CHECK (phazite::getTimelineEnd (context).inBeats() == doctest::Approx (8.0)); // the clip's end

    REQUIRE (phazite::insertSection (song.getSong(), {}, { .name = "Intro", .length = phazite::BeatDuration::fromBeats (32.0) }).isValid());
    const auto sections = phazite::getSongSections (context);
    REQUIRE (sections.size() == 1);
    CHECK (sections[0].name == juce::String ("Intro"));
    CHECK (sections[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (sections[0].range.getEnd().inBeats() == doctest::Approx (32.0));
    CHECK (sections[0].colour == phazite::getRoleColour ({}));                     // without a role or a color of its own
    CHECK (phazite::getTimelineEnd (context).inBeats() == doctest::Approx (32.0)); // sections count, unlike for the song's end

    phazite::movePlayhead (song.getSong(), beat (6.0));
    CHECK (phazite::getPlayStartBeat (context).inBeats() == doctest::Approx (6.0));
}

TEST_CASE ("Sections by ID are added, renamed, colored, moved, copied and resized, each as one undo step")
{
    Song song;
    auto& context = song.context;
    auto& undo = song.getSong().getUndoManager();
    const auto startsOf = [&context]
    {
        std::vector<double> starts;

        for (const auto& section : phazite::getSongSections (context))
            starts.push_back (std::round (section.range.getStart().inBeats() * 1000.0) / 1000.0); // to the thousandth of a beat

        return starts;
    };

    // Each eight bars from the start of the bar asked for, selected alone.
    const auto first = phazite::addSectionAt (context, beat (1.0));
    REQUIRE (first.isValid());
    CHECK (context.getSongSelection().isSelected (first));
    CHECK (undo.getUndoDescription() == juce::String ("Add Section"));
    const auto second = phazite::addSectionAt (context, beat (33.0));
    REQUIRE (second.isValid());
    CHECK_FALSE (context.getSongSelection().isSelected (first));
    REQUIRE (phazite::getSongSection (context, second).has_value());
    CHECK (phazite::getSongSection (context, second)->range.getStart().inBeats() == doctest::Approx (32.0));
    CHECK (phazite::getSongSection (context, second)->range.getEnd().inBeats() == doctest::Approx (64.0));
    CHECK (phazite::getSongSection (context, second)->energy >= 0.0f);

    const auto boundaries = phazite::getSectionBoundaries (context);
    REQUIRE (boundaries.size() == 3);
    CHECK (boundaries[2].inBeats() == doctest::Approx (64.0));

    phazite::renameSection (context, first, "Intro");
    CHECK (phazite::getSongSection (context, first)->name == juce::String ("Intro"));
    CHECK (undo.getUndoDescription() == juce::String ("Rename Section"));
    phazite::setSectionColour (context, first, juce::Colours::red);
    CHECK (phazite::getSongSection (context, first)->colour == juce::Colours::red);
    CHECK (undo.getUndoDescription() == juce::String ("Change Section Color"));

    // Moved after the second, with the clip in it; the second makes room.
    phazite::moveSection (context, first, beat (64.0));
    CHECK (undo.getUndoDescription() == juce::String ("Move Section"));
    auto sections = phazite::getSongSections (context);
    REQUIRE (sections.size() == 2);
    CHECK (sections[1].name == juce::String ("Intro"));
    CHECK (startsOf() == std::vector { 0.0, 32.0 });
    REQUIRE (phazite::getClips (context, song.a).size() == 1);
    CHECK (phazite::getClips (context, song.a)[0].range.getStart().inBeats() == doctest::Approx (36.0));

    // Copied to the start as aliases: the sections after it make room, and its clip is linked to the original.
    const auto copy = phazite::copySection (context, sections[1].id, beat (0.0), true);
    REQUIRE (copy.isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Copy Section as Aliases"));
    CHECK (phazite::getSongSection (context, copy)->name == juce::String ("Intro"));
    CHECK (startsOf() == std::vector { 0.0, 32.0, 64.0 });
    const auto clips = phazite::getClips (context, song.a);
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (clips[0].linked);

    // Shorter, with the rest of the song moving back, and then alone, leaving a gap.
    phazite::resizeSection (context, copy, beats (16.0), true);
    CHECK (undo.getUndoDescription() == juce::String ("Resize Section"));
    CHECK (phazite::getSongSection (context, copy)->range.getEnd().inBeats() == doctest::Approx (16.0));
    CHECK (startsOf() == std::vector { 0.0, 16.0, 48.0 });
    phazite::resizeSection (context, copy, beats (8.0), false);
    CHECK (phazite::getSongSection (context, copy)->range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (startsOf() == std::vector { 0.0, 16.0, 48.0 });

    // Nothing for a section that is gone.
    CHECK_FALSE (phazite::getSongSection (context, {}).has_value());
    CHECK_FALSE (phazite::copySection (context, {}, beat (0.0), false).isValid());
}

TEST_CASE ("The time range selected across tracks, by IDs")
{
    Song song;
    auto& range = song.context.getSongRange();
    CHECK (range.isEmpty());

    range.set ({ beat (2.0), beat (6.0) }, { song.a, song.b });
    CHECK_FALSE (range.isEmpty());
    CHECK (range.getRange().getStart().inBeats() == doctest::Approx (2.0));
    CHECK (range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (range.getTracks() == std::vector { song.a, song.b });
    CHECK (getRangeSelection (song.context).getTracks().size() == 2); // what the model's commands act on
    CHECK (range.isSource (&getRangeSelection (song.context)));
    CHECK_FALSE (range.isSource (nullptr));

    // Tracks that are gone are left out, and with none left it is empty.
    const auto gone = song.b;
    phazite::deleteTracks (song.getSong(), { song.b }, nullptr);
    CHECK (range.getTracks() == std::vector { song.a });
    range.set ({ beat (0.0), beat (1.0) }, { gone });
    CHECK (range.isEmpty());

    range.set ({ beat (0.0), beat (1.0) }, { song.a });
    range.clear();
    CHECK (range.isEmpty());
}

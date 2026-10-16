// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist as the UI reaches it, by IDs and plain values (src/model/arrangement/PlaylistModel.h), and the time range
// selected across tracks by IDs (src/model/arrangement/SongRange.h).
#include "TestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/SongRange.h"
#include "model/arrangement/Tracks.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "model/transport/Playback.h"

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

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

// An open project at 120 BPM (a beat is half a second) with tracks A and B, A with a MIDI clip from beat 4 to 8 that
// holds two notes.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");

        auto* midi = phazite::insertMidiClip (*a, { te::BeatPosition::fromBeats (4.0), te::BeatPosition::fromBeats (8.0) }, nullptr);
        REQUIRE (midi != nullptr);
        midi->getSequence().addNote (60, te::BeatPosition(), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);
        midi->getSequence().addNote (64, te::BeatPosition::fromBeats (2.0), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);
        clip = phazite::getClipID (*midi);
        context.setEdit (edit.get());
    }

    ~Song() { context.setEdit (nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngine() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
    phazite::ClipID clip;
};
} // namespace

TEST_CASE ("The playlist's rows are its tracks top to bottom, without those in collapsed folders")
{
    Song song;
    auto& context = song.context;
    auto* folder = phazite::groupIntoFolder ({ song.b }, nullptr);
    REQUIRE (folder != nullptr);

    auto rows = phazite::getPlaylistRows (context);
    REQUIRE (rows.size() == 3);
    CHECK (rows[0].id == idOf (*song.a));
    CHECK (rows[0].name == juce::String ("A"));
    CHECK (rows[0].kind == phazite::TrackType::track);
    CHECK (rows[0].colour == phazite::getTrackColour (*song.a, 0));
    CHECK (rows[0].depth == 0);
    CHECK (rows[1].id == idOf (*folder));
    CHECK (rows[1].kind == phazite::TrackType::folder);
    CHECK (rows[2].id == idOf (*song.b));
    CHECK (rows[2].depth == 1);
    CHECK (phazite::isInside (context, idOf (*song.b), idOf (*folder)));
    CHECK_FALSE (phazite::isInside (context, idOf (*song.a), idOf (*folder)));
    CHECK_FALSE (phazite::isInside (context, idOf (*folder), idOf (*folder)));
    CHECK (phazite::canHoldClips (context, idOf (*song.a)));
    CHECK_FALSE (phazite::canHoldClips (context, idOf (*folder)));

    // A collapsed folder hides its tracks from the rows; they are still the playlist's.
    phazite::setCollapsed (context, idOf (*folder), true);
    rows = phazite::getPlaylistRows (context);
    REQUIRE (rows.size() == 2);
    CHECK (rows[1].collapsed);
    CHECK (phazite::getAllPlaylistTracks (context) == std::vector { idOf (*song.a), idOf (*folder), idOf (*song.b) });
    REQUIRE (phazite::getTrackRow (context, idOf (*song.b)).has_value());
    CHECK (phazite::getTrackRow (context, idOf (*song.b))->depth == 1);

    // Heights are view state: no undo steps.
    song.edit->getUndoManager().clearUndoHistory();
    CHECK_FALSE (phazite::getTrackRow (context, idOf (*song.a))->savedHeight.has_value());
    phazite::setSavedTrackHeight (context, idOf (*song.a), 120);
    CHECK (phazite::getTrackRow (context, idOf (*song.a))->savedHeight.value_or (0) == 120);
    CHECK_FALSE (song.edit->getUndoManager().canUndo());

    // A track with an instrument takes a double-click's MIDI clip.
    CHECK_FALSE (phazite::getTrackRow (context, idOf (*song.a))->hasInstrument);
    phazite::chooseInstrument (context, idOf (*song.a), song.find ("phaziteSynth"));
    CHECK (phazite::getTrackRow (context, idOf (*song.a))->hasInstrument);

    // Nothing for what is gone, or without a project.
    const auto gone = idOf (*song.b);
    phazite::deleteTracks ({ song.b }, nullptr);
    CHECK_FALSE (phazite::getTrackRow (context, gone).has_value());
    CHECK (phazite::getClips (context, gone).empty());
    context.setEdit (nullptr);
    CHECK (phazite::getPlaylistRows (context).empty());
    CHECK (phazite::getAllPlaylistTracks (context).empty());
}

TEST_CASE ("Tracks by ID are renamed, colored, moved and added, each as one undo step")
{
    Song song;
    auto& context = song.context;
    auto& undo = song.edit->getUndoManager();
    undo.clearUndoHistory();

    phazite::renameTrack (context, idOf (*song.a), "Lead");
    CHECK (song.a->getName() == juce::String ("Lead"));
    CHECK (undo.getUndoDescription() == juce::String ("Rename Track"));

    phazite::setTrackColours (context, { idOf (*song.a), idOf (*song.b) }, juce::Colours::red);
    CHECK (phazite::getPlaylistRows (context)[1].colour == juce::Colours::red);
    CHECK (undo.getUndoDescription() == juce::String ("Change Track Colors"));

    phazite::moveTracksAfter (context, { idOf (*song.a) }, idOf (*song.b));
    CHECK (phazite::getAllPlaylistTracks (context) == std::vector { idOf (*song.b), idOf (*song.a) });
    phazite::moveTracksAfter (context, { idOf (*song.a) }, {}); // to the start
    CHECK (phazite::getAllPlaylistTracks (context) == std::vector { idOf (*song.a), idOf (*song.b) });
    CHECK (undo.getUndoDescription() == juce::String ("Move Track"));

    const auto added = phazite::addTrackAtEnd (context, true);
    REQUIRE (added.isValid());
    CHECK (phazite::getPlaylistRows (context).back().id == added);
    CHECK (phazite::getPlaylistRows (context).back().kind == phazite::TrackType::folder);
    CHECK (context.getSongSelection().isSelected (added));
    CHECK (undo.getUndoDescription() == juce::String ("Add Folder"));
}

TEST_CASE ("Clips by ID: what the playlist draws of them, and the tools' edits")
{
    Song song;
    auto& context = song.context;

    const auto clips = phazite::getClips (context, idOf (*song.a));
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].id == song.clip);
    CHECK (clips[0].track == idOf (*song.a));
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
    phazite::moveClip (context, song.clip, beat (8.0), idOf (*song.b));
    CHECK (phazite::getClip (context, song.clip)->track == idOf (*song.b));
    phazite::trimClipEnd (context, song.clip, beat (14.0));
    CHECK (phazite::getClip (context, song.clip)->range.getEnd().inBeats() == doctest::Approx (14.0));
    phazite::trimClipStart (context, song.clip, beat (9.0));
    CHECK (phazite::getClip (context, song.clip)->range.getStart().inBeats() == doctest::Approx (9.0));
    CHECK (phazite::getNoteShapes (context, song.clip)[0].start == doctest::Approx (-1.0)); // the notes stay where they were

    const auto halves = phazite::splitClips (context, { song.clip }, beat (12.0));
    REQUIRE (halves.size() == 1);
    CHECK (phazite::getClip (context, halves[0])->range.getStart().inBeats() == doctest::Approx (12.0));
    CHECK (phazite::getClips (context, idOf (*song.b)).size() == 2);

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
    const auto placed = phazite::placeClip (context, idOf (*song.a), beat (16.0), halves[0]);
    REQUIRE (placed.isValid());
    CHECK (phazite::getClip (context, placed)->track == idOf (*song.a));
    CHECK (phazite::getClip (context, placed)->range.getStart().inBeats() == doctest::Approx (16.0));

    // A double-click adds an empty MIDI clip, selected.
    const auto added = phazite::addMidiClipAt (context, idOf (*song.a), beat (24.0));
    REQUIRE (added.isValid());
    CHECK (phazite::getClip (context, added)->range.getStart().inBeats() == doctest::Approx (24.0));
    CHECK (context.getSongSelection().isSelected (added));
}

TEST_CASE ("Audio clips by ID are faded, crossfaded and slipped")
{
    Song song;
    auto& context = song.context;
    phazite::test::TemporaryFolder temp;
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 4.0); // 8 beats
    CHECK (phazite::canImport (context, file));
    CHECK_FALSE (phazite::canImport (context, temp.folder.getChildFile ("Notes.txt")));

    const auto imported = phazite::importFiles (context, { file.getFullPathName() }, idOf (*song.b), beat (0.0));
    REQUIRE (imported.size() == 1);
    const auto first = imported[0];
    REQUIRE (phazite::getClip (context, first).has_value());
    CHECK (phazite::getClip (context, first)->kind == phazite::ClipType::audio);
    CHECK (phazite::getClip (context, first)->track == idOf (*song.b));
    CHECK (phazite::getClip (context, first)->range.getEnd().inBeats() == doctest::Approx (8.0));

    // A fade in to a quarter of the clip, as a part of its length.
    phazite::fadeClipTo (context, first, true, beat (2.0));
    CHECK (phazite::getClip (context, first)->fadeIn == doctest::Approx (0.25));
    CHECK (phazite::getFadeGain (phazite::FadeShape::linear, 0.5f) == doctest::Approx (0.5f));

    // Split in two, the halves meet in the middle, where a drag a beat to the right crossfades them over two beats.
    const auto halves = phazite::splitClips (context, { first }, beat (4.0));
    REQUIRE (halves.size() == 1);
    const auto second = halves[0];
    CHECK (phazite::getCrossfadeSpots (context, idOf (*song.a), beats (0.01)).empty());
    const auto spots = phazite::getCrossfadeSpots (context, idOf (*song.b), beats (0.01));
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

    // Slipped: what plays inside moves a beat earlier, and the clip stays where it is.
    CHECK (phazite::getSlipOffset (context, first) == doctest::Approx (0.0));
    phazite::slipClip (context, first, 0.0, beat (1.0), beat (0.0));
    CHECK (phazite::getSlipOffset (context, first) == doctest::Approx (0.5)); // seconds
    CHECK (phazite::getClip (context, first)->range.getStart().inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("The playlist's sections, play start and time line by plain values")
{
    Song song;
    auto& context = song.context;
    CHECK (phazite::getSongSections (context).empty());
    CHECK (phazite::getTimelineEnd (context).inBeats() == doctest::Approx (8.0)); // the clip's end

    REQUIRE (phazite::insertSection (*song.edit, {}, { .name = "Intro", .length = te::BeatDuration::fromBeats (32.0) }) != nullptr);
    const auto sections = phazite::getSongSections (context);
    REQUIRE (sections.size() == 1);
    CHECK (sections[0].name == juce::String ("Intro"));
    CHECK (sections[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (sections[0].range.getEnd().inBeats() == doctest::Approx (32.0));
    CHECK (sections[0].colour == phazite::getSectionColour (*phazite::getSections (*song.edit).getFirst()));
    CHECK (phazite::getTimelineEnd (context).inBeats() == doctest::Approx (32.0)); // sections count, unlike for the song's end

    phazite::movePlayhead (*song.edit, song.edit->tempoSequence.toTime (te::BeatPosition::fromBeats (6.0)));
    CHECK (phazite::getPlayStartBeat (context).inBeats() == doctest::Approx (6.0));
}

TEST_CASE ("The time range selected across tracks, by IDs")
{
    Song song;
    auto& range = song.context.getSongRange();
    CHECK (range.isEmpty());

    range.set ({ beat (2.0), beat (6.0) }, { idOf (*song.a), idOf (*song.b) });
    CHECK_FALSE (range.isEmpty());
    CHECK (range.getRange().getStart().inBeats() == doctest::Approx (2.0));
    CHECK (range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (range.getTracks() == std::vector { idOf (*song.a), idOf (*song.b) });
    CHECK (song.context.getRangeSelection().getTracks().size() == 2); // what the model's commands act on
    CHECK (range.isSource (&song.context.getRangeSelection()));
    CHECK_FALSE (range.isSource (nullptr));

    // Tracks that are gone are left out, and with none left it is empty.
    const auto gone = idOf (*song.b);
    phazite::deleteTracks ({ song.b }, nullptr);
    CHECK (range.getTracks() == std::vector { idOf (*song.a) });
    range.set ({ beat (0.0), beat (1.0) }, { gone });
    CHECK (range.isEmpty());

    range.set ({ beat (0.0), beat (1.0) }, { idOf (*song.a) });
    range.clear();
    CHECK (range.isEmpty());
}

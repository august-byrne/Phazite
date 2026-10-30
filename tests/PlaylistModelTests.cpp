// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist as the UI reaches it, by IDs and plain values (src/model/arrangement/PlaylistModel.h), and the time range
// selected across tracks by IDs (src/model/arrangement/SongRange.h).
#include "TestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongRange.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
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

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
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
        setEdit (context, edit.get());
    }

    ~Song() { setEdit (context, nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
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
    setEdit (context, nullptr);
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
    CHECK (phazite::getClipSubRows (phazite::getClips (context, idOf (*song.b))) == std::vector<phazite::ClipSubRow> (2)); // the whole row

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

    REQUIRE (phazite::insertSection (*song.edit, {}, { .name = "Intro", .length = phazite::BeatDuration::fromBeats (32.0) }).isValid());
    const auto sections = phazite::getSongSections (context);
    REQUIRE (sections.size() == 1);
    CHECK (sections[0].name == juce::String ("Intro"));
    CHECK (sections[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (sections[0].range.getEnd().inBeats() == doctest::Approx (32.0));
    CHECK (sections[0].colour == phazite::getRoleColour ({}));                     // without a role or a color of its own
    CHECK (phazite::getTimelineEnd (context).inBeats() == doctest::Approx (32.0)); // sections count, unlike for the song's end

    phazite::movePlayhead (*song.edit, song.edit->tempoSequence.toTime (te::BeatPosition::fromBeats (6.0)));
    CHECK (phazite::getPlayStartBeat (context).inBeats() == doctest::Approx (6.0));
}

TEST_CASE ("Sections by ID are added, renamed, colored, moved, copied and resized, each as one undo step")
{
    Song song;
    auto& context = song.context;
    auto& undo = song.edit->getUndoManager();
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
    REQUIRE (phazite::getClips (context, idOf (*song.a)).size() == 1);
    CHECK (phazite::getClips (context, idOf (*song.a))[0].range.getStart().inBeats() == doctest::Approx (36.0));

    // Copied to the start as aliases: the sections after it make room, and its clip is linked to the original.
    const auto copy = phazite::copySection (context, sections[1].id, beat (0.0), true);
    REQUIRE (copy.isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Copy Section as Aliases"));
    CHECK (phazite::getSongSection (context, copy)->name == juce::String ("Intro"));
    CHECK (startsOf() == std::vector { 0.0, 32.0, 64.0 });
    const auto clips = phazite::getClips (context, idOf (*song.a));
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

    range.set ({ beat (2.0), beat (6.0) }, { idOf (*song.a), idOf (*song.b) });
    CHECK_FALSE (range.isEmpty());
    CHECK (range.getRange().getStart().inBeats() == doctest::Approx (2.0));
    CHECK (range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (range.getTracks() == std::vector { idOf (*song.a), idOf (*song.b) });
    CHECK (getRangeSelection (song.context).getTracks().size() == 2); // what the model's commands act on
    CHECK (range.isSource (&getRangeSelection (song.context)));
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

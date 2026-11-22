// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestClips.h"
#include "TestHelpers.h"
#include "UndoCheck.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "ui/timeline/SmartTool.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace
{
using phazite::test::lasting;

phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// A song with two tracks, a selection and the selection commands.
struct ClipEdit
{
    ClipEdit()
    {
        tracks = phazite::ensureTracks (song, 2);
        song.getUndoManager().clearUndoHistory();
        selection.setSong (&song);
        phazite::addSelectionCommands (registry, selection, range);
    }

    ~ClipEdit() { selection.deselectAll(); }

    phazite::ClipID addClip (int track, double start, double length)
    {
        const auto clip = phazite::insertMidiClip (song, tracks[(size_t) track], { beats (start), beats (start + length) }, nullptr);
        REQUIRE (clip.isValid());
        return clip;
    }

    // Where a clip is in beats; and an audio clip's place in the song's seconds, its fades and their shapes.
    phazite::BeatRange rangeOf (phazite::ClipID clip) const { return phazite::getClipRange (song, clip); }

    phazite::PlayedPosition playedOf (phazite::ClipID clip) const { return phazite::getPlayedPosition (song, clip); }

    double fadeOf (phazite::ClipID clip, bool fadeIn) const
    {
        const auto value = phazite::getClipValue (song, clip);
        REQUIRE (value.has_value());
        return (fadeIn ? value->fadeIn : value->fadeOut).seconds;
    }

    phazite::FadeCurve fadeCurveOf (phazite::ClipID clip, bool fadeIn) const
    {
        const auto value = phazite::getClipValue (song, clip);
        REQUIRE (value.has_value());
        return phazite::getFadeCurve (fadeIn ? value->fadeIn : value->fadeOut);
    }

    phazite::Song::Ptr songPtr = phazite::createSong(); // 120 BPM: a beat is 0.5 s
    phazite::Song& song = *songPtr;
    std::vector<phazite::TrackID> tracks;
    phazite::Selection selection;
    phazite::RangeSelection range { selection };
    phazite::CommandRegistry registry;
};

// A one-second WAV file of silence, for audio clips.
struct SilentFile
{
    SilentFile()
    {
        juce::AudioBuffer<float> buffer (1, 44100);
        buffer.clear();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (temp.getFile());

        if (auto writer = juce::WavAudioFormat().createWriterFor (
                stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16)))
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    juce::TemporaryFile temp { ".wav" };
};
} // namespace

TEST_CASE ("The Smart Tool picks what a drag does from where it starts on a clip")
{
    using phazite::ui::ClipZone;
    const juce::Rectangle<float> clip (100.0f, 50.0f, 200.0f, 60.0f);
    const auto zoneAt = [&clip] (float x, float y, bool hasFades = true) { return phazite::ui::getClipZone (clip, { x, y }, hasFades); };

    CHECK (zoneAt (102.0f, 80.0f) == ClipZone::trimStart); // the edges
    CHECK (zoneAt (298.0f, 80.0f) == ClipZone::trimEnd);
    CHECK (zoneAt (105.0f, 52.0f) == ClipZone::fadeIn); // the top corners
    CHECK (zoneAt (295.0f, 52.0f) == ClipZone::fadeOut);
    CHECK (zoneAt (105.0f, 52.0f, false) == ClipZone::trimStart); // a MIDI clip has no fades
    CHECK (zoneAt (200.0f, 60.0f) == ClipZone::selectRange);      // the upper half
    CHECK (zoneAt (200.0f, 100.0f) == ClipZone::move);            // the lower half

    // A narrow clip keeps most of its width for moving, and a short one (on a short track) moves from anywhere
    // but its edges.
    CHECK (phazite::ui::getClipZone ({ 0.0f, 0.0f, 12.0f, 40.0f }, { 6.0f, 30.0f }, true) == ClipZone::move);
    CHECK (phazite::ui::getClipZone ({ 0.0f, 0.0f, 200.0f, 29.0f }, { 100.0f, 5.0f }, false) == ClipZone::move);
    CHECK (phazite::ui::getClipZone ({ 0.0f, 0.0f, 200.0f, 29.0f }, { 4.0f, 20.0f }, false) == ClipZone::trimStart);

    for (const auto zone :
         { ClipZone::trimStart, ClipZone::trimEnd, ClipZone::fadeIn, ClipZone::fadeOut, ClipZone::selectRange, ClipZone::move })
        CHECK (phazite::ui::describeClipZone (zone).isNotEmpty());
}

TEST_CASE ("A new MIDI clip is called after its track, whatever the track is called, and selected")
{
    ClipEdit e;
    phazite::setTrackName (e.song, e.tracks[0], "Bass");

    const auto clip = phazite::insertMidiClip (e.song, e.tracks[0], { beats (4.0), beats (8.0) }, &e.selection);
    REQUIRE (clip.isValid());
    CHECK (phazite::getClipValue (e.song, clip)->name.isEmpty()); // no name of its own
    CHECK (phazite::getClipName (e.song, clip) == juce::String ("Bass"));
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.rangeOf (clip).getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getSelectedClips (e.selection) == std::vector { clip });

    // Its track renamed, or moved where it is called "Track 2" by its place, it goes on being called after it.
    phazite::setTrackName (e.song, e.tracks[0], "Sub Bass");
    CHECK (phazite::getClipName (e.song, clip) == juce::String ("Sub Bass"));
    const auto unnamed = phazite::insertMidiClip (e.song, e.tracks[1], { beats (0.0), beats (4.0) }, nullptr);
    CHECK (phazite::getClipName (e.song, unnamed) == phazite::getTrackName (e.song, e.tracks[1]));
}

TEST_CASE ("Renaming a clip names its aliases too, as one undo step; an empty name, or its track's, takes its own away")
{
    // The owner, 2026-11-07: the playlist shows a clip's name only where it isn't its track's, which the header shows.
    ClipEdit e;
    phazite::setTrackName (e.song, e.tracks[0], "Keys");
    const auto clip = e.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips (e.song, { clip }, true);
    REQUIRE (aliases.size() == 1);
    const auto other = e.addClip (1, 0.0, 4.0);
    const auto nameOf = [&e] (phazite::ClipID id) { return phazite::getClipName (e.song, id); };
    const auto hasOwnName = [&e] (phazite::ClipID id) { return phazite::getClipValue (e.song, id)->name.isNotEmpty(); };
    e.song.getUndoManager().clearUndoHistory();

    phazite::renameClips (e.song, { clip }, "  Verse chords ");
    CHECK (phazite::describeUndo (&e.song) == juce::String ("Undo Rename Clip"));
    CHECK (nameOf (clip) == juce::String ("Verse chords"));
    CHECK (nameOf (aliases[0]) == juce::String ("Verse chords"));
    CHECK_FALSE (hasOwnName (other));

    // A name they have already makes no step.
    phazite::undo (e.song);
    phazite::redo (e.song);
    REQUIRE (e.song.getUndoManager().canUndo());
    phazite::renameClips (e.song, { aliases[0] }, "Verse chords");
    phazite::undo (e.song);
    CHECK (nameOf (clip) == juce::String ("Keys"));

    // Their track's name, or none: called after their track again, with no name of their own.
    phazite::redo (e.song);
    phazite::renameClips (e.song, { clip, aliases[0] }, "Keys");
    CHECK (phazite::describeUndo (&e.song) == juce::String ("Undo Rename Clips"));
    CHECK_FALSE (hasOwnName (clip));
    CHECK_FALSE (hasOwnName (aliases[0]));
    phazite::undo (e.song);
    CHECK (nameOf (clip) == juce::String ("Verse chords"));
    phazite::renameClips (e.song, { clip }, {});
    CHECK_FALSE (hasOwnName (clip));
    CHECK (nameOf (clip) == juce::String ("Keys"));
}

TEST_CASE ("Moving a clip keeps its length, and can take it to another track")
{
    ClipEdit e;
    const auto clip = e.addClip (0, 0.0, 4.0);

    phazite::moveClip (e.song, clip, beats (8.0), e.tracks[1]);
    CHECK (phazite::getClipTrack (e.song, clip) == e.tracks[1]);
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (e.rangeOf (clip).getLength().inBeats() == doctest::Approx (4.0));

    phazite::moveClip (e.song, clip, beats (-3.0), e.tracks[1]); // not before the song's start
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("What writes a clip by its value adds it among a track's clips, changes, moves, links and copies it, each undone exactly")
{
    ClipEdit e;
    const auto first = e.addClip (0, 0.0, 4.0);
    phazite::test::addMidiNote (e.song, first, 60, beats (0.0), lasting (1.0), 100);
    phazite::test::UndoCheck undo (e.song);

    // Added before the track's first clip, with the notes given, keeping the ID its value was made with.
    phazite::ClipID added;
    undo.checkSeveral ("a clip added before another",
                       [&]
                       {
                           auto clip = phazite::makeMidiClip (e.song, "Lead", { beats (8.0), beats (12.0) });
                           clip.track = e.tracks[0];
                           const auto notes = phazite::getClipNotesValue (e.song, first);
                           added = phazite::addClip (e.song, clip, &notes, 0);
                           CHECK (added == clip.id);
                       });
    CHECK (phazite::getClips (e.song, e.tracks[0]) == std::vector<phazite::ClipID> { added, first });
    CHECK (phazite::getClipNotes (e.song, added).size() == 1);
    CHECK (e.rangeOf (added).getStart().inBeats() == doctest::Approx (8.0));

    // Changed and moved to the end of another track's clips by its value.
    undo.checkSeveral ("a clip changed and moved",
                       [&]
                       {
                           auto clip = *phazite::getClipValue (e.song, added);
                           clip.muted = true;
                           clip.name = {};
                           clip.track = e.tracks[1];
                           phazite::setClip (e.song, clip);
                       });
    CHECK (phazite::getClipTrack (e.song, added) == e.tracks[1]);
    CHECK (phazite::areAllClipsMuted (e.song, { added }));
    CHECK (phazite::getClipName (e.song, added) == phazite::getTrackName (e.song, e.tracks[1]));

    // In one alias group, and in none again.
    undo.checkSeveral ("two clips linked",
                       [&]
                       {
                           const auto link = e.song.makeID();
                           phazite::setClipLink (e.song, first, link);
                           phazite::setClipLink (e.song, added, link);
                       });
    CHECK (phazite::getAliasClips (e.song, first) == std::vector<phazite::ClipID> { first, added });
    undo.checkSeveral ("two clips unlinked",
                       [&]
                       {
                           phazite::setClipLink (e.song, added, 0);
                           phazite::setClipLink (e.song, first, 0);
                       });
    CHECK_FALSE (phazite::hasAliases (e.song, { first, added }));

    // A track's clips copied onto another, each with an ID of its own and its notes.
    undo.checkSeveral ("a track's clips copied", [&] { phazite::copyTrackClips (e.song, e.tracks[1], e.tracks[0], {}, false); });
    auto copies = phazite::getClips (e.song, e.tracks[0]);
    REQUIRE (copies.size() == 2);
    REQUIRE (std::find (copies.begin(), copies.end(), first) != copies.end());
    copies.erase (std::find (copies.begin(), copies.end(), first));
    REQUIRE (copies.size() == 1);
    CHECK (copies[0] != added);
    CHECK (phazite::getClipNotes (e.song, copies[0]).size() == 1);

    undo.walkBackAndForth();
}

TEST_CASE ("Trimming a clip leaves its content in place")
{
    ClipEdit e;
    const auto clip = e.addClip (0, 4.0, 4.0);
    phazite::test::addMidiNote (e.song, clip, 60, beats (0.0), lasting (1.0), 100);
    const auto contentStart = [&e, clip] { return phazite::getContentStart (e.song, clip).inBeats(); };

    phazite::trimClipStart (e.song, clip, beats (5.0));
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (5.0));
    CHECK (e.rangeOf (clip).getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (contentStart() == doctest::Approx (4.0)); // so the note is where it was

    phazite::trimClipStart (e.song, clip, beats (2.0)); // not before the content starts
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (4.0));

    phazite::trimClipEnd (e.song, clip, beats (6.0));
    CHECK (e.rangeOf (clip).getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (contentStart() == doctest::Approx (4.0));

    phazite::trimClipEnd (e.song, clip, beats (0.0)); // a clip keeps some length
    CHECK (e.rangeOf (clip).getLength().inBeats() == doctest::Approx (1.0 / 64.0));
}

TEST_CASE ("Copies of a clip are separate, and aliases stay linked")
{
    ClipEdit e;
    const auto original = e.addClip (0, 0.0, 4.0);

    const auto copies = phazite::copyClips (e.song, { original }, false);
    REQUIRE (copies.size() == 1);
    CHECK (copies[0] != original);
    CHECK (e.rangeOf (copies[0]).getStart().inBeats() == doctest::Approx (0.0)); // in the same place, to be moved
    CHECK_FALSE (phazite::hasAliases (e.song, { copies[0] }));
    CHECK_FALSE (phazite::hasAliases (e.song, { original }));

    const auto aliases = phazite::copyClips (e.song, { original }, true);
    REQUIRE (aliases.size() == 1);
    CHECK (phazite::hasAliases (e.song, { original }));
    CHECK (phazite::getClipValue (e.song, aliases[0])->link == phazite::getClipValue (e.song, original)->link);

    // A copy of an alias is a separate clip again.
    const auto copyOfAlias = phazite::copyClips (e.song, { aliases[0] }, false);
    REQUIRE (copyOfAlias.size() == 1);
    CHECK_FALSE (phazite::hasAliases (e.song, { copyOfAlias[0] }));
    CHECK (phazite::hasAliases (e.song, { aliases[0] }));
}

TEST_CASE ("Fades stay within an audio clip")
{
    ClipEdit e;
    SilentFile file;
    const auto clip = phazite::test::addAudioClip (e.song, e.tracks[0], "Silence", file.temp.getFile(), 0.0, 1.0);
    REQUIRE (clip.isValid());

    phazite::setClipFade (e.song, clip, true, 0.25);
    CHECK (e.fadeOf (clip, true) == doctest::Approx (0.25));

    phazite::setClipFade (e.song, clip, false, -1.0);
    CHECK (e.fadeOf (clip, false) == doctest::Approx (0.0));

    phazite::setClipFade (e.song, clip, true, 5.0);
    CHECK (e.fadeOf (clip, true) <= 1.0);
}

TEST_CASE ("Delete and duplicate act on the selected clips")
{
    ClipEdit e;
    const auto a = e.addClip (0, 0.0, 4.0);
    const auto b = e.addClip (1, 4.0, 4.0);
    e.selection.selectOnly (a);
    e.selection.addToSelection (b);

    // Duplicates go right after the selection: here, 8 beats later.
    CHECK (phazite::getCurrentName (*e.registry.find ("edit.duplicate")) == juce::String ("Duplicate 2 Clips"));
    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (e.song.getUndoManager().getUndoDescription() == juce::String ("Duplicate 2 Clips"));

    const auto copies = phazite::getSelectedClips (e.selection);
    REQUIRE (copies.size() == 2);
    CHECK (phazite::getClipTrack (e.song, copies[0]) == e.tracks[0]);
    CHECK (e.rangeOf (copies[0]).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getClipTrack (e.song, copies[1]) == e.tracks[1]);
    CHECK (e.rangeOf (copies[1]).getStart().inBeats() == doctest::Approx (12.0));

    REQUIRE (e.registry.invoke ("edit.delete"));
    CHECK (phazite::getClips (e.song, e.tracks[0]).size() == 1);
    CHECK (phazite::getClips (e.song, e.tracks[1]).size() == 1);

    phazite::undo (e.song);
    CHECK (phazite::getClips (e.song, e.tracks[0]).size() == 2);
}

TEST_CASE ("Select All selects every clip, or every track while tracks are selected")
{
    ClipEdit e;
    e.addClip (0, 0.0, 4.0);
    e.addClip (1, 0.0, 4.0);

    REQUIRE (e.registry.invoke ("edit.selectAll"));
    CHECK (phazite::getSelectedClips (e.selection).size() == 2);

    e.selection.selectOnly (e.tracks[0]);
    REQUIRE (e.registry.invoke ("edit.selectAll"));
    CHECK (phazite::getSelectedClips (e.selection).empty());
    CHECK (e.selection.getNumObjectsSelected() == 2);
}

namespace
{
// A track's clips as "start-end" in beats, in order, such as "0-2 4-8".
juce::String describeClips (const phazite::Song& song, phazite::TrackID track)
{
    std::vector<phazite::BeatRange> ranges;

    for (const auto clip : phazite::getClips (song, track))
        ranges.push_back (phazite::getClipRange (song, clip));

    std::sort (ranges.begin(), ranges.end(), [] (phazite::BeatRange a, phazite::BeatRange b) { return a.getStart() < b.getStart(); });
    juce::StringArray parts;

    for (const auto& clipRange : ranges)
        parts.add (juce::String (clipRange.getStart().inBeats(), 2) + "-" + juce::String (clipRange.getEnd().inBeats(), 2));

    return parts.joinIntoString (" ");
}
} // namespace

TEST_CASE ("Split, mute and nudge act on the selected clips")
{
    ClipEdit e;
    const auto clip = e.addClip (0, 0.0, 8.0);
    e.selection.selectOnly (clip);
    phazite::movePlayhead (e.song, beats (2.0));

    REQUIRE (e.registry.invoke ("edit.split")); // at the playhead
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-2.00 2.00-8.00"));

    e.selection.selectOnly (clip);
    REQUIRE (e.registry.invoke ("edit.mute"));
    CHECK (phazite::areAllClipsMuted (e.song, { clip }));
    CHECK (e.registry.find ("edit.mute")->isTicked());
    REQUIRE (e.registry.invoke ("edit.mute"));
    CHECK_FALSE (phazite::areAllClipsMuted (e.song, { clip }));

    phazite::nudgeClips (e.song, { clip }, lasting (-1.0)); // not before the start of the song
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (0.0));
    phazite::nudgeClips (e.song, { clip }, lasting (8.0));
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (8.0));
}

TEST_CASE ("Make Unique turns aliases back into separate clips")
{
    ClipEdit e;
    const auto original = e.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips (e.song, { original }, true);
    REQUIRE (aliases.size() == 1);

    e.selection.selectOnly (aliases[0]);
    REQUIRE (e.registry.invoke ("clip.makeUnique"));
    CHECK_FALSE (phazite::hasAliases (e.song, { aliases[0] }));
    CHECK_FALSE (phazite::hasAliases (e.song, { original })); // left alone in its group, it is no one's alias either
    CHECK_FALSE (e.registry.find ("clip.makeUnique")->isEnabled());
}

TEST_CASE ("Delete and Close Gap moves what comes after back")
{
    ClipEdit e;
    e.addClip (0, 0.0, 4.0);
    const auto middle = e.addClip (0, 4.0, 4.0);
    e.addClip (0, 8.0, 4.0);

    e.selection.selectOnly (middle);
    REQUIRE (e.registry.invoke ("edit.deleteAndCloseGap"));
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-4.00 4.00-8.00"));
}

TEST_CASE ("A time range is deleted, split, inserted and removed")
{
    ClipEdit e;
    e.addClip (0, 0.0, 8.0);
    e.addClip (1, 0.0, 8.0);
    e.range.set ({ beats (2.0), beats (4.0) }, { e.tracks[0] });
    CHECK (phazite::describeSelection (e.selection, e.range, "Delete") == juce::String ("Delete Range"));

    // Delete: on the range's track only, where the clip is split around the hole.
    REQUIRE (e.registry.invoke ("edit.delete"));
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-2.00 4.00-8.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("0.00-8.00"));
    phazite::undo (e.song);
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-8.00"));

    // Split: at both edges.
    REQUIRE (e.registry.invoke ("edit.split"));
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-2.00 2.00-4.00 4.00-8.00"));
    phazite::undo (e.song);

    // Insert Time: on every track, as long as the range.
    REQUIRE (e.registry.invoke ("edit.insertTime"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("0.00-2.00 4.00-10.00"));
    phazite::undo (e.song);
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("0.00-8.00"));

    // Remove Time: from every track, closing the gap.
    REQUIRE (e.registry.invoke ("edit.removeTime"));
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-2.00 2.00-6.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("0.00-2.00 2.00-6.00"));

    // A range whose tracks are all gone is empty.
    phazite::deleteTracks (e.song, { e.tracks[0] }, nullptr);
    CHECK (e.range.isEmpty());
}

TEST_CASE ("Removing time keeps the tempo that played after it: a change inside moves to its start, or the song's start takes it")
{
    // An intro at 90 BPM, and a verse from beat 16 at 128 that the chorus after it, from beat 32, plays at too: the change
    // in a range removed went, so what came after played at the tempo before it.
    const auto makeSong = [] (ClipEdit& e)
    {
        phazite::setTempo (e.song, 90.0);
        const phazite::UndoStep step (e.song, "Add Tempo Change");
        phazite::addTempoChange (e.song, beats (16.0), 128.0);
    };

    const auto describeTempos = [] (const phazite::Song& song)
    {
        juce::StringArray tempos;

        for (const auto& tempo : phazite::getTempoChanges (song))
            tempos.add (juce::String (tempo.beat.inBeats()) + ": " + juce::String (tempo.bpm));

        return tempos.joinIntoString (", ");
    };

    const auto removeTime = [] (phazite::Song& song, double from, double to)
    {
        const phazite::UndoStep step (song, "Remove Time");
        phazite::removeTime (song, { beats (from), beats (to) });
    };

    // The verse removed: the chorus, now from beat 16, at 128 still.
    {
        ClipEdit e;
        makeSong (e);
        removeTime (e.song, 16.0, 32.0);
        CHECK (describeTempos (e.song) == juce::String ("0: 90, 16: 128"));
    }

    // The intro removed: the song starts at the verse's 128, one tempo at its start; undone, as it was.
    {
        ClipEdit e;
        makeSong (e);
        removeTime (e.song, 0.0, 16.0);
        CHECK (describeTempos (e.song) == juce::String ("0: 128"));
        phazite::undo (e.song);
        CHECK (describeTempos (e.song) == juce::String ("0: 90, 16: 128"));
    }
}

TEST_CASE ("Duplicate copies a time range right after it, and the range moves to the copy")
{
    ClipEdit e;
    const auto crossing = e.addClip (0, 1.0, 2.0); // reaches into the range from before it
    REQUIRE (phazite::addNote (e.song, crossing, 60, beats (2.5), lasting (0.5), 100).isValid());
    const auto after = e.addClip (0, 4.5, 3.5); // where the copy lands
    e.addClip (1, 4.0, 2.0);                    // after the range, on a track with nothing in it
    e.range.set ({ beats (2.0), beats (4.0) }, { e.tracks[0], e.tracks[1] });

    const auto* duplicate = e.registry.find ("edit.duplicate");
    REQUIRE (duplicate != nullptr);
    CHECK (duplicate->isEnabled());
    CHECK (phazite::getCurrentName (*duplicate) == juce::String ("Duplicate Range"));
    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (e.song.getUndoManager().getUndoDescription() == juce::String ("Duplicate Range"));

    // The part inside the range goes right after it, in place of what was there; the clip it came from stays whole.
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-8.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("4.00-6.00")); // nothing of the range landed there

    phazite::ClipID copy;

    for (const auto clip : phazite::getClips (e.song, e.tracks[0]))
        if (clip != crossing && clip != after)
            copy = clip;

    REQUIRE (copy.isValid());
    REQUIRE (phazite::isMidiClip (e.song, copy));
    CHECK_FALSE (phazite::hasAliases (e.song, { copy }));
    const auto copiedNotes = phazite::getClipNotes (e.song, copy);
    REQUIRE (copiedNotes.size() == 1);
    CHECK (copiedNotes.front().start.inBeats() == doctest::Approx (4.5));
    const auto crossingNotes = phazite::getClipNotes (e.song, crossing);
    REQUIRE (crossingNotes.size() == 1);
    CHECK (crossingNotes.front().start.inBeats() == doctest::Approx (2.5));

    // The copy is the range now, on both tracks, and nothing else got selected: Duplicate again repeats it.
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (e.range.getTracks().size() == 2);
    CHECK (e.selection.getNumObjectsSelected() == 0);

    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-6.00 6.00-7.00 7.00-8.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("4.00-6.00 6.00-8.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (6.0));

    // Each was one undo step, and Undo puts the range back where that step found it, so Duplicate after Undo repeats
    // the same part again.
    phazite::undo (e.song);
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-8.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (4.0));
    phazite::undo (e.song);
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("1.00-3.00 4.50-8.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("4.00-6.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (2.0));
    CHECK (e.range.getRange().getEnd().inBeats() == doctest::Approx (4.0));
    CHECK (e.range.getTracks().size() == 2);

    // Redo makes the copy again, and the range goes back to it.
    phazite::redo (e.song);
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-8.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
}

TEST_CASE ("With a time range and a track selected, Duplicate acts on the range, as its name and Delete do")
{
    ClipEdit e; // a click on a track's header selects the track and leaves the range (Playlist::trackClicked)
    e.addClip (0, 0.0, 4.0);
    e.range.set ({ beats (0.0), beats (4.0) }, { e.tracks[0] });
    e.selection.selectOnly (e.tracks[1]);

    CHECK (phazite::getCurrentName (*e.registry.find ("edit.duplicate")) == juce::String ("Duplicate Range"));
    CHECK (phazite::getCurrentName (*e.registry.find ("edit.delete")) == juce::String ("Delete Range"));
    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (phazite::getTrackIDs (e.song).size() == 2); // no track was copied
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-4.00 4.00-8.00"));
    CHECK (e.selection.isSelected (e.tracks[1]));
    CHECK_FALSE (e.registry.find ("edit.duplicateAsAlias")->isEnabled()); // still clips and sections only
}

TEST_CASE ("Each tool has a name, a command and a hint")
{
    juce::StringArray ids;

    for (const auto tool : phazite::ui::allTools)
    {
        CHECK (phazite::ui::getToolName (tool).isNotEmpty());
        CHECK (phazite::ui::describeTool (tool).isNotEmpty());
        CHECK_FALSE (ids.contains (phazite::ui::getToolCommandID (tool)));
        ids.add (phazite::ui::getToolCommandID (tool));
    }
}

TEST_CASE ("A crossfade overlaps two touching audio clips, as far as their audio reaches")
{
    ClipEdit e;
    SilentFile file; // a second long
    const auto first = phazite::test::addAudioClip (e.song, e.tracks[0], "A", file.temp.getFile(), 0.0, 0.5);
    const auto second = phazite::test::addAudioClip (e.song, e.tracks[0], "B", file.temp.getFile(), 0.5, 0.5, 0.25);
    REQUIRE (first.isValid());
    REQUIRE (second.isValid());

    phazite::setCrossfade (e.song, first, second, 0.5, 0.2);
    CHECK (e.playedOf (first).end == doctest::Approx (0.6));
    CHECK (e.playedOf (second).start == doctest::Approx (0.4));
    CHECK (e.fadeOf (first, false) == doctest::Approx (0.2));
    CHECK (e.fadeOf (second, true) == doctest::Approx (0.2));

    // The second clip's audio starts a quarter of a second before it does, so no further.
    phazite::setCrossfade (e.song, first, second, 0.5, 2.0);
    CHECK (e.playedOf (second).start == doctest::Approx (0.25));
    CHECK (e.playedOf (first).end == doctest::Approx (0.75));

    // A crossfade of nothing puts them back.
    phazite::setCrossfade (e.song, first, second, 0.5, 0.0);
    CHECK (e.playedOf (first).end == doctest::Approx (0.5));
    CHECK (e.playedOf (second).start == doctest::Approx (0.5));
    CHECK (e.fadeOf (second, true) == doctest::Approx (0.0));

    phazite::setFadeShape (e.song, { first, second }, phazite::sCurveFade);
    CHECK (e.fadeCurveOf (first, false) == phazite::sCurveFade);
    CHECK (e.fadeCurveOf (second, true) == phazite::sCurveFade);
}

TEST_CASE ("Duplicated clips go right after them all")
{
    ClipEdit e;
    const auto a = e.addClip (0, 0.0, 4.0);
    const auto b = e.addClip (1, 2.0, 4.0); // together they span beats 0-6

    // Each copy starts 6 beats after its original, on its track; without a selection, nothing gets selected.
    auto copies = phazite::duplicateClips (e.song, { a, b }, false, nullptr);
    REQUIRE (copies.size() == 2);
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-4.00 6.00-10.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("2.00-6.00 8.00-12.00"));
    CHECK_FALSE (phazite::hasAliases (e.song, { copies.front() }));
    CHECK (e.selection.getNumObjectsSelected() == 0);

    // As aliases, with a selection: the copy is selected instead of what was.
    e.selection.selectOnly (e.tracks[1]);
    copies = phazite::duplicateClips (e.song, { a }, true, &e.selection);
    REQUIRE (copies.size() == 1);
    CHECK (phazite::hasAliases (e.song, { copies.front() }));
    CHECK (e.rangeOf (copies.front()).getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.selection.isSelected (copies.front()));
    CHECK_FALSE (e.selection.isSelected (e.tracks[1]));

    // No clips: nothing is copied, and the selection stays.
    CHECK (phazite::duplicateClips (e.song, {}, false, &e.selection).empty());
    CHECK (e.selection.isSelected (copies.front()));
}

TEST_CASE ("Splitting at a range's edges")
{
    ClipEdit e;
    e.addClip (0, 0.0, 8.0);
    e.addClip (1, 1.0, 6.0); // across the range's edges too, but on a track the range doesn't cover

    phazite::splitClipsAtEdges (e.song, { e.tracks[0] }, { beats (2.0), beats (6.0) });
    CHECK (describeClips (e.song, e.tracks[0]) == juce::String ("0.00-2.00 2.00-6.00 6.00-8.00"));
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("1.00-7.00"));

    // A clip that ends inside the range is cut once.
    phazite::splitClipsAtEdges (e.song, { e.tracks[1] }, { beats (5.0), beats (9.0) });
    CHECK (describeClips (e.song, e.tracks[1]) == juce::String ("1.00-5.00 5.00-7.00"));
}

TEST_CASE ("Aliases, audio clips and fade shapes")
{
    ClipEdit e;
    const auto midi = e.addClip (0, 0.0, 4.0);
    CHECK_FALSE (phazite::hasAliases (e.song, { midi }));
    CHECK_FALSE (phazite::hasAliases (e.song, {}));
    CHECK_FALSE (phazite::hasAudioClips (e.song, { midi }));
    CHECK_FALSE (phazite::hasFadeShape (e.song, { midi }, phazite::linearFade)); // there is no audio clip

    const auto aliases = phazite::copyClips (e.song, { midi }, true);
    REQUIRE (aliases.size() == 1);
    CHECK (phazite::hasAliases (e.song, { midi }));
    CHECK (phazite::hasAliases (e.song, aliases));

    // The shape is on when every audio clip fades in and out with it; MIDI clips don't count.
    SilentFile file;
    const auto audio = phazite::test::addAudioClip (e.song, e.tracks[1], "Silence", file.temp.getFile(), 0.0, 1.0);
    REQUIRE (audio.isValid());
    const std::vector both { midi, audio };
    CHECK (phazite::hasAudioClips (e.song, both));

    phazite::setFadeShape (e.song, both, phazite::convexFade);
    CHECK (phazite::hasFadeShape (e.song, both, phazite::convexFade));
    CHECK_FALSE (phazite::hasFadeShape (e.song, both, phazite::linearFade));

    auto straightOut = *phazite::getClipValue (e.song, audio); // in and out differ: out is straight
    straightOut.fadeOut.bend = 0.0f;
    phazite::setClip (e.song, straightOut);
    CHECK_FALSE (phazite::hasFadeShape (e.song, both, phazite::convexFade));
}

TEST_CASE ("A double-click's MIDI clip lasts a bar of the time signature there, as one undo step")
{
    ClipEdit e;
    phazite::setTimeSignature (e.song, { 3, 4 });
    e.song.getUndoManager().clearUndoHistory();

    const auto clip = phazite::addMidiClipAt (e.song, e.tracks[0], beats (6.0), &e.selection);
    REQUIRE (clip.isValid());
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (6.0));
    CHECK (e.rangeOf (clip).getEnd().inBeats() == doctest::Approx (9.0));
    CHECK (phazite::getSelectedClips (e.selection) == std::vector { clip });
    CHECK (e.song.getUndoManager().getUndoDescription() == juce::String ("Add MIDI Clip"));

    phazite::undo (e.song);
    CHECK (phazite::getClips (e.song, e.tracks[0]).empty());
}

TEST_CASE ("Draw places a copy of the clip used last, or a bar-long MIDI clip on a track with an instrument")
{
    ClipEdit e;
    CHECK_FALSE (phazite::placeClip (e.song, e.tracks[0], beats (0.0), {}).isValid()); // nothing used yet, and no instrument

    // A MIDI clip's copy is an alias, which stays linked to it.
    const auto source = e.addClip (0, 0.0, 2.0);
    const auto placed = phazite::placeClip (e.song, e.tracks[1], beats (8.0), source);
    REQUIRE (placed.isValid());
    CHECK (phazite::getClipTrack (e.song, placed) == e.tracks[1]);
    CHECK (e.rangeOf (placed).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (e.rangeOf (placed).getLength().inBeats() == doctest::Approx (2.0));
    CHECK (phazite::hasAliases (e.song, { placed }));

    // Once the clip used last is gone: an empty MIDI clip a bar long, on a track with an instrument only.
    phazite::deleteClips (e.song, { source }, false);
    CHECK_FALSE (phazite::placeClip (e.song, e.tracks[0], beats (4.0), source).isValid());

    REQUIRE (phazite::setInstrument (e.song, e.tracks[0], phazite::makeOwnDeviceState (e.song, phazite::devices::Synth::typeID)).isValid());
    const auto empty = phazite::placeClip (e.song, e.tracks[0], beats (4.0), source);
    REQUIRE (empty.isValid());
    REQUIRE (phazite::isMidiClip (e.song, empty));
    CHECK (phazite::getClipNotes (e.song, empty).empty());
    CHECK (e.rangeOf (empty).getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.rangeOf (empty).getLength().inBeats() == doctest::Approx (4.0)); // a bar of 4/4
}

TEST_CASE ("Slip moves what a clip plays, not the clip, and its content can't start after the clip")
{
    ClipEdit e;
    const auto clip = e.addClip (0, 4.0, 4.0);

    phazite::slipClip (e.song, clip, 1.0, 0.25);
    CHECK (phazite::getClipStartOffset (e.song, clip) == doctest::Approx (0.75));
    CHECK (e.rangeOf (clip).getStart().inBeats() == doctest::Approx (4.0));

    phazite::slipClip (e.song, clip, 1.0, 3.0);
    CHECK (phazite::getClipStartOffset (e.song, clip) == doctest::Approx (0.0));
}

TEST_CASE ("The last clip left of a group of linked clips is linked no more, whether the others or their tracks went")
{
    // It kept its link, and still showed as linked, to nothing (known bug 52).
    ClipEdit e;
    const auto original = e.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips (e.song, { original }, true);
    REQUIRE (aliases.size() == 1);
    const auto other = phazite::copyClips (e.song, { original }, true);
    REQUIRE (other.size() == 1);
    phazite::moveClip (e.song, other[0], beats (16.0), e.tracks[1]);
    e.song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (e.song);
    const auto isLinked = [&e] (phazite::ClipID clip) { return phazite::hasAliases (e.song, { clip }); };

    SUBCASE ("The others deleted")
    {
        undo.check ("one of three deleted",
                    [&]
                    {
                        const phazite::UndoStep step (e.song, "Delete Clip");
                        phazite::deleteClips (e.song, { aliases[0] }, false);
                    });
        CHECK (isLinked (original)); // two are left, linked
        undo.check ("another deleted",
                    [&]
                    {
                        const phazite::UndoStep step (e.song, "Delete Clip");
                        phazite::deleteClips (e.song, { original }, false);
                    });
        CHECK_FALSE (isLinked (other[0]));
    }

    SUBCASE ("Their track deleted")
    {
        undo.check ("the first track deleted",
                    [&]
                    {
                        const phazite::UndoStep step (e.song, "Delete Track");
                        phazite::deleteTracks (e.song, { e.tracks[0] }, nullptr);
                    });
        CHECK_FALSE (isLinked (other[0]));
    }

    undo.walkBackAndForth();
}

TEST_CASE ("Where linked clips play, an editor showing one of them sees its notes playing")
{
    ClipEdit e;
    const auto original = e.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips (e.song, { original, original }, true);
    REQUIRE (aliases.size() == 2);
    // One alias later in the song, whole; one that plays only beats 2 to 4 of the notes (a snippet), from beat 16.
    const auto place = [&e] (phazite::ClipID clip, double start, double length, double offset)
    { phazite::setClipPlace (e.song, clip, beats (start), lasting (length), lasting (offset)); };

    place (aliases[0], 8.0, 4.0, 0.0);
    place (aliases[1], 16.0, 2.0, 2.0);
    const std::vector shown { original };

    CHECK (phazite::getLinkedPlayBeat (e.song, shown, 9.5) == std::optional<double> (1.5));
    CHECK (phazite::getLinkedPlayBeat (e.song, shown, 16.5) == std::optional<double> (2.5));

    // The clip shown plays there itself, or nothing does: nothing to show in it.
    CHECK_FALSE (phazite::getLinkedPlayBeat (e.song, shown, 2.0).has_value());
    CHECK_FALSE (phazite::getLinkedPlayBeat (e.song, shown, 6.0).has_value());
    CHECK_FALSE (phazite::getLinkedPlayBeat (e.song, { original, aliases[0] }, 9.5).has_value());

    // A clip made unique has no linked clips.
    phazite::makeClipsUnique (e.song, { original });
    CHECK_FALSE (phazite::getLinkedPlayBeat (e.song, shown, 9.5).has_value());
}

namespace
{
// A clip's notes in their order, all of each (TestClips.h).
std::string describeNotes (const phazite::Song& song, phazite::ClipID clip)
{
    return phazite::test::describeClipNotes (song, clip).toStdString();
}

// The song of ClipEdit open in a project context, as the playlist reads it: getLinkedClips() names a clip's alias group.
struct Opened
{
    explicit Opened (phazite::Song& song) { setSong (context, &song); }
    ~Opened() { setSong (context, nullptr); }

    std::vector<phazite::ClipID> groupOf (phazite::ClipID clip) const
    {
        auto clips = phazite::getLinkedClips (context, clip);
        std::sort (clips.begin(), clips.end());
        return clips;
    }

    phazite::ProjectContext context { phazite::test::getEngineContext() };
};

std::vector<phazite::ClipID> sorted (std::vector<phazite::ClipID> clips)
{
    std::sort (clips.begin(), clips.end());
    return clips;
}
} // namespace

TEST_CASE ("A clip of an alias group split, duplicated, copied, deleted, made unique and moved: which clips are in the group, each "
           "holding the same notes, Undo and Redo of each")
{
    // Clips.h: a split's right half, an alias copied or duplicated join the group; a copy doesn't; deleting a clip unlinks the
    // last one of its group; Make Unique takes a clip out; a clip moved stays in. The clip X on the first track from beat 0
    // to 4, with two notes; its alias Y on the second track from 8 to 12.
    ClipEdit e;
    const auto x = e.addClip (0, 0.0, 4.0);
    const auto first = phazite::test::addMidiNote (e.song, x, 60, beats (0.5), lasting (0.5), 100);
    const auto second = phazite::test::addMidiNote (e.song, x, 64, beats (2.5), lasting (1.0), 90);
    const auto aliases = phazite::copyClips (e.song, { x }, true);
    REQUIRE (aliases.size() == 1);
    const auto y = aliases.front();
    phazite::moveClip (e.song, y, beats (8.0), e.tracks[1]);
    e.song.getUndoManager().clearUndoHistory();
    const Opened opened (e.song);
    REQUIRE (opened.groupOf (x) == sorted ({ x, y }));
    phazite::test::UndoCheck check (e.song);

    using Clips = std::vector<phazite::ClipID>;
    const auto edit = [&] (const juce::String& name, const std::function<void()>& change)
    {
        check.check (name,
                     [&]
                     {
                         const phazite::UndoStep step (e.song, name);
                         change();
                     });
    };

    // Each clip of a group holds the notes the first holds.
    const auto holdTheSameNotes = [&e] (const Clips& clips)
    {
        for (const auto clip : clips)
        {
            INFO ("clip " << clip.getRaw());
            CHECK (describeNotes (e.song, clip) == describeNotes (e.song, clips.front()));
        }
    };

    // The groups as Undo leaves them, then as Redo does.
    const auto undoAndRedo = [&] (const juce::String& name, const std::function<void()>& undone, const std::function<void()>& redone)
    {
        check.stepBackOrForth ("Undo " + name, [&e] { phazite::undo (e.song); });
        undone();
        check.stepBackOrForth ("Redo " + name, [&e] { phazite::redo (e.song); });
        redone();
    };

    // Split: both halves are in the group, holding its notes; a note edited through the right half is edited in all.
    phazite::ClipID right;
    edit ("Split", [&] { right = phazite::splitClips (e.song, { x }, beats (2.0)).at (0); });
    CHECK (opened.groupOf (x) == sorted ({ x, y, right }));
    holdTheSameNotes ({ x, y, right });
    undoAndRedo (
        "Split", [&] { CHECK (opened.groupOf (x) == sorted ({ x, y })); }, [&] { CHECK (opened.groupOf (x) == sorted ({ x, y, right })); });

    edit ("Transpose Note", [&] { phazite::transposeNotes (e.song, { { right, second.note } }, 1); });
    holdTheSameNotes ({ x, y, right });
    CHECK (phazite::getNoteData (e.song, phazite::NoteRef { y, second.note }).value().pitch == 65);

    // Duplicated as an alias: in the group; as a copy: on its own, its notes a copy that no edit of the group reaches.
    Clips duplicated;
    edit ("Duplicate as Alias", [&] { duplicated = phazite::duplicateClips (e.song, { y }, true, nullptr); });
    REQUIRE (duplicated.size() == 1);
    const auto d = duplicated.front();
    CHECK (opened.groupOf (x) == sorted ({ x, y, right, d }));
    holdTheSameNotes ({ x, y, right, d });
    undoAndRedo (
        "Duplicate as Alias", [&] { CHECK (opened.groupOf (x) == sorted ({ x, y, right })); },
        [&] { CHECK (opened.groupOf (d) == sorted ({ x, y, right, d })); });

    edit ("Duplicate", [&] { duplicated = phazite::duplicateClips (e.song, { y }, false, nullptr); });
    REQUIRE (duplicated.size() == 1);
    const auto copy = duplicated.front();
    CHECK (opened.groupOf (copy) == Clips { copy });
    CHECK (opened.groupOf (x) == sorted ({ x, y, right, d }));
    holdTheSameNotes ({ y, copy }); // copied as they were

    // Copied as an alias (moved, as a drag does): in the group; as a copy: not.
    phazite::ClipID a, k;
    edit ("Copy as Alias",
          [&]
          {
              a = phazite::copyClips (e.song, { x }, true).at (0);
              phazite::moveClip (e.song, a, beats (20.0), e.tracks[0]);
          });
    CHECK (opened.groupOf (x) == sorted ({ x, y, right, d, a }));
    holdTheSameNotes ({ x, y, right, d, a });
    undoAndRedo (
        "Copy as Alias", [&] { CHECK (opened.groupOf (x) == sorted ({ x, y, right, d })); },
        [&] { CHECK (opened.groupOf (x) == sorted ({ x, y, right, d, a })); });

    edit ("Copy",
          [&]
          {
              k = phazite::copyClips (e.song, { x }, false).at (0);
              phazite::moveClip (e.song, k, beats (28.0), e.tracks[0]);
          });
    CHECK (opened.groupOf (k) == Clips { k });
    edit ("Transpose Note", [&] { phazite::transposeNotes (e.song, { { a, first.note } }, -2); });
    holdTheSameNotes ({ x, y, right, d, a });
    CHECK (phazite::getNoteData (e.song, phazite::NoteRef { k, first.note }).value().pitch == 60); // the copy's own, as it was
    CHECK (phazite::getNoteData (e.song, phazite::NoteRef { right, first.note }).value().pitch == 58);

    // One deleted: the others stay a group. All but one: it is no one's alias, linked to nothing.
    edit ("Delete Clip", [&] { phazite::deleteClips (e.song, { d }, false); });
    CHECK (opened.groupOf (x) == sorted ({ x, y, right, a }));
    undoAndRedo (
        "Delete Clip", [&] { CHECK (opened.groupOf (x) == sorted ({ x, y, right, d, a })); },
        [&] { CHECK (opened.groupOf (x) == sorted ({ x, y, right, a })); });

    edit ("Delete Clips", [&] { phazite::deleteClips (e.song, { x, right, a }, false); });
    CHECK (opened.groupOf (y) == Clips { y });
    CHECK_FALSE (phazite::hasAliases (e.song, { y }));
    undoAndRedo (
        "Delete Clips", [&] { CHECK (opened.groupOf (y) == sorted ({ x, y, right, a })); },
        [&] { CHECK (opened.groupOf (y) == Clips { y }); });
    check.stepBackOrForth ("Undo Delete Clips", [&e] { phazite::undo (e.song); });
    REQUIRE (opened.groupOf (y) == sorted ({ x, y, right, a }));
    holdTheSameNotes ({ x, y, right, a });

    // Made unique: out of the group, its notes its own; the rest a group still.
    edit ("Make Unique", [&] { phazite::makeClipsUnique (e.song, { y }); });
    CHECK (opened.groupOf (y) == Clips { y });
    CHECK (opened.groupOf (x) == sorted ({ x, right, a }));
    undoAndRedo (
        "Make Unique", [&] { CHECK (opened.groupOf (y) == sorted ({ x, y, right, a })); },
        [&] { CHECK (opened.groupOf (x) == sorted ({ x, right, a })); });
    edit ("Transpose Note", [&] { phazite::transposeNotes (e.song, { { x, second.note } }, 2); });
    holdTheSameNotes ({ x, right, a });
    CHECK (phazite::getNoteData (e.song, phazite::NoteRef { a, second.note }).value().pitch == 67);
    CHECK (phazite::getNoteData (e.song, phazite::NoteRef { y, second.note }).value().pitch == 65);

    // Moved to another track: still in the group.
    edit ("Move Clip", [&] { phazite::moveClip (e.song, right, beats (24.0), e.tracks[1]); });
    CHECK (phazite::getClipTrack (e.song, right) == e.tracks[1]);
    CHECK (opened.groupOf (right) == sorted ({ x, right, a }));
    holdTheSameNotes ({ x, right, a });
    undoAndRedo (
        "Move Clip", [&] { CHECK (phazite::getClipTrack (e.song, right) == e.tracks[0]); },
        [&] { CHECK (opened.groupOf (right) == sorted ({ x, right, a })); });

    check.walkBackAndForth();
    CHECK (opened.groupOf (x) == sorted ({ x, right, a }));
}

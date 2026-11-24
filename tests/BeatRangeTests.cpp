// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's last beat (schema::maxBeats, the bound of every beat its descriptions give, Schema.h): an edit never leaves a
// value the song can't hold. Each area's writer keeps a beat it sets within its description's range (SongText.h:
// keepDescribed()), and an edit that moves a group together moves it only as far as its last member can go
// (keepDistanceDescribed()), the distances inside it kept: a clip moved, resized, nudged, duplicated and pasted past the
// end; a MIDI and an audio clip's start trimmed and split past it, what the clip plays staying where it was; a section; a
// tempo change, and two of a kind pushed onto the last beat, one kept; a chord; an automation point dragged; a note placed
// in a clip that ends there. Each one named undo step, exact (UndoCheck.h), and the song's state read back from its own
// file's form the state (SongCheck.h).
// Before, a beat past the range was held and saved where it was made, and opened at the last beat (known bug 247).
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace
{
using namespace phazite::test;

constexpr double lastBeat = phazite::schema::maxBeats;

// A number as near as the beats near the last one are compared: doctest's Approx is near in proportion to the number, which
// near a million beats lets several beats through; within a millionth of a beat (or of a second) here.
doctest::Approx near (double number)
{
    return doctest::Approx (number).epsilon (1.0e-12);
}

phazite::Song& songOf (TestSong& s)
{
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    return *song;
}

double startOf (const phazite::Song& song, phazite::ClipID clip)
{
    const auto value = phazite::getClipValue (song, clip);
    REQUIRE (value.has_value());
    return value->start.inBeats();
}

// A change of its own, as one named step.
std::function<void()> inStep (phazite::Song& song, const juce::String& name, const std::function<void()>& change)
{
    return [&song, name, change]
    {
        const phazite::UndoStep step (song, name);
        change();
    };
}
} // namespace

TEST_CASE ("A clip moved, resized, nudged, duplicated and pasted past the song's last beat stays within it, a group as far "
           "apart as it was")
{
    TestSong s;
    auto& song = songOf (s);
    const auto track = s.firstTrack;
    const auto a = s.addMidiClip (track, lastBeat - 20.0, 4.0);
    const auto b = s.addMidiClip (track, lastBeat - 8.0, 4.0);
    UndoCheck undo (s.context);

    // Moved past it: at the last beat.
    undo.check ("Move Clip", inStep (song, "Move Clip", [&] { phazite::moveClip (song, a, at (1.2e6), track); }));
    CHECK (startOf (song, a) == near (lastBeat));
    undo.check ("Move Clip", inStep (song, "Move Clip", [&] { phazite::moveClip (song, a, at (lastBeat - 20.0), track); }));

    // Made longer than the range: as long as it gives.
    undo.check ("Trim Clip", inStep (song, "Trim Clip", [&] { phazite::trimClipEnd (song, b, at (3.0e6)); }));
    CHECK (phazite::getClipValue (song, b)->length.inBeats() == near (lastBeat));
    undo.check ("Trim Clip", inStep (song, "Trim Clip", [&] { phazite::trimClipEnd (song, b, at (lastBeat - 4.0)); }));

    // Nudged together by 100 beats: the later as far as the last beat, the earlier 12 beats before it.
    undo.check ("Nudge Clips", inStep (song, "Nudge Clips", [&] { phazite::nudgeClips (song, { a, b }, lasting (100.0)); }));
    CHECK (startOf (song, b) == near (lastBeat));
    CHECK (startOf (song, a) == near (lastBeat - 12.0));
    undo.check ("Nudge Clips", inStep (song, "Nudge Clips", [&] { phazite::nudgeClips (song, { a, b }, lasting (-8.0)); }));

    // Duplicated: the copies after their span (16 beats), or as far as the later copy can start, 12 beats apart still.
    std::vector<phazite::ClipID> copies;
    undo.check ("Duplicate Clips",
                inStep (song, "Duplicate Clips", [&] { copies = phazite::duplicateClips (song, { a, b }, false, nullptr); }));
    REQUIRE (copies.size() == 2);
    CHECK (startOf (song, copies[0]) == near (lastBeat - 12.0));
    CHECK (startOf (song, copies[1]) == near (lastBeat));

    // Pasted at the playhead a beat before the end (in place of the copies there): as far as the later can start, 12 beats
    // apart.
    auto& selection = phazite::getSelection (s.context);
    selection.selectOnly (a);
    selection.addToSelection (b);
    phazite::Clipboard clipboard;
    phazite::copySelection (clipboard, selection, phazite::getRangeSelection (s.context));
    phazite::movePlayhead (song, at (lastBeat - 1.0));
    undo.check ("Paste", inStep (song, phazite::describePaste (clipboard), [&] { phazite::paste (clipboard, s.context); }));
    const auto pasted = selection.getClips();
    REQUIRE (pasted.size() == 2);
    CHECK (std::abs (startOf (song, pasted[0]) - startOf (song, pasted[1])) == near (12.0));
    CHECK (std::max (startOf (song, pasted[0]), startOf (song, pasted[1])) == near (lastBeat));

    checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A MIDI clip's start trimmed past the song's last beat stops at it, and a split past it splits there, its end and "
           "what it plays where they were")
{
    // From 10 beats before the last beat to 10 after it: the start asked 5 beats past the last beat stops at it, 10 beats into
    // the clip, so that its end stays and its notes play where they did (before, the length and the offset were worked out
    // from the beat asked, and the start then kept: its end 5 beats early, its notes 5 beats early).
    TestSong s;
    auto& song = songOf (s);
    const auto clip = s.addMidiClip (s.firstTrack, lastBeat - 10.0, 20.0);
    UndoCheck undo (s.context);

    undo.check ("Trim Clip", inStep (song, "Trim Clip", [&] { phazite::trimClipStart (song, clip, at (lastBeat + 5.0)); }));
    const auto trimmed = phazite::getClipValue (song, clip);
    REQUIRE (trimmed.has_value());
    CHECK (trimmed->start.inBeats() == near (lastBeat));
    CHECK (trimmed->length.inBeats() == near (10.0));
    CHECK (trimmed->offset.inBeats() == near (10.0));
    undo.check ("Trim Clip", inStep (song, "Trim Clip", [&] { phazite::trimClipStart (song, clip, at (lastBeat - 10.0)); }));
    REQUIRE (phazite::getClipValue (song, clip)->offset.inBeats() == near (0.0));

    // Split 5 beats past the last beat: at it, the right half 10 beats into the clip's notes.
    std::vector<phazite::ClipID> parts;
    undo.check ("Split Clip", inStep (song, "Split Clip", [&] { parts = phazite::splitClips (song, { clip }, at (lastBeat + 5.0)); }));
    REQUIRE (parts.size() == 1);
    const auto right = phazite::getClipValue (song, parts.front());
    REQUIRE (right.has_value());
    CHECK (right->start.inBeats() == near (lastBeat));
    CHECK (right->length.inBeats() == near (10.0));
    CHECK (right->offset.inBeats() == near (10.0));
    CHECK (phazite::getClipRange (song, clip).getEnd().inBeats() == near (lastBeat));

    checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("An audio clip's start trimmed past the song's last beat stops at it, its end and what it plays where they were")
{
    // A ten-second file (20 beats at 120 BPM) from 10 beats before the last beat: its start asked 5 beats past the last beat
    // stops at it, its end and the place of its audio in the song as they were.
    TestSong s;
    auto& song = songOf (s);
    juce::AudioBuffer<float> silence (1, 441000);
    silence.clear();
    const auto file = s.getSoundsFolder().getChildFile ("Ten seconds.wav");
    file.getParentDirectory().createDirectory();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
    }

    const auto clip = s.addAudioClip (s.addTrack ("Audio"), file, lastBeat - 10.0);
    const auto before = phazite::getPlayedPosition (song, clip);
    UndoCheck undo (s.context);

    undo.check ("Trim Clip", inStep (song, "Trim Clip", [&] { phazite::trimClipStart (song, clip, at (lastBeat + 5.0)); }));
    CHECK (startOf (song, clip) == near (lastBeat));
    const auto after = phazite::getPlayedPosition (song, clip);
    CHECK (after.end == near (before.end));
    CHECK (after.start - after.offset == near (before.start - before.offset)); // where the file's start plays
    CHECK (after.offset == near (before.offset + 5.0));

    checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A section, a tempo change and a chord put past the song's last beat stand at it; a section longer than the range "
           "lasts as long as it gives")
{
    TestSong s;
    auto& song = songOf (s);
    UndoCheck undo (s.context);

    phazite::SectionID section;
    phazite::SectionInfo info;
    info.name = "Far";
    info.length = lasting (8.0);
    undo.check ("Add Section", inStep (song, "Add Section", [&] { section = phazite::insertSection (song, at (1.2e6), info); }));
    REQUIRE (phazite::getSections (song).size() == 1);
    CHECK (phazite::getSections (song).front().range.getStart().inBeats() == near (lastBeat));
    undo.check ("Resize Section", inStep (song, "Resize Section", [&] { phazite::resizeSection (song, section, lasting (3.0e6), false); }));
    CHECK (phazite::getSections (song).front().range.getLength().inBeats() == near (lastBeat));

    undo.check ("Add Tempo Change", [&] { phazite::addTempoChange (song, at (1.2e6), 100.0); });
    CHECK (phazite::getTempoChanges (song).back().beat.inBeats() == near (lastBeat));

    phazite::Numeral numeral;
    numeral.degree = 4;
    undo.check ("Add Chord", inStep (song, "Add Chord", [&] { phazite::putLaneChord (song, { at (1.2e6), at (1.2e6 + 4.0) }, numeral); }));
    REQUIRE_FALSE (phazite::getLaneChords (song).empty());
    CHECK (phazite::getLaneChords (song).back().range.getStart().inBeats() == near (lastBeat));

    checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Tempo changes and time signatures pushed past the song's last beat become one there, the later, which plays")
{
    // Two of a kind that the bound puts on the last beat: one of them stays, the later of the two, as the map plays two at a
    // beat (engine/core/TempoMap.cpp: the last at or before a beat counts); the file's rule is one of a kind at a beat.
    TestSong s;
    auto& song = songOf (s);
    UndoCheck undo (s.context);

    undo.check ("Add Tempo Change", [&] { phazite::addTempoChange (song, at (lastBeat - 5.0), 100.0); });
    undo.check ("Add Tempo Change", [&] { phazite::addTempoChange (song, at (lastBeat - 2.0), 90.0); });
    undo.check ("Change Time Signatures",
                [&]
                {
                    phazite::setTimeSignatureChanges (
                        song, { { at (0.0), { 4, 4 } }, { at (lastBeat - 5.0), { 3, 4 } }, { at (lastBeat - 2.0), { 7, 8 } } });
                });

    undo.check ("Insert Time",
                inStep (song, "Insert Time", [&] { phazite::insertTempoMapTime (song, at (lastBeat - 10.0), lasting (10.0)); }));
    const auto tempos = phazite::getTempoChanges (song);
    REQUIRE (tempos.size() == 2);
    CHECK (tempos.back().beat.inBeats() == near (lastBeat));
    CHECK (tempos.back().bpm == near (90.0));
    const auto signatures = phazite::getTimeSignatureChanges (song);
    REQUIRE (signatures.size() == 2);
    CHECK (signatures.back().beat.inBeats() == near (lastBeat));
    CHECK (signatures.back().signature.numerator == 7);
    CHECK (signatures.back().signature.denominator == 8);

    checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Automation points dragged past the song's last beat go as far as the last of them can, as far apart as they were")
{
    TestSong s;
    auto& song = songOf (s);
    const auto volume = s.getVolumeParameter (s.firstTrack);
    UndoCheck undo (s.context);

    undo.checkSeveral ("Points",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Add Points");

                           for (const auto beat : { lastBeat - 10.0, lastBeat - 5.0 })
                           {
                               phazite::AutomationPoint point;
                               point.beat = at (beat);
                               point.position = 0.5f;
                               phazite::addPoint (song, volume, point);
                           }
                       });

    phazite::BeatDuration moved;
    undo.check ("Move Points",
                inStep (song, "Move Points", [&] { moved = phazite::movePoints (song, volume, { 0, 1 }, lasting (100.0), 0.0f); }));
    CHECK (moved.inBeats() == near (5.0));
    const auto points = phazite::getPoints (song, volume);
    REQUIRE (points.size() == 2);
    CHECK (points[0].beat.inBeats() == near (lastBeat - 5.0));
    CHECK (points[1].beat.inBeats() == near (lastBeat));

    // One point dragged alone past the end: at it.
    undo.check ("Move Point", inStep (song, "Move Point", [&] { phazite::movePoints (song, volume, { 0 }, lasting (100.0), 0.0f); }));
    CHECK (phazite::getPoints (song, volume)[0].beat.inBeats() == near (lastBeat));

    checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A note placed past a clip that ends at the song's last beat is held where it was placed, the clip grown to hold it")
{
    // Each of the clip's fields within its range (its start before the last beat, its length a few beats) and the note's (its
    // start counts from the clip's content start): what the song holds, though the clip's end is past the last beat.
    TestSong s;
    auto& song = songOf (s);
    const auto clip = s.addMidiClip (s.firstTrack, lastBeat - 4.0, 4.0);
    UndoCheck undo (s.context);
    phazite::NoteRef note;
    undo.check ("Add Note",
                inStep (song, "Add Note", [&] { note = phazite::addNote (song, clip, 60, at (lastBeat + 2.0), lasting (1.0), 100); }));
    const auto data = phazite::getNoteData (song, note);
    REQUIRE (data.has_value());
    CHECK (data->start.inBeats() == near (lastBeat + 2.0));
    CHECK (phazite::getClipRange (song, clip).getEnd().inBeats() >= lastBeat + 3.0);
    checkSong (song);
    undo.walkBackAndForth();
}

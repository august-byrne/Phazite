// SPDX-License-Identifier: AGPL-3.0-or-later
// What the selection commands do (src/model/arrangement/internal/Selection.h), called directly: each acts on the first kind of
// thing selected, in the order notes, time range, sections, clips, tracks.
#include "TestClips.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>

namespace
{
using phazite::test::lasting;

phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// Two tracks, A and B: A holds a MIDI clip at beats 0-4 with two notes, and another clip at 8-12; B holds a clip at
// 20-24. Two sections, an intro at 0-16 and a verse at 16-32 (4/4 at 120 BPM). Nothing is selected.
struct Song
{
    Song()
    {
        const auto tracks = phazite::ensureTracks (song, 2);
        a = tracks[0];
        b = tracks[1];
        phazite::setTrackName (song, a, "A");
        phazite::setTrackName (song, b, "B");
        selection.setSong (&song);

        first = phazite::insertMidiClip (song, a, { beats (0.0), beats (4.0) }, nullptr);
        second = phazite::insertMidiClip (song, a, { beats (8.0), beats (12.0) }, nullptr);
        REQUIRE (first.isValid());
        REQUIRE (second.isValid());
        REQUIRE (phazite::insertMidiClip (song, b, { beats (20.0), beats (24.0) }, nullptr).isValid());
        REQUIRE (phazite::addNote (song, first, 60, beats (0.0), lasting (1.0), 100).isValid());
        REQUIRE (phazite::addNote (song, first, 64, beats (1.0), lasting (1.0), 100).isValid());
        selection.setNoteClips ({ first });

        intro = phazite::insertSection (song, {}, { .name = "Intro", .length = phazite::BeatDuration::fromBeats (16.0) });
        verse = phazite::insertSection (song, phazite::BeatPosition::fromBeats (16.0),
                                        { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) });
        REQUIRE (intro.isValid());
        REQUIRE (verse.isValid());
        song.getUndoManager().clearUndoHistory();
    }

    ~Song() { selection.deselectAll(); }

    // Selects the first clip's notes, and nothing else.
    void selectNotes() { selection.selectNotes (phazite::getRefs (phazite::getClipNotes (song, first))); }

    int countNotes() const { return (int) phazite::getClipNotes (song, first).size(); }

    // A track's clips in song order, in whole beats: "0-4 8-12".
    juce::String describe (phazite::TrackID track) const
    {
        std::vector<phazite::BeatRange> ranges;

        for (const auto clip : phazite::getClips (song, track))
            ranges.push_back (phazite::getClipRange (song, clip));

        std::sort (ranges.begin(), ranges.end(), [] (phazite::BeatRange x, phazite::BeatRange y) { return x.getStart() < y.getStart(); });
        juce::StringArray parts;

        for (const auto& clipRange : ranges)
            parts.add (juce::String (juce::roundToInt (clipRange.getStart().inBeats())) + "-"
                       + juce::String (juce::roundToInt (clipRange.getEnd().inBeats())));

        return parts.joinIntoString (" ");
    }

    phazite::Song::Ptr songPtr = phazite::createSong();
    phazite::Song& song = *songPtr;
    phazite::TrackID a, b;
    phazite::ClipID first, second;
    phazite::SectionID intro, verse;
    phazite::Selection selection;
    phazite::RangeSelection range { selection };
};
} // namespace

TEST_CASE ("Delete takes notes first, then the range, sections, clips and tracks")
{
    Song s;

    // Notes, before a range over the second clip.
    s.selectNotes();
    s.range.set ({ beats (8.0), beats (12.0) }, { s.a });
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (s.countNotes() == 0);
    CHECK (s.describe (s.a) == juce::String ("0-4 8-12"));

    // The range, before a section: what lies in it.
    s.selection.selectOnly (s.verse);
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (s.describe (s.a) == juce::String ("0-4"));
    CHECK (phazite::getSections (s.song).size() == 2);

    // A section, with what is in it (B's clip), before a clip and a track.
    s.range.clear();
    s.selection.addToSelection (s.first);
    s.selection.addToSelection (s.b);
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (phazite::getSections (s.song).size() == 1);
    CHECK (s.describe (s.b).isEmpty());
    CHECK (s.describe (s.a) == juce::String ("0-4"));

    // A clip, before a track.
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (s.describe (s.a).isEmpty());
    CHECK (phazite::getTrackIDs (s.song).size() == 2);

    // And then the track, and the one that takes its place is selected.
    phazite::deleteSelection (s.selection, s.range, false);
    REQUIRE (phazite::getTrackIDs (s.song).size() == 1);
    CHECK (s.selection.isSelected (s.a));
}

TEST_CASE ("Delete and Close Gap leaves tracks alone")
{
    Song s;

    // A clip goes, and what comes after it on its track moves back.
    s.selection.selectOnly (s.first);
    phazite::deleteSelection (s.selection, s.range, true);
    CHECK (s.describe (s.a) == juce::String ("4-8"));

    // A track stays.
    s.selection.selectOnly (s.b);
    phazite::deleteSelection (s.selection, s.range, true);
    CHECK (phazite::getTrackIDs (s.song).size() == 2);
    CHECK (s.selection.isSelected (s.b));
}

TEST_CASE ("Duplicate takes notes first, then the range, sections, clips and tracks; as aliases it skips notes, the range and tracks")
{
    Song s;

    // Notes, before the range: the copies go right after them and are selected instead.
    s.selectNotes();
    s.range.set ({ beats (8.0), beats (12.0) }, { s.a });
    phazite::duplicateSelection (s.selection, s.range, false);
    CHECK (s.countNotes() == 4);
    CHECK (phazite::getSelectedNotes (s.selection).size() == 2);
    CHECK (s.describe (s.a) == juce::String ("0-4 8-12"));

    // As aliases, notes and the range are skipped: with nothing else selected, nothing is copied.
    phazite::duplicateSelection (s.selection, s.range, true);
    CHECK (s.countNotes() == 4);
    CHECK (s.describe (s.a) == juce::String ("0-4 8-12"));

    // The range, in an undo step: its copy goes right after it, and becomes the range. Undo puts the range back, and
    // Redo moves it to the copy again.
    s.selection.deselectAll();
    {
        const phazite::UndoStep step (s.song, "Duplicate Range");
        phazite::duplicateSelection (s.selection, s.range, false);
    }
    CHECK (s.describe (s.a) == juce::String ("0-4 8-12 12-16"));
    CHECK (s.range.getRange().getStart().inBeats() == doctest::Approx (12.0));
    phazite::undo (s.song);
    CHECK (s.describe (s.a) == juce::String ("0-4 8-12"));
    CHECK (s.range.getRange().getStart().inBeats() == doctest::Approx (8.0));
    phazite::redo (s.song);
    CHECK (s.describe (s.a) == juce::String ("0-4 8-12 12-16"));
    CHECK (s.range.getRange().getStart().inBeats() == doctest::Approx (12.0));

    // Sections, before clips: the copy, with what is in the section, is selected instead.
    s.range.clear();
    s.selection.selectOnly (s.verse);
    s.selection.addToSelection (s.first);
    phazite::duplicateSelection (s.selection, s.range, false);
    CHECK (phazite::getSections (s.song).size() == 3);
    CHECK (phazite::getSelectedSections (s.selection).size() == 1);
    CHECK_FALSE (phazite::hasSelectedClips (s.selection));
    CHECK (s.describe (s.b) == juce::String ("20-24 36-40"));

    // Clips, before tracks, as aliases too.
    s.selection.selectOnly (s.second);
    s.selection.addToSelection (s.b);
    phazite::duplicateSelection (s.selection, s.range, true);
    const auto copies = phazite::getSelectedClips (s.selection);
    REQUIRE (copies.size() == 1);
    CHECK (phazite::hasAliases (s.song, { copies.front() }));
    CHECK_FALSE (s.selection.isSelected (s.b));

    // Tracks last, and never as aliases.
    s.selection.selectOnly (s.b);
    phazite::duplicateSelection (s.selection, s.range, true);
    CHECK (phazite::getTrackIDs (s.song).size() == 2);
    phazite::duplicateSelection (s.selection, s.range, false);
    CHECK (phazite::getTrackIDs (s.song).size() == 3);
}

TEST_CASE ("Split splits a range's clips at both edges, or the selected clips at the playhead")
{
    Song s;

    s.range.set ({ beats (1.0), beats (3.0) }, { s.a });
    phazite::splitSelection (s.selection, s.range);
    CHECK (s.describe (s.a) == juce::String ("0-1 1-3 3-4 8-12"));

    // Without a range: the selected clips, at the playhead (beat 10).
    s.range.clear();
    phazite::movePlayhead (s.song, beats (10.0));
    s.selection.selectOnly (s.second);
    phazite::splitSelection (s.selection, s.range);
    CHECK (s.describe (s.a) == juce::String ("0-1 1-3 3-4 8-10 10-12"));
    CHECK (s.describe (s.b) == juce::String ("20-24"));
}

TEST_CASE ("Select All and Clear Selection")
{
    Song s;

    // Every clip (sections aren't clips here), and the range is cleared.
    s.range.set ({ beats (8.0), beats (12.0) }, { s.a });
    phazite::selectAllClipsOrTracks (s.selection, s.range);
    CHECK (phazite::getSelectedClips (s.selection).size() == 3);
    CHECK (s.range.isEmpty());

    // Every track while tracks are selected.
    s.selection.selectOnly (s.a);
    phazite::selectAllClipsOrTracks (s.selection, s.range);
    CHECK (phazite::getSelectedTracks (s.selection).size() == 2);
    CHECK_FALSE (phazite::hasSelectedClips (s.selection));

    // Clear: the range and everything else.
    s.range.set ({ beats (8.0), beats (12.0) }, { s.a });
    CHECK (phazite::isAnythingSelected (s.selection, s.range));
    phazite::clearSelection (s.selection, s.range);
    CHECK (s.range.isEmpty());
    CHECK (s.selection.getNumObjectsSelected() == 0);
    CHECK_FALSE (phazite::isAnythingSelected (s.selection, s.range));
}

TEST_CASE ("Mute acts on the selected notes, and on the selected clips while no notes are")
{
    Song s;

    s.selectNotes();
    CHECK_FALSE (phazite::isSelectionMuted (s.selection));
    phazite::toggleSelectionMuted (s.selection);
    CHECK (phazite::isSelectionMuted (s.selection));
    const auto notes = phazite::getClipNotes (s.song, s.first);
    CHECK (std::all_of (notes.begin(), notes.end(), [] (const phazite::NoteData& note) { return note.muted; }));
    CHECK_FALSE (phazite::areAllClipsMuted (s.song, { s.first }));

    // With no notes selected, the clips.
    s.selection.selectOnly (s.first);
    s.selection.addToSelection (s.second);
    CHECK_FALSE (phazite::isSelectionMuted (s.selection));
    phazite::toggleSelectionMuted (s.selection);
    CHECK (phazite::areAllClipsMuted (s.song, { s.first }));
    CHECK (phazite::areAllClipsMuted (s.song, { s.second }));
    CHECK (phazite::isSelectionMuted (s.selection));
    phazite::toggleSelectionMuted (s.selection);
    CHECK_FALSE (phazite::areAllClipsMuted (s.song, { s.first }));
}

TEST_CASE ("When the selection commands are enabled")
{
    Song s;

    // Delete and Duplicate; Delete and Close Gap and Split; Duplicate as Alias; Mute; Deselect All.
    const auto check = [&s] (bool canDelete, bool rangeOrClips, bool asAliases, bool canMute, bool anything)
    {
        CHECK (phazite::canDeleteSelection (s.selection, s.range) == canDelete);
        CHECK (phazite::hasSelectedRangeOrClips (s.selection, s.range) == rangeOrClips);
        CHECK (phazite::canDuplicateAsAliases (s.selection) == asAliases);
        CHECK (phazite::canMuteSelection (s.selection) == canMute);
        CHECK (phazite::isAnythingSelected (s.selection, s.range) == anything);
    };

    SUBCASE ("Nothing selected")
    {
        check (false, false, false, false, false);
    }

    SUBCASE ("A range")
    {
        s.range.set ({ beats (8.0), beats (12.0) }, { s.a });
        check (true, true, false, false, true);
    }

    SUBCASE ("A clip")
    {
        s.selection.selectOnly (s.first);
        check (true, true, true, true, true);
    }

    SUBCASE ("A track")
    {
        s.selection.selectOnly (s.a);
        check (true, false, false, false, true);
    }

    SUBCASE ("A section")
    {
        s.selection.selectOnly (s.intro);
        check (true, false, true, false, true);
    }

    SUBCASE ("Notes")
    {
        s.selectNotes();
        check (true, false, false, true, true);
    }
}

TEST_CASE ("Phazite's selection holds items by their IDs: one kind at a time, and nothing of an item that is gone")
{
    Song s;
    auto& selection = s.selection;
    int changes = 0;

    struct Counter : public juce::ChangeListener
    {
        explicit Counter (int& c)
            : count (c)
        {
        }
        void changeListenerCallback (juce::ChangeBroadcaster*) override { ++count; }
        int& count;
    } counter { changes };

    selection.addChangeListener (&counter);

    // Clips and tracks together, each kind in the order it was selected; a device starts a selection of its own.
    selection.selectOnly (s.second);
    selection.addToSelection (s.b);
    selection.addToSelection (s.first);
    selection.addToSelection (s.a);
    CHECK (selection.getClips() == std::vector { s.second, s.first });
    CHECK (selection.getTracks() == std::vector { s.b, s.a });
    CHECK (selection.getNumObjectsSelected() == 4);

    const auto device = phazite::insertDeviceState (s.song, s.b, phazite::makeOwnDeviceState (s.song, "phaziteEq"));
    REQUIRE (device.isValid());
    selection.addToSelection (device);
    CHECK (selection.getDevices() == std::vector { device });
    CHECK (selection.getClips().empty());
    CHECK (selection.getTracks().empty());
    selection.addToSelection (s.b);
    CHECK (selection.getTracks() == std::vector { s.b });
    CHECK (selection.getDevices().empty());

    // Notes are all that is selected while they are, and only those of the clips the piano roll shows.
    s.selectNotes();
    CHECK (selection.getTracks().empty());
    CHECK (selection.getNumObjectsSelected() == 2);
    CHECK (phazite::getSelectedNotes (selection).size() == 2);
    selection.setNoteClips ({ s.second });
    CHECK (phazite::getSelectedNotes (selection).empty());

    // A clip that is deleted is selected no more, without a word from it.
    selection.selectOnly (s.second);
    phazite::deleteClips (s.song, { s.second }, false);
    CHECK (selection.getClips().empty());
    CHECK (selection.getNumObjectsSelected() == 0);

    // Another song: nothing of the last one, and a change to hear.
    phazite::test::dispatchPendingMessages();
    const auto before = changes;
    selection.selectOnly (s.a);
    selection.setSong (nullptr);
    CHECK (selection.getSong() == nullptr);
    CHECK_FALSE (selection.isSelected (s.a));
    phazite::test::dispatchPendingMessages();
    CHECK (changes > before);
    selection.removeChangeListener (&counter);
}

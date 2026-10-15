// SPDX-License-Identifier: AGPL-3.0-or-later
// What the selection commands do (src/model/arrangement/Selection.h), called directly: each acts on the first kind of
// thing selected, in the order notes, time range, sections, clips, tracks.
#include "TestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/Selection.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/Undo.h"
#include "model/notes/Notes.h"

#include <algorithm>

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// Two tracks, A and B: A holds a MIDI clip at beats 0-4 with two notes, and another clip at 8-12; B holds a clip at
// 20-24. Two sections, an intro at 0-16 and a verse at 16-32 (4/4 at 120 BPM). Nothing is selected.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");
        selection.edit = edit.get();

        first = phazite::insertMidiClip (*a, { beats (0.0), beats (4.0) }, nullptr);
        second = phazite::insertMidiClip (*a, { beats (8.0), beats (12.0) }, nullptr);
        REQUIRE (first != nullptr);
        REQUIRE (second != nullptr);
        REQUIRE (phazite::insertMidiClip (*b, { beats (20.0), beats (24.0) }, nullptr) != nullptr);
        REQUIRE (phazite::addNote (*first, 60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
        REQUIRE (phazite::addNote (*first, 64, beats (1.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
        events = std::make_unique<te::SelectedMidiEvents> (*first);

        intro = phazite::insertSection (*edit, beats (0.0), { .name = "Intro", .length = te::BeatDuration::fromBeats (16.0) });
        verse = phazite::insertSection (*edit, beats (16.0), { .name = "Verse", .length = te::BeatDuration::fromBeats (16.0) });
        REQUIRE (intro != nullptr);
        REQUIRE (verse != nullptr);
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song()
    {
        selection.deselectAll();
        events = nullptr;
    }

    // Selects the first clip's notes, and nothing else: Tracktion selects their events object alone.
    void selectNotes() { events->setSelected (selection, first->getSequence().getNotes(), false); }

    int countNotes() const { return first->getSequence().getNumNotes(); }

    // A track's clips in song order, in whole beats: "0-4 8-12".
    static juce::String describe (te::ClipTrack& track)
    {
        juce::Array<te::Clip*> clips (track.getClips());
        std::sort (clips.begin(), clips.end(), [] (te::Clip* x, te::Clip* y) { return x->getStartBeat() < y->getStartBeat(); });
        juce::StringArray parts;

        for (auto* clip : clips)
            parts.add (juce::String (juce::roundToInt (clip->getStartBeat().inBeats())) + "-"
                       + juce::String (juce::roundToInt (clip->getEndBeat().inBeats())));

        return parts.joinIntoString (" ");
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
    te::MidiClip* first = nullptr;
    te::MidiClip* second = nullptr;
    te::ArrangerClip* intro = nullptr;
    te::ArrangerClip* verse = nullptr;
    std::unique_ptr<te::SelectedMidiEvents> events;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
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
    CHECK (Song::describe (*s.a) == juce::String ("0-4 8-12"));

    // The range, before a section: what lies in it.
    s.selection.selectOnly (s.verse);
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (Song::describe (*s.a) == juce::String ("0-4"));
    CHECK (phazite::getSections (*s.edit).size() == 2);

    // A section, with what is in it (B's clip), before a clip and a track.
    s.range.clear();
    s.selection.addToSelection (s.first);
    s.selection.addToSelection (s.b);
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (phazite::getSections (*s.edit).size() == 1);
    CHECK (Song::describe (*s.b).isEmpty());
    CHECK (Song::describe (*s.a) == juce::String ("0-4"));

    // A clip, before a track.
    phazite::deleteSelection (s.selection, s.range, false);
    CHECK (Song::describe (*s.a).isEmpty());
    CHECK (te::getAudioTracks (*s.edit).size() == 2);

    // And then the track, and the one that takes its place is selected.
    phazite::deleteSelection (s.selection, s.range, false);
    REQUIRE (te::getAudioTracks (*s.edit).size() == 1);
    CHECK (s.selection.isSelected (s.a));
}

TEST_CASE ("Delete and Close Gap leaves tracks alone")
{
    Song s;

    // A clip goes, and what comes after it on its track moves back.
    s.selection.selectOnly (s.first);
    phazite::deleteSelection (s.selection, s.range, true);
    CHECK (Song::describe (*s.a) == juce::String ("4-8"));

    // A track stays.
    s.selection.selectOnly (s.b);
    phazite::deleteSelection (s.selection, s.range, true);
    CHECK (te::getAudioTracks (*s.edit).size() == 2);
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
    CHECK (Song::describe (*s.a) == juce::String ("0-4 8-12"));

    // As aliases, notes and the range are skipped: with nothing else selected, nothing is copied.
    phazite::duplicateSelection (s.selection, s.range, true);
    CHECK (s.countNotes() == 4);
    CHECK (Song::describe (*s.a) == juce::String ("0-4 8-12"));

    // The range, in an undo step: its copy goes right after it, and becomes the range. Undo puts the range back, and
    // Redo moves it to the copy again.
    s.selection.deselectAll();
    {
        const phazite::UndoStep step (*s.edit, "Duplicate Range");
        phazite::duplicateSelection (s.selection, s.range, false);
    }
    CHECK (Song::describe (*s.a) == juce::String ("0-4 8-12 12-16"));
    CHECK (s.range.getRange().getStart().inBeats() == doctest::Approx (12.0));
    phazite::undo (*s.edit);
    CHECK (Song::describe (*s.a) == juce::String ("0-4 8-12"));
    CHECK (s.range.getRange().getStart().inBeats() == doctest::Approx (8.0));
    phazite::redo (*s.edit);
    CHECK (Song::describe (*s.a) == juce::String ("0-4 8-12 12-16"));
    CHECK (s.range.getRange().getStart().inBeats() == doctest::Approx (12.0));

    // Sections, before clips: the copy, with what is in the section, is selected instead.
    s.range.clear();
    s.selection.selectOnly (s.verse);
    s.selection.addToSelection (s.first);
    phazite::duplicateSelection (s.selection, s.range, false);
    CHECK (phazite::getSections (*s.edit).size() == 3);
    CHECK (phazite::getSelectedSections (s.selection).size() == 1);
    CHECK_FALSE (phazite::hasSelectedClips (s.selection));
    CHECK (Song::describe (*s.b) == juce::String ("20-24 36-40"));

    // Clips, before tracks, as aliases too.
    s.selection.selectOnly (s.second);
    s.selection.addToSelection (s.b);
    phazite::duplicateSelection (s.selection, s.range, true);
    const auto copies = phazite::getSelectedClips (s.selection);
    REQUIRE (copies.size() == 1);
    CHECK (copies.getFirst()->isLinked());
    CHECK_FALSE (s.selection.isSelected (s.b));

    // Tracks last, and never as aliases.
    s.selection.selectOnly (s.b);
    phazite::duplicateSelection (s.selection, s.range, true);
    CHECK (te::getAudioTracks (*s.edit).size() == 2);
    phazite::duplicateSelection (s.selection, s.range, false);
    CHECK (te::getAudioTracks (*s.edit).size() == 3);
}

TEST_CASE ("Split splits a range's clips at both edges, or the selected clips at the playhead")
{
    Song s;

    s.range.set ({ beats (1.0), beats (3.0) }, { s.a });
    phazite::splitSelection (s.selection, s.range);
    CHECK (Song::describe (*s.a) == juce::String ("0-1 1-3 3-4 8-12"));

    // Without a range: the selected clips, at the playhead (beat 10, 5 seconds in at 120 BPM).
    s.range.clear();
    s.edit->getTransport().setPosition (te::TimePosition::fromSeconds (5.0));
    s.selection.selectOnly (s.second);
    phazite::splitSelection (s.selection, s.range);
    CHECK (Song::describe (*s.a) == juce::String ("0-1 1-3 3-4 8-10 10-12"));
    CHECK (Song::describe (*s.b) == juce::String ("20-24"));
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

TEST_CASE ("Mute acts on the selected notes rather than their clips")
{
    Song s;

    s.selectNotes();
    s.selection.addToSelection (s.first);
    CHECK_FALSE (phazite::isSelectionMuted (s.selection));
    phazite::toggleSelectionMuted (s.selection);
    CHECK (phazite::isSelectionMuted (s.selection));
    CHECK (std::all_of (s.first->getSequence().getNotes().begin(), s.first->getSequence().getNotes().end(),
                        [] (te::MidiNote* note) { return note->isMute(); }));
    CHECK_FALSE (s.first->isMuted());

    // With no notes selected, the clips.
    s.selection.selectOnly (s.first);
    s.selection.addToSelection (s.second);
    CHECK_FALSE (phazite::isSelectionMuted (s.selection));
    phazite::toggleSelectionMuted (s.selection);
    CHECK (s.first->isMuted());
    CHECK (s.second->isMuted());
    CHECK (phazite::isSelectionMuted (s.selection));
    phazite::toggleSelectionMuted (s.selection);
    CHECK_FALSE (s.first->isMuted());
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

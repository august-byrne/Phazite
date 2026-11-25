// SPDX-License-Identifier: AGPL-3.0-or-later The song's sections as an area of its values (model/base/internal/SongValues.h,
// SectionsArea.h; roadmap step S5.20, the sections' switch). What hears the sections change hears it from one place
// (Listener::sectionsChanged()), which the sections' area tells, by the IDs of the sections added, taken out or changed: for each
// edit made through the model, once for each change of the value, its Undo and its Redo once, and nothing of another area;
// another area's change tells it nothing. A song's file writes a SECTIONS while the song has a section, and none for an empty
// list. A set equal to the sections held makes no step; every edit (the edits of time, copy and paste) is one exact undo step; a
// section selected stays so across its removal's Undo, as the selection holds it by its ID; a pasted section's ID is past every
// ID the song holds; the project's unsaved check compares them as one value; and the versions give the song another's sections,
// or none, through the sections' code, the whole song's and the song's own line alike, and a merge's clash on a section's
// property or on a whole section is chosen each way.
//   phazite_tests.exe --test-case="Song values' sections*"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsArea.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

namespace
{
using phazite::test::at;
using phazite::test::lasting;
namespace schema = phazite::schema;
using phazite::SectionID;

// How many calls the values' listener heard, of each area, and the sections it was told of.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }
    void chordLaneChanged() override { ++chords; }
    void midiLinksChanged() override { ++links; }

    void sectionsChanged (const std::vector<SectionID>& changed) override
    {
        ++sections;
        calls.push_back (changed);
        named.insert (changed.begin(), changed.end());
    }

    int notes = 0, clips = 0, tempo = 0, chords = 0, links = 0, sections = 0;
    std::vector<std::vector<SectionID>> calls;
    std::set<SectionID> named;
};

// A song in a project of its own, with its versions and the values' listener recording what they are told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    struct Heard
    {
        Told told;
        juce::int64 project = 0, versions = 0;
        std::uint64_t ids = 0; // how far the song's count of IDs moved on (Song.h: makeID())
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        const auto idsBefore = phazite::getNextID (song);
        change();
        return { told, s.getProject().getChangeCount() - projectBefore, versions->countSongChanges() - versionsBefore,
                 phazite::getNextID (song) - idsBefore };
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
};

phazite::SectionsValue heldSections (const phazite::Song& song)
{
    return phazite::getSectionsValue (song);
}

bool hasSectionsPlace (const phazite::Song& song)
{
    return phazite::test::copyWholeSong (song).getChildWithName (schema::SECTIONS).isValid();
}

std::set<SectionID> idsOf (std::initializer_list<SectionID> ids)
{
    return { ids };
}

std::vector<juce::String> describeSections (const phazite::Song& song)
{
    std::vector<juce::String> lines;

    for (const auto& section : phazite::getSections (song))
        lines.push_back (section.name + " " + juce::String (section.range.getStart().inBeats()) + " "
                         + juce::String (section.range.getLength().inBeats()));

    return lines;
}

// A song of its own (no project), with its first section added and its history cleared.
struct SectionsSong
{
    SectionsSong()
    {
        verse = phazite::insertSection (*song, at (0.0), { .name = "Verse", .length = lasting (16.0), .role = "verse" });
        chorus = phazite::insertSection (*song, at (16.0), { .name = "Chorus", .length = lasting (8.0), .role = "chorus" });
        song->getUndoManager().clearUndoHistory();
    }

    phazite::Song::Ptr song = phazite::createSong();
    SectionID verse, chorus;
};
} // namespace

TEST_CASE ("Song values' sections: each edit through the model tells the sections changed by their IDs, once for each write, its "
           "Undo and its Redo once, and nothing of another area")
{
    Watched w;
    auto& song = w.song;
    REQUIRE_FALSE (hasSectionsPlace (song));
    SectionID verse, chorus, bridge;

    // Every edit is heard by what counts from the area alone, the first section's too. An edit that writes the sections more
    // than once in its step (an edit of time and the section after it) is told each write; its Undo and its Redo once, the
    // whole step's.
    const auto check = [&w] (const juce::String& name, const std::function<void()>& change,
                             const std::function<std::set<SectionID>()>& named, int writes = 1, int idsMade = 0)
    {
        INFO (name.toStdString());
        const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
        const auto heard = w.hearEach (name, change);
        REQUIRE (heard.size() == moments.size());
        const auto expected = named();

        for (size_t i = 0; i < heard.size(); ++i)
        {
            INFO (moments[i]);
            CHECK (heard[i].told.sections == (i == 0 ? writes : 1));
            CHECK (heard[i].told.named == expected);
            CHECK (heard[i].told.clips == 0);
            CHECK (heard[i].told.notes == 0);
            CHECK (heard[i].told.tempo == 0);
            CHECK (heard[i].told.chords == 0);
            CHECK (heard[i].told.links == 0);
            CHECK (heard[i].project == heard[i].told.sections);
            CHECK (heard[i].versions == heard[i].told.sections);

            // The song's count of IDs moves on in the step alone (no Undo takes it back), told to no one: neither the project
            // nor the versions count it (Song.h: makeID()).
            CHECK (heard[i].ids == (i == 0 ? (std::uint64_t) idsMade : 0));

            for (const auto& call : heard[i].told.calls) // each section named once in a call
                CHECK (std::set<SectionID> (call.begin(), call.end()).size() == call.size());
        }
    };

    check ("Add Section", [&] { verse = phazite::addSectionAt (w.song, at (0.0), nullptr); }, [&] { return idsOf ({ verse }); }, 1, 1);
    CHECK (hasSectionsPlace (song));
    check ("Add Section", [&] { chorus = phazite::appendSection (w.song, nullptr); }, [&] { return idsOf ({ chorus }); }, 1, 1);
    check ("Add Section", [&] { bridge = phazite::appendSection (w.song, nullptr); }, [&] { return idsOf ({ bridge }); }, 1, 1);
    check ("Rename Section", [&] { phazite::renameSection (w.song, verse, "Intro"); }, [&] { return idsOf ({ verse }); });
    check (
        "Change Section Color", [&] { phazite::setSectionColour (w.song, chorus, juce::Colours::orange); },
        [&] { return idsOf ({ chorus }); });
    check ("Resize Section", [&] { phazite::resizeSection (w.song, bridge, lasting (12.0), false); }, [&] { return idsOf ({ bridge }); });

    // Longer with what comes after it: the time inserted moves the sections after it (one write), then it gets longer (another).
    check (
        "Resize Section", [&] { phazite::resizeSection (w.song, verse, lasting (40.0), true); },
        [&] { return idsOf ({ verse, chorus, bridge }); }, 2);

    // Moved past another: both move, in one write.
    check (
        "Move Section", [&] { phazite::moveSection (w.song, verse, phazite::getSection (w.song, chorus)->range.getEnd()); },
        [&] { return idsOf ({ verse, chorus }); });

    // Copied right after the last: no room to make for it (the time inserted moves nothing: no write), then the copy pasted,
    // with an ID of its own.
    SectionID copy;
    check ("Copy Section", [&] { copy = phazite::duplicateSection (w.song, bridge, false); }, [&] { return idsOf ({ copy }); }, 1, 1);
    CHECK (copy.isValid());

    // Copied to the song's start: room made (one write, every section moved), then the copy pasted (another).
    SectionID first;
    check (
        "Copy Section", [&] { first = phazite::copySection (w.song, copy, at (0.0), false); },
        [&] { return idsOf ({ first, chorus, verse, bridge, copy }); }, 2, 1);
    check (
        "Delete Section", [&] { phazite::deleteSection (w.song, first); }, [&] { return idsOf ({ first, chorus, verse, bridge, copy }); });
    CHECK (heldSections (song).sections.size() == 4);

    check ("Delete Section", [&] { phazite::deleteSection (w.song, chorus); }, [&] { return idsOf ({ chorus, verse, bridge, copy }); });

    // Every section taken away: an empty list, which a file writes no SECTIONS for.
    check ("Remove Sections", [&] { phazite::removeSections (w.song); }, [&] { return idsOf ({ verse, bridge, copy }); });
    CHECK_FALSE (hasSectionsPlace (song));
    CHECK (heldSections (song).sections.empty());
    phazite::test::checkSong (song);

    // A section given the name or the colour it has, and one that isn't there renamed: no change, nothing told, no step.
    phazite::undo (song);
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    const auto same = w.hear (
        [&]
        {
            const phazite::UndoStep step (song, "Nothing");
            phazite::renameSection (song, verse, phazite::getSection (song, verse)->name);
            phazite::renameSection (song, SectionID::fromRaw (999999), "Nowhere");
            phazite::removeSectionTime (song, { at (100000.0), at (100004.0) });
        });
    CHECK (same.told.sections == 0);
    CHECK (same.project == 0);
    CHECK (same.versions == 0);
    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
}

TEST_CASE ("Song values' sections: another area's change tells nothing of the sections, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());

    {
        const phazite::UndoStep step (song, "Add Section");
        REQUIRE (phazite::addSectionAt (song, at (0.0), nullptr).isValid());
    }

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
        {
            CHECK (heard.told.sections == 0);
        }
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' sections: sections set equal to those the song holds make no undo step, tell nothing and keep the value "
           "held")
{
    SectionsSong s;
    auto& song = *s.song;
    auto& values = *song.getValues();
    const auto held = values.sections.get();
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    Told told;
    values.addListener (&told);

    // The same sections in a new vector: alike, not the same value.
    const phazite::SectionsValue alike { immer::flex_vector<phazite::SectionValue> (held.sections.begin(), held.sections.end()) };
    REQUIRE_FALSE (alike.isSameAs (held));

    {
        const phazite::UndoStep step (song, "Nothing");
        values.setSections (song, alike);
    }

    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
    CHECK (told.sections == 0);
    CHECK (values.sections.get().isSameAs (held));
    values.removeListener (&told);
}

TEST_CASE ("Song values' sections: the SECTIONS its first section makes goes with its Undo, and every writer of the sections, the "
           "edits of time and copy and paste among them, is one exact undo step")
{
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    const auto track = t.firstTrack;
    REQUIRE (t.addMidiClip (track, 0.0, 8.0).isValid());
    REQUIRE (t.addMidiClip (track, 16.0, 8.0).isValid());
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    const auto edit = [&song, &undo] (const juce::String& name, const std::function<void()>& change)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        change();
                    });
    };

    // The first section: a file writes a SECTIONS with it; its Undo takes it back, and the SECTIONS with it.
    SectionID verse;
    edit ("Add Section", [&] { verse = phazite::addSectionAt (song, at (0.0), nullptr); });
    REQUIRE (hasSectionsPlace (song));
    phazite::undo (song);
    CHECK_FALSE (hasSectionsPlace (song));
    CHECK (heldSections (song) == phazite::SectionsValue {});
    phazite::redo (song);
    REQUIRE (phazite::getSection (song, verse).has_value());

    // Every writer, each one step.
    SectionID chorus, bridge;
    edit ("Add Section", [&] { chorus = phazite::appendSection (song, nullptr); });
    edit ("Add Section", [&] { bridge = phazite::appendSection (song, nullptr); });
    edit ("Rename Section", [&] { phazite::renameSection (song, chorus, "  Hook  "); });
    CHECK (phazite::getSection (song, chorus)->name == "Hook");
    edit ("Change Section Color", [&] { phazite::setSectionColour (song, chorus, juce::Colours::red); });
    edit ("Resize Section", [&] { phazite::resizeSection (song, verse, lasting (24.0), false); });
    edit ("Resize Section", [&] { phazite::resizeSection (song, verse, lasting (40.0), false); }); // up to the next at most
    CHECK (phazite::getSection (song, verse)->range.getEnd() == phazite::getSection (song, chorus)->range.getStart());
    edit ("Resize Section", [&] { phazite::resizeSection (song, verse, lasting (36.0), true); });
    edit ("Resize Section", [&] { phazite::resizeSection (song, verse, lasting (20.0), true); });
    edit ("Move Section", [&] { phazite::moveSection (song, verse, phazite::getSectionBoundaries (song).back()); });
    edit ("Copy Section", [&] { CHECK (phazite::copySection (song, chorus, at (0.0), false).isValid()); });
    edit ("Duplicate Sections", [&] { CHECK (phazite::duplicateSections (song, { chorus, bridge }, true, nullptr).size() == 2); });
    edit ("Delete Sections", [&] { phazite::deleteSections (song, { verse, bridge }); });

    // The edits of time on their own: the sections from it on move, one it is inside of gets longer, one inside the range goes.
    edit ("Insert Time", [&] { phazite::insertTime (song, at (4.0), lasting (8.0)); });
    edit ("Remove Time", [&] { phazite::removeTime (song, { at (2.0), at (10.0) }); });
    edit ("Move Time", [&] { phazite::moveTime (song, { at (0.0), at (8.0) }, at (24.0)); });
    edit ("Copy Time", [&] { phazite::copyTime (song, { at (0.0), at (16.0) }, at (32.0), false); });
    edit ("Remove Sections", [&] { phazite::removeSections (song); });
    CHECK_FALSE (hasSectionsPlace (song));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' sections: the edits of time move, lengthen, shorten, take away and copy the sections as they did")
{
    SectionsSong s;
    auto& song = *s.song;
    // Verse 0-16, Chorus 16-24.

    phazite::insertSectionTime (song, at (4.0), lasting (4.0)); // inside the verse: it gets longer, the chorus moves
    CHECK (describeSections (song) == std::vector<juce::String> { "Verse 0 20", "Chorus 20 8" });

    phazite::removeSectionTime (song, { at (18.0), at (22.0) }); // the verse's end and the chorus's start
    CHECK (describeSections (song) == std::vector<juce::String> { "Verse 0 18", "Chorus 18 6" });

    phazite::moveSectionTime (song, { at (18.0), at (24.0) }, at (0.0)); // the chorus to the start, the verse after it
    CHECK (describeSections (song) == std::vector<juce::String> { "Chorus 0 6", "Verse 6 18" });

    const auto copy = phazite::copySections (song, { at (0.0), at (24.0) });
    REQUIRE (copy.sections.size() == 2);
    CHECK (copy.sections[0].id == s.chorus); // in song order
    phazite::pasteSections (song, copy, at (24.0));
    CHECK (describeSections (song) == std::vector<juce::String> { "Chorus 0 6", "Verse 6 18", "Chorus 24 6", "Verse 30 18" });

    phazite::removeSectionTime (song, { at (0.0), at (24.0) }); // the first two inside it go
    CHECK (describeSections (song) == std::vector<juce::String> { "Chorus 0 6", "Verse 6 18" });
    CHECK_FALSE (phazite::getSection (song, s.chorus).has_value());
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' sections: a pasted section's ID is past every ID the song holds, and the song counts on past its sections'")
{
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    const auto clip = t.addMidiClip (t.firstTrack, 0.0, 4.0);
    t.addNote (clip, 60, 0.0, 1.0);

    SectionID verse;

    {
        const phazite::UndoStep step (song, "Add Section");
        verse = phazite::addSectionAt (song, at (0.0), nullptr);
    }

    // The song's next ID past its sections' (IDsHeld.h: the sections' area counted).
    CHECK (phazite::findLargestIDHeld (song) >= verse.getRaw());
    CHECK (phazite::findNextID (song) > verse.getRaw());

    const auto largestBefore = std::max (phazite::findLargestIDHeld (song), phazite::findLargestID (phazite::test::copyWholeSong (song)));
    SectionID copy;

    {
        const phazite::UndoStep step (song, "Copy Section");
        copy = phazite::copySection (song, verse, phazite::getSection (song, verse)->range.getEnd(), false);
    }

    REQUIRE (copy.isValid());
    CHECK (copy.getRaw() > largestBefore);
    CHECK (copy != verse);
    phazite::test::checkSong (song); // every ID once across the tree and the values (the id rule, on the song whole)
}

TEST_CASE ("Song values' sections: a section selected stays selected by its ID: gone with its removal, selected again with its Undo")
{
    // As the selection always held sections (Selection.cpp: isThere(), which asks the sections' area now): by their IDs,
    // each there or not when it is asked.
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    auto& selection = phazite::getSelection (t.context);
    SectionID verse, chorus;

    {
        const phazite::UndoStep step (song, "Sections");
        verse = phazite::addSectionAt (song, at (0.0), nullptr);
        chorus = phazite::appendSection (song, nullptr);
    }

    selection.selectOnly (chorus);
    REQUIRE (selection.isSelected (chorus));

    {
        const phazite::UndoStep step (song, "Delete Section");
        phazite::deleteSection (song, chorus);
    }

    CHECK_FALSE (selection.isSelected (chorus));
    CHECK (selection.getNumObjectsSelected() == 0);
    phazite::undo (song);
    CHECK (selection.isSelected (chorus));
    CHECK (selection.getNumObjectsSelected() == 1);
    CHECK (phazite::getIDs (phazite::getSelectedSections (selection)) == std::vector { chorus });
    phazite::redo (song);
    CHECK_FALSE (selection.isSelected (chorus));
    CHECK_FALSE (selection.isSelected (verse));
}

TEST_CASE ("Song values' sections: a section's edit is an unsaved change, which its Undo takes back, and sections changed back are "
           "saved again")
{
    // The sections are heard from the song's values (Listener::sectionsChanged()), compared with the saved copy's as one value
    // with whether each has a SECTIONS (WholeState.h: holdSameSections()), as the whole song's comparison says.
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    auto& project = t.getProject();
    phazite::test::dispatchPendingMessages();
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    const auto edit = [&song] (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (song, name);
        change();
    };

    // The first section, which makes the SECTIONS: unsaved, as saved again once undone.
    SectionID verse;
    edit ("Add Section", [&] { verse = phazite::addSectionAt (song, at (0.0), nullptr); });
    CHECK_FALSE (project.isWholeAsSaved());
    phazite::undo (song);
    CHECK (project.isWholeAsSaved());
    CHECK_FALSE (project.hasUnsavedChanges());
    phazite::redo (song);
    CHECK (project.hasUnsavedChanges());
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // Renamed and renamed back, before anything asked: as saved.
    edit ("Rename Section", [&] { phazite::renameSection (song, verse, "Intro"); });
    edit ("Rename Section",
          [&] { phazite::renameSection (song, verse, phazite::getSection (song, verse)->name == "Intro" ? "Section 1" : "Intro"); });
    CHECK (project.isWholeAsSaved());
    CHECK_FALSE (project.hasUnsavedChanges());

    // Resized: unsaved; undone, as saved.
    edit ("Resize Section", [&] { phazite::resizeSection (song, verse, lasting (8.0), false); });
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
    phazite::undo (song);
    CHECK (project.isWholeAsSaved());

    // Every section taken away: unsaved, an empty list, which writes no SECTIONS, as the whole song's comparison says; and
    // saved so, a section made again is a change.
    edit ("Remove Sections", [&] { phazite::removeSections (song); });
    CHECK_FALSE (hasSectionsPlace (song));
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
    REQUIRE (project.save().wasOk());
    edit ("Add Section", [&] { phazite::addSectionAt (song, at (0.0), nullptr); });
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
}

TEST_CASE ("Song values' sections: a version's sections, and a version without them, applied both ways, the whole song's and the "
           "song's own line, each one exact undo step")
{
    // The versions give the song another song's sections through the sections pass (ApplySong.cpp: makeSectionsLike(), through
    // Sections.h's makeSectionsAs()). A restore of the song's own line gives the song another's sections too, as it always did.
    SectionsSong s;
    auto& song = *s.song;
    phazite::test::UndoCheck undo (song);
    const auto without = phazite::test::copyWholeSong (*phazite::createSong());
    const auto withSections = phazite::makeSong (phazite::test::copyWholeSong (song));
    REQUIRE (withSections != nullptr);

    {
        const phazite::UndoStep step (*withSections, "Other Sections");
        phazite::renameSection (*withSections, s.verse, "Intro");
        phazite::deleteSection (*withSections, s.chorus);
        phazite::appendSection (*withSections, nullptr);
    }

    const auto other = phazite::test::copyWholeSong (*withSections);

    const auto apply = [&] (const juce::String& name, const juce::ValueTree& from, const std::function<void (const juce::ValueTree&)>& make)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        make (from);
                    });
    };

    const auto songLike = [&song] (const juce::ValueTree& from) { phazite::makeSongLike (song, from); };
    const auto ownLike = [&song] (const juce::ValueTree& from) { phazite::makeSongOwnLike (song, from); };

    apply ("The Version's Song", other, songLike);
    CHECK (heldSections (song) == heldSections (*withSections));
    phazite::test::checkSong (song);

    apply ("No Sections", without, songLike);
    CHECK_FALSE (hasSectionsPlace (song));
    CHECK (heldSections (song) == phazite::SectionsValue {});
    phazite::test::checkSong (song);

    // The song's own line: its sections the other's, either way.
    apply ("The Version's Own", other, ownLike);
    REQUIRE (hasSectionsPlace (song));
    CHECK (heldSections (song) == heldSections (*withSections));
    apply ("No Sections' Own", without, ownLike);
    CHECK_FALSE (hasSectionsPlace (song));
    apply ("The Version's Own Again", other, ownLike);
    CHECK (heldSections (song) == heldSections (*withSections));
    phazite::test::checkSong (song);

    // A section that stands in for what its version couldn't read keeps the song's section of its ID, in its place; the rest
    // are the version's.
    {
        const phazite::UndoStep step (song, "Rename Section");
        phazite::renameSection (song, s.verse, "Mine");
    }

    auto standIn = other.createCopy();
    auto standInSections = standIn.getChildWithName (schema::SECTIONS);
    REQUIRE (standInSections.getNumChildren() == 2);
    standInSections.getChild (0).setProperty (schema::versions::missing, true, nullptr);
    standInSections.getChild (1).setProperty (schema::sectionName, "Theirs", nullptr);
    apply ("A Stand-in Section", standIn, songLike);
    REQUIRE (heldSections (song).sections.size() == 2);
    CHECK (heldSections (song).sections[0].id == s.verse);
    CHECK (heldSections (song).sections[0].name == "Mine");
    CHECK (heldSections (song).sections[1].name == "Theirs");
    phazite::test::checkSong (song);

    // A SECTIONS that stands in for what its version couldn't read leaves the song's sections as they are, whole.
    auto wholeStandIn = without.createCopy();
    wholeStandIn.appendChild (juce::ValueTree (schema::SECTIONS, { { schema::versions::missing, true } }), nullptr);
    const auto before = heldSections (song);
    apply ("A Stand-in", wholeStandIn, songLike);
    CHECK (heldSections (song).isSameAs (before));
    CHECK (hasSectionsPlace (song));

    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' sections: a merge's clash on a section's name, and on a whole section, is chosen each way through the "
           "sections' value")
{
    // A section merges by its ID, a property at a time: both sides renamed the base's section, so the merge reports one clash
    // on its name, whose choice the sections' chooser makes (Sections.cpp: chooseSectionClash()), in the choice's step.
    SectionsSong s;
    auto& base = *s.song;
    const auto ours = phazite::makeSong (phazite::test::copyWholeSong (base));
    const auto theirs = phazite::makeSong (phazite::test::copyWholeSong (base));
    REQUIRE (ours != nullptr);
    REQUIRE (theirs != nullptr);

    {
        const phazite::UndoStep step (*ours, "Rename Section");
        phazite::renameSection (*ours, s.verse, "Ours");
    }

    {
        const phazite::UndoStep step (*theirs, "Rename Section");
        phazite::renameSection (*theirs, s.verse, "Theirs");
    }

    const phazite::MergeSides sides { phazite::test::copyWholeSong (base), phazite::test::copyWholeSong (*ours),
                                      phazite::test::copyWholeSong (*theirs) };
    const auto merged = phazite::mergeSongs (sides.base, sides.before, sides.theirs, [&ours] { return ours->makeID(); });
    const phazite::MergeClash* clash = nullptr;

    for (const auto& each : merged.clashes)
        if (! each.path.isEmpty() && each.path[0].startsWith (schema::SECTIONS.toString() + ":"))
            clash = &each;

    REQUIRE (clash != nullptr);
    REQUIRE (clash->path.size() == 2);
    CHECK (clash->property == schema::sectionName.getName());

    {
        const phazite::UndoStep step (*ours, "Merge");
        phazite::makeSongLike (*ours, merged.song);
    }

    phazite::test::UndoCheck undo (*ours);
    const juce::String names[] = { "Verse", "Ours", "Theirs" };

    for (const auto value : { 2, 0, 1 })
    {
        INFO ("value " << value);
        undo.check ("Choose",
                    [&]
                    {
                        const phazite::UndoStep step (*ours, "Choose");
                        CHECK (phazite::chooseSectionClash (*ours, *clash, value, sides) == phazite::AreaChoice::chosen);
                    });
        CHECK (phazite::getSection (*ours, s.verse)->name == names[value]);
        CHECK (heldSections (*ours).sections.size() == 2);
        phazite::test::checkSong (*ours);
    }

    // A clash elsewhere is none of the sections'; one of the section's ID, or of a property a section hasn't, or that the
    // section can't hold (a length of 0), refused.
    auto elsewhere = *clash;
    elsewhere.path = { schema::TEMPOMAP.toString() + ":" + schema::TEMPOMAP.toString() };
    CHECK (phazite::chooseSectionClash (*ours, elsewhere, 0, sides) == phazite::AreaChoice::notTheArea);
    auto ofID = *clash;
    ofID.property = schema::sectionID.getName();
    ofID.base = 77;
    CHECK (phazite::chooseSectionClash (*ours, ofID, 0, sides) == phazite::AreaChoice::refused);
    auto unknown = *clash;
    unknown.property = juce::Identifier ("shape");
    CHECK (phazite::chooseSectionClash (*ours, unknown, 0, sides) == phazite::AreaChoice::refused);
    auto noLength = *clash;
    noLength.property = schema::sectionLength.getName();
    noLength.base = 0.0;
    CHECK (phazite::chooseSectionClash (*ours, noLength, 0, sides) == phazite::AreaChoice::refused);
    CHECK (phazite::getSection (*ours, s.verse)->name == "Ours");

    // A whole section (a merge of two songs Phazite holds reports none, but one of a commit from anyone may): the side
    // chosen's in its place, or none where that side had none.
    auto whole = *clash;
    whole.property = {};
    auto baseVerse = phazite::writeSectionElement (*heldSections (base).find (s.verse));
    auto theirVerse = baseVerse.createCopy();
    theirVerse.setProperty (schema::sectionName, "Whole", nullptr);
    theirVerse.setProperty (schema::sectionLength, 4.0, nullptr);
    whole.baseElement = baseVerse;
    whole.oursElement = {};
    whole.theirsElement = theirVerse;

    undo.check ("Choose Whole",
                [&]
                {
                    const phazite::UndoStep step (*ours, "Choose Whole");
                    CHECK (phazite::chooseSectionClash (*ours, whole, 2, sides) == phazite::AreaChoice::chosen);
                });
    REQUIRE (heldSections (*ours).sections.size() == 2);
    CHECK (heldSections (*ours).sections[0].id == s.verse); // in its place
    CHECK (heldSections (*ours).sections[0].name == "Whole");
    CHECK (heldSections (*ours).sections[0].length == lasting (4.0));

    undo.check ("Choose None",
                [&]
                {
                    const phazite::UndoStep step (*ours, "Choose None");
                    CHECK (phazite::chooseSectionClash (*ours, whole, 1, sides) == phazite::AreaChoice::chosen);
                });
    CHECK_FALSE (phazite::getSection (*ours, s.verse).has_value());
    CHECK (heldSections (*ours).sections.size() == 1);

    undo.check ("Choose Back",
                [&]
                {
                    const phazite::UndoStep step (*ours, "Choose Back");
                    CHECK (phazite::chooseSectionClash (*ours, whole, 0, sides) == phazite::AreaChoice::chosen);
                });
    CHECK (phazite::getSection (*ours, s.verse)->name == "Verse");
    CHECK (heldSections (*ours).sections.back().id == s.verse); // after the others, where the song had none

    // A section of another ID than the path's, or one that stands in, refused.
    auto otherID = whole;
    otherID.theirsElement = phazite::writeSectionElement (*heldSections (base).find (s.chorus));
    CHECK (phazite::chooseSectionClash (*ours, otherID, 2, sides) == phazite::AreaChoice::refused);
    auto standIn = whole;
    standIn.theirsElement = theirVerse.createCopy();
    standIn.theirsElement.setProperty (schema::versions::missing, true, nullptr);
    CHECK (phazite::chooseSectionClash (*ours, standIn, 2, sides) == phazite::AreaChoice::refused);

    phazite::test::checkSong (*ours);
    undo.walkBackAndForth();
}

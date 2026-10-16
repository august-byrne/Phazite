// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes' IDs (model/notes/Notes.h, schema::phaziteNoteID): each note has one that no other note of its clip has, which it
// keeps through edits, undo and redo, and saving and opening the project. A copy of a clip keeps its notes' IDs.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"
#include "model/base/Schema.h"
#include "model/commands/Undo.h"
#include "model/notes/Notes.h"
#include "model/project/Project.h"
#include "model/project/ProjectContext.h"

#include <algorithm>

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

std::vector<std::uint64_t> idsOf (const te::MidiClip& clip)
{
    std::vector<std::uint64_t> ids;

    for (auto* note : clip.getSequence().getNotes())
        ids.push_back (phazite::getNoteID (*note).getRaw());

    return ids;
}

bool allDifferent (std::vector<std::uint64_t> ids)
{
    std::sort (ids.begin(), ids.end());
    return std::adjacent_find (ids.begin(), ids.end()) == ids.end() && std::find (ids.begin(), ids.end(), 0u) == ids.end();
}

te::MidiNote& addNote (te::MidiClip& clip, int pitch, double start)
{
    auto* note = phazite::addNote (clip, pitch, beats (start), te::BeatDuration::fromBeats (1.0), 100);
    REQUIRE (note != nullptr);
    return *note;
}

// The open project: an Edit in the project context, with a MIDI clip a bar long on its first track.
struct Song
{
    Song()
    {
        context.setEdit (edit.get());
        clip = phazite::insertMidiClip (*te::getAudioTracks (*edit)[0], { beats (0.0), beats (4.0) }, nullptr);
        REQUIRE (clip != nullptr);
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { context.setEdit (nullptr); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngine() };
    te::MidiClip* clip = nullptr;
};
} // namespace

TEST_CASE ("Each note added to the open project gets an ID of its own, which it keeps through edits, undo and redo")
{
    Song song;
    auto& clip = *song.clip;

    phazite::NoteID firstID, secondID;

    {
        const phazite::UndoStep step (*song.edit, "Add Notes");
        firstID = phazite::getNoteID (addNote (clip, 60, 0.0));
        secondID = phazite::getNoteID (addNote (clip, 64, 1.0));
    }

    CHECK (firstID.isValid());
    CHECK (secondID.isValid());
    CHECK (firstID != secondID);
    REQUIRE (phazite::findNote (clip, secondID) != nullptr);
    CHECK (phazite::findNote (clip, secondID)->getNoteNumber() == 64);
    CHECK (phazite::getNoteRef (clip, *phazite::findNote (clip, secondID)) == phazite::NoteRef { phazite::getClipID (clip), secondID });
    CHECK (phazite::findNote (clip, {}) == nullptr);

    // Moved, it is the same note; deleted and put back by an undo, too.
    {
        const phazite::UndoStep step (*song.edit, "Move Note");
        phazite::moveNote (clip, *phazite::findNote (clip, secondID), beats (2.0), 65);
    }

    {
        const phazite::UndoStep step (*song.edit, "Delete Note");
        phazite::deleteNotes ({ { &clip, phazite::findNote (clip, secondID) } });
    }

    CHECK (phazite::findNote (clip, secondID) == nullptr);
    phazite::undo (*song.edit);
    REQUIRE (phazite::findNote (clip, secondID) != nullptr);
    CHECK (phazite::findNote (clip, secondID)->getNoteNumber() == 65);

    // Undoing the move and the adding takes them away; redoing them brings back the same notes.
    phazite::undo (*song.edit);
    phazite::undo (*song.edit);
    CHECK (clip.getSequence().getNumNotes() == 0);
    phazite::redo (*song.edit);
    CHECK (idsOf (clip) == std::vector { firstID.getRaw(), secondID.getRaw() });

    // A note added later never gets an ID a note had before, even one deleted since.
    {
        const phazite::UndoStep step (*song.edit, "Delete Note");
        phazite::deleteNotes ({ { &clip, phazite::findNote (clip, secondID) } });
    }

    const auto thirdID = phazite::getNoteID (addNote (clip, 67, 3.0));
    CHECK (thirdID.getRaw() > std::max (firstID.getRaw(), secondID.getRaw()));
}

TEST_CASE ("A copy of a clip keeps its notes' IDs, an alias, a duplicate and the halves of a split alike")
{
    Song song;
    auto& clip = *song.clip;

    for (auto i = 0; i < 4; ++i)
        addNote (clip, 60 + i, (double) i);

    const auto ids = idsOf (clip);
    REQUIRE (allDifferent (ids));

    const auto aliases = phazite::copyClips ({ &clip }, true);
    const auto copies = phazite::copyClips ({ &clip }, false);
    REQUIRE (aliases.size() == 1);
    REQUIRE (copies.size() == 1);
    CHECK (idsOf (*dynamic_cast<te::MidiClip*> (aliases.getFirst())) == ids);
    CHECK (idsOf (*dynamic_cast<te::MidiClip*> (copies.getFirst())) == ids);

    // Editing the original edits its alias too (Tracktion copies the notes over): the alias's notes keep the same IDs.
    phazite::moveNote (clip, *phazite::findNote (clip, phazite::NoteID::fromRaw (ids[0])), beats (0.5), 72);
    phazite::test::settle();
    auto* alias = dynamic_cast<te::MidiClip*> (aliases.getFirst());
    REQUIRE (alias != nullptr);
    std::vector<std::uint64_t> aliasIDs = idsOf (*alias), originalIDs = idsOf (clip);
    std::sort (aliasIDs.begin(), aliasIDs.end());
    std::sort (originalIDs.begin(), originalIDs.end());
    CHECK (aliasIDs == originalIDs);

    // Split, each half holds the notes it plays with their IDs.
    const auto halves = phazite::splitClips ({ &clip }, beats (2.0));
    REQUIRE (halves.size() == 1);
    auto* right = dynamic_cast<te::MidiClip*> (halves.getFirst());
    REQUIRE (right != nullptr);
    CHECK (allDifferent (idsOf (clip)));
    CHECK (phazite::findNote (*right, phazite::NoteID::fromRaw (ids[3])) != nullptr);
}

TEST_CASE ("Notes without IDs, or with another note's, get IDs of their own")
{
    // An Edit no project context holds: its notes have no IDs until they are assigned.
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* clip = phazite::insertMidiClip (*te::getAudioTracks (*edit)[0], { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip != nullptr);
    auto& first = addNote (*clip, 60, 0.0);
    addNote (*clip, 62, 1.0);
    CHECK_FALSE (phazite::getNoteID (first).isValid());

    phazite::assignNoteIDs (*edit);
    const auto ids = idsOf (*clip);
    CHECK (allDifferent (ids));

    // A copy of a note's state in the same clip: the copy gets a new ID, and the original keeps its own.
    clip->getSequence().state.appendChild (first.state.createCopy(), nullptr);
    phazite::assignNoteIDs (*edit);
    const auto after = idsOf (*clip);
    REQUIRE (after.size() == 3);
    CHECK (allDifferent (after));
    CHECK (phazite::getNoteID (first).getRaw() == ids[0]);
    CHECK (std::count (after.begin(), after.end(), ids[1]) == 1);

    // The open project's context assigns them when it takes the Edit.
    clip->getSequence().state.appendChild (juce::ValueTree (te::IDs::NOTE, { { "p", 70 }, { "b", 2.0 }, { "l", 1.0 }, { "v", 100 } }),
                                           nullptr);
    phazite::ProjectContext context (phazite::test::getEngine());
    context.setEdit (edit.get());
    CHECK (allDifferent (idsOf (*clip)));
    context.setEdit (nullptr);
}

TEST_CASE ("Notes' IDs are saved with the project and the same when it opens again, with nothing to save")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    juce::String error;
    std::vector<std::uint64_t> saved;

    // A new project whose content adds notes before it is open: the first save gives them their IDs.
    auto project = phazite::Project::create (engine, temp.folder, "Notes", error,
                                             [] (te::Edit& edit)
                                             {
                                                 auto* track = phazite::insertTrack (edit, nullptr, nullptr);
                                                 auto* clip = phazite::insertMidiClip (*track, { beats (0.0), beats (4.0) }, nullptr);
                                                 addNote (*clip, 60, 0.0);
                                                 addNote (*clip, 67, 2.0);
                                             });
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    phazite::test::settle();

    const auto clipOf = [] (phazite::Project& p) -> te::MidiClip*
    {
        for (auto* track : te::getAudioTracks (p.getEdit()))
            for (auto* clip : track->getClips())
                if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
                    return midi;

        return nullptr;
    };

    REQUIRE (clipOf (*project) != nullptr);
    saved = idsOf (*clipOf (*project));
    CHECK (allDifferent (saved));
    CHECK_FALSE (project->hasUnsavedChanges());

    // The file holds them, and they are the same when it opens again.
    const auto xml = phazite::test::readXml (project->getFile());
    REQUIRE (xml != nullptr);
    CHECK (xml->toString().contains (phazite::schema::phaziteNoteID.toString() + "=\""));
    const auto file = project->getFile();
    project = nullptr;

    project = phazite::Project::open (engine, file, false, error);
    REQUIRE (project != nullptr);
    phazite::test::settle();
    REQUIRE (clipOf (*project) != nullptr);
    CHECK (idsOf (*clipOf (*project)) == saved);
    CHECK_FALSE (project->hasUnsavedChanges());
}

TEST_CASE ("Notes that replace others keep their IDs: a quantized note, a chopped note's first piece; a copy gets its own")
{
    Song song;
    auto& clip = *song.clip;
    auto& note = addNote (clip, 60, 0.3);
    const auto id = phazite::getNoteID (note);
    REQUIRE (id.isValid());

    const auto quantized =
        phazite::replaceNotes (*song.edit, { { &clip, &note } }, phazite::quantize (phazite::getNoteData ({ { &clip, &note } }), {}));
    REQUIRE (quantized.size() == 1);
    CHECK (phazite::getNoteID (*quantized[0].note) == id);
    CHECK (phazite::getNoteStart (clip, *quantized[0].note).inBeats() == doctest::Approx (0.25));

    const auto pieces = phazite::replaceNotes (*song.edit, quantized, phazite::chop (phazite::getNoteData (quantized), 0.25));
    REQUIRE (pieces.size() == 4);
    CHECK (phazite::getNoteID (*pieces[0].note) == id);
    CHECK (allDifferent (idsOf (clip)));

    // A copy, while its original is still there.
    const auto copies = phazite::duplicateNotes ({ pieces[0] });
    REQUIRE (copies.size() == 1);
    CHECK (phazite::getNoteID (*copies[0].note) != id);
    CHECK (allDifferent (idsOf (clip)));
}

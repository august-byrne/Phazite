// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes' IDs (model/notes/internal/Notes.h, schema::noteID): each note has one that no other note of its clip has, which it
// keeps through edits, undo and redo, and saving and opening the project. A copy of a clip keeps its notes' IDs.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mirror/internal/Mirror.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <functional>

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// The IDs of a clip's notes, in the order the song holds them.
std::vector<std::uint64_t> idsOf (const phazite::Song& song, phazite::ClipID clip)
{
    std::vector<std::uint64_t> ids;

    for (const auto& note : phazite::getClipNotes (song, clip))
        ids.push_back (note.id.getRaw());

    return ids;
}

// The IDs Tracktion's notes of a clip carry: those of the song's notes they play (schema::phaziteNoteID), 0 for none.
std::vector<std::uint64_t> playedIDsOf (const te::MidiClip& clip)
{
    std::vector<std::uint64_t> ids;

    for (auto* note : clip.getSequence().getNotes())
        ids.push_back ((std::uint64_t) (juce::int64) note->state[phazite::schema::phaziteNoteID]);

    return ids;
}

// Tracktion's note that plays one of the song's, by its ID, or nullptr.
te::MidiNote* findPlayedNote (const te::MidiClip& clip, phazite::NoteID id)
{
    for (auto* note : clip.getSequence().getNotes())
        if ((juce::int64) note->state[phazite::schema::phaziteNoteID] == (juce::int64) id.getRaw())
            return note;

    return nullptr;
}

bool allDifferent (std::vector<std::uint64_t> ids)
{
    std::sort (ids.begin(), ids.end());
    return std::adjacent_find (ids.begin(), ids.end()) == ids.end() && std::find (ids.begin(), ids.end(), 0u) == ids.end();
}

phazite::NoteRef addNote (phazite::Song& song, phazite::ClipID clip, int pitch, double start)
{
    const auto note = phazite::addNote (song, clip, pitch, beats (start), phazite::BeatDuration::fromBeats (1.0), 100);
    REQUIRE (note.isValid());
    return note;
}

// A note of the song's, which must be there.
phazite::NoteData noteOf (const phazite::Song& song, const phazite::NoteRef& note)
{
    const auto data = phazite::getNoteData (song, note);
    REQUIRE (data.has_value());
    return *data;
}

// The open project: an Edit in the project context, with a MIDI clip a bar long on its first track.
struct OpenProject
{
    OpenProject()
    {
        setEdit (context, edit.get());
        clip = phazite::insertMidiClip (song, phazite::getTrackID (*te::getAudioTracks (*edit)[0]), { beats (0.0), beats (4.0) }, nullptr);
        REQUIRE (clip.isValid());
        song.getUndoManager().clearUndoHistory();
    }

    ~OpenProject() { setEdit (context, nullptr); }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
    phazite::Song& song = phazite::getSong (*edit);
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::ClipID clip;
};
} // namespace

TEST_CASE ("Each note added to the open project gets an ID of its own, which it keeps through edits, undo and redo")
{
    OpenProject project;
    auto& song = project.song;
    const auto clip = project.clip;

    phazite::NoteID firstID, secondID;

    {
        const phazite::UndoStep step (*project.edit, "Add Notes");
        firstID = addNote (song, clip, 60, 0.0).note;
        secondID = addNote (song, clip, 64, 1.0).note;
    }

    const phazite::NoteRef second { clip, secondID };
    CHECK (firstID.isValid());
    CHECK (secondID.isValid());
    CHECK (firstID != secondID);
    REQUIRE (phazite::findNoteState (song, second).isValid());
    CHECK (noteOf (song, second).pitch == 64);
    CHECK (noteOf (song, second).getRef() == phazite::NoteRef { clip, secondID });
    CHECK_FALSE (phazite::findNoteState (song, { clip, {} }).isValid());

    // Moved, it is the same note; deleted and put back by an undo, too.
    {
        const phazite::UndoStep step (*project.edit, "Move Note");
        phazite::moveNote (song, second, beats (2.0), 65);
    }

    {
        const phazite::UndoStep step (*project.edit, "Delete Note");
        phazite::deleteNotes (song, { second });
    }

    CHECK_FALSE (phazite::findNoteState (song, second).isValid());
    phazite::undo (*project.edit);
    REQUIRE (phazite::findNoteState (song, second).isValid());
    CHECK (noteOf (song, second).pitch == 65);

    // Undoing the move and the adding takes them away; redoing them brings back the same notes.
    phazite::undo (*project.edit);
    phazite::undo (*project.edit);
    CHECK (phazite::getClipNotes (song, clip).empty());
    phazite::redo (*project.edit);
    CHECK (idsOf (song, clip) == std::vector { firstID.getRaw(), secondID.getRaw() });

    // A note added later never gets an ID a note had before, even one deleted since.
    {
        const phazite::UndoStep step (*project.edit, "Delete Note");
        phazite::deleteNotes (song, { second });
    }

    const auto thirdID = addNote (song, clip, 67, 3.0).note;
    CHECK (thirdID.getRaw() > std::max (firstID.getRaw(), secondID.getRaw()));
}

TEST_CASE ("A copy of a clip keeps its notes' IDs, an alias, a duplicate and the halves of a split alike")
{
    OpenProject project;
    auto& song = project.song;
    const auto clip = project.clip;

    for (auto i = 0; i < 4; ++i)
        addNote (song, clip, 60 + i, (double) i);

    const auto ids = idsOf (song, clip);
    REQUIRE (allDifferent (ids));

    const auto aliases = phazite::copyClips (song, { clip }, true);
    const auto copies = phazite::copyClips (song, { clip }, false);
    REQUIRE (aliases.size() == 1);
    REQUIRE (copies.size() == 1);
    CHECK (idsOf (song, aliases.front()) == ids);
    CHECK (idsOf (song, copies.front()) == ids);

    // Editing the original edits its alias too: the alias's notes keep the same IDs.
    phazite::moveNote (song, { clip, phazite::NoteID::fromRaw (ids[0]) }, beats (0.5), 72);
    phazite::test::settle();
    const auto alias = aliases.front();
    REQUIRE (phazite::isMidiState (phazite::findClipState (song, alias)));
    std::vector<std::uint64_t> aliasIDs = idsOf (song, alias), originalIDs = idsOf (song, clip);
    std::sort (aliasIDs.begin(), aliasIDs.end());
    std::sort (originalIDs.begin(), originalIDs.end());
    CHECK (aliasIDs == originalIDs);

    // Split, each half holds the notes it plays with their IDs.
    const auto halves = phazite::splitClips (song, { clip }, beats (2.0));
    REQUIRE (halves.size() == 1);
    const auto right = halves.front();
    REQUIRE (phazite::isMidiState (phazite::findClipState (song, right)));
    CHECK (allDifferent (idsOf (song, clip)));
    CHECK (phazite::findNoteState (song, { right, phazite::NoteID::fromRaw (ids[3]) }).isValid());
}

TEST_CASE ("Each note of the song has an ID from the start, which Tracktion's note that plays it carries")
{
    // An Edit no project context holds too: the song gives its notes their IDs.
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    const auto clip = phazite::insertMidiClip (song, phazite::ensureTracks (song, 1).front(), { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip.isValid());
    const auto first = addNote (song, clip, 60, 0.0);
    addNote (song, clip, 62, 1.0);
    CHECK (first.note.isValid());
    CHECK (allDifferent (idsOf (song, clip)));

    auto* played = dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, phazite::toTracktion (clip)));
    REQUIRE (played != nullptr);
    auto* firstPlayed = findPlayedNote (*played, first.note);
    REQUIRE (firstPlayed != nullptr);
    CHECK (allDifferent (playedIDsOf (*played)));

    // Tracktion's notes that play none of the song's (written into its clip from outside), or that carry another's ID, go
    // as the clip is placed again.
    auto& sequence = played->getSequence();
    sequence.state.appendChild (firstPlayed->state.createCopy(), nullptr);
    sequence.state.appendChild (juce::ValueTree (te::IDs::NOTE, { { "p", 70 }, { "b", 2.0 }, { "l", 1.0 }, { "v", 100 } }), nullptr);
    REQUIRE (playedIDsOf (*played).size() == 4);
    phazite::mirror::reconcileClip (*edit, clip);
    CHECK (playedIDsOf (*played).size() == 2);
    CHECK (allDifferent (playedIDsOf (*played)));
    CHECK (findPlayedNote (*played, first.note) == firstPlayed);
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
                                                 auto& song = phazite::getSong (edit);
                                                 const auto track = phazite::insertTrack (song, {}, nullptr);
                                                 const auto clip =
                                                     phazite::insertMidiClip (song, track, { beats (0.0), beats (4.0) }, nullptr);
                                                 addNote (song, clip, 60, 0.0);
                                                 addNote (song, clip, 67, 2.0);
                                             });
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    phazite::test::settle();

    const auto clipOf = [] (phazite::Project& p)
    {
        for (const auto& clipState : phazite::getClipStates (phazite::getSong (p.getEdit())))
            if (phazite::isMidiState (clipState))
                return phazite::getClipID (clipState);

        return phazite::ClipID();
    };

    REQUIRE (clipOf (*project).isValid());
    saved = idsOf (phazite::getSong (project->getEdit()), clipOf (*project));
    CHECK (allDifferent (saved));
    CHECK_FALSE (project->hasUnsavedChanges());

    // The file holds them, on the song's notes, and they are the same when it opens again.
    const auto xml = phazite::test::readXml (project->getFile());
    REQUIRE (xml != nullptr);
    const std::function<const juce::XmlElement*(const juce::XmlElement&)> findNote = [&findNote] (const juce::XmlElement& element)
    {
        if (element.hasTagName (phazite::schema::NOTE.toString()))
            return &element;

        for (const auto* child : element.getChildIterator())
            if (const auto* found = findNote (*child))
                return found;

        return static_cast<const juce::XmlElement*> (nullptr);
    };

    const auto* note = findNote (*xml);
    REQUIRE (note != nullptr);
    CHECK (note->hasAttribute (phazite::schema::noteID.toString()));
    const auto file = project->getFile();
    project = nullptr;

    project = phazite::Project::open (engine, file, false, error);
    REQUIRE (project != nullptr);
    phazite::test::settle();
    REQUIRE (clipOf (*project).isValid());
    CHECK (idsOf (phazite::getSong (project->getEdit()), clipOf (*project)) == saved);
    CHECK_FALSE (project->hasUnsavedChanges());
}

TEST_CASE ("Notes that replace others keep their IDs: a quantized note, a chopped note's first piece; a copy gets its own")
{
    OpenProject project;
    auto& song = project.song;
    const auto clip = project.clip;
    const auto note = addNote (song, clip, 60, 0.3);
    const auto id = note.note;
    REQUIRE (id.isValid());

    const auto quantized =
        phazite::replaceNotes (song, { note }, phazite::quantize (phazite::getNoteData (song, std::vector { note }), {}));
    REQUIRE (quantized.size() == 1);
    CHECK (quantized[0].note == id);
    CHECK (noteOf (song, quantized[0]).start.inBeats() == doctest::Approx (0.25));

    const auto pieces = phazite::replaceNotes (song, quantized, phazite::chop (phazite::getNoteData (song, quantized), 0.25));
    REQUIRE (pieces.size() == 4);
    CHECK (pieces[0].note == id);
    CHECK (allDifferent (idsOf (song, clip)));

    // A copy, while its original is still there.
    const auto copies = phazite::duplicateNotes (song, { pieces[0] });
    REQUIRE (copies.size() == 1);
    CHECK (copies[0].note != id);
    CHECK (allDifferent (idsOf (song, clip)));
}

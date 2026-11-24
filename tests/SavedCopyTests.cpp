// SPDX-License-Identifier: AGPL-3.0-or-later
// The project's saved copy as the song's state (model/project/internal/Project.h: savedState, a SongState), and the saves
// that write the song from a state on another thread (Auto Save's save in the background, an autosave):
// - the project's three answers to whether the song is as saved agree after every random edit, its Undo and its Redo
//   (UndoCheck.h: SavedCopyCheck: the quick check, area by area; the whole comparison, WholeState.h's holdSameSong(); and
//   the files' texts, what counts alone), among them an edit of each area alone (RandomEdits.h: editOneArea()), so that a
//   mark of an area's own that is missing shows; as they do after every command of the command script
//   (CommandGoldenTests.cpp);
// - a container's macro changed is an unsaved change, which the whole comparison finds (closing's check asks to save it);
// - a save or an autosave in the background writes the song as it was as it began: an edit made while it writes (the writer
//   held at its start: Project::setWriteStartForTests()) is no part of its file, and leaves the project unsaved (or with
//   changes to autosave) once it is written;
// - a save in the background whose write fails leaves the project unsaved and its saved copy as it was, and a later save
//   writes it;
// - two saves of one song write the same bytes, whichever thread wrote the tree and the text;
// - a save in the background whose project is closed at once still writes its file whole.
#include "RandomEdits.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/delay/Delay.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Containers.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace
{
using phazite::test::TestSong;

// The text a save writes of the song as it is now (model/base/internal/SongText.h), as the project file holds it.
juce::String writeTextNow (const phazite::Song& song)
{
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (song));
    return text.toString();
}

juce::String readText (const juce::File& file)
{
    juce::MemoryBlock bytes;
    REQUIRE (file.loadFileAsData (bytes));
    return bytes.toString();
}

// A project's writer held at the start of each of its writes (Project::setWriteStartForTests()) until the test lets it go;
// let go, and its hold taken away once the write is over, however the test ends: a failed REQUIRE would leave the writer
// waiting, and the project closing for it. Made after the project, so that it goes first.
class HeldWriter
{
public:
    explicit HeldWriter (phazite::Project& p)
        : project (p)
    {
        project.setWriteStartForTests (
            [this]
            {
                started.signal();
                letGo.wait (-1);
            });
    }

    ~HeldWriter()
    {
        letGo.signal();
        project.setWriteStartForTests (nullptr); // once the write's thread is gone
    }

    // Whether the writer has come to its hold: what the test waits for (dispatchMessagesUntil()).
    bool hasStarted() { return started.wait (0); }

    void letItGo() { letGo.signal(); }

private:
    phazite::Project& project;
    juce::WaitableEvent started { true }, letGo { true }; // each stays signalled once signalled

    JUCE_DECLARE_NON_COPYABLE (HeldWriter)
};
} // namespace

TEST_CASE ("The unsaved check, the whole comparison with the saved copy and the files' texts agree after every random edit, its "
           "Undo and its Redo")
{
    phazite::test::TemporaryFolder sounds;
    auto checked = 0, unsaved = 0;

    for (const auto seed : { 20261124, 20261125, 20261126 })
    {
        CAPTURE (seed);
        TestSong s;
        auto& song = *phazite::findSong (s.context);
        phazite::test::RandomEdits edits (song, seed, sounds.folder);
        edits.editsOneAreaAlone = true;

        {
            const phazite::UndoStep step (song, "One of Each");
            edits.addOneOfEach();
        }

        phazite::test::UndoCheck undoCheck (song);
        phazite::test::SavedCopyCheck savedCheck (s.getProject());
        undoCheck.atEachMoment = [&savedCheck] (const juce::String& when) { savedCheck.check (when); };

        // Each area alone first, while the song holds one of what each changes; then among the other edits, at random.
        for (int kind = 0; kind < phazite::test::RandomEdits::numOneAreaKinds; ++kind)
            undoCheck.check ("one area alone, kind " + juce::String (kind),
                             [&]
                             {
                                 const phazite::UndoStep step (song, "One Area");
                                 edits.editOneArea (kind);
                             });

        for (int i = 0; i < 40; ++i)
            undoCheck.check ("random edit " + juce::String (i),
                             [&]
                             {
                                 const phazite::UndoStep step (song, "Random Edit");
                                 edits.edit();
                             });

        // Many edits, the check asked once after them all: what each area marked, compared at once.
        for (int i = 0; i < 12; ++i)
        {
            const phazite::UndoStep step (song, "Random Edit");
            edits.edit();
        }

        savedCheck.check ("twelve edits");
        checked += savedCheck.getNumChecked();
        unsaved += savedCheck.getNumUnsaved();
    }

    // Some found the song unsaved, and some as saved (an edit's Undo, and the edits that change nothing that counts).
    CHECK (unsaved > 0);
    CHECK (unsaved < checked);
}

TEST_CASE ("A container's macro renamed, turned or given a modulation is an unsaved change, which closing's check asks to save, "
           "and its Undo is saved again")
{
    // The whole comparison (WholeState.h: holdSameSong(), through holdSameChannels()) compares each container's macros, as
    // the file holds them under the container: without, closing's check (Project::checkUnsavedChanges()) found a song saved
    // whose macro alone had changed, and the change was lost without a question.
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    phazite::DeviceID container, delay;

    {
        const phazite::UndoStep step (song, "Add Container");
        container = phazite::addContainer (song, s.firstTrack);
        REQUIRE (container.isValid());
        delay = phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), phazite::devices::Delay::typeID);
    }

    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE_FALSE (macros.empty());
    const auto macro = macros.front();
    const phazite::ParameterRef mix { delay, "mix" };
    REQUIRE (phazite::canModulate (song, macro, mix));
    REQUIRE (project.save().wasOk());
    REQUIRE (project.isWholeAsSaved());

    const std::vector<std::pair<juce::String, std::function<void()>>> edits {
        { "Rename Macro", [&song, macro] { phazite::renameMacro (song, macro, "Brightness"); } },
        { "Turn Macro", [&song, macro] { phazite::setMacroValue (song, macro, 0.75f); } },
        { "Modulate", [&song, macro, mix] { phazite::setModulation (song, macro, mix, 0.5f); } },
    };

    for (const auto& [name, edit] : edits)
    {
        INFO (name.toStdString());

        {
            const phazite::UndoStep step (song, name);
            edit();
        }

        CHECK (project.hasUnsavedChanges());
        CHECK_FALSE (project.isWholeAsSaved());
        CHECK (project.checkUnsavedChanges());

        phazite::undo (song);
        CHECK (project.isWholeAsSaved());
        CHECK_FALSE (project.checkUnsavedChanges());
        CHECK_FALSE (project.hasUnsavedChanges());
    }
}

TEST_CASE ("A save in the background writes the song as it was as it began: an edit made while it writes is no part of its file, "
           "and leaves the project unsaved")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    REQUIRE (project.save().wasOk());
    s.addTrack ("Bass");
    REQUIRE (project.hasUnsavedChanges());
    HeldWriter held (project);

    // The state taken; the edit made while the writer holds at its start, before it made anything of the state; the write let
    // go.
    project.saveInBackground();
    REQUIRE (project.hasBackgroundSave());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held.hasStarted(); }));
    const auto expected = writeTextNow (song); // nothing changed the song since the save took its state
    s.addTrack ("Drums");
    held.letItGo();

    REQUIRE (phazite::test::dispatchMessagesUntil ([&project] { return ! project.isAutosaving(); }));
    CHECK (project.finishBackgroundSave());
    CHECK (readText (project.getFile()) == expected);
    CHECK (project.hasUnsavedChanges()); // the edit made while it wrote

    // Saved again, then nothing is left unsaved, and the file holds the edit.
    REQUIRE (project.save().wasOk());
    CHECK_FALSE (project.hasUnsavedChanges());
    CHECK (readText (project.getFile()) == writeTextNow (song));
    CHECK (readText (project.getFile()) != expected);
}

TEST_CASE ("An autosave writes the song as it was as it began: an edit made while it writes is no part of it, and is left to the "
           "next")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    REQUIRE (project.save().wasOk());
    s.addTrack ("Bass");
    REQUIRE (project.hasChangesToAutosave());
    HeldWriter held (project);

    project.autosaveInBackground();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held.hasStarted(); }));
    const auto expected = writeTextNow (song);
    s.addTrack ("Drums");
    held.letItGo();

    REQUIRE (phazite::test::dispatchMessagesUntil ([&project] { return ! project.isAutosaving(); }));
    CHECK_FALSE (project.takeAutosaveFailure());
    CHECK (readText (phazite::Project::getAutosaveFile (project.getFile())) == expected);
    CHECK (project.hasChangesToAutosave()); // the edit made while it wrote
    CHECK (project.hasUnsavedChanges());
}

TEST_CASE ("A save in the background whose write fails leaves the project unsaved and its saved copy as it was, and a later save "
           "writes it")
{
    // The write made to fail as a full disk fails it, at the step that puts the written file in the project file's place
    // (model/base/WholeFile.h: WholeFileHook), the same on every system and without rights of any kind.
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    REQUIRE (project.save().wasOk());
    const auto file = project.getFile();
    const auto savedText = readText (file);
    s.rename (s.firstTrack, "Bass"); // one undo step
    REQUIRE (project.hasUnsavedChanges());

    {
        const phazite::WholeFileHook failing ([file] (const juce::File& written) { return written != file; });
        project.saveInBackground();
        REQUIRE (project.hasBackgroundSave());
        REQUIRE (phazite::test::dispatchMessagesUntil ([&project] { return ! project.isAutosaving(); }));
    }

    CHECK_FALSE (project.finishBackgroundSave());
    CHECK_FALSE (project.hasBackgroundSave());
    CHECK (project.hasUnsavedChanges());
    CHECK (project.checkUnsavedChanges());
    CHECK (readText (file) == savedText);

    // The saved copy is the one before the edit still: its Undo is as saved, its Redo not.
    phazite::undo (song);
    CHECK (project.isWholeAsSaved());
    phazite::redo (song);
    CHECK_FALSE (project.isWholeAsSaved());

    // A later save in the background, the file's place writable again: written, and saved.
    project.saveInBackground();
    REQUIRE (project.hasBackgroundSave());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&project] { return ! project.isAutosaving(); }));
    CHECK (project.finishBackgroundSave());
    CHECK_FALSE (project.hasUnsavedChanges());
    CHECK (project.isWholeAsSaved());
    CHECK (readText (file) == writeTextNow (song));
}

TEST_CASE ("Two saves of one song write the same bytes, the tree and the text made on the writer's thread or on the message thread")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    s.addTrack ("Bass");
    s.addMidiClip (s.firstTrack, 0.0, 4.0);

    // On the message thread, twice.
    REQUIRE (project.save().wasOk());
    const auto first = readText (project.getFile());
    REQUIRE (project.save().wasOk());
    CHECK (readText (project.getFile()) == first);
    CHECK (first == writeTextNow (song));

    // In the background (Auto Save's), after an edit, then on the message thread: the same song, the same bytes.
    s.addTrack ("Drums");
    project.saveInBackground();
    REQUIRE (project.hasBackgroundSave());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&project] { return ! project.isAutosaving(); }));
    REQUIRE (project.finishBackgroundSave());
    CHECK_FALSE (project.hasUnsavedChanges());
    const auto inBackground = readText (project.getFile());
    REQUIRE (project.save().wasOk());
    CHECK (readText (project.getFile()) == inBackground);
    CHECK (inBackground != first);
}

TEST_CASE ("A save in the background whose project is closed at once still writes its file whole")
{
    phazite::test::TemporaryFolder folder;
    juce::String error;
    auto project = phazite::Project::create (folder.folder, "Closed At Once", error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    const auto file = project->getFile();

    {
        const phazite::UndoStep step (project->getSong(), "Add Tracks");
        phazite::ensureTracks (project->getSong(), 8);
    }

    REQUIRE (project->hasUnsavedChanges());
    project->saveInBackground();
    REQUIRE (project->hasBackgroundSave());
    const auto expected = writeTextNow (project->getSong());
    project.reset(); // closed as the writer's thread writes: it waits for it

    CHECK (readText (file) == expected);
}

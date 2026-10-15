// SPDX-License-Identifier: AGPL-3.0-or-later
// The app's own code (src/app, the phazite_app library) without a window: the session that opens, makes and saves
// projects. Nothing here may reach a dialog (an AlertWindow, a FileChooser or a menu): Linux CI has no display.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Sections.h"
#include "model/base/Schema.h"
#include "model/mixer/Mixer.h"
#include "model/notes/Notes.h"
#include "model/project/StarterProject.h"
#include "model/transport/Tempo.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"

namespace
{
// A project made and closed again, as an earlier session would have left it.
juce::File createClosedProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project->getFile();
}
} // namespace

TEST_CASE ("The session opens and saves a project")
{
    auto& engine = phazite::test::getEngine();
    auto& uiBehaviour = phazite::getUIBehaviour (engine);
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    {
        phazite::ProjectSession session (engine, uiBehaviour, recent, temp.folder);
        int changes = 0;
        session.onProjectChanged = [&changes] { ++changes; };

        // Without an autosave to restore, it opens at once, as the project the commands and live MIDI act on.
        session.openProject (file);
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == file);
        CHECK (session.getContext().getEdit() == &session.getProject()->getEdit());
        CHECK (session.getSelection().getEdit() == &session.getProject()->getEdit());
        CHECK (uiBehaviour.getLastFocusedEdit() == &session.getProject()->getEdit());
        CHECK (recent.getProjects() == juce::Array<juce::File> { file });
        CHECK (changes == 1);
        phazite::test::settle();
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());

        // Its commands save it.
        phazite::CommandRegistry registry;
        session.addCommands (registry);
        const auto* save = registry.find ("project.save");
        REQUIRE (save != nullptr);
        CHECK (save->isEnabled());
        session.getProject()->getPhaziteState().setProperty (phazite::schema::outline, "pop", nullptr);
        REQUIRE (session.getProject()->hasUnsavedChanges());
        REQUIRE (registry.invoke ("project.save"));
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());

        // With nothing unsaved, closing asks nothing.
        auto closed = false;
        session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
        CHECK (closed);
    }

    // The session's project goes with it.
    CHECK (uiBehaviour.getLastFocusedEdit() == nullptr);
}

TEST_CASE ("The first start makes My First Song")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings); // no recent projects: a first start

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder);
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    const auto file = session.getProject()->getFile();
    CHECK (file
           == temp.folder.getChildFile ("My First Song").getChildFile (juce::String ("My First Song") + phazite::Project::fileExtension));
    CHECK (file.existsAsFile());
    CHECK (te::getAudioTracks (*session.getContext().getEdit()).size() == 2); // the starter song: drums and a synth
    CHECK (recent.getProjects() == juce::Array<juce::File> { file });

    // The computer keyboard plays the synth, in its octave: its track is selected, and live MIDI follows the selection.
    auto* synth = phazite::findSynthTrack (*session.getContext().getEdit());
    REQUIRE (synth != nullptr);
    CHECK (synth->getName() == "Synth");
    CHECK (session.getSelection().isSelected (synth));

    // It is what the project starts with: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (session.getContext().getEdit()->getUndoManager().canUndo());
}

TEST_CASE ("A new project starts as the new-project window chooses, with the computer keyboard on its synth")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder);
    CHECK (phazite::ProjectSession::suggestNewName (temp.folder) == "Untitled");

    phazite::NewProjectSettings choice;
    choice.tempo = 90.0;
    choice.timeSignature = { 6, 8 };
    choice.keyRoot = 2; // D
    choice.scale = "minor";
    choice.outline = "hipHop";
    REQUIRE (phazite::isValid (choice));
    REQUIRE (session.createProject (temp.folder, "Untitled", choice).wasOk());

    auto* edit = session.getContext().getEdit();
    REQUIRE (edit != nullptr);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (90.0));
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 6, 8 });
    CHECK (phazite::getSections (*edit).size() == (int) phazite::findOutline ("hipHop")->sections.size());

    const auto tracks = te::getAudioTracks (*edit);
    REQUIRE (tracks.size() == 2);
    CHECK (tracks[0]->getName() == "Drums");
    CHECK (tracks[1]->getName() == "Synth");

    for (auto* track : tracks)
    {
        CHECK (phazite::getInstrument (*track) != nullptr);
        CHECK (track->getClips().isEmpty());
        const auto key = phazite::getTrackKey (*track);
        REQUIRE (key.has_value());
        CHECK (key->root == 2);
        CHECK (phazite::getSavedScaleName (key->scale) == "minor");
    }

    CHECK (session.getSelection().isSelected (tracks[1]));

    // What it starts with is in its first save: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (edit->getUndoManager().canUndo());
    CHECK (phazite::ProjectSession::suggestNewName (temp.folder) == "Untitled 2");

    // A folder of that name is there: nothing changes.
    const auto again = session.createProject (temp.folder, "Untitled", {});
    CHECK (again.failed());
    CHECK (again.getErrorMessage().contains ("already"));
    CHECK (session.getContext().getEdit() == edit);

    // Without instruments, a key or an outline: an empty song at 120 BPM in 4/4.
    phazite::NewProjectSettings plain;
    plain.withInstruments = false;
    REQUIRE (session.createProject (temp.folder, "Plain", plain).wasOk());
    auto* plainEdit = session.getContext().getEdit();
    REQUIRE (plainEdit != nullptr);
    CHECK (phazite::getTempo (*plainEdit) == doctest::Approx (120.0));
    CHECK (phazite::getSections (*plainEdit).isEmpty());
    CHECK (phazite::findSynthTrack (*plainEdit) == nullptr);
}

TEST_CASE ("New-project settings are checked")
{
    phazite::NewProjectSettings settings;
    CHECK (phazite::isValid (settings));

    settings.tempo = 400.0;
    CHECK_FALSE (phazite::isValid (settings));
    settings.tempo = 120.0;

    settings.timeSignature = { 4, 3 };
    CHECK_FALSE (phazite::isValid (settings));
    settings.timeSignature = { 7, 8 };
    CHECK (phazite::isValid (settings));

    settings.keyRoot = 12;
    CHECK_FALSE (phazite::isValid (settings));
    settings.keyRoot = 11;
    settings.scale = "nonsense";
    CHECK_FALSE (phazite::isValid (settings));
    settings.scale = "dorian";
    CHECK (phazite::isValid (settings));

    settings.outline = "polka";
    CHECK_FALSE (phazite::isValid (settings));
    settings.outline = "techno";
    CHECK (phazite::isValid (settings));
}

TEST_CASE ("Startup opens the most recent project")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto older = createClosedProject (temp.folder, "Older");
    const auto newer = createClosedProject (temp.folder, "Newer");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (older);
    recent.add (newer);

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder);
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == newer);
    CHECK (recent.getProjects() == juce::Array<juce::File> { newer, older });
}

TEST_CASE ("The app's services are made together, with the settings in memory")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::AppServices services (engine, { .settingsInMemory = true, .packsFolder = temp.folder });

    // The library shows Phazite Sounds' folder, installed or not.
    CHECK (services.library.getFactorySounds() == services.soundPack.getFolder());
    CHECK (services.soundPack.getFolder().isAChildOf (temp.folder));
    CHECK_FALSE (services.soundPack.isInstalled());

    // The recent projects are kept in the services' settings.
    const auto project = temp.folder.getChildFile ("Song/Song.phazite");
    REQUIRE (project.create().wasOk());
    services.recentProjects.add (project);
    CHECK (services.settings.get (phazite::Settings::recentProjects) == juce::StringArray { project.getFullPathName() });

    // The library indexes on its own thread; it is done before the services go.
    CHECK (phazite::test::runTimersUntil ([&services] { return ! services.library.isIndexing(); }, 10000));
}

TEST_CASE ("A project from a newer Phazite stays in Open Recent, and startup opens the next one")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    REQUIRE (projects.createDirectory().wasOk());
    const auto a = createClosedProject (projects, "A");

    // B: a copy of A, saved by a newer Phazite.
    const auto folderB = projects.getChildFile ("B");
    REQUIRE (a.getParentDirectory().copyDirectoryTo (folderB));
    const auto b = folderB.getChildFile (juce::String ("B") + phazite::Project::fileExtension);
    REQUIRE (folderB.getChildFile (a.getFileName()).moveFileTo (b));
    auto xml = phazite::test::readXml (b);
    REQUIRE (xml->getChildByName ("PHAZITE") != nullptr);
    xml->getChildByName ("PHAZITE")->setAttribute ("schemaVersion", phazite::schema::currentVersion + 1);
    REQUIRE (xml->writeTo (b));

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (a);
    recent.add (b); // the most recent

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, projects);
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String&, const juce::String& message) { errors.add (message); };
    session.openAtStartup();

    // A opens instead, after an error that says why B didn't, and B stays in Open Recent for the version that opens it.
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == a);
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].contains ("newer"));
    CHECK (recent.getProjects().contains (a));
    CHECK (recent.getProjects().contains (b));
    CHECK_FALSE (projects.getChildFile ("My First Song").exists()); // no first project was made
}

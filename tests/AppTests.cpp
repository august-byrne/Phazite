// SPDX-License-Identifier: AGPL-3.0-or-later
// The app's own code (src/app, the phazite_app library) without a window: the session that opens, makes and saves
// projects. Nothing here may reach a dialog (an AlertWindow, a FileChooser or a menu): Linux CI has no display.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/ProjectContext.h"
#include "model/project/internal/StarterProject.h"
#include "model/transport/internal/Tempo.h"
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
        phazite::ProjectSession session (engine, uiBehaviour, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
        int changes = 0;
        session.onProjectChanged = [&changes] { ++changes; };

        // Without an autosave to restore, it opens at once, as the project the commands and live MIDI act on.
        session.openProject (file);
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == file);
        CHECK (getEdit (session.getContext()) == &session.getProject()->getEdit());
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

TEST_CASE ("The first start makes My First Song, which its first save names and places")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings); // no recent projects: a first start

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder.getChildFile ("Projects"), unsaved);
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    const auto file = session.getProject()->getFile();
    CHECK (session.isUntitled());
    CHECK (file.isAChildOf (unsaved));
    CHECK (file.getFileName() == juce::String ("My First Song") + phazite::Project::fileExtension);
    CHECK (file.existsAsFile());
    CHECK (te::getAudioTracks (*getEdit (session.getContext())).size() == 2); // the starter song: drums and a synth
    CHECK (recent.getProjects().isEmpty());                                   // until it is saved

    // The computer keyboard plays the synth, in its octave: its track is selected, and live MIDI follows the selection.
    auto* synth = phazite::findSynthTrack (*getEdit (session.getContext()));
    REQUIRE (synth != nullptr);
    CHECK (synth->getName() == "Synth");
    CHECK (session.getSelection().isSelected (synth));

    // It is what the project starts with: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (getEdit (session.getContext())->getUndoManager().canUndo());
}

TEST_CASE ("A new project starts as the new-project window chooses, with the computer keyboard on its synth")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder, temp.folder.getChildFile ("Unsaved"));

    phazite::NewProjectSettings choice;
    choice.tempo = 90.0;
    choice.timeSignature = { 6, 8 };
    choice.keyRoot = 2; // D
    choice.scale = "minor";
    choice.outline = "hipHop";
    REQUIRE (phazite::isValid (choice));
    REQUIRE (session.createProject (choice).wasOk());
    CHECK (session.isUntitled());
    CHECK (session.getProject()->getFile().getFileNameWithoutExtension() == "Untitled");

    auto* edit = getEdit (session.getContext());
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

    // Without instruments, a key or an outline: an empty song at 120 BPM in 4/4.
    phazite::NewProjectSettings plain;
    plain.withInstruments = false;
    REQUIRE (session.createProject (plain).wasOk());
    auto* plainEdit = getEdit (session.getContext());
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

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == newer);
    CHECK (recent.getProjects() == juce::Array<juce::File> { newer, older });
}

TEST_CASE ("The app's services are made together, with the settings in memory")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::AppServices services (
        engine, { .settingsInMemory = true, .packsFolder = temp.folder, .presetsFolder = temp.folder.getChildFile ("Presets") });

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

TEST_CASE ("A project that doesn't open leaves Open Recent, and startup makes a first project instead")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");

    // A project file that holds no project.
    const auto broken = projects.getChildFile (juce::String ("Broken/Broken") + phazite::Project::fileExtension);
    REQUIRE (broken.create().wasOk());
    REQUIRE (broken.replaceWithText ("Not a project"));

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (broken);

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, projects, temp.folder.getChildFile ("Unsaved"));
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String&, const juce::String& message) { errors.add (message); };
    session.openAtStartup();

    // An error says why, the file is left as it was, and a first project opens.
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].contains (broken.getFullPathName()));
    CHECK (broken.loadFileAsString() == juce::String ("Not a project"));
    CHECK_FALSE (recent.getProjects().contains (broken));
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.isUntitled());
}

TEST_CASE ("A new project is untitled until its first save asks what to call it and where it goes")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, projects, unsaved);
    phazite::CommandRegistry registry;
    session.addCommands (registry);

    // The save dialog: what it suggests, and what it answers (no file: Cancel).
    juce::Array<juce::File> suggested;
    juce::File answer;
    session.chooseSaveFile = [&suggested, &answer] (const juce::File& file, std::function<void (const juce::File&)> callback)
    {
        suggested.add (file);
        callback (answer);
    };

    juce::StringArray errors;
    session.onError = [&errors] (const juce::String&, const juce::String& message) { errors.add (message); };

    REQUIRE (session.createProject ({}).wasOk());
    REQUIRE (session.isUntitled());
    const auto untitledFile = session.getProject()->getFile();
    CHECK (untitledFile.isAChildOf (unsaved));
    CHECK (recent.getProjects().isEmpty());
    CHECK (session.getExportFolder() == temp.folder.getChildFile ("Exports")); // not the untitled project's own folder

    auto& edit = *getEdit (session.getContext());
    session.getProject()->getPhaziteState().setProperty (phazite::schema::outline, "pop", &edit.getUndoManager());
    REQUIRE (session.getProject()->hasUnsavedChanges());

    // Save asks, from Documents/Phazite/Projects. Cancelled, the project stays untitled, its changes unsaved.
    REQUIRE (registry.invoke ("project.save"));
    REQUIRE (suggested.size() == 1);
    CHECK (suggested[0] == projects.getChildFile ("Untitled"));
    CHECK (session.isUntitled());
    CHECK (session.getProject()->hasUnsavedChanges());

    // Answered, it goes into a folder of its own there, with its undo history, and is a recent project from now on.
    answer = projects.getChildFile ("Beat.phazite"); // with the extension, as the dialog adds it
    REQUIRE (registry.invoke ("project.save"));
    const auto file = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    CHECK (session.getProject()->getFile() == file);
    CHECK (file.existsAsFile());
    CHECK_FALSE (session.isUntitled());
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK (edit.getUndoManager().canUndo());
    CHECK (recent.getProjects() == juce::Array<juce::File> { file });
    CHECK (session.getExportFolder() == file.getSiblingFile ("Renders"));
    CHECK (errors.isEmpty());

    // Later saves don't ask.
    session.getProject()->getPhaziteState().setProperty (phazite::schema::outline, "rock", &edit.getUndoManager());
    REQUIRE (registry.invoke ("project.save"));
    CHECK (suggested.size() == 2);
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());

    // The folder it was made in goes once it has closed.
    CHECK (untitledFile.existsAsFile());
    REQUIRE (session.createProject ({}).wasOk());
    CHECK_FALSE (untitledFile.getParentDirectory().exists());

    // A name a folder has there already is refused, and the project stays untitled.
    answer = projects.getChildFile ("Beat"); // without the extension, as a dialog may answer
    REQUIRE (registry.invoke ("project.saveAs"));
    CHECK (suggested.getLast() == projects.getChildFile ("Untitled"));
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].contains ("already"));
    CHECK (session.isUntitled());
}

namespace
{
// An untitled project's folder as an earlier Phazite left it, with an autosave newer than the project if it didn't close
// properly after a change, such as in a crash.
juce::File leaveUntitledProject (const juce::File& folder, bool withAutosave)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), folder, "Untitled", error);
    REQUIRE (project != nullptr);
    const auto file = project->getFile();
    juce::MemoryBlock autosave;

    if (withAutosave)
    {
        project->getPhaziteState().setProperty (phazite::schema::outline, "pop", nullptr);
        REQUIRE (project->autosave().wasOk());
        REQUIRE (phazite::Project::getAutosaveFile (file).loadFileAsData (autosave));
    }

    project = nullptr; // closing deletes the autosave; a crash wouldn't

    if (withAutosave)
    {
        REQUIRE (phazite::Project::getAutosaveFile (file).replaceWithData (autosave.getData(), autosave.getSize()));
        REQUIRE (file.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::minutes (1)));
    }

    return file;
}
} // namespace

TEST_CASE ("At startup, Phazite offers to restore an untitled project it didn't close properly, and clears up the others")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    // One with changes saved automatically, and one that closed without any, as earlier Phazites left them.
    const auto crashed = leaveUntitledProject (unsaved.getChildFile ("2026-10-16 10-00-00"), true);
    const auto closed = leaveUntitledProject (unsaved.getChildFile ("2026-10-16 09-00-00"), false);

    {
        phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, projects, unsaved);
        juce::Array<juce::File> asked;
        session.askToRestore = [&asked] (const juce::File& file, bool untitled, std::function<void (bool)> callback)
        {
            asked.add (file);
            CHECK (untitled);
            callback (true);
        };

        session.openAtStartup();

        // The one with nothing to restore is gone; the other is restored, untitled, with its changes unsaved.
        CHECK_FALSE (closed.getParentDirectory().getParentDirectory().exists());
        CHECK (asked == juce::Array<juce::File> { crashed });
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == crashed);
        CHECK (session.isUntitled());
        CHECK (session.getProject()->hasUnsavedChanges());
        CHECK (session.getProject()->getPhaziteState()[phazite::schema::outline].toString() == juce::String ("pop"));
    }

    // Closed without saving, it is gone too.
    CHECK_FALSE (crashed.getParentDirectory().getParentDirectory().exists());

    // Discarded, it goes, and startup goes on as usual: here, with a first project.
    const auto discarded = leaveUntitledProject (unsaved.getChildFile ("2026-10-16 11-00-00"), true);
    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, projects, unsaved);
    session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (false); };
    session.openAtStartup();
    CHECK_FALSE (discarded.getParentDirectory().getParentDirectory().exists());
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile().getFileNameWithoutExtension() == "My First Song");

#if JUCE_WINDOWS
    // A folder another Phazite holds is left alone, whatever is in it. (Linux's locks don't keep a process from itself.)
    const auto held = phazite::UnsavedFolder::create (engine, temp.folder.getChildFile ("Held"));
    REQUIRE (held != nullptr);
    CHECK (phazite::UnsavedFolder::findProjectToRestore (engine, temp.folder.getChildFile ("Held")) == juce::File());
    CHECK (held->getFolder().isDirectory());
#endif
}

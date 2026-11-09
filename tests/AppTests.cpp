// SPDX-License-Identifier: AGPL-3.0-or-later
// The app's own code (src/app, the phazite_app library) without a window: the session that opens, makes and saves
// projects. Nothing here may reach a dialog (an AlertWindow, a FileChooser or a menu): Linux CI has no display.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "app/RunMarker.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"

namespace
{
// A project made and closed again, as an earlier session would have left it.
juce::File createClosedProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project->getFile();
}
} // namespace

TEST_CASE ("The session opens and saves a project")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    {
        phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
        int changes = 0;
        session.onProjectChanged = [&changes] { ++changes; };

        // Without an autosave to restore, it opens at once, as the project the commands and live MIDI act on.
        session.openProject (file);
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == file);
        CHECK (findSong (session.getContext()) == &session.getProject()->getSong());
        CHECK (session.getSelection().getSong() == &session.getProject()->getSong());
        CHECK (getSession (session.getContext()) != nullptr); // what plays it
        CHECK (recent.getProjects() == juce::Array<juce::File> { file });
        CHECK (changes == 1);
        phazite::test::settle();
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());

        // Its commands save it.
        phazite::CommandRegistry registry;
        phazite::test::RenderHost renders;
        session.addCommands (registry, renders);
        const auto* save = registry.find ("project.save");
        REQUIRE (save != nullptr);
        CHECK (save->isEnabled());
        session.getProject()->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
        REQUIRE (session.getProject()->hasUnsavedChanges());
        REQUIRE (registry.invoke ("project.save"));
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());

        // With nothing unsaved, closing asks nothing.
        auto closed = false;
        session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
        CHECK (closed);
    }
}

TEST_CASE ("The first start makes an empty, untitled project, which its first save names and places")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings); // no recent projects: a first start

    phazite::ProjectSession session (engine, recent, temp.folder.getChildFile ("Projects"), unsaved);
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    const auto file = session.getProject()->getFile();
    CHECK (session.isUntitled());
    CHECK (file.isAChildOf (unsaved));
    CHECK (file.getFileName() == juce::String ("Untitled") + phazite::Project::fileExtension);
    CHECK (file.existsAsFile());
    CHECK (phazite::getAllPlaylistTracks (session.getContext()).empty()); // a new project starts empty
    CHECK (recent.getProjects().isEmpty());                               // until it is saved

    // It is what the project starts with: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (session.getProject()->getSong().getUndoManager().canUndo());
}

TEST_CASE ("An example song opens as a new untitled project, with the computer keyboard on its lead")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));

    // Help > Example Songs.
    phazite::CommandRegistry registry;
    phazite::test::RenderHost renders;
    session.addCommands (registry, renders);
    REQUIRE (registry.invoke ("help.example.exodus"));
    phazite::test::dispatchPendingMessages();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.isUntitled());
    CHECK (session.getProject()->getFile().getFileNameWithoutExtension() == "Exodus");

    auto& context = session.getContext();
    juce::StringArray names;

    for (const auto& row : phazite::getPlaylistRows (context))
        names.add (row.name);

    CHECK (names.joinIntoString (", ") == "Drums, Sub, Bass, Arp, Arp Pad, Sparkle");

    // The computer keyboard plays the arpeggio's sound: its track is selected, and live MIDI follows the selection.
    const auto selected = context.getSongSelection().getTracks();
    REQUIRE (selected.size() == 1);
    CHECK (phazite::getTrackRow (context, selected.front())->name == "Arp");

    // The song is what the project starts with: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (findSong (context)->getUndoManager().canUndo());
}

TEST_CASE ("A new project starts as the new-project window chooses, with no tracks; the tracks added get its key")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));

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

    auto& context = session.getContext();
    auto* song = findSong (context);
    REQUIRE (song != nullptr);
    CHECK (phazite::getTempo (*song) == doctest::Approx (90.0));
    CHECK (phazite::getTimeSignature (*song) == phazite::TimeSignature { 6, 8 });
    CHECK (phazite::getSections (*song).size() == phazite::findOutline ("hipHop")->sections.size());
    CHECK (phazite::getAllPlaylistTracks (context).empty());

    // What it starts with is in its first save: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (song->getUndoManager().canUndo());

    // A track added gets the key chosen.
    const auto track = phazite::addTrackAtEnd (context, false);
    const auto key = phazite::getTrackKey (context, track);
    REQUIRE (key.has_value());
    CHECK (key->root == 2);
    CHECK (phazite::getSavedScaleName (key->scale) == "minor");

    // Without a key or an outline: an empty song at 120 BPM in 4/4.
    REQUIRE (session.createProject ({}).wasOk());
    auto* plainSong = findSong (session.getContext());
    REQUIRE (plainSong != nullptr);
    CHECK (phazite::getTempo (*plainSong) == doctest::Approx (120.0));
    CHECK (phazite::getSections (*plainSong).empty());
    CHECK (phazite::getAllPlaylistTracks (session.getContext()).empty());
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
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto older = createClosedProject (temp.folder, "Older");
    const auto newer = createClosedProject (temp.folder, "Newer");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (older);
    recent.add (newer);

    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == newer);
    CHECK (recent.getProjects() == juce::Array<juce::File> { newer, older });
}

TEST_CASE ("The project or DAWproject a command line names is the file to open, as Explorer passes a file double-clicked")
{
    phazite::test::TemporaryFolder temp;
    const auto project = createClosedProject (temp.folder.getChildFile ("With Spaces"), "Song");
    const auto dawProject = temp.folder.getChildFile ("Elsewhere.dawproject");
    const auto sound = temp.folder.getChildFile ("Kick.wav");
    REQUIRE (dawProject.replaceWithText ("a zip, as other DAWs write it"));
    REQUIRE (sound.replaceWithText ("not a project"));

    using phazite::ProjectSession;
    CHECK (ProjectSession::findFileToOpen ("\"" + project.getFullPathName() + "\"") == project);
    CHECK (ProjectSession::findFileToOpen ("--some-option \"" + project.getFullPathName() + "\"") == project);
    CHECK (ProjectSession::findFileToOpen ("\"" + dawProject.getFullPathName() + "\"") == dawProject);
    CHECK (ProjectSession::findFileToOpen ("\"" + sound.getFullPathName() + "\"") == juce::File());
    CHECK (ProjectSession::findFileToOpen ("\"" + temp.folder.getChildFile ("Gone.phazite").getFullPathName() + "\"") == juce::File());
    CHECK (ProjectSession::findFileToOpen ({}) == juce::File());
}

TEST_CASE ("Startup opens the project Phazite was started with; after a crash it asks how only if that was the project open then")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto older = createClosedProject (temp.folder, "Older");
    const auto newer = createClosedProject (temp.folder, "Newer");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (older);
    recent.add (newer); // each project opened goes first, as the one open last

    auto asked = 0;
    const auto answerOpen = [&asked] (const juce::File&, std::function<void (phazite::ProjectSession::AfterCrash)> callback)
    {
        ++asked;
        callback (phazite::ProjectSession::AfterCrash::open);
    };

    {
        phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
        session.askAfterCrash = answerOpen;
        session.openAtStartup (false, older);
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == older);
    }

    {
        phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
        session.askAfterCrash = answerOpen;
        session.openAtStartup (true, recent.getProjects()[1]); // not the one open at the crash
        REQUIRE (session.getProject() != nullptr);
        CHECK (asked == 0);
    }

    {
        phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
        session.askAfterCrash = answerOpen;
        const auto wasOpen = recent.getProjects().getFirst();
        session.openAtStartup (true, wasOpen);
        CHECK (asked == 1);
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == wasOpen);
    }
}

TEST_CASE ("A DAWproject double-clicked imports as a new project, and the open one's changes are saved first")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto dawProject = temp.folder.getChildFile ("Elsewhere.dawproject");

    {
        phazite::test::TestSong elsewhere;
        elsewhere.addTrack ("Bass");
        auto content = phazite::describeDawProject (*phazite::findSong (elsewhere.context), "Elsewhere", {});
        content.bounces.clear();
        REQUIRE (phazite::writeDawProject (content, {}, dawProject).wasOk());
    }

    const auto project = createClosedProject (temp.folder, "Song");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.setAutoSave (true); // so the changes are saved without asking, as with Auto Save on
    session.openProject (project);
    REQUIRE (session.getProject() != nullptr);
    phazite::test::settle();

    // A change not saved yet, then the DAWproject from Explorer: the change saved, the import open, untitled.
    session.getProject()->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
    REQUIRE (session.getProject()->hasUnsavedChanges());
    session.openFile (dawProject);
    phazite::test::dispatchPendingMessages();
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.isUntitled());
    juce::StringArray names;

    for (const auto track : phazite::getTrackIDs (session.getProject()->getSong()))
        names.add (phazite::getTrackName (session.getProject()->getSong(), track));

    CHECK (names.contains ("Bass"));
    CHECK (project.loadFileAsString().contains ("pop"));

    // A project double-clicked while it is open already stays as it is.
    session.openFile (project);
    session.openFile (project);
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == project);
}

TEST_CASE ("The app's services are made together, with the settings in memory")
{
    auto& engine = phazite::test::getEngineContext();
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

    // So are the plug-ins a scan found, and those that crashed as they loaded.
    auto& known = phazite::getPluginHost (services.engineContext).getKnownPlugins();
    juce::PluginDescription scanned;
    scanned.name = "Scanned";
    scanned.pluginFormatName = "VST3";
    scanned.fileOrIdentifier = temp.folder.getChildFile ("Scanned.vst3").getFullPathName();
    known.addType (scanned);
    CHECK (phazite::test::dispatchMessagesUntil (
        [&] { return services.settings.get (phazite::Settings::knownPlugins).contains (scanned.fileOrIdentifier); }));
    known.removeType (scanned);

    auto& plugins = phazite::getPluginHost (services.engineContext);
    plugins.setCrashedWhileLoading ({ "Crashed" });
    plugins.sendChangeMessage();
    CHECK (phazite::test::dispatchMessagesUntil (
        [&] { return services.settings.get (phazite::Settings::pluginsCrashedLoading) == juce::StringArray { "Crashed" }; }));
    plugins.setCrashedWhileLoading ({}); // the host is the engine context's, which the other tests share

    // The library indexes on its own thread; it is done before the services go.
    CHECK (phazite::test::runTimersUntil ([&services] { return ! services.library.isIndexing(); }, 10000));
}

TEST_CASE ("A project that doesn't open leaves Open Recent, and startup makes a first project instead")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");

    // A project file that holds no project.
    const auto broken = projects.getChildFile (juce::String ("Broken/Broken") + phazite::Project::fileExtension);
    REQUIRE (broken.create().wasOk());
    REQUIRE (broken.replaceWithText ("Not a project"));

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (broken);

    phazite::ProjectSession session (engine, recent, projects, temp.folder.getChildFile ("Unsaved"));
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
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, projects, unsaved);
    phazite::CommandRegistry registry;
    phazite::test::RenderHost renders;
    session.addCommands (registry, renders);

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

    auto& song = *findSong (session.getContext());
    song.getState().setProperty (phazite::schema::outline, "pop", &song.getUndoManager());
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
    CHECK (song.getUndoManager().canUndo());
    CHECK (recent.getProjects() == juce::Array<juce::File> { file });
    CHECK (session.getExportFolder() == file.getSiblingFile ("Renders"));
    CHECK (errors.isEmpty());

    // Later saves don't ask.
    song.getState().setProperty (phazite::schema::outline, "rock", &song.getUndoManager());
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

TEST_CASE ("A first save and Save As wait for the versions being written, which go with the project, every one of them")
{
    // CI's linux-debug, 2026-11-06: a first save just as a new project opened couldn't copy its versions, as the initial
    // commit was being written into them on the history's thread; and a version written meanwhile went into the folder left
    // behind.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, projects, temp.folder.getChildFile ("Unsaved"));
    phazite::CommandRegistry registry;
    phazite::test::RenderHost renders;
    session.addCommands (registry, renders);

    auto answer = projects.getChildFile ("Beat.phazite");
    session.chooseSaveFile = [&answer] (const juce::File&, std::function<void (const juce::File&)> callback) { callback (answer); };
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String&, const juce::String& message) { errors.add (message); };

    auto& context = session.getContext();
    const auto settled = [&context]
    {
        const auto* history = phazite::getVersionHistory (context);
        return history != nullptr && ! history->isBusy() && ! phazite::getVersions (context).empty();
    };

    const auto names = [&context]
    {
        juce::StringArray all;

        for (const auto& version : phazite::getVersions (context))
            all.add (version.name);

        return all;
    };

    // Saved as soon as its versions are read, which is when its initial commit starts on its way to the disk.
    REQUIRE (session.createProject ({}).wasOk());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&context] { return ! phazite::isReadingVersions (context); }));
    REQUIRE (registry.invoke ("project.save"));
    CHECK (errors.isEmpty());
    REQUIRE (phazite::test::dispatchMessagesUntil (settled));
    CHECK (names() == juce::StringArray ("Initial commit"));

    // A copy saved as a version is being written: the copy has it, and goes on in its own folder.
    phazite::setVersionDraft (context, "Sketch", {});
    REQUIRE (phazite::saveVersion (context));
    answer = projects.getChildFile ("Beat copy.phazite");
    REQUIRE (registry.invoke ("project.saveAs"));
    CHECK (errors.isEmpty());
    REQUIRE (session.getProject()->getFile() == projects.getChildFile ("Beat copy").getChildFile ("Beat copy.phazite"));
    REQUIRE (phazite::test::dispatchMessagesUntil (settled));
    CHECK (names() == juce::StringArray ("Sketch", "Initial commit"));

    // The first has both, as written there, when it opens again.
    const auto first = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    session.openProject (first);
    REQUIRE (session.getProject()->getFile() == first);
    REQUIRE (phazite::test::dispatchMessagesUntil (settled));
    CHECK (names() == juce::StringArray ("Sketch", "Initial commit"));
    CHECK (errors.isEmpty());
}

namespace
{
// An untitled project's folder as an earlier Phazite left it, with an autosave newer than the project if it didn't close
// properly after a change, such as in a crash.
juce::File leaveUntitledProject (const juce::File& folder, bool withAutosave)
{
    juce::String error;
    auto project = phazite::Project::create (folder, "Untitled", error);
    REQUIRE (project != nullptr);
    const auto file = project->getFile();
    juce::MemoryBlock autosave;

    if (withAutosave)
    {
        project->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
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
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    // One with changes saved automatically, and one that closed without any, as earlier Phazites left them.
    const auto crashed = leaveUntitledProject (unsaved.getChildFile ("2026-10-16 10-00-00"), true);
    const auto closed = leaveUntitledProject (unsaved.getChildFile ("2026-10-16 09-00-00"), false);

    {
        phazite::ProjectSession session (engine, recent, projects, unsaved);
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
        CHECK (session.getProject()->getSong().getState()[phazite::schema::outline].toString() == juce::String ("pop"));
    }

    // Closed without saving, it is gone too.
    CHECK_FALSE (crashed.getParentDirectory().getParentDirectory().exists());

    // Discarded, it goes, and startup goes on as usual: here, with a first project.
    const auto discarded = leaveUntitledProject (unsaved.getChildFile ("2026-10-16 11-00-00"), true);
    phazite::ProjectSession session (engine, recent, projects, unsaved);
    session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (false); };
    session.openAtStartup();
    CHECK_FALSE (discarded.getParentDirectory().getParentDirectory().exists());
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile().getFileNameWithoutExtension() == "Untitled");

#if JUCE_WINDOWS
    // A folder another Phazite holds is left alone, whatever is in it. (Linux's locks don't keep a process from itself.)
    const auto held = phazite::UnsavedFolder::create (temp.folder.getChildFile ("Held"));
    REQUIRE (held != nullptr);
    CHECK (phazite::UnsavedFolder::findProjectToRestore (temp.folder.getChildFile ("Held")) == juce::File());
    CHECK (held->getFolder().isDirectory());
#endif
}

TEST_CASE ("An untitled song's recordings survive a crash before its first autosave")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    // A song that recorded a take, then crashed before anything saved it automatically: the take is in its Audio folder
    // only.
    const auto crashed = leaveUntitledProject (unsaved.getChildFile ("2026-10-21 10-00-00"), false);
    const auto take = phazite::test::writeSound (crashed.getSiblingFile ("Audio").getChildFile ("Take 1.wav"), 1.0);

    phazite::ProjectSession session (engine, recent, projects, unsaved);
    juce::Array<juce::File> asked;
    juce::StringArray notices;
    session.askToRestore = [&asked] (const juce::File& file, bool, std::function<void (bool)> callback)
    {
        asked.add (file);
        callback (true);
    };
    session.onNotice = [&notices] (const juce::String& title, const juce::String&) { notices.add (title); };
    session.openAtStartup();

    // Offered and restored: the take is back in the song, at its start, as an unsaved change and no undo step.
    CHECK (asked == juce::Array<juce::File> { crashed });
    REQUIRE (session.getProject() != nullptr);
    const auto& song = session.getProject()->getSong();
    juce::Array<juce::File> played;

    for (const auto& clip : phazite::getClipStates (song))
        if (phazite::isAudioState (clip))
            played.add (phazite::getClipFile (song, clip));

    CHECK (played == juce::Array<juce::File> { take });
    CHECK (notices == juce::StringArray { "Recordings recovered" });
    CHECK (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (song.getUndoManager().canUndo());
}

TEST_CASE ("The run marker tells a start after Phazite didn't close properly")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Running");

    {
        const phazite::RunMarker marker (file);
        CHECK_FALSE (marker.didLastRunEndBadly());
        CHECK (file.existsAsFile());
    }

    CHECK_FALSE (file.exists());     // closed properly
    REQUIRE (file.create().wasOk()); // as a crash leaves it
    const phazite::RunMarker afterCrash (file);
    CHECK (afterCrash.didLastRunEndBadly());
}

TEST_CASE ("After a crash, Phazite asks how to open the last project, and can open it with its plug-ins off")
{
    using AfterCrash = phazite::ProjectSession::AfterCrash;
    auto& engine = phazite::test::getEngineContext();
    const phazite::test::TestPlugin testPlugin;
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    // The last project open, with a plug-in on its first track.
    juce::File file;
    phazite::DeviceID plugin;

    {
        juce::String error;
        auto project = phazite::Project::create (projects, "Crashing", error);
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        plugin = phazite::test::TestPlugin::add (song, phazite::getTrackIDs (song).front());
        REQUIRE (project->save().wasOk());
        file = project->getFile();
    }

    recent.add (file);

    const auto openAfterCrash = [&] (AfterCrash choice, juce::StringArray& notices)
    {
        auto session = std::make_unique<phazite::ProjectSession> (engine, recent, projects, unsaved);
        juce::Array<juce::File> asked;
        session->askAfterCrash = [&asked, choice] (const juce::File& projectFile, std::function<void (AfterCrash)> callback)
        {
            asked.add (projectFile);
            callback (choice);
        };
        session->onNotice = [&notices] (const juce::String& title, const juce::String&) { notices.add (title); };
        session->openAtStartup (true);
        CHECK (asked == juce::Array<juce::File> { file });
        return session;
    };

    // How the plug-in plays in the session's project.
    const auto getStatus = [plugin] (phazite::ProjectSession& session)
    {
        REQUIRE (session.getProject() != nullptr);
        auto* playback = getSession (session.getContext());
        REQUIRE (playback != nullptr);
        return playback->getPlugins().getStatus (plugin);
    };

    // With its plug-ins off: the plug-in stays unloaded, and Phazite says so; the next project loads them again.
    {
        juce::StringArray notices;
        auto session = openAfterCrash (AfterCrash::openWithoutPlugins, notices);
        CHECK (getStatus (*session) == phazite::PluginInstances::Status::off);
        CHECK (phazite::findPluginInstance (session->getProject()->getSong(), plugin) == nullptr);
        CHECK (notices == juce::StringArray { "Plug-ins off" });
    }

    {
        juce::StringArray notices;
        auto session = openAfterCrash (AfterCrash::open, notices);
        CHECK (getStatus (*session) == phazite::PluginInstances::Status::playing);
        CHECK (phazite::findPluginInstance (session->getProject()->getSong(), plugin) != nullptr);
        CHECK (notices.isEmpty());
    }

    // Or not at all: a new project instead.
    {
        juce::StringArray notices;
        auto session = openAfterCrash (AfterCrash::startNew, notices);
        REQUIRE (session->getProject() != nullptr);
        CHECK (session->getProject()->getFile() != file);
    }
}

TEST_CASE ("A project file that won't open offers its newest backup that reads in its place, which a save writes back")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    // Saved twice, so that the first save is a backup; then the project file damaged.
    const auto file = createClosedProject (temp.folder, "Song");

    {
        juce::String error;
        auto project = phazite::Project::open (file, false, error);
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        song.getState().setProperty (phazite::schema::notes, "Saved twice", &song.getUndoManager());
        REQUIRE (project->save().wasOk());
    }

    REQUIRE (file.replaceWithText ("<PHAZITE damaged"));

    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    juce::String offered;
    session.askWhichCopy =
        [&offered] (const juce::String&, const juce::String& backup, const juce::String&, std::function<void (int)> answer)
    {
        offered = backup;
        answer (1);
    };

    session.openProject (file);
    CHECK (offered.startsWith ("the backup of"));
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == file);
    CHECK (session.getProject()->hasUnsavedChanges());

    // Saved, the project file reads again.
    REQUIRE (session.getProject()->save().wasOk());
    juce::String error;
    CHECK (phazite::Project::open (file, false, error) != nullptr);
}

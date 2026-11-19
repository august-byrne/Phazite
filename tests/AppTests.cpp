// SPDX-License-Identifier: AGPL-3.0-or-later
// The app's own code (src/app, the phazite_app library) without a window: the session that opens, makes and saves
// projects. Nothing here may reach a dialog (an AlertWindow, a FileChooser or a menu): Linux CI has no display.
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "HeldFile.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/AppServices.h"
#include "app/MainWindow.h"
#include "app/ProjectSession.h"
#include "app/RunMarker.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"

#include <optional>
#include <vector>

// Last, so that its macros can't change the headers above: a file held by another program, as Windows lets one hold it.
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
        int changes = 0; // before the session, which tells of its project as it goes too
        phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
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
        phazite::test::dispatchPendingMessages();
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

TEST_CASE ("A problem with the open song that the model can't show itself shows as an error")
{
    // Such as sound lost from a take, which the song's playback session reports as the take is finished
    // (model/transport/internal/Take.h), through the project context.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String& title, const juce::String& message) { errors.add (title + ": " + message); };
    session.openProject (file);

    auto* playback = getSession (session.getContext());
    REQUIRE (playback != nullptr);
    REQUIRE (playback->onProblem != nullptr);
    playback->onProblem ("Part of the recording was lost", "Vocals at bar 4 (2.0 s).");
    CHECK (errors == juce::StringArray { "Part of the recording was lost: Vocals at bar 4 (2.0 s)." });
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
    phazite::test::dispatchPendingMessages();
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

    // Its folder of drums, each a track of its own, and its two buses with their parts.
    CHECK (names.joinIntoString (", ")
           == "Drums, Kick, Snare, Clap, Open Hat, Perc, Bass + Sparkle, Rolling Bass, Sub, Sparkle, Arp, Square, Wide Saw, Supersaw");

    // The computer keyboard plays the arpeggio's lead layer: its track is selected, and live MIDI follows the selection.
    const auto selected = context.getSongSelection().getTracks();
    REQUIRE (selected.size() == 1);
    CHECK (phazite::getTrackRow (context, selected.front())->name == "Square");

    // The song is what the project starts with: no unsaved changes, nothing to undo.
    phazite::test::dispatchPendingMessages();
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
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (song->getUndoManager().canUndo());

    // A track added gets the key chosen.
    const auto track = phazite::addTrackAtEnd (context, false);
    const auto key = phazite::getTrackKey (context, track);
    REQUIRE (key.has_value());
    CHECK (key->root == 2);
    CHECK (phazite::getSavedScaleName (key->scale) == "minor");

    // Without a key or an outline: an empty song at 120 BPM in 4/4.
    const auto plain = session.createProject ({});
    REQUIRE_MESSAGE (plain.wasOk(), plain.getErrorMessage().toStdString());
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
    phazite::test::dispatchPendingMessages();

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

    // So are the plug-ins a scan found, and those that crashed as they loaded; and the library hears of those found, so that
    // the browser and search list them again.
    REQUIRE (phazite::test::runTimersUntil ([&services] { return ! services.library.isIndexing(); }));
    phazite::test::dispatchPendingMessages(); // the index's own change told
    auto libraryTold = false;
    services.library.onIndexChanged = [&libraryTold] { libraryTold = true; };
    auto& known = phazite::getPluginHost (services.engineContext).getKnownPlugins();
    juce::PluginDescription scanned;
    scanned.name = "Scanned";
    scanned.pluginFormatName = "VST3";
    scanned.fileOrIdentifier = temp.folder.getChildFile ("Scanned.vst3").getFullPathName();
    known.addType (scanned);
    CHECK (phazite::test::dispatchMessagesUntil (
        [&] { return services.settings.get (phazite::Settings::knownPlugins).contains (scanned.fileOrIdentifier); }));
    CHECK (phazite::test::dispatchMessagesUntil ([&libraryTold] { return libraryTold; }));
    services.library.onIndexChanged = nullptr;
    known.removeType (scanned);

    auto& plugins = phazite::getPluginHost (services.engineContext);
    plugins.setCrashedWhileLoading ({ "Crashed" });
    plugins.sendChangeMessage();
    CHECK (phazite::test::dispatchMessagesUntil (
        [&] { return services.settings.get (phazite::Settings::pluginsCrashedLoading) == juce::StringArray { "Crashed" }; }));
    plugins.setCrashedWhileLoading ({}); // the host is the engine context's, which the other tests share

    // The library indexes on its own thread; it is done before the services go.
    CHECK (phazite::test::runTimersUntil ([&services] { return ! services.library.isIndexing(); }));
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

    // An error says why, once no backup and no commit were found in the background, the file is left as it was, and a
    // first project opens.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&errors] { return ! errors.isEmpty(); }));
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&session] { return ! session.isUntitled(); })); // copied in the background
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&context] { return ! phazite::isReadingVersions (context); },
                                                   phazite::test::versionHistoryTimeout));
    REQUIRE (registry.invoke ("project.save"));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&session] { return ! session.isUntitled(); }));
    CHECK (errors.isEmpty());
    REQUIRE (phazite::test::dispatchMessagesUntil (settled, phazite::test::versionHistoryTimeout));
    CHECK (names() == juce::StringArray ("First version"));

    // A copy saved as a version is being written: the copy has it, and goes on in its own folder. (A version records a change:
    // one of none isn't written.)
    phazite::setProjectNotes (context, "A sketch");
    phazite::setVersionDraft (context, "Sketch", {});
    REQUIRE (phazite::saveVersion (context));
    answer = projects.getChildFile ("Beat copy.phazite");
    REQUIRE (registry.invoke ("project.saveAs"));
    CHECK (errors.isEmpty());
    REQUIRE (session.getProject()->getFile() == projects.getChildFile ("Beat copy").getChildFile ("Beat copy.phazite"));
    REQUIRE (phazite::test::dispatchMessagesUntil (settled, phazite::test::versionHistoryTimeout));
    CHECK (names() == juce::StringArray ("Sketch", "First version"));

    // The first has both, as written there, when it opens again.
    const auto first = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    session.openProject (first);
    REQUIRE (session.getProject()->getFile() == first);
    REQUIRE (phazite::test::dispatchMessagesUntil (settled, phazite::test::versionHistoryTimeout));
    CHECK (names() == juce::StringArray ("Sketch", "First version"));
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

TEST_CASE ("A project restored after a crash asks before it closes, though nothing changed since, and keeps what it restored")
{
    using Answer = phazite::ProjectSession::UnsavedChanges;
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    const auto unsaved = temp.folder.getChildFile ("Unsaved");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    const auto other = createClosedProject (projects, "Other");

    // An untitled project with changes saved automatically, as a crash left it, restored at the next start.
    const auto crashed = leaveUntitledProject (unsaved.getChildFile ("2026-11-13 10-00-00"), true);
    phazite::ProjectSession session (engine, recent, projects, unsaved);
    session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (true); };
    juce::Array<juce::File> asked;
    auto answer = Answer::cancel;
    session.askToSave = [&asked, &answer] (const juce::File& file, const juce::String&, std::function<void (Answer)> callback)
    {
        asked.add (file);
        callback (answer);
    };

    session.openAtStartup();
    REQUIRE (session.getProject() != nullptr);
    REQUIRE (session.getProject()->getFile() == crashed);
    phazite::test::dispatchPendingMessages();

    // Another project opened before anything else changed: what was restored is in no file the user chose, so Phazite
    // asks. Cancelled, the project stays open, with its folder and its autosave.
    session.openProject (other);
    CHECK (asked == juce::Array<juce::File> { crashed });
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == crashed);
    CHECK (crashed.existsAsFile());
    CHECK (phazite::Project::getAutosaveFile (crashed).existsAsFile());
    CHECK (session.getProject()->hasUnsavedChanges());

    // Asked again the next time; "Don't save" is the user's word that it may go, once no write of it is under way (one
    // that held a file open left the folder for a later start: seen once beside other builds).
    REQUIRE (phazite::test::dispatchMessagesUntil ([&session] { return ! session.getProject()->isAutosaving(); }));
    answer = Answer::discard;
    session.openProject (other);
    CHECK (asked.size() == 2);
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == other);

    // Its folder goes once nothing of its versions is still being written into it (whenNoVersionsWriteInto()).
    CHECK (phazite::test::dispatchMessagesUntil ([&crashed] { return ! crashed.getParentDirectory().getParentDirectory().exists(); }));
}

TEST_CASE ("A saved project restored after a crash saves what it restored as it closes with Auto Save, and asks without")
{
    using Answer = phazite::ProjectSession::UnsavedChanges;
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    for (const auto autoSave : { true, false })
    {
        // A project in a folder of its own, left by a crash with an autosave that holds an outline its file doesn't.
        const auto file = leaveUntitledProject (projects.getChildFile (autoSave ? "With" : "Without"), true);
        REQUIRE_FALSE (file.loadFileAsString().contains ("pop"));

        phazite::ProjectSession session (engine, recent, projects, temp.folder.getChildFile ("Unsaved"));
        session.setAutoSave (autoSave);
        session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (true); };
        auto asked = 0;
        session.askToSave = [&asked] (const juce::File&, const juce::String&, std::function<void (Answer)> callback)
        {
            ++asked;
            callback (Answer::save);
        };

        session.openProject (file);
        REQUIRE (session.getProject() != nullptr);
        REQUIRE_FALSE (session.isUntitled());

        // Closed at once, before Auto Save's timer came by.
        auto closed = false;
        session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
        CHECK (closed);
        CHECK (asked == (autoSave ? 0 : 1));
        CHECK (file.loadFileAsString().contains ("pop"));
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    }
}

TEST_CASE ("A save that fails says so, and the project stays open with its changes unsaved")
{
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String& title, const juce::String& message) { errors.add (title + ": " + message); };
    phazite::CommandRegistry registry;
    phazite::test::RenderHost renders;
    session.addCommands (registry, renders);
    session.setAutoSave (true);
    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);
    phazite::test::dispatchPendingMessages();

    session.getProject()->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
    REQUIRE (session.getProject()->hasUnsavedChanges());

    // A folder where the project file goes, which a file can't take the place of: Save says that it didn't.
    REQUIRE (file.deleteFile());
    REQUIRE (file.createDirectory().wasOk());
    REQUIRE (file.getChildFile ("In the way").replaceWithText ("in the way"));
    REQUIRE (registry.invoke ("project.save"));
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].startsWith ("Phazite couldn't save the project: "));
    CHECK (errors[0].contains (file.getFullPathName()));
    CHECK (session.getProject()->hasUnsavedChanges());

    // Closing, which saves with Auto Save, goes on to ask what to do instead (known bug 88: it gave up, and the user could
    // neither quit nor open another project). Cancelled, the project stays open.
    using Answer = phazite::ProjectSession::UnsavedChanges;
    auto answer = Answer::cancel;
    juce::StringArray asked; // why the save failed, each time it asked
    session.askToSave = [&asked, &answer] (const juce::File&, const juce::String& saveFailed, std::function<void (Answer)> callback)
    {
        asked.add (saveFailed);
        callback (answer);
    };

    auto closed = true;
    session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
    REQUIRE (asked.size() == 1);
    CHECK (asked[0].contains (file.getFullPathName()));
    CHECK_FALSE (closed);
    CHECK (errors.size() == 1);
    CHECK (session.getProject()->hasUnsavedChanges());

    // "Save As...": the changes saved somewhere else, as a copy, and it closes.
    answer = Answer::saveAs;
    const auto elsewhere = temp.folder.getChildFile ("Elsewhere");
    session.chooseSaveFile = [&elsewhere] (const juce::File&, std::function<void (const juce::File&)> callback)
    { callback (elsewhere.getChildFile ("Rescued")); };
    session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
    CHECK (asked.size() == 2);
    CHECK (closed);
    const auto rescued = elsewhere.getChildFile ("Rescued").getChildFile ("Rescued.phazite");
    CHECK (rescued.loadFileAsString().contains ("pop"));
    CHECK (recent.getProjects().contains (rescued));

    // "Don't Save" closes it.
    answer = Answer::discard;
    session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
    CHECK (asked.size() == 3);
    CHECK (closed);
    CHECK (session.getProject()->hasUnsavedChanges()); // closeThen() leaves the closing to its caller

    // Once the file can be written, it is saved.
    REQUIRE (file.deleteRecursively());
    REQUIRE (registry.invoke ("project.save"));
    CHECK (errors.size() == 1);
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK (file.loadFileAsString().contains ("pop"));
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

        // It loads in the background, as the project opened (model/plugins/internal/PluginInstances.h).
        CHECK (phazite::test::dispatchMessagesUntil ([&] { return getStatus (*session) != phazite::PluginInstances::Status::loading; }));
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&offered] { return offered.isNotEmpty(); })); // read in the background
    CHECK (offered.startsWith ("the backup of"));
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == file);
    CHECK (session.getProject()->hasUnsavedChanges());

    // Saved, the project file reads again.
    REQUIRE (session.getProject()->save().wasOk());
    juce::String error;
    CHECK (phazite::Project::open (file, false, error) != nullptr);
}

namespace
{
// A project with a commit on its branch ("Kept", with a track named Bass), copied with its folders into parentFolder, as
// an earlier session left it, its project file then damaged as a write cut short could leave it; its backups taken away
// unless keepBackups, and the commit's pieces damaged too if commitDamaged. Its tracks, as the commit has them.
struct DamagedProject
{
    juce::File file;
    size_t tracks = 0;
};

DamagedProject makeDamagedProject (const juce::File& parentFolder, bool keepBackups, bool commitDamaged)
{
    DamagedProject made;
    phazite::test::TestSong s;
    s.addTrack ("Bass");
    phazite::setVersionDraft (s.context, "Kept", {});
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    phazite::test::waitForVersionsWrites();
    made.tracks = phazite::getTrackElements (phazite::findSong (s.context)->getState()).size();

    const auto folder = s.getProject().getFile().getParentDirectory();
    const auto copy = parentFolder.getChildFile (folder.getFileName());
    REQUIRE (folder.copyDirectoryTo (copy));
    made.file = copy.getChildFile (s.getProject().getFile().getFileName());
    const auto backups = copy.getChildFile ("Backups");
    REQUIRE (backups.deleteRecursively());

    if (keepBackups) // the file as it was saved, a backup, named as Phazite names them
    {
        REQUIRE (backups.createDirectory());
        REQUIRE (made.file.copyFileTo (
            backups.getChildFile (made.file.getFileNameWithoutExtension() + " 2026-11-01 10-00-00" + made.file.getFileExtension())));
    }

    REQUIRE (made.file.replaceWithText ("<PHAZITE damaged"));

    if (commitDamaged)
        for (const auto& pack : copy.getChildFile ("Versions").findChildFiles (juce::File::findFiles, true, "*.pack"))
            REQUIRE (pack.replaceWithText ("damaged"));

    return made;
}
} // namespace

TEST_CASE ("A project file that won't open offers its branch's last commit, read on a thread of its own while the window holds")
{
    // Known bug 184: the backups and the last commit were read on the message thread before the question showed.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    struct Offer
    {
        juce::String backup, commit;
    };

    std::vector<Offer> offers;
    juce::StringArray errors;
    auto answer = 0;
    const auto makeSession = [&]
    {
        auto session = std::make_unique<phazite::ProjectSession> (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
        session->askWhichCopy =
            [&] (const juce::String&, const juce::String& backup, const juce::String& commit, std::function<void (int)> then)
        {
            offers.push_back ({ backup, commit });
            then (answer);
        };
        session->onError = [&errors] (const juce::String& title, const juce::String& message) { errors.add (title + ": " + message); };
        return session;
    };

    SUBCASE ("Opened in the project's place, its song read from the store on no message thread")
    {
        const auto damaged = makeDamagedProject (temp.folder, false, false);
        auto session = makeSession();
        answer = 2;
        const auto before = phazite::VersionStore::countSongsRead();
        session->openProject (damaged.file);
        CHECK (session->getContext().isHoldingForWork()); // the window held while it reads
        REQUIRE (phazite::test::dispatchMessagesUntil ([&offers] { return ! offers.empty(); }));
        CHECK_FALSE (session->getContext().isHoldingForWork()); // let go of before the question
        CHECK (offers[0].backup.isEmpty());
        CHECK (offers[0].commit == "the last version, \"Kept\"");
        CHECK (phazite::VersionStore::countSongsRead().onMessageThread == before.onMessageThread);
        CHECK (phazite::VersionStore::countSongsRead().all > before.all);

        REQUIRE (session->getProject() != nullptr);
        CHECK (session->getProject()->getFile() == damaged.file);
        CHECK (session->getProject()->hasUnsavedChanges());
        CHECK (phazite::getTrackElements (session->getProject()->getSong().getState()).size() == damaged.tracks);
        CHECK (errors.isEmpty());
    }

    SUBCASE ("Declined: a first project instead, the damaged one off the recent list")
    {
        const auto damaged = makeDamagedProject (temp.folder, true, false);
        auto session = makeSession();
        answer = 0;
        session->openProject (damaged.file);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&offers] { return ! offers.empty(); }));
        CHECK (offers[0].backup.startsWith ("the backup of"));
        CHECK (offers[0].commit.isNotEmpty());
        REQUIRE (session->getProject() != nullptr);
        CHECK (session->getProject()->getFile() != damaged.file);
        CHECK_FALSE (recent.getProjects().contains (damaged.file));
    }

    SUBCASE ("Its commit unreadable too: the backup alone offered, and with none, the error")
    {
        const auto damaged = makeDamagedProject (temp.folder, true, true);
        auto session = makeSession();
        answer = 0;
        session->openProject (damaged.file);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&offers] { return ! offers.empty(); }));
        CHECK (offers[0].backup.isNotEmpty());
        CHECK (offers[0].commit.isEmpty());

        const auto bare = makeDamagedProject (temp.folder.getChildFile ("Bare"), false, true);
        session->openProject (bare.file);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&errors] { return ! errors.isEmpty(); }));
        CHECK (offers.size() == 1);
        CHECK (errors[0].startsWith ("Phazite couldn't open the project"));
    }

    SUBCASE ("Cancelled during the read, or Escape before its window: what was read by then is offered, or the error said")
    {
        const auto damaged = makeDamagedProject (temp.folder, true, false);
        auto session = makeSession();
        answer = 0;
        phazite::VersionHistory::holdReadsForTests = true; // the commit's read held where it starts
        session->openProject (damaged.file);
        REQUIRE (session->getContext().isHoldingForWork());
        CHECK (session->cancelHeldWork()); // Escape, as the window's Cancel
        CHECK_FALSE (session->getContext().isHoldingForWork());
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! offers.empty() || ! errors.isEmpty(); }));
        phazite::VersionHistory::holdReadsForTests = false;
        REQUIRE (offers.size() + (size_t) errors.size() == 1);

        // The backup, if it was in by then; never the commit, whose read the cancel stopped.
        if (! offers.empty())
            CHECK (offers[0].commit.isEmpty());
    }

    SUBCASE ("A quit during the read stops it, saying nothing, and offering nothing")
    {
        const auto damaged = makeDamagedProject (temp.folder, true, false);
        auto session = makeSession();
        phazite::VersionHistory::holdReadsForTests = true;
        session->openProject (damaged.file);
        REQUIRE (session->getContext().isHoldingForWork());
        session->cancelWork(); // as the quit cancels every work that holds the window
        CHECK_FALSE (session->getContext().isHoldingForWork());
        phazite::VersionHistory::holdReadsForTests = false;
        session = nullptr; // the read ends with the session, waited for
        phazite::test::dispatchPendingMessages();
        CHECK (offers.empty());
        CHECK (errors.isEmpty());
    }
}

TEST_CASE ("An autosave that can't be read gives way to the project's own file, and Phazite says so")
{
    // Known bug 87: with Restore chosen, an autosave that didn't parse offered an older backup in place of the project's
    // file, which was whole and newer.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    const auto file = createClosedProject (temp.folder, "Song");

    {
        juce::String error;
        auto project = phazite::Project::open (file, false, error);
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        song.getState().setProperty (phazite::schema::notes, "In the file", &song.getUndoManager());
        REQUIRE (project->save().wasOk());
    }

    // An autosave cut short, newer than the file, as a crash in the middle of its write could have left it.
    const auto autosave = phazite::Project::getAutosaveFile (file);
    REQUIRE (autosave.replaceWithText ("<PHAZITE cut short"));
    REQUIRE (file.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::minutes (1)));
    REQUIRE (phazite::Project::hasNewerAutosave (file));

    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> answer) { answer (true); };
    auto offered = 0;
    session.askWhichCopy = [&offered] (const juce::String&, const juce::String&, const juce::String&, std::function<void (int)> answer)
    {
        ++offered;
        answer (0);
    };
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String& title, const juce::String& message) { errors.add (title + ": " + message); };

    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == file);
    CHECK (offered == 0);
    CHECK (session.getProject()->getSong().getState()[phazite::schema::notes].toString() == "In the file");
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].contains ("automatically"));
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges()); // it is the file, as saved
}

TEST_CASE ("A backup that can't be made is told, and the save goes on")
{
    // Known bug 40: Project::backUpProjectFile() ignored copyFileTo()'s result.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    const auto file = createClosedProject (temp.folder, "Song");
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    juce::StringArray said;
    session.onError = [&said] (const juce::String& title, const juce::String& message) { said.add (title + ": " + message); };
    session.onNotice = [&said] (const juce::String& title, const juce::String& message) { said.add (title + ": " + message); };
    session.setAutoSave (false);
    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);
    phazite::test::dispatchPendingMessages();

    // A file where the Backups folder goes: no backup can go there.
    const auto backups = file.getSiblingFile ("Backups");
    REQUIRE (backups.deleteRecursively());
    REQUIRE (backups.replaceWithText ("in the way"));

    auto& song = session.getProject()->getSong();
    song.getState().setProperty (phazite::schema::notes, "Changed", &song.getUndoManager());
    session.saveProject();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK (file.loadFileAsString().contains ("Changed"));
    REQUIRE (said.size() == 1);
    CHECK (said[0].contains ("backup"));

    // Told once, not at every save while it goes on failing.
    song.getState().setProperty (phazite::schema::notes, "Changed again", &song.getUndoManager());
    session.saveProject();
    CHECK (said.size() == 1);
}

TEST_CASE ("A project double-clicked while a dialog is open opens once the dialog has gone")
{
    // Known bug 113: anotherInstanceStarted() (src/app/Main.cpp) dropped the file while a dialog was open.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    const auto first = createClosedProject (temp.folder, "First");
    const auto second = createClosedProject (temp.folder, "Second");
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.openProject (first);
    REQUIRE (session.getProject() != nullptr);

    REQUIRE (juce::Component::getCurrentlyModalComponent() == nullptr); // no dialog an earlier test left
    juce::Component dialog;
    phazite::test::TestWindow window (dialog, 200, 100);
    dialog.enterModalState (false);
    REQUIRE (juce::Component::getCurrentlyModalComponent() == &dialog);

    // Not while the dialog asks something: the project would change under its question.
    session.openFile (second);
    phazite::test::dispatchPendingMessages();
    CHECK (session.getProject()->getFile() == first);

    dialog.exitModalState (0);
    CHECK (phazite::test::dispatchMessagesUntil ([&session, &second] { return session.getProject()->getFile() == second; }));
}

namespace
{
// A recording whose copy takes a while.
constexpr juce::int64 largeRecording = (juce::int64) 8 << 20;

// While one lives, a first save's copy that meets a file another program holds stops at its first try again (Project::
// setCopyRetryHook()) until the test lets it go on (letGo()); as it goes, it lets the copy go on, so that nothing waits for
// it. Made after the session, so that it goes first.
struct CopyStopped
{
    CopyStopped()
    {
        phazite::Project::setCopyRetryHook (
            [state = state] (int)
            {
                state->reached = true;

                while (! state->goOn.load())
                    juce::Thread::sleep (1);
            });
    }

    ~CopyStopped()
    {
        letGo();
        phazite::Project::setCopyRetryHook (nullptr);
    }

    bool isReached() const { return state->reached.load(); }
    void letGo() { state->goOn = true; }

    struct State
    {
        std::atomic<bool> reached { false }, goOn { false };
    };

    const std::shared_ptr<State> state = std::make_shared<State>();
};
} // namespace

TEST_CASE ("A new project's first save copies its folders in the background, with its progress, and can be cancelled")
{
    // Known bug 90: the first save copied the Audio, Cache and Versions folders on the message thread.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto projects = temp.folder.getChildFile ("Projects");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, projects, temp.folder.getChildFile ("Unsaved"));
    phazite::CommandRegistry registry;
    phazite::test::RenderHost renders;
    session.addCommands (registry, renders);
    session.setWorkShownAfter (0); // its progress window at the next turn, not after a second
    session.chooseSaveFile = [&projects] (const juce::File&, std::function<void (const juce::File&)> callback)
    { callback (projects.getChildFile ("Beat")); };
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String& title, const juce::String& message) { errors.add (title + ": " + message); };

    REQUIRE (session.createProject ({}).wasOk());
    const auto untitled = session.getProject()->getFile();

    // A large recording, which another program holds as the copy reaches it, the copy stopped there: it is still under way
    // when the progress window shows.
    juce::MemoryBlock recording ((size_t) largeRecording, true);
    const auto take = session.getProject()->getAudioFolder().getChildFile ("Take 1.wav");
    REQUIRE (take.replaceWithData (recording.getData(), recording.getSize()));

    {
        const phazite::test::HeldFile held (take);
        CopyStopped stopped;

        if (! held.works())
        {
            MESSAGE ("The file can't be held from this user: the copy can't be kept under way.");
            return;
        }

        // The copy runs in the background, the window held, its progress window showing; the project stays where it was
        // until it is whole.
        REQUIRE (registry.invoke ("project.save"));
        CHECK (session.getContext().isHoldingForWork());
        REQUIRE (phazite::test::dispatchMessagesUntil ([&renders, &stopped] { return renders.render != nullptr && stopped.isReached(); }));
        CHECK (renders.render->getTitle() == "Saving Beat");
        CHECK (renders.render->getProgress() >= 0.0f);
        CHECK (session.isUntitled());
        CHECK (session.getProject()->getFile() == untitled);

        // Cancelled, nothing is left of the copy, and the project is still untitled.
        stopped.letGo(); // its next try sees it is to stop
        renders.render = nullptr;
        phazite::test::dispatchPendingMessages();
        CHECK (session.isUntitled());
        CHECK_FALSE (session.getContext().isHoldingForWork());
        CHECK (projects.findChildFiles (juce::File::findFilesAndDirectories, false).isEmpty());
        CHECK (errors.isEmpty());
    }

    // Again, to the end: the project in its folder, its recording whole there.
    REQUIRE (registry.invoke ("project.save"));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&session] { return ! session.isUntitled(); }));
    phazite::test::dispatchPendingMessages();
    CHECK (renders.render == nullptr);
    CHECK_FALSE (session.getContext().isHoldingForWork());
    const auto file = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    CHECK (session.getProject()->getFile() == file);
    CHECK (file.existsAsFile());
    CHECK (file.getSiblingFile ("Audio").getChildFile ("Take 1.wav").getSize() == take.getSize());
    CHECK (projects.findChildFiles (juce::File::findFilesAndDirectories, false).size() == 1);
    CHECK (recent.getProjects() == juce::Array<juce::File> { file });
    CHECK (errors.isEmpty());
}

TEST_CASE ("A window opens where it was if that is on the screens, fitted to one if it is partly off, and nowhere if it is off them")
{
    // Roadmap S1.15: the main window's place, as the settings keep it, against the screens there are as Phazite starts.
    using Box = juce::Rectangle<int>;
    const juce::Array<Box> screens { Box (0, 0, 1920, 1040), Box (1920, 0, 1280, 984) };

    CHECK (phazite::placeWindow ({ 100, 50, 1000, 700 }, screens) == std::optional<Box> (Box (100, 50, 1000, 700)));
    CHECK (phazite::placeWindow ({ 1500, 50, 1000, 700 }, screens) == std::optional<Box> (Box (1500, 50, 1000, 700))); // across both
    CHECK (phazite::placeWindow ({ 2000, 50, 1000, 700 }, { screens.getFirst() }) == std::nullopt);                    // its screen gone
    CHECK (phazite::placeWindow ({ 1900, 980, 1000, 700 }, { screens.getFirst() }) == std::nullopt); // a corner of it shows
    CHECK (phazite::placeWindow ({ 100, 50, 1000, 700 }, {}) == std::nullopt);                       // no screen at all
    CHECK (phazite::placeWindow ({}, screens) == std::nullopt);                                      // nothing kept

    // Partly off: moved onto the screen that holds most of it; larger than it, fitted to it.
    CHECK (phazite::placeWindow ({ 1500, 600, 1000, 700 }, { screens.getFirst() }) == std::optional<Box> (Box (920, 340, 1000, 700)));
    CHECK (phazite::placeWindow ({ -50, -30, 2500, 1400 }, screens) == std::optional<Box> (screens.getFirst()));
    CHECK (phazite::placeWindow ({ 2100, -200, 900, 700 }, screens) == std::optional<Box> (Box (2100, 0, 900, 700)));

    // Two 2560-pixel screens at 150 %, 1706.67 wide each, rounded up so that they meet: they share a pixel, and a window
    // across both stays as it was; partly off them, it is fitted to the one that holds most of it.
    const juce::Array<Box> scaled { Box (0, 0, 1707, 960), Box (1706, 0, 1707, 960) };
    CHECK (phazite::placeWindow ({ 1200, 50, 1000, 700 }, scaled) == std::optional<Box> (Box (1200, 50, 1000, 700)));
    CHECK (phazite::placeWindow ({ 1200, 500, 1000, 700 }, scaled) == std::optional<Box> (Box (707, 260, 1000, 700)));
}

TEST_CASE ("A copy of a project leaves out a link that leads elsewhere, and copies a reparse point that leads to itself")
{
    // The reviewer's finding: on Windows JUCE's isSymbolicLink() is true for any reparse point, a folder OneDrive keeps among
    // them, which the copy left out (JUCE workaround W35).
    const auto entry = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Project/Audio/Takes");
    const auto elsewhere = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    CHECK (phazite::isLinkElsewhere (entry, true, elsewhere));    // a symbolic link or a junction
    CHECK_FALSE (phazite::isLinkElsewhere (entry, true, entry));  // a reparse point that is itself: the cloud's
    CHECK_FALSE (phazite::isLinkElsewhere (entry, false, entry)); // a folder
}

namespace
{
// A session with a new untitled project, whose first save goes to Projects/Beat; with a host for its progress window
// unless told not to.
struct FirstSave
{
    // Whether a first save's copy is under way: the window is held while it is.
    bool isSaving() { return session.getContext().isHoldingForWork(); }

    // Waits for the save under way to end; whether the project was saved (if not, an error said why).
    bool waitForSave()
    {
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! isSaving(); }));
        return ! session.isUntitled();
    }

    explicit FirstSave (bool withHost = true)
    {
        if (withHost)
            session.addCommands (registry, renders);

        session.chooseSaveFile = [this] (const juce::File&, std::function<void (const juce::File&)> callback)
        { callback (projects.getChildFile ("Beat")); };
        session.onError = [this] (const juce::String& title, const juce::String& message) { errors.add (title + ": " + message); };
        REQUIRE (session.createProject ({}).wasOk());
    }

    // A recording in the project's Audio folder, one whose copy takes a while unless told how large.
    juce::File addRecording (juce::int64 bytes = largeRecording)
    {
        juce::MemoryBlock recording ((size_t) bytes, true);
        const auto take = session.getProject()->getAudioFolder().getChildFile ("Take 1.wav");
        REQUIRE (take.replaceWithData (recording.getData(), recording.getSize()));
        return take;
    }

    // What is in Projects, by name.
    juce::StringArray listProjects() const
    {
        juce::StringArray names;

        for (const auto& file : projects.findChildFiles (juce::File::findFilesAndDirectories, false))
            names.add (file.getFileName());

        names.sort (false);
        return names;
    }

    phazite::test::TemporaryFolder temp;
    const juce::File projects = temp.folder.getChildFile ("Projects");
    phazite::Settings settings;
    phazite::RecentProjects recent { settings };
    phazite::ProjectSession session { phazite::test::getEngineContext(), recent, projects, temp.folder.getChildFile ("Unsaved") };
    phazite::CommandRegistry registry;
    phazite::test::RenderHost renders;
    juce::StringArray errors;
};
} // namespace

TEST_CASE ("A first save copies the project's versions last, with the commit its history wrote while the rest was copied")
{
    // Known bug 169: the versions were copied with the rest, in the background, while the message thread went on; the
    // history's initial commit, which a new project makes once its versions are read, was written into the folder the
    // project left meanwhile, after its versions were listed, so that the saved project lost it (or, as CI met it, a file
    // being written there couldn't be copied). The history's writes made slow, so that every write is still under way as
    // the copy of the rest ends.
    std::atomic<int> writesToVersions { 0 };
    const phazite::WholeFileHook slow (
        [&writesToVersions] (const juce::File& file)
        {
            if (file.getFullPathName().contains ("Versions"))
            {
                ++writesToVersions;
                juce::Thread::sleep (200);
            }

            return true;
        });

    FirstSave s;
    s.addRecording (largeRecording * 4);
    REQUIRE (s.registry.invoke ("project.save"));
    REQUIRE (s.isSaving()); // in the background, the message thread going on
    CHECK (s.waitForSave());
    CHECK (s.errors.isEmpty());
    CHECK (writesToVersions.load() > 0);

    // The initial commit, in the project's own folder: its branch, the commit's pieces, and the history that says so.
    REQUIRE (phazite::test::waitForVersionHistory (s.session.getContext()));
    const auto versions = s.projects.getChildFile ("Beat").getChildFile ("Versions");
    CHECK (versions.getChildFile ("branches").findChildFiles (juce::File::findFiles, true, "*.xml").size() == 1);
    CHECK (versions.getChildFile ("current").existsAsFile());
    CHECK (phazite::getVersions (s.session.getContext()).size() == 1);
}

namespace
{
// A file of so many bytes in the untitled project's Versions folder, in `local` itself, where its history writes nothing
// (its own files are in local/<computer>/).
juce::File addToVersions (FirstSave& s, juce::int64 bytes)
{
    const auto file = s.session.getProject()->getFolder().getChildFile ("Versions").getChildFile ("local").getChildFile ("big.bin");

    // The history's thread may be making its own folder, local/<computer>, at this moment: of two threads that make the same
    // folder, JUCE's createDirectory() tells the one that came second that it failed (the folder "already exists"), as
    // it told this test twice on CI. So what counts is that the folder is there.
    const auto made = file.getParentDirectory().createDirectory();
    INFO (made.getErrorMessage());
    REQUIRE (file.getParentDirectory().isDirectory());

    // Written in place: File::replaceWithData() renames a temporary file over it, a few tries in half a second, which a
    // virus scanner reading a new file of many megabytes (a CI runner's) can outlast. Said why if it fails.
    juce::MemoryBlock block ((size_t) bytes, true);
    juce::FileOutputStream out (file);
    INFO (out.getStatus().getErrorMessage());
    REQUIRE (out.openedOk());
    REQUIRE (out.setPosition (0));
    REQUIRE (out.truncate().wasOk());
    REQUIRE (out.write (block.getData(), block.getSize()));
    out.flush();
    REQUIRE (out.getStatus().wasOk());
    return file;
}
} // namespace

TEST_CASE ("A first save of a small project with a large history copies the history whole, in the background")
{
    // A reviewer, of known bug 169's fix: only the files but the versions were counted, so a project of a little audio and a
    // long history was copied at once, the window waiting for the whole of its history. Every first save copies in the
    // background now.
    FirstSave s;
    REQUIRE (phazite::test::waitForVersionHistory (s.session.getContext()));
    addToVersions (s, largeRecording);
    s.addRecording (1024);

    REQUIRE (s.registry.invoke ("project.save"));
    REQUIRE (s.isSaving());
    CHECK (s.waitForSave());
    CHECK (s.errors.isEmpty());
    CHECK (s.projects.getChildFile ("Beat/Versions/local/big.bin").getSize() == largeRecording);
    REQUIRE (phazite::test::waitForVersionHistory (s.session.getContext()));
    CHECK (phazite::getVersions (s.session.getContext()).size() == 1);
}

TEST_CASE ("A first save's versions are copied by their history's thread, after its writes: none starts meanwhile, and Cancel stops it")
{
    // A reviewer, of known bug 169's fix: the versions were copied on the message thread, without progress or Cancel. Now
    // they are copied by the history's thread after what it was given to write; a commit asked for meanwhile is refused.
    // Here the history's commit is held while the save reaches its versions, so that their copy waits for it.
    std::atomic<bool> holding { true };
    const phazite::WholeFileHook held (
        [&holding] (const juce::File& file)
        {
            while (holding.load() && file.getFullPathName().contains ("Versions"))
                juce::Thread::sleep (1);

            return true;
        });

    FirstSave s;

    // Let go of before the project goes, which waits for every write its history was given: so a check that fails while
    // they are held ends the test rather than hanging it.
    const juce::ScopeGuard letWritesGo { [&holding] { holding = false; } };

    auto& context = s.session.getContext();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&context] { return ! phazite::isReadingVersions (context); }));
    auto* history = phazite::getVersionHistory (context);
    REQUIRE (history != nullptr);
    addToVersions (s, largeRecording);
    s.addRecording (1024);

    REQUIRE (s.registry.invoke ("project.save"));
    REQUIRE (s.isSaving());

    // Once the rest is copied, the history holds every write: a commit is refused for the save.
    juce::String refused;
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            history->saveVersion ("Meanwhile", {}, [&refused] (juce::Result r) { refused = r.getErrorMessage(); });
            return refused.contains ("into its folder");
        }));

    // Cancelled (Escape, before its progress window shows) while its copy waits behind the commit: nothing is left of it,
    // the project stays where it was, and the history writes again once the commit it held is done.
    CHECK (s.session.cancelHeldWork());
    CHECK_FALSE (s.isSaving());
    holding = false;
    CHECK (s.listProjects().isEmpty());
    CHECK (s.session.isUntitled());
    REQUIRE (phazite::test::waitForVersionHistory (context));
    CHECK_FALSE (phazite::isSavingVersion (context));
    CHECK (phazite::getVersions (context).size() >= 1);
}

TEST_CASE ("A commit's bounce made while a first save copies, after its folder was listed, isn't taken for one the project has")
{
    // A reviewer, of known bug 169's fix: such a bounce is in no list the save copies, and the history, which read the
    // bounces in the folder the project left, went on saying it had one: that commit couldn't be bounced again.
    FirstSave s;
    auto& context = s.session.getContext();
    REQUIRE (phazite::test::waitForVersionHistory (context));
    auto* history = phazite::getVersionHistory (context);
    const auto versions = phazite::getVersions (context);
    REQUIRE (versions.size() == 1);
    const auto id = versions.front().id;
    s.addRecording (largeRecording * 4);

    REQUIRE (s.registry.invoke ("project.save"));
    REQUIRE (s.isSaving());

    // Its bounce, finished as the rest is copied: in the folder the project leaves.
    const auto bounce = history->getBounceFile (id);
    REQUIRE (bounce.getParentDirectory().createDirectory());
    REQUIRE (bounce.replaceWithText ("flac"));
    history->readBounces();
    REQUIRE (history->hasBounce (id));

    REQUIRE (s.waitForSave());
    CHECK (history->hasBounce (id) == history->getBounceFile (id).existsAsFile());
    CHECK (history->getBounceFile (id).isAChildOf (s.projects.getChildFile ("Beat")));
}

TEST_CASE ("A first save whose background copy fails says why, and the project stays untitled")
{
    // The reviewer's finding on the first wave: the copy called its callback straight from the member that the progress
    // window's deletion freed, and the session's callback read the title that went with it. Its progress window shown.
    FirstSave s;
    s.session.setWorkShownAfter (0);
    s.addRecording();
    REQUIRE (s.registry.invoke ("project.save"));
    REQUIRE (s.isSaving());

    // Its folder made meanwhile: the copy can't take its place.
    REQUIRE (s.projects.getChildFile ("Beat").createDirectory());
    CHECK_FALSE (s.waitForSave());
    CHECK (s.renders.render == nullptr); // its progress window gone
    REQUIRE (s.errors.size() == 1);
    CHECK (s.errors[0].startsWith ("Phazite couldn't save the project: "));
    CHECK (s.errors[0].contains ("Beat"));
    CHECK (s.session.isUntitled());
    CHECK (s.listProjects() == juce::StringArray ("Beat")); // the copy gone, the folder made meanwhile left as it was
}

TEST_CASE ("A first save's copy without a host stops when another project opens in its place")
{
    // The reviewer's finding on the first wave: the copy the session keeps itself outlived its project.
    FirstSave s (false);
    s.addRecording();
    s.session.saveProject();
    REQUIRE (s.listProjects() == juce::StringArray ("Beat (saving)"));

    REQUIRE (s.session.createProject ({}).wasOk());
    CHECK (s.listProjects().isEmpty()); // stopped, and what it had copied gone
    CHECK_FALSE (s.isSaving());         // the window's hold let go of
    phazite::test::dispatchPendingMessages();
    CHECK (s.session.isUntitled());
}

TEST_CASE ("A first save copies what the project's folders hold, but not what a link in them leads to")
{
    // The reviewer's finding on the first wave: the copy followed links, so a folder from someone else with a link inside
    // copied what it led to.
    FirstSave s;
    const auto outside = s.temp.folder.getChildFile ("Private");
    REQUIRE (outside.createDirectory());
    REQUIRE (outside.getChildFile ("memo.wav").replaceWithText ("private"));
    REQUIRE (phazite::test::linkFolder (s.session.getProject()->getAudioFolder().getChildFile ("Linked"), outside));
    s.addRecording (1024);

    REQUIRE (s.registry.invoke ("project.save"));
    CHECK (s.waitForSave());
    CHECK (s.errors.isEmpty());
    const auto audio = s.projects.getChildFile ("Beat").getChildFile ("Audio");
    CHECK (audio.getChildFile ("Take 1.wav").existsAsFile());
    CHECK_FALSE (audio.getChildFile ("Linked").exists());
    CHECK (outside.getChildFile ("memo.wav").existsAsFile());
}

TEST_CASE ("A first save goes on without a clip's render in the Cache folder that went while it copied")
{
    // The reviewer's finding on the first wave: a render of a clip, which Phazite renders again as the clip plays, that changed
    // or went during the background copy failed the whole save.
    FirstSave s;
    s.addRecording (largeRecording * 4);
    const auto cached = s.session.getProject()->getCacheFolder().getChildFile (juce::String (phazite::renderFilePrefix) + "1234.wav");
    REQUIRE (cached.getParentDirectory().createDirectory());
    REQUIRE (cached.replaceWithText ("frozen"));

    REQUIRE (s.registry.invoke ("project.save"));
    REQUIRE (s.isSaving());
    REQUIRE (cached.deleteFile()); // listed, then gone before the copy reaches it, behind the large recording
    CHECK (s.waitForSave());
    CHECK (s.errors.isEmpty());
}

TEST_CASE ("A first save removes the copies a crashed save left beside it, and only those Phazite marked as its own")
{
    // The reviewer's finding on the first wave: a crash during a copy left "<name> (saving)" beside the user's projects,
    // with full copies of the audio, and nothing removed it.
    FirstSave s;
    const auto makeCopy = [&s] (const juce::String& name, bool marked, juce::Time made)
    {
        const auto folder = s.projects.getChildFile (name);
        REQUIRE (folder.getChildFile ("Audio").createDirectory());
        REQUIRE (folder.getChildFile ("Audio").getChildFile ("Take 1.wav").replaceWithText ("audio"));

        if (marked)
        {
            const auto marker = folder.getChildFile (phazite::Project::copyMarkerName);
            REQUIRE (marker.replaceWithText ({}));
            REQUIRE (marker.setLastModificationTime (made));
        }
    };

    const auto before = juce::Time::getCurrentTime() - juce::RelativeTime::hours (1);
    makeCopy ("Old (saving)", true, before);                                                             // left by a crash: goes
    makeCopy ("Mine (saving)", false, before);                                                           // not Phazite's: stays
    makeCopy ("Running (saving)", true, juce::Time::getCurrentTime() + juce::RelativeTime::minutes (1)); // as one being written: stays

    // A project the user named so, whose marker a program held as it was made: never taken for one (the reviewer's finding).
    makeCopy ("Named (saving)", true, before);
    REQUIRE (s.projects.getChildFile ("Named (saving)/Named (saving).phazite").replaceWithText ("<PHAZITE/>"));

    CHECK (phazite::isLeftoverCopy (s.projects.getChildFile ("Old (saving)")));
    CHECK_FALSE (phazite::isLeftoverCopy (s.projects.getChildFile ("Mine (saving)")));
    CHECK_FALSE (phazite::isLeftoverCopy (s.projects.getChildFile ("Running (saving)")));
    CHECK_FALSE (phazite::isLeftoverCopy (s.projects.getChildFile ("Named (saving)")));

    // The save removes the leftover in the background (to the system's trash as the app runs; deleted here, so that a test
    // run fills nobody's trash); the others stay.
    phazite::setLeftoverCopyRemoval ([] (const juce::File& folder) { return folder.deleteRecursively(); });
    const juce::ScopeGuard asTheAppDoes { [] { phazite::setLeftoverCopyRemoval (nullptr); } };
    s.addRecording (1024);
    REQUIRE (s.registry.invoke ("project.save"));
    CHECK (s.waitForSave());
    INFO (s.errors.joinIntoString ("; ").toStdString());
    CHECK (s.errors.isEmpty());
    const juce::StringArray staying ("Beat", "Mine (saving)", "Named (saving)", "Running (saving)");
    CHECK (phazite::test::dispatchMessagesUntil ([&s, &staying] { return s.listProjects() == staying; }));
}

#if JUCE_WINDOWS
TEST_CASE ("A first save fails, saying which, when a frozen track's file or a clip's render can't be copied")
{
    // The reviewer's finding: every failure to copy a file of the Cache folder was passed over, a full disk or a file another
    // program holds included, and a frozen track's file may be all that is left of a plug-in this computer hasn't.
    FirstSave s;
    s.addRecording (1024);
    const auto cache = s.session.getProject()->getCacheFolder();
    REQUIRE (cache.createDirectory());

    for (const auto& name : { juce::String ("Frozen 5.wav"), juce::String (phazite::renderFilePrefix) + "1234.wav" })
    {
        CAPTURE (name.toStdString());
        const auto file = cache.getChildFile (name);
        REQUIRE (file.replaceWithText ("audio"));

        {
            // Held by another program, sharing it with none, for longer than the copy tries it again: it can't read it.
            const auto held = CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE (held != INVALID_HANDLE_VALUE);
            s.errors.clear();
            const auto saveRan = s.registry.invoke ("project.save");
            const auto saved = saveRan && s.waitForSave();
            CloseHandle (held);
            REQUIRE (saveRan);
            CHECK_FALSE (saved);
            REQUIRE (s.errors.size() == 1);
            CHECK (s.errors[0].contains (name));
            CHECK (s.session.isUntitled());
            CHECK (s.listProjects().isEmpty());
        }

        REQUIRE (file.deleteFile());
    }
}
#endif

TEST_CASE ("A first save that fails part way leaves no folder where it was going, and the project untitled where it was, with its changes")
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
    session.chooseSaveFile = [&projects] (const juce::File&, std::function<void (const juce::File&)> callback)
    { callback (projects.getChildFile ("Beat.phazite")); };
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String&, const juce::String& message) { errors.add (message); };

    REQUIRE (session.createProject ({}).wasOk());
    REQUIRE (session.isUntitled());
    const auto untitledFile = session.getProject()->getFile();
    auto& song = *findSong (session.getContext());
    song.getState().setProperty (phazite::schema::outline, "pop", &song.getUndoManager());
    REQUIRE (projects.createDirectory().wasOk());

    // A take in its Audio folder that can't be read as the save copies it.
    const auto take = untitledFile.getSiblingFile ("Audio").getChildFile ("Take 1.wav");
    REQUIRE (take.getParentDirectory().createDirectory().wasOk());
    REQUIRE (take.replaceWithText ("a take"));

    {
        const phazite::test::HeldFile held (take);

        if (! held.works())
        {
            MESSAGE ("The file can't be held from this user: nothing to fail.");
            return;
        }

        // Held for longer than the copy tries it again (a second or so): the save fails, saying which file.
        REQUIRE (registry.invoke ("project.save"));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&session] { return ! session.getContext().isHoldingForWork(); }));
        REQUIRE (errors.size() == 1);
        CHECK (errors[0].contains ("Audio/Take 1.wav"));
        CHECK (projects.findChildFiles (juce::File::findFilesAndDirectories, false).isEmpty()); // nothing, half made or not
        CHECK (session.isUntitled());
        CHECK (session.getProject()->getFile() == untitledFile);
        CHECK (session.getProject()->hasUnsavedChanges());
        CHECK (untitledFile.existsAsFile());
        CHECK (song.getUndoManager().canUndo());
        CHECK (recent.getProjects().isEmpty());
    }

    // Tried again with the same name once the take can be read, it is saved there, with its take.
    REQUIRE (registry.invoke ("project.save"));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&session] { return ! session.isUntitled(); }));
    const auto file = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    CHECK (errors.size() == 1);
    CHECK (session.getProject()->getFile() == file);
    CHECK_FALSE (session.isUntitled());
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK (file.getSiblingFile ("Audio").getChildFile ("Take 1.wav").loadFileAsString() == "a take");
    CHECK (file.loadFileAsString().contains ("pop"));
}

TEST_CASE ("A first save tries again a file another program holds a moment, and saves once it is let go of")
{
    // A reviewer, of the known bugs' round (2026-11-16): a small project's first save copied on the message thread, which
    // tries a file once, so a file an antivirus or the indexer held a moment as it was copied failed the save. Held here for
    // the copy's first three tries, then let go of.
    FirstSave s;
    const auto take = s.addRecording (1024);

    struct Held
    {
        std::unique_ptr<phazite::test::HeldFile> file;
        std::atomic<int> tries { 0 };
    };

    const auto held = std::make_shared<Held>();
    held->file = std::make_unique<phazite::test::HeldFile> (take);

    if (! held->file->works())
    {
        MESSAGE ("The file can't be held from this user: nothing to try again.");
        return;
    }

    phazite::Project::setCopyRetryHook (
        [held] (int tried)
        {
            held->tries = tried;

            if (tried == 3)
                held->file = nullptr; // on the copy's thread, before it tries again
        });

    const juce::ScopeGuard noHook { [] { phazite::Project::setCopyRetryHook (nullptr); } };
    REQUIRE (s.registry.invoke ("project.save"));
    CHECK (s.waitForSave());
    CHECK (s.errors.isEmpty());
    CHECK (held->tries.load() == 3);
    CHECK (s.projects.getChildFile ("Beat").getChildFile ("Audio").getChildFile ("Take 1.wav").getSize() == 1024);
}

TEST_CASE ("A small project's first save waits on the message thread for no commit its history is writing")
{
    // A reviewer, of the known bugs' round (2026-11-16): a first save of less than 8 MiB copied at once on the message
    // thread, which first waited for every write the history had been given (a commit being written, however long that
    // takes). The history's initial commit is held here until the save has been asked for and has returned; a hold longer
    // than hangTimeout gives up, which only a message thread waiting for it lets happen.
    std::atomic<bool> holding { true }, gaveUp { false };
    const auto giveUpAt = juce::Time::getMillisecondCounter() + (juce::uint32) phazite::test::hangTimeout;
    const phazite::WholeFileHook held (
        [&] (const juce::File& file)
        {
            while (holding.load() && file.getFullPathName().contains ("Versions"))
            {
                if (juce::Time::getMillisecondCounter() > giveUpAt)
                {
                    gaveUp = true;
                    break;
                }

                juce::Thread::sleep (1);
            }

            return true;
        });

    FirstSave s;
    const juce::ScopeGuard letWritesGo { [&holding] { holding = false; } }; // before the project waits for them
    auto& context = s.session.getContext();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&context] { return ! phazite::isReadingVersions (context); }));
    auto* history = phazite::getVersionHistory (context);
    REQUIRE (history != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([history] { return history->isBusy(); })); // its initial commit, held
    s.addRecording (1024);

    REQUIRE (s.registry.invoke ("project.save"));
    CHECK_FALSE (gaveUp.load()); // returned, the commit still held
    CHECK (s.isSaving());
    CHECK (s.session.isUntitled());

    holding = false;
    CHECK (s.waitForSave());
    CHECK (s.errors.isEmpty());
    REQUIRE (phazite::test::waitForVersionHistory (context));
    CHECK (phazite::getVersions (context).size() == 1);
    CHECK (s.projects.getChildFile ("Beat").getChildFile ("Versions").getChildFile ("current").existsAsFile());
}

TEST_CASE ("Closing a project while it records stops recording first, as Stop does, and keeps the take, which the save question holds")
{
    // A reviewer, 2026-11-15: the session went with its take still recording, which deleted the take's files and kept nothing
    // of what was played. Every close goes through closeThen(): a quit, another project opened, a new one.
    using Answer = phazite::ProjectSession::UnsavedChanges;
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    auto asked = 0;
    session.askToSave = [&asked] (const juce::File&, const juce::String&, std::function<void (Answer)> callback)
    {
        ++asked;
        callback (Answer::save);
    };
    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);

    auto& song = session.getProject()->getSong();
    const auto track = phazite::getTrackIDs (song).front();
    REQUIRE (phazite::setInstrument (song, track, phazite::makeOwnDeviceState (song, "phaziteSynth")).isValid());
    phazite::routeLiveMidiInput (song);
    auto& audio = phazite::getAudioIO (engine);
    const auto countIn = audio.isCountInOn();
    audio.setCountInOn (false);
    const juce::ScopeGuard countInBack { [&audio, countIn] { audio.setCountInOn (countIn); } }; // the app's setting
    phazite::test::EnginePlayback playback;
    playback.play (0.01);
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    playback.play (0.25);
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    playback.play (0.25);
    audio.playLive (juce::MidiMessage::noteOff (1, 60));
    playback.play (0.25);

    auto closed = false;
    session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
    CHECK (closed);
    CHECK_FALSE (phazite::isRecording (song));
    CHECK (asked <= 1); // asked, or saved by Auto Save

    // Its note in a clip of the track, in the file saved.
    const auto clips = phazite::getClipStates (phazite::findTrackState (song, track));
    REQUIRE (clips.size() == 1);
    CHECK (file.loadFileAsString().contains ("<NOTE"));
}
TEST_CASE ("Save As while recording stops recording first and keeps the take, which the copy holds")
{
    // A reviewer, 2026-11-15: Save As went on in the copy, closing the project it left with its take still recording, whose
    // files went with nothing of it kept. (Switching to a commit or a branch can't run while recording: canSwitchBranch().)
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    const auto copyFolder = temp.folder.getChildFile ("Copies");
    session.chooseSaveFile = [&copyFolder] (const juce::File&, std::function<void (const juce::File&)> callback)
    { callback (copyFolder.getChildFile ("Song copy")); };
    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);

    auto& song = session.getProject()->getSong();
    const auto track = phazite::getTrackIDs (song).front();
    REQUIRE (phazite::setInstrument (song, track, phazite::makeOwnDeviceState (song, "phaziteSynth")).isValid());
    phazite::routeLiveMidiInput (song);
    auto& audio = phazite::getAudioIO (engine);
    const auto countIn = audio.isCountInOn();
    audio.setCountInOn (false);
    const juce::ScopeGuard countInBack { [&audio, countIn] { audio.setCountInOn (countIn); } }; // the app's setting
    phazite::test::EnginePlayback playback;
    playback.play (0.01);
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    playback.play (0.25);
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    playback.play (0.25);
    audio.playLive (juce::MidiMessage::noteOff (1, 60));
    playback.play (0.25);

    session.saveProjectAs();
    REQUIRE (session.getProject() != nullptr);
    const auto copy = session.getProject()->getFile();
    CHECK (copy.isAChildOf (copyFolder));
    CHECK (copy.loadFileAsString().contains ("<NOTE"));
    auto& copied = session.getProject()->getSong();
    CHECK_FALSE (phazite::isRecording (copied));
    CHECK (phazite::getClipStates (phazite::findTrackState (copied, track)).size() == 1);
}
TEST_CASE ("The window's hold while a branch's song is read lets play and stop through, and a quit cancels the read")
{
    // Found in review (known bug 126's round): the hold took every key (no stopping playback during a long read), and a quit
    // while it showed was refused. Now it passes on what runs while a mouse button is down, and a quit (or the computer's
    // session ending) cancels the read, the song and the branch as they were, and goes on to the close.
    auto& engine = phazite::test::getEngineContext();
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (engine, recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    phazite::test::RenderHost host;
    phazite::CommandRegistry registry;
    session.setAutoSave (false); // so that closing with changes asks
    session.onError = [] (const juce::String&, const juce::String&) {};
    session.addCommands (registry, host);
    session.openProject (file);
    auto& context = session.getContext();
    REQUIRE (phazite::test::waitForVersionHistory (context));
    const auto save = [] (std::function<void (bool)> then) { then (true); };
    const auto main = phazite::getCurrentBranch (context)->id;
    REQUIRE (phazite::newBranch (context, {}, save));
    const auto idea = phazite::getCurrentBranch (context)->id;

    juce::Array<juce::KeyPress> passedOn;
    context.setInputHolder (&session.getInputHold()); // as the main window sets it
    session.getInputHold().setWindow (nullptr,
                                      [&passedOn] (const juce::KeyPress& key)
                                      {
                                          passedOn.add (key);
                                          return true;
                                      });

    REQUIRE (phazite::switchBranch (context, main, save));
    auto* hold = juce::Component::getCurrentlyModalComponent();
    REQUIRE (hold != nullptr);
    CHECK (session.isHoldingInput (hold));
    CHECK_FALSE (registry.invoke ("track.add")); // nothing that changes the song runs meanwhile

    // A key goes to what runs the window's keys while it holds (its KeyDispatcher::keyPressedWhileHeld()).
    const juce::KeyPress space (juce::KeyPress::spaceKey);
    hold->keyPressed (space);
    CHECK (passedOn.size() == 1);

    // A quit cancels it: the hold gone, the song and the branch as they were.
    session.cancelWork();
    CHECK (juce::Component::getCurrentlyModalComponent() == nullptr);
    CHECK_FALSE (context.isHoldingForWork());
    REQUIRE (phazite::test::waitForVersionHistory (context));
    CHECK (phazite::getCurrentBranch (context)->id == idea);

    // Said where the Source Control view says its problems (the second review: it was stopped silently); and the close
    // goes on to its question, which the user answers Cancel: the song and the branch as they were, open still.
    CHECK (phazite::getReadingProblem (context).contains ("closing"));
    phazite::addTrackAtEnd (context, false); // a change, so that closing asks
    using Answer = phazite::ProjectSession::UnsavedChanges;
    auto asked = 0;
    session.askToSave = [&asked] (const juce::File&, const juce::String&, std::function<void (Answer)> answer)
    {
        ++asked;
        answer (Answer::cancel);
    };
    REQUIRE (session.getProject()->hasUnsavedChanges());
    std::optional<bool> closed;
    session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&closed] { return closed.has_value(); }));
    CHECK (asked == 1);
    CHECK_FALSE (*closed);
    CHECK (session.getProject() != nullptr);
    CHECK (phazite::getCurrentBranch (context)->id == idea);
    context.setInputHolder (nullptr);
    session.getInputHold().setWindow (nullptr, nullptr);
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project and the dialogs around it: new, open, save, the unsaved-changes question, and
// restoring an autosave after a crash.
#pragma once

#include "InputHold.h"
#include "model/base/internal/Selection.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/EngineContext.h"
#include "model/project/NewProject.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/project/internal/UnsavedProjects.h"
#include "services/RecentProjects.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class TrackRender;
class TrackRenderHost;

class ProjectSession : private juce::ChangeListener
{
public:
    // Its projects play on the engine context, which outlives it (the app's: src/app/Main.cpp). New projects are kept in
    // unsavedFolder (getUnsavedProjectsFolder() in the app) until their first save asks where they go, starting in
    // projectsFolder, where the Open dialog starts too.
    ProjectSession (EngineContext&, RecentProjects&, juce::File projectsFolder, juce::File unsavedFolder);
    ~ProjectSession() override;

    Project* getProject() const noexcept { return project.get(); }

    // Whether the open project is a new one that hasn't been saved yet: its first save asks what it is called and where
    // it goes (model/project/internal/UnsavedProjects.h).
    bool isUntitled() const;
    bool hasAutosaveFailed() const; // several autosaves in a row, until one works: the status bar says so

    // The open project's song and what is selected in it: tracks and clips (live MIDI input follows the selected
    // track), and a time range across tracks.
    ProjectContext& getContext() noexcept { return context; }
    Selection& getSelection() noexcept { return phazite::getSelection (context); }
    RangeSelection& getRangeSelection() noexcept { return phazite::getRangeSelection (context); }

    std::function<void()> onProjectClosing;  // the open project is about to close, still open: let go of what holds its song
    std::function<void()> onProjectChanged;  // another project is open now (or none), or it was saved
    std::function<void()> onExport;          // shows the export window (the main window's), for Export Song
    std::function<void()> onCleanUpAudio;    // shows the window of Clean Up Unused Audio... (the main window's)
    juce::Component* dialogParent = nullptr; // dialogs appear over this
    // Opens the New Project window, modal; unset, launchAsync(). Tests open it on no screen (tests/MainWindowTests.cpp).
    std::function<juce::DialogWindow*(juce::DialogWindow::LaunchOptions&)> launchDialog;
    std::function<void (const juce::String& title, const juce::String& message)> onError;  // shows an error; unset, a dialog
    std::function<void (const juce::String& title, const juce::String& message)> onNotice; // says what Phazite did; unset, a dialog

    // Asks what to call the project and where it goes, from a suggested file, for its first save and Save As, and calls
    // back with the file chosen, <folder>/<name> (the project's own folder is made there), or with no file if the user
    // cancelled. Unset, a file chooser asks; tests answer themselves.
    std::function<void (const juce::File& suggested, std::function<void (const juce::File&)>)> chooseSaveFile;

    // Asks whether to save the open project's unsaved changes before it closes (closeThen()), and calls back with the
    // answer: to save them, to close without them, or to leave the project open. After a save that failed (saveFailed says
    // why: a drive gone, a folder that can't be written), it offers Save As in place of Save, to save them somewhere else
    // (known bug 88: with Auto Save on, the user could neither quit nor open another project). Unset, a dialog asks; tests
    // answer themselves.
    enum class UnsavedChanges
    {
        save,
        saveAs,
        discard,
        cancel
    };

    std::function<void (const juce::File& projectFile, const juce::String& saveFailed, std::function<void (UnsavedChanges)>)> askToSave;

    // Asks whether to restore a project's changes that Phazite saved automatically before it didn't close properly, and
    // calls back with the answer; untitled if the project was never saved. Unset, a dialog asks; tests answer themselves.
    std::function<void (const juce::File& projectFile, bool untitled, std::function<void (bool restore)>)> askToRestore;

    // For a project file that won't open: asks whether to open its newest backup that reads, or its branch's last commit,
    // each described ("the backup of 9 Nov 2026 14:02", empty where there is none), and calls back with the answer: 1 for the
    // backup, 2 for the commit, 0 for neither. Unset, a dialog asks; tests answer themselves.
    std::function<void (const juce::String& error, const juce::String& backup, const juce::String& commit, std::function<void (int)>)>
        askWhichCopy;

    // After Phazite didn't close properly (a crash, perhaps in a plug-in of the project that was open), asks how to open that
    // project (docs/archive/performance-audit.md, R2): as usual, with its plug-ins off, or not, starting a new one. Unset, a dialog
    // asks; tests answer themselves.
    enum class AfterCrash
    {
        open,
        openWithoutPlugins,
        startNew
    };

    std::function<void (const juce::File& projectFile, std::function<void (AfterCrash)>)> askAfterCrash;

    // Restores the untitled project an earlier Phazite didn't close properly, if the user wants it, or opens the most
    // recent project (offering to restore its autosave), or makes a first project; after a crash, asking how first. Given a
    // file to open (Phazite started by a double-click on one: findFileToOpen()), opens that instead, asking how after a crash
    // only if it is the project that was open then; an untitled project left by a crash waits for the next start.
    void openAtStartup (bool afterCrash = false, const juce::File& fileToOpen = {});

    // The project or DAWproject file a command line names (Windows' Explorer passes the file double-clicked, in quotes),
    // relative to the current folder or whole; none if it names no such file.
    static juce::File findFileToOpen (const juce::String& commandLine);

    // Opens a project, or imports a DAWproject (importDawProject()), asking whether to save the open project first: a file
    // double-clicked while Phazite runs. While a dialog is open, once it has closed, the last file asked for (known bug 113:
    // it was dropped).
    void openFile (const juce::File&);

    void newProject(); // the new-project window
    void openProject();
    void openProject (const juce::File&);
    void chooseRecentProject(); // a menu of the recent projects
    void saveProject();         // asks where first if the project is untitled
    void saveProjectAs();       // a copy under a new name, which then stays open; the first save if untitled
    void exportSong();          // through onExport
    void cleanUpAudio();        // through onCleanUpAudio

    // Import DAWproject... (model/project/internal/DawProject.h): a file chooser, then the file chosen. One exported from a
    // version of the open project comes back as a new branch from that version; any other opens as a new untitled project,
    // after asking whether to save the one that is open.
    void importDawProject();
    void importDawProject (const juce::File&);

    // Makes an untitled project with what the settings choose, and opens it: the new-project window's Create.
    juce::Result createProject (const NewProjectSettings&);

    // Opens an example song (model/project/ExampleSongs.h) as a new untitled project, after asking whether to save the one
    // that is open, with the computer keyboard on its lead: Help > Example Songs.
    void openExampleSong (const juce::String& id);

    // The name the save dialog suggests: the project's own, or with " 2" and so on if a project has it in the folder.
    static juce::String suggestName (const juce::File& folder, const juce::String& name);

    // Where Export Song puts the song unless another file is chosen: the project's Renders folder, or, while it is
    // untitled, Exports next to the projects folder (the untitled project's own folder goes when it closes).
    juce::File getExportFolder() const;

    // Asks whether to save unsaved changes first; with Auto Save on, a project with a file of its own saves them without
    // asking, unless the save fails, which goes on to ask (askToSave). Calls back with false if the user cancelled.
    void closeThen (std::function<void (bool closed)>);

    // As the program quits, once closeThen() said it may: the project closed, then what the versions of the projects that
    // closed still write (a commit given as it closed: model/versions/VersionsModel.h, countVersionsBeingFinished())
    // finished on their own threads, the progress window saying so meanwhile, and then the call back. Its Cancel calls
    // back at once: the program's end finishes them then, without a window.
    void closeAndFinishThen (std::function<void()>);

    // The computer's session ending (Windows shutting down, or the user signing out), or not after all (the user cancelled
    // the close it asked for): while it ends, a close or a save waits for no plug-in, one still to answer keeping the
    // settings it gave last, and those already waiting go on at once (a message later), so that closing asks its question
    // before Windows ends Phazite (known bug 168). Whether a close waits for the plug-ins now, which then asks what closing
    // asks.
    void setSessionEnding (bool);
    bool isClosing() const noexcept { return closesWaiting > 0; }

    // Whether what reads the plug-ins' states (a command, a preset loaded) waits for them now, the window holding its input;
    // whether a modal component is that hold, no dialog; and what to do once no such wait is left (at once if none is).
    bool isWaitingForPluginStates() const noexcept { return gatesWaiting > 0; }
    bool isHoldingInput (const juce::Component* modal) const noexcept;

    // The window's one hold on its input (InputHold.h), which a wait for the plug-ins' states takes, and the work that reads
    // audio files too (model/arrangement/internal/AudioWork.h: InputHolder), as the main window gives it them: whether a
    // modal is a hold, isHoldingInput() answers for either.
    InputHold& getInputHold() noexcept { return inputHold; }
    void afterPluginStatesGate (std::function<void()>);

    // Auto Save (File > Auto Save, project.autoSave), as VS Code's: while on, a project with a file of its own saves itself a
    // moment after each change (model/project/internal/Project.h: AutosaveTimer::setAutoSave()), so its changes show in
    // source control at once; a new project, once its first save put it somewhere. The settings keep it (the main
    // window's), told by onAutoSaveChanged.
    void setAutoSave (bool);
    void toggleAutoSave(); // and tells onAutoSaveChanged
    bool isAutoSaving() const noexcept { return autoSave; }
    std::function<void (bool)> onAutoSaveChanged;

    juce::Array<juce::File> getRecentProjects() const { return recent.getProjects(); }

    // The "project." commands, and the versions', whose bounces render in a host (the main window's progress window), as a
    // new project's first save copies its folders when they are large (Project::moveTo()).
    void addCommands (CommandRegistry&, TrackRenderHost&);

private:
    EngineContext& engineContext;
    const juce::File projectsFolder, unsavedFolder;
    RecentProjects& recent;
    ProjectContext context;                              // the open project's song, as commands and views reach it
    std::unique_ptr<UnsavedFolder> projectUnsavedFolder; // where the project was made while untitled, until it closes
    std::unique_ptr<Project> project;
    std::unique_ptr<AutosaveTimer> autosaveTimer;
    std::unique_ptr<juce::FileChooser> fileChooser;
    TrackRenderHost* renders = nullptr;     // addCommands()'
    std::unique_ptr<TrackRender> firstSave; // a first save's copy without a host to run it (tests'), until the next

    // A file double-clicked while a dialog was open, opened once none is.
    juce::File fileWaiting;
    juce::TimedCallback openWhenNoDialog { [this] { openFileWhenNoDialog(); } };
    void openFileWhenNoDialog();
    bool autoSave = true;
    void updateAutoSave(); // the timer saves the project, or autosaves it, as Auto Save and the project say

    // The project, and the folder it was made in if it was untitled, deleted once the project has closed.
    void setProject (std::unique_ptr<Project>, std::unique_ptr<UnsavedFolder> = {});
    juce::Result createUntitled (const juce::String& name, const std::function<void (Song&)>& addContent);
    void openRecentOrFirst();
    void openWithRestoreQuestion (const juce::File&);
    void open (const juce::File&, bool restoreAutosave);
    void restoreUntitled (const juce::File&, bool restore);
    void askWhetherToRestore (const juce::File&, bool untitled, std::function<void (bool restore)>);
    void askWhichCopyToOpen (const juce::String& error, const juce::String& backup, const juce::String& commit, std::function<void (int)>);
    void askHowToOpenAfterCrash (const juce::File&, std::function<void (AfterCrash)>);
    bool pluginsOffUntilOpen = false; // the project opening opens with its plug-ins off
    void createFirstProject();        // an empty one, as every new project starts

    // Saves the project, asking where first if it is untitled, and calls back with whether it was saved.
    void saveThen (std::function<void (bool saved)>);
    void chooseWhereToSave (const juce::File& suggested, std::function<void (const juce::File&)>);
    void moveUntitledTo (const juce::File& chosen, std::function<void (bool saved)>);

    // Recording stopped as Stop stops it, the take kept, and the takes waiting for a drag placed: before the project is
    // saved elsewhere, moved (an untitled project's first save) or closed, which would lose them.
    void keepRecording();

    // What closing asks when there are unsaved changes, after a save that failed if saveFailed says why; and Save As there,
    // a copy where the user chooses, which the project closes after.
    void askWhetherToSave (const juce::String& saveFailed, std::function<void (bool closed)>);
    void saveCopyThenClose (std::function<void (bool closed)>);

    // What closing does once every plug-in has given its state (closeThen()); and the notice that a save holds an older
    // state of some plug-ins than theirs, by name (none for none).
    void closeWithPluginStates (std::function<void (bool closed)>);
    void tellOlderPluginStates (const juce::StringArray& plugins);
    std::unique_ptr<juce::Timer> finishing; // what looks whether the versions' writes are done (closeAndFinishThen())

    // The open project's plug-ins asked for their states (every one's, or those that said they changed), and then what comes
    // next (ProjectSession.cpp); and this session's life, which what comes a message later checks.
    // A command's wait (forCommand) offers to go on without them or to drop it, which then is told as false.
    void withPluginStates (std::function<void (bool sameProject)>, bool all = true, bool forCommand = false);
    std::weak_ptr<void> shownWait; // the wait the progress window shows, if it does
    bool sessionEnding = false;    // setSessionEnding()'s
    int gatesWaiting = 0;          // gatePluginStates()'s, still waiting
    std::vector<std::function<void()>> afterGates;
    InputHold inputHold; // the window's one hold on its input, whatever waits (InputHold.h)
    void gatePluginStates (std::function<void (bool go)>);
    int closesWaiting = 0; // closeThen()s that wait for the plug-ins' states
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    void showError (const juce::String& title, const juce::String& message);
    void showNotice (const juce::String& title, const juce::String& message);

    // The selection changed, so live MIDI follows it; or the audio or MIDI devices changed, so live
    // MIDI goes to the project again (see routeLiveMidiInput()).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};
} // namespace phazite

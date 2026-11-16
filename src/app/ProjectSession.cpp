// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSession.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/ExampleSongs.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/NewProject.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "services/Library.h"
#include "ui/shell/NewProjectPanel.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    // The name typed in the save dialog, without the project file's extension if the dialog added it.
    juce::String getChosenName (const juce::File& chosen)
    {
        return chosen.hasFileExtension (Project::fileExtension) ? chosen.getFileNameWithoutExtension() : chosen.getFileName();
    }

    // What other DAWs export (model/project/internal/DawProject.h), which Phazite imports.
    constexpr const char* dawProjectExtension = ".dawproject";

    constexpr int waitShownMilliseconds = 1000; // how long a save or a close waits for plug-ins before the window says so

    // What keeps the window from taking input while a command waits for the plug-ins' states, before the progress window
    // shows: a modal component of its own, never shown, which leaves a click alone (no beep, nothing brought forward) and
    // takes key presses for nothing. JUCE sends every key event to the front modal component, so a key let go meanwhile
    // reaches nothing else: as it goes, the window's key listeners hear that keys changed, so that none is taken for still
    // held after the wait (known bug 166).
    struct InputHold final : juce::Component
    {
        InputHold() { enterModalState (false); }

        ~InputHold() override
        {
            if (isCurrentlyModal (false))
                exitModalState (0);
        }

        void inputAttemptWhenModal() override {}
        bool keyPressed (const juce::KeyPress&) override { return true; }

        bool keyStateChanged (bool) override { return true; }

        // Its end: what holds keys (the shortcuts' dispatcher, the computer keyboard) told that keys changed, as JUCE's window
        // tells it (ComponentPeer::handleKeyUpOrDown()), so that each lets go of those no longer down, a note's included.
        void release()
        {
            if (isCurrentlyModal (false))
                exitModalState (0);

            // Every window's, which may have no focused component.
            std::vector<juce::Component::SafePointer<juce::Component>> windows;

            for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
                windows.emplace_back (&juce::ComponentPeer::getPeer (i)->getComponent());

            for (const auto& window : windows)
                if (auto* shown = window.getComponent())
                    if (auto* peer = shown->getPeer())
                        peer->handleKeyUpOrDown (false);
        }
    };

    // A wait for the plug-ins' states: whether it is over, whether the progress window (the renders' host) shows it, with
    // what title; whether it is a command's, whose window offers to go on without them or to drop the command; and whether
    // the command was dropped.
    struct StatesWait
    {
        bool over = false, shown = false, forCommand = false, dropped = false;
        juce::String title;
    };

    // The wait as the progress window shows it: what it names, and its Cancel (the window going while the wait isn't over):
    // a save's or a close's goes on without the plug-ins still to answer (the states they gave last are kept, as for a
    // time-out); a command's is dropped, nothing done, and its window's Go On Without Them goes on (known bug 166).
    struct WaitForStates final : TrackRender::Impl
    {
        WaitForStates (Song& s, std::shared_ptr<StatesWait> shownWait)
            : song (&s),
              wait (std::move (shownWait))
        {
            wait->shown = true;
        }

        ~WaitForStates() override
        {
            wait->shown = false;

            if (auto* waiting = song.get(); waiting != nullptr && ! wait->over)
            {
                wait->dropped = wait->forCommand && ! goingOn;
                giveUpPluginStates (*waiting);
            }
        }

        float getProgress() const override { return -1.0f; } // no end known: JUCE's bar shows it busy
        juce::String getTitle() const override { return wait->title; }
        juce::String getGoOnName() const override { return wait->forCommand ? juce::String ("Go On Without Them") : juce::String(); }

        void goOn() override
        {
            goingOn = true;

            if (auto* waiting = song.get(); waiting != nullptr && ! wait->over)
                giveUpPluginStates (*waiting);
        }

        juce::WeakReference<Song> song;
        std::shared_ptr<StatesWait> wait;
        bool goingOn = false;
    };

    // The wait for the versions' writes as the program quits (closeAndFinishThen()), as the progress window shows it: its
    // Cancel (the window going while the wait isn't over) goes on without the window.
    struct WaitForVersions final : TrackRender::Impl
    {
        explicit WaitForVersions (std::shared_ptr<StatesWait> shownWait)
            : wait (std::move (shownWait))
        {
            wait->shown = true;
        }

        ~WaitForVersions() override { wait->shown = false; }

        float getProgress() const override { return -1.0f; }
        juce::String getTitle() const override { return wait->title; }

        std::shared_ptr<StatesWait> wait;
    };
} // namespace

ProjectSession::ProjectSession (EngineContext& engine, RecentProjects& recentProjects, juce::File projects, juce::File unsaved)
    : engineContext (engine),
      projectsFolder (std::move (projects)),
      unsavedFolder (std::move (unsaved)),
      recent (recentProjects),
      context (engine)
{
    engineContext.addChangeListener (this);
    getSelection().addChangeListener (this);

    // What went wrong with the open song that the model can't show itself (sound lost from a take), as an error.
    setProblemReporter (context, [this] (const juce::String& title, const juce::String& message) { showError (title, message); });

    // What reads the plug-ins' states gets them first, the session's way (gatePluginStates()).
    context.setPluginStatesGate ([this] (std::function<void (bool)> run) { gatePluginStates (std::move (run)); });
}

ProjectSession::~ProjectSession()
{
    getSelection().removeChangeListener (this);
    engineContext.removeChangeListener (this);
    setProject (nullptr); // closes the project on the message thread, before the engine context goes
}

void ProjectSession::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (project == nullptr)
        return;

    if (source == &getSelection())
        followSelectedTrack (getSelection());
    else
        routeLiveMidiInput (project->getSong()); // a MIDI input plugged in, or the audio device changed
}

bool ProjectSession::isUntitled() const
{
    return project != nullptr && projectUnsavedFolder != nullptr && project->getFile().isAChildOf (projectUnsavedFolder->getFolder());
}

bool ProjectSession::hasAutosaveFailed() const
{
    return autosaveTimer != nullptr && autosaveTimer->hasFailedRepeatedly();
}

void ProjectSession::openAtStartup (bool afterCrash, const juce::File& fileToOpen)
{
    // An untitled project that an earlier Phazite didn't close properly, such as in a crash, comes first: its changes are
    // nowhere else.
    const auto leftover = UnsavedFolder::findProjectToRestore (unsavedFolder);

    // Unless a file was double-clicked: then that one. The last run's crash concerns it only if it was the project open
    // then; an untitled project the crash left stays in its folder, for the next start to offer.
    if (fileToOpen != juce::File())
    {
        const auto recentProjects = recent.getProjects();
        const auto wasOpen = leftover == juce::File() && ! recentProjects.isEmpty() && recentProjects.getFirst() == fileToOpen;
        const auto openIt = [this, fileToOpen]
        {
            if (! fileToOpen.hasFileExtension (dawProjectExtension))
            {
                openWithRestoreQuestion (fileToOpen); // which makes a first project if it doesn't open
                return;
            }

            importDawProject (fileToOpen);

            if (project == nullptr) // it didn't import, and said why
                createFirstProject();
        };

        if (! afterCrash || ! wasOpen)
        {
            openIt();
            return;
        }

        askHowToOpenAfterCrash (fileToOpen,
                                [this, openIt] (AfterCrash choice)
                                {
                                    if (choice == AfterCrash::startNew)
                                    {
                                        createFirstProject();
                                        return;
                                    }

                                    if (choice == AfterCrash::openWithoutPlugins)
                                    {
                                        pluginsOffUntilOpen = true;
                                        getPluginHost (engineContext).setLoadsPlugins (false);
                                    }

                                    openIt();
                                });
        return;
    }

    const auto openAsUsual = [this, leftover]
    {
        if (leftover != juce::File())
            askWhetherToRestore (leftover, true, [this, leftover] (bool restore) { restoreUntitled (leftover, restore); });
        else
            openRecentOrFirst();
    };

    const auto recentProjects = recent.getProjects();
    const auto last = leftover != juce::File() ? leftover : (recentProjects.isEmpty() ? juce::File() : recentProjects.getFirst());

    if (! afterCrash || last == juce::File())
    {
        openAsUsual();
        return;
    }

    askHowToOpenAfterCrash (last,
                            [this, openAsUsual] (AfterCrash choice)
                            {
                                if (choice == AfterCrash::startNew)
                                {
                                    createFirstProject();
                                    return;
                                }

                                if (choice == AfterCrash::openWithoutPlugins)
                                {
                                    pluginsOffUntilOpen = true;
                                    getPluginHost (engineContext).setLoadsPlugins (false);
                                }

                                openAsUsual();
                            });
}

void ProjectSession::askHowToOpenAfterCrash (const juce::File& file, std::function<void (AfterCrash)> callback)
{
    if (askAfterCrash != nullptr)
    {
        askAfterCrash (file, std::move (callback));
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::WarningIcon)
                             .withTitle ("Phazite didn't close properly")
                             .withMessage ("The last time, \"" + file.getFileNameWithoutExtension()
                                           + "\" was open. If a plug-in in it crashed Phazite, open it with its plug-ins off: they "
                                             "stay as they were, silent, until it opens again.")
                             .withButton ("Open", 1)
                             .withButton ("Open with Plug-ins Off", 2)
                             .withButton ("Start a New Project", 0)
                             .withAssociatedComponent (dialogParent);

    // Escape, or closing the window, answers 0, as the last button does.
    juce::AlertWindow::showAsync (
        options, [callback] (int result)
        { callback (result == 1   ? AfterCrash::open
                    : result == 2 ? AfterCrash::openWithoutPlugins
                                  : AfterCrash::startNew); });
}

void ProjectSession::openRecentOrFirst()
{
    if (const auto projects = recent.getProjects(); ! projects.isEmpty())
        openWithRestoreQuestion (projects.getFirst());
    else
        createFirstProject();
}

void ProjectSession::restoreUntitled (const juce::File& file, bool restore)
{
    // Restored, it goes on untitled in its folder; discarded, the folder goes.
    if (auto folder = UnsavedFolder::takeOver (file); folder != nullptr && restore)
    {
        juce::String error, notRestored;

        if (auto restored = Project::open (file, true, error, &notRestored))
        {
            if (notRestored.isNotEmpty())
                showError ("Phazite couldn't restore the unsaved changes", notRestored);

            // Recordings made after the last autosave, which a crash left out of the song, come back on tracks of their own.
            const auto recovered = UnsavedFolder::recoverUnusedAudio (restored->getSong());
            setProject (std::move (restored), std::move (folder));

            if (recovered > 0)
                showNotice ("Recordings recovered", juce::String (recovered)
                                                        + (recovered == 1 ? " recording that wasn't in the song yet is"
                                                                          : " recordings that weren't in the song yet are")
                                                        + " on new tracks at the start of the song.");

            return;
        }

        showError ("Phazite couldn't restore the project", error);
    }

    openRecentOrFirst();
}

juce::Result ProjectSession::createUntitled (const juce::String& name, const std::function<void (Song&)>& addContent)
{
    auto folder = UnsavedFolder::create (unsavedFolder);

    if (folder == nullptr)
        return juce::Result::fail ("Phazite couldn't make a folder for a new project in " + unsavedFolder.getFullPathName() + ".");

    juce::String error;
    auto created = Project::create (folder->getFolder(), name, error, addContent);

    if (created == nullptr)
        return juce::Result::fail (error);

    setProject (std::move (created), std::move (folder));
    return juce::Result::ok();
}

void ProjectSession::createFirstProject()
{
    // Empty, as every new project starts (the owner, 2026-11-01): the welcome window offers the example songs.
    if (const auto result = createProject ({}); result.failed())
        showError ("Phazite couldn't create a project", result.getErrorMessage());
}

void ProjectSession::openExampleSong (const juce::String& id)
{
    const auto* song = findExampleSong (id);

    if (song == nullptr)
        return;

    closeThen (
        [this, song] (bool closed)
        {
            if (! closed)
                return;

            const auto content = getExampleSongContent (engineContext, song->id, Library::getDefaultFactoryPresetsFolder());

            if (const auto result = createUntitled (song->name, content); result.failed())
            {
                showError ("Phazite couldn't open the example song", result.getErrorMessage());
                return;
            }

            // Live MIDI follows the selection, so the computer keyboard plays the song's lead.
            if (const auto track = findExampleTrackToPlay (context, *song); track.isValid())
                context.getSongSelection().selectOnly (track);
        });
}

void ProjectSession::newProject()
{
    closeThen (
        [this] (bool closed)
        {
            if (! closed)
                return;

            auto* panel = new ui::NewProjectPanel ([this] (const NewProjectSettings& settings) { return createProject (settings); });
            panel->setSize (540, 424);

            juce::DialogWindow::LaunchOptions o;
            o.dialogTitle = "New Project";
            o.dialogBackgroundColour = ui::getPalette (*panel).surface;
            o.escapeKeyTriggersCloseButton = true;
            o.useNativeTitleBar = true;
            o.resizable = false;
            o.componentToCentreAround = dialogParent;
            o.content.setOwned (panel);

            if (launchDialog != nullptr)
                launchDialog (o);
            else
                o.launchAsync();
        });
}

juce::String ProjectSession::suggestName (const juce::File& folder, const juce::String& name)
{
    auto suggested = name;

    for (int n = 2; folder.getChildFile (suggested).exists(); ++n)
        suggested = name + " " + juce::String (n);

    return suggested;
}

juce::Result ProjectSession::createProject (const NewProjectSettings& settings)
{
    // What the project starts with is part of its first save, not undo steps.
    return createUntitled ("Untitled", [settings] (Song& song) { addNewProjectContent (song, settings); });
}

juce::File ProjectSession::getExportFolder() const
{
    if (project == nullptr || isUntitled())
        return projectsFolder.getSiblingFile ("Exports");

    return project->getRendersFolder();
}

void ProjectSession::openProject()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Open a project", projectsFolder, juce::String ("*") + Project::fileExtension);

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  if (const auto file = chooser.getResult(); file.existsAsFile())
                                      openProject (file);
                              });
}

void ProjectSession::openProject (const juce::File& file)
{
    closeThen (
        [this, file] (bool closed)
        {
            if (closed)
                openWithRestoreQuestion (file);
        });
}

juce::File ProjectSession::findFileToOpen (const juce::String& commandLine)
{
    for (const auto& token : juce::StringArray::fromTokens (commandLine, true))
    {
        const auto argument = token.trim().unquoted();

        if (argument.isEmpty() || argument.startsWithChar ('-'))
            continue;

        if (const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (argument);
            file.existsAsFile() && (file.hasFileExtension (Project::fileExtension) || file.hasFileExtension (dawProjectExtension)))
            return file;
    }

    return {};
}

void ProjectSession::openFile (const juce::File& file)
{
    // Not under a dialog's question, which may be about the project open now: once it has gone.
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
    {
        fileWaiting = file;
        openWhenNoDialog.startTimer (100);
        return;
    }

    if (file.hasFileExtension (dawProjectExtension))
        importDawProject (file); // which asks whether to save the open project, if it replaces it
    else if (project == nullptr || file != project->getFile())
        openProject (file);
}

void ProjectSession::openFileWhenNoDialog()
{
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return;

    openWhenNoDialog.stopTimer();
    openFile (std::exchange (fileWaiting, {}));
}

void ProjectSession::chooseRecentProject()
{
    const auto projects = recent.getProjects();
    juce::PopupMenu menu;
    menu.addSectionHeader ("Open a recent project");

    for (int i = 0; i < projects.size(); ++i)
        menu.addItem (i + 1, projects[i].getFileNameWithoutExtension());

    if (projects.isEmpty())
        menu.addItem (-1, "No recent projects yet", false);

    // Near the top left of the main window: the command can come from the keyboard, with the
    // pointer anywhere.
    auto options = juce::PopupMenu::Options();

    if (dialogParent != nullptr)
        options = options.withTargetScreenArea (dialogParent->getScreenBounds().withSize (1, 1).translated (80, 60));

    menu.showMenuAsync (options,
                        [this, projects] (int chosen)
                        {
                            if (chosen > 0 && chosen <= projects.size()) // 0: dismissed
                                openProject (projects[chosen - 1]);
                        });
}

void ProjectSession::saveProjectAs()
{
    if (project == nullptr)
        return;

    // The copy goes on with the take, and the project it leaves as it was saved last.
    keepRecording();

    if (isUntitled())
    {
        saveThen ([] (bool) {}); // its first save asks where it goes anyway
        return;
    }

    // A copy, next to the project at first, in a folder of its own; the project keeps its last saved version.
    const auto parentFolder = project->getFolder().getParentDirectory();
    const auto suggested =
        parentFolder.getChildFile (suggestName (parentFolder, project->getFile().getFileNameWithoutExtension() + " copy"));

    chooseWhereToSave (suggested,
                       [this] (const juce::File& chosen)
                       {
                           if (chosen == juce::File() || project == nullptr)
                               return;

                           juce::String error;
                           waitForVersionsWritten (context); // the copy takes them whole
                           const auto newFile = project->saveCopyAs (chosen.getParentDirectory(), getChosenName (chosen), error);

                           if (newFile == juce::File())
                           {
                               showError ("Phazite couldn't save the project", error);
                               return;
                           }

                           // Continue in the copy; the unsaved changes went into it.
                           setProject (nullptr);
                           open (newFile, false);
                       });
}

void ProjectSession::saveProject()
{
    saveThen ([] (bool) {});
}

void ProjectSession::saveThen (std::function<void (bool saved)> callback)
{
    if (project == nullptr)
    {
        callback (false);
        return;
    }

    // A new project is named and placed by its first save, from Documents/Phazite/Projects.
    if (isUntitled())
    {
        const auto name = project->getFile().getFileNameWithoutExtension();
        chooseWhereToSave (projectsFolder.getChildFile (suggestName (projectsFolder, name)),
                           [this, callback] (const juce::File& chosen)
                           {
                               if (chosen == juce::File())
                                   callback (false);
                               else
                                   moveUntitledTo (chosen, callback);
                           });
        return;
    }

    // Every plug-in asked for its state at once, the message thread waiting for none (known bug 69); the save once they have
    // answered, or their time is up (the file keeps the state a plug-in gave before, and a notice says so).
    // A project other than the one asked for meanwhile (closed by a quit that went first) isn't saved.
    withPluginStates (
        [this, callback] (bool sameProject)
        {
            if (! sameProject)
            {
                callback (false);
                return;
            }

            const auto result = project->save();

            if (result.failed())
                showError ("Phazite couldn't save the project", result.getErrorMessage());
            else
                tellOlderPluginStates (getPluginsWithOlderStates (project->getSong()));

            if (onProjectChanged != nullptr)
                onProjectChanged();

            callback (result.wasOk());
        });
}

void ProjectSession::withPluginStates (std::function<void (bool sameProject)> then, bool all, bool forCommand)
{
    // Its plug-ins asked for their states at once; then what comes next: at once if they are in already, else a message after
    // they are (or once the wait is given up), never as the song's plug-ins go, and only while this session lives, told
    // whether the project asked for is still the one open. A wait of more than a second shows in the progress window, whose
    // Cancel goes on without the plug-ins still to answer. While the computer's session ends, at once, with the states they
    // gave last (known bug 168).
    if (sessionEnding)
    {
        then (project != nullptr);
        return;
    }

    const auto number = context.getProjectNumber();
    const auto wait = std::make_shared<StatesWait>();
    wait->forCommand = forCommand;
    const auto calledAt = std::make_shared<bool> (true); // while whenPluginStatesIn() hasn't returned
    const std::weak_ptr<bool> session = alive;

    const auto next = [this, number, wait, then]
    {
        wait->over = true;

        if (wait->shown && renders != nullptr)
            renders->renderFinished (wait->title, juce::Result::ok()); // which deletes the window

        then (! wait->dropped && project != nullptr && context.getProjectNumber() == number);
    };

    whenPluginStatesIn (project->getSong(), all ? SandboxedInstance::Ask::all : SandboxedInstance::Ask::changedAndAnswering,
                        [session, calledAt, next] (bool)
                        {
                            if (*calledAt)
                                next();
                            else
                                juce::MessageManager::callAsync (
                                    [session, next]
                                    {
                                        if (! session.expired())
                                            next();
                                    });
                        });

    *calledAt = false;

    if (wait->over || renders == nullptr)
        return;

    juce::Timer::callAfterDelay (waitShownMilliseconds,
                                 [this, session, number, wait]
                                 {
                                     if (session.expired() || wait->over || project == nullptr || context.getProjectNumber() != number)
                                         return;

                                     // One window at a time: another wait shown already waits for the same answers.
                                     if (const auto other = std::static_pointer_cast<StatesWait> (shownWait.lock());
                                         other != nullptr && other->shown)
                                         return;

                                     shownWait = wait;
                                     auto& song = project->getSong();
                                     wait->title = "Waiting for the settings of " + getPluginsAnswering (song).joinIntoString (", ");
                                     renders->runRender (std::make_unique<TrackRender> (std::make_unique<WaitForStates> (song, wait)));
                                 });
}

void ProjectSession::tellOlderPluginStates (const juce::StringArray& plugins)
{
    if (plugins.isEmpty())
        return;

    const auto one = plugins.size() == 1;
    showError ("Plug-in settings not saved", "Phazite saved the project, but " + plugins.joinIntoString (", ") + " didn't give "
                                                 + (one ? "its" : "their")
                                                 + " settings in time, or crashed: the project keeps the settings " + (one ? "it" : "they")
                                                 + " gave before. Save again to try once more.");
}

void ProjectSession::moveUntitledTo (const juce::File& chosen, std::function<void (bool saved)> callback)
{
    if (! isUntitled())
    {
        callback (false);
        return;
    }

    // Its folder moves, with the files a take writes.
    keepRecording();

    // Its versions move with it, whole, copied last by the history's thread, after what it was asked to write: the initial
    // commit as a new project opens, which may come while the rest is copied (known bug 169).
    project->setVersionsCopier (makeVersionsCopier (context));

    // A large copy runs in the host's progress window, which tells a failure itself.
    firstSave = nullptr;
    const auto inWindow = std::make_shared<bool> (false);
    const auto title = "Saving " + juce::File::createLegalFileName (getChosenName (chosen).trim());

    auto copying = project->moveTo (chosen.getParentDirectory(), getChosenName (chosen),
                                    [this, callback, inWindow, title] (const juce::Result& result)
                                    {
                                        // The host deletes the copy, and this with it: what comes after takes copies.
                                        const auto then = callback;
                                        const auto saved = result.wasOk();

                                        if (saved)
                                        {
                                            // Its folder from now on; the untitled one goes when the project closes. It
                                            // saves itself from now on, with Auto Save.
                                            recent.add (project->getFile());
                                            updateAutoSave();

                                            if (onProjectChanged != nullptr)
                                                onProjectChanged();
                                        }

                                        if (*inWindow && renders != nullptr)
                                            renders->renderFinished (title, result);
                                        else if (! saved)
                                            showError ("Phazite couldn't save the project", result.getErrorMessage());

                                        then (saved);
                                    });

    if (copying == nullptr)
        return; // done, and called back

    *inWindow = true;

    if (renders != nullptr)
        renders->runRender (std::move (copying));
    else
        firstSave = std::move (copying);
}

void ProjectSession::chooseWhereToSave (const juce::File& suggested, std::function<void (const juce::File&)> callback)
{
    if (chooseSaveFile != nullptr)
    {
        chooseSaveFile (suggested, std::move (callback));
        return;
    }

    suggested.getParentDirectory().createDirectory();
    fileChooser = std::make_unique<juce::FileChooser> ("Save the project: Phazite makes a folder of this name for it", suggested,
                                                       juce::String ("*") + Project::fileExtension);

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                              [callback] (const juce::FileChooser& chooser) { callback (chooser.getResult()); });
}

void ProjectSession::exportSong()
{
    if (project != nullptr && onExport != nullptr)
        onExport();
}

void ProjectSession::setAutoSave (bool shouldAutoSave)
{
    autoSave = shouldAutoSave;
    updateAutoSave();
}

void ProjectSession::toggleAutoSave()
{
    setAutoSave (! autoSave);

    if (onAutoSaveChanged != nullptr)
        onAutoSaveChanged (autoSave);
}

void ProjectSession::updateAutoSave()
{
    if (autosaveTimer != nullptr)
        autosaveTimer->setAutoSave (autoSave && project != nullptr && ! isUntitled());
}

void ProjectSession::keepRecording()
{
    // Recording stops, as Stop stops it, keeping the take; and a take still waiting for a drag to end is placed: what a save
    // and the question to save then hold. A session that goes with a take still recording keeps nothing of it.
    auto* song = findSong (context);

    if (song == nullptr)
        return;

    if (isRecording (*song))
        stopRecording (*song);

    if (auto* session = getSession (context))
        session->placeTakesNow();
}

void ProjectSession::closeThen (std::function<void (bool)> callback)
{
    if (project == nullptr)
    {
        callback (true);
        return;
    }

    keepRecording();

    // Every plug-in asked for its settings at once, rather than trusted to say they changed, the message thread waiting for
    // none (known bug 69); then what closing does with them, a close always going on: if another project is open by then
    // (a quit or an open that went first), closing that one.
    ++closesWaiting;
    withPluginStates (
        [this, callback] (bool sameProject)
        {
            --closesWaiting;

            if (sameProject)
                closeWithPluginStates (callback);
            else
                closeThen (callback);
        });
}

void ProjectSession::closeAndFinishThen (std::function<void()> callback)
{
    setProject (nullptr);

    if (countVersionsBeingFinished() == 0 || renders == nullptr)
    {
        callback();
        return;
    }

    // The progress window says what is being finished, until it is; its Cancel goes on at once.
    struct Finishing final : juce::Timer
    {
        std::function<void()> tick;
        void timerCallback() override { tick(); }
    };

    const auto wait = std::make_shared<StatesWait>();
    wait->title = "Finishing the commits of " + getProjectsBeingFinished().joinIntoString (", ");
    renders->runRender (std::make_unique<TrackRender> (std::make_unique<WaitForVersions> (wait)));

    auto timer = std::make_unique<Finishing>();
    timer->tick = [this, wait, callback]
    {
        if (countVersionsBeingFinished() > 0 && wait->shown)
            return;

        finishing->stopTimer(); // it goes with the session, or the next quit's
        wait->over = true;

        if (wait->shown && renders != nullptr)
            renders->renderFinished (wait->title, juce::Result::ok()); // which deletes the window

        callback();
    };

    timer->startTimer (100);
    finishing = std::move (timer);
}

void ProjectSession::setSessionEnding (bool ending)
{
    // What waits for the plug-ins goes on without those still to answer, as the progress window's Cancel does.
    sessionEnding = ending;

    if (ending && project != nullptr)
        giveUpPluginStates (project->getSong());
}

void ProjectSession::closeWithPluginStates (std::function<void (bool)> callback)
{
    // Its plug-ins' settings as they gave them, compared with the file's (Project::checkUnsavedChanges()).
    if (project == nullptr || ! project->checkUnsavedChanges())
    {
        callback (true);
        return;
    }

    // With Auto Save, as in VS Code: the changes saved, nothing asked; unless the save fails, which goes on to ask.
    if (autoSave && ! isUntitled())
    {
        if (const auto result = project->save(); result.failed())
            askWhetherToSave (result.getErrorMessage(), std::move (callback));
        else
            callback (true);

        return;
    }

    askWhetherToSave ({}, std::move (callback));
}

void ProjectSession::askWhetherToSave (const juce::String& saveFailed, std::function<void (bool closed)> callback)
{
    // A save that fails asks again, offering Save As; an untitled project's save dialog cancelled leaves it open.
    const auto answer = [this, callback] (UnsavedChanges choice)
    {
        if (project == nullptr)
        {
            callback (true);
            return;
        }

        if (choice == UnsavedChanges::save && ! isUntitled())
        {
            const auto result = project->save();

            if (onProjectChanged != nullptr)
                onProjectChanged();

            if (result.failed())
                askWhetherToSave (result.getErrorMessage(), callback);
            else
                callback (true);
        }
        else if (choice == UnsavedChanges::save)
        {
            saveThen (callback);
        }
        else if (choice == UnsavedChanges::saveAs)
        {
            saveCopyThenClose (callback);
        }
        else
        {
            callback (choice == UnsavedChanges::discard); // "Don't save" closes; "Cancel" doesn't
        }
    };

    if (askToSave != nullptr)
    {
        askToSave (project->getFile(), saveFailed, answer);
        return;
    }

    const auto name = "\"" + project->getFile().getFileNameWithoutExtension() + "\"";
    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withAssociatedComponent (dialogParent);

    if (saveFailed.isEmpty())
        options = options.withTitle ("Save changes?").withMessage ("Save the changes to " + name + " first?").withButton ("Save", 1);
    else
        options = options.withIconType (juce::MessageBoxIconType::WarningIcon)
                      .withTitle ("Phazite couldn't save the project")
                      .withMessage (saveFailed + "\n\nSave the changes to " + name + " somewhere else, or close it without them?")
                      .withButton ("Save As...", 3);

    // Escape, or closing the window, answers 0, as Cancel does.
    juce::AlertWindow::showAsync (options.withButton ("Don't Save", 2).withButton ("Cancel", 0),
                                  [answer] (int result)
                                  {
                                      answer (result == 1   ? UnsavedChanges::save
                                              : result == 3 ? UnsavedChanges::saveAs
                                              : result == 2 ? UnsavedChanges::discard
                                                            : UnsavedChanges::cancel);
                                  });
}

void ProjectSession::saveCopyThenClose (std::function<void (bool closed)> callback)
{
    if (project == nullptr || isUntitled())
    {
        saveThen (std::move (callback)); // an untitled project's first save asks where it goes anyway
        return;
    }

    // Next to the projects at first, as the drive the project was on may be the one gone.
    chooseWhereToSave (projectsFolder.getChildFile (suggestName (projectsFolder, project->getFile().getFileNameWithoutExtension())),
                       [this, callback] (const juce::File& chosen)
                       {
                           if (chosen == juce::File() || project == nullptr)
                           {
                               callback (false);
                               return;
                           }

                           juce::String error;
                           waitForVersionsWritten (context); // the copy takes them whole
                           const auto newFile = project->saveCopyAs (chosen.getParentDirectory(), getChosenName (chosen), error);

                           if (newFile == juce::File())
                           {
                               askWhetherToSave (error, callback);
                               return;
                           }

                           recent.add (newFile); // where the changes are now
                           callback (true);
                       });
}

void ProjectSession::openWithRestoreQuestion (const juce::File& file)
{
    if (! Project::hasNewerAutosave (file))
        open (file, false);
    else
        askWhetherToRestore (file, false, [this, file] (bool restore) { open (file, restore); });
}

void ProjectSession::askWhetherToRestore (const juce::File& file, bool untitled, std::function<void (bool restore)> callback)
{
    if (askToRestore != nullptr)
    {
        askToRestore (file, untitled, std::move (callback));
        return;
    }

    const auto name = "\"" + file.getFileNameWithoutExtension() + "\"";
    const auto options =
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle ("Restore unsaved changes?")
            .withMessage ("Phazite didn't close properly the last time " + name + " was open" + (untitled ? ", before it was saved" : "")
                          + ", but it had saved your changes automatically. Restore them?")
            .withButton ("Restore", 1)
            .withButton (untitled ? "Discard them" : "Open the last saved version", 0)
            .withAssociatedComponent (dialogParent);

    // Escape, or closing the window, answers 0, as the second button does.
    juce::AlertWindow::showAsync (options, [callback] (int result) { callback (result == 1); });
}

void ProjectSession::open (const juce::File& file, bool restoreAutosave)
{
    juce::String error, notRestored;

    if (auto opened = Project::open (file, restoreAutosave, error, &notRestored))
    {
        setProject (std::move (opened));

        if (notRestored.isNotEmpty())
            showError ("Phazite couldn't restore the unsaved changes", notRestored);

        return;
    }

    // A file there that doesn't read: its newest backup that does, or its branch's last commit, offered in its place.
    if (file.existsAsFile())
    {
        auto [backupFile, backup] = Project::readNewestBackup (file);
        auto [commit, commitName] = readLastCommitSong (file);

        if (backup.isValid() || commit.isValid())
        {
            const auto backupText =
                backup.isValid() ? "the backup of " + backupFile.getLastModificationTime().formatted ("%d %b %Y %H:%M") : juce::String();
            const auto commitText = commit.isValid() ? "the last commit, \"" + commitName + "\"" : juce::String();

            const auto choose = [this, file, backupSong = backup, commitSong = commit] (int choice)
            {
                juce::String copyError;

                if (const auto& song = choice == 1 ? backupSong : commitSong; choice != 0 && song.isValid())
                {
                    if (auto opened = Project::openWithSong (file, song, copyError))
                    {
                        setProject (std::move (opened));
                        return;
                    }
                }

                recent.remove (file);

                if (copyError.isNotEmpty())
                    showError ("Phazite couldn't open the project", copyError);

                if (project == nullptr)
                    createFirstProject();
            };

            askWhichCopyToOpen (error, backupText, commitText, choose);
            return;
        }
    }

    recent.remove (file);
    showError ("Phazite couldn't open the project", error);

    if (project == nullptr)
        createFirstProject();
}

void ProjectSession::askWhichCopyToOpen (const juce::String& error, const juce::String& backup, const juce::String& commit,
                                         std::function<void (int)> callback)
{
    if (askWhichCopy != nullptr)
    {
        askWhichCopy (error, backup, commit, std::move (callback));
        return;
    }

    juce::StringArray copies;

    for (const auto& copy : { backup, commit })
        if (copy.isNotEmpty())
            copies.add (copy);

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle ("Phazite couldn't read the project")
                       .withMessage (error + "\n\n" + copies.joinIntoString (" and ") + (copies.size() > 1 ? " still read" : " still reads")
                                     + ". Open it in the project's place? Saving then writes it into the project's file.")
                       .withAssociatedComponent (dialogParent);

    if (backup.isNotEmpty())
        options = options.withButton ("Open the Backup", 1);

    if (commit.isNotEmpty())
        options = options.withButton ("Open the Last Commit", 2);

    // Escape, or closing the window, answers 0, as Cancel does.
    juce::AlertWindow::showAsync (options.withButton ("Cancel", 0), [callback] (int result) { callback (result); });
}

void ProjectSession::setProject (std::unique_ptr<Project> newProject, std::unique_ptr<UnsavedFolder> newUnsavedFolder)
{
    // A fall-back for a path that didn't keep it first (closeThen(), saveProjectAs()), before anything is dropped.
    if (project != nullptr)
        keepRecording();

    autosaveTimer = nullptr;
    firstSave = nullptr; // a first save's copy of the project closing stops, and goes (it would move that project)

    // What holds the song (an export, a render, the devices' windows, which store what their plug-ins changed) goes while
    // it is still open and playing, before the context lets go of it.
    if (project != nullptr && onProjectClosing != nullptr)
        onProjectClosing();

    setSong (context, nullptr);
    project = std::move (newProject);                    // the previous project closes here, deleting its autosave
    projectUnsavedFolder = std::move (newUnsavedFolder); // and then the folder it was made in, if it was untitled

    // Plug-ins load again for the next project, once the one that opens without them is open (its song in the context,
    // which makes what plays it).
    const auto pluginsWereOff = std::exchange (pluginsOffUntilOpen, false);

    if (pluginsWereOff && project != nullptr)
        showNotice ("Plug-ins off", "\"" + project->getFile().getFileNameWithoutExtension()
                                        + "\" is open with its plug-ins off: they stay as they were, silent. Open it again "
                                          "(File > Open Recent) to turn them on.");

    if (project != nullptr)
        setSong (context, &project->getSong());

    if (pluginsWereOff)
        getPluginHost (engineContext).setLoadsPlugins (true);

    if (project != nullptr)
    {
        project->onBackupFailed = [this] (const juce::String& message) { showError ("No backup was made", message); };
        autosaveTimer = std::make_unique<AutosaveTimer> (*project);
        autosaveTimer->onRepeatedFailures = [this]
        {
            showError ("Autosave failed",
                       "Phazite couldn't save your changes automatically, " + juce::String (AutosaveTimer::failuresToReport)
                           + " times in a row. Save the project to keep them, and check that its folder can be written to.");
        };
        autosaveTimer->onOlderPluginStates = [this] (const juce::StringArray& plugins) { tellOlderPluginStates (plugins); };

        if (! isUntitled()) // it is in Open Recent once it is saved
            recent.add (project->getFile());

        updateAutoSave();

        routeLiveMidiInput (project->getSong()); // MIDI keyboards and the computer keyboard play it
    }

    if (onProjectChanged != nullptr)
        onProjectChanged();
}

void ProjectSession::showNotice (const juce::String& title, const juce::String& message)
{
    if (onNotice != nullptr)
    {
        onNotice (title, message);
        return;
    }

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::InfoIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton ("OK", 0)
                                      .withAssociatedComponent (dialogParent),
                                  static_cast<juce::ModalComponentManager::Callback*> (nullptr));
}

void ProjectSession::showError (const juce::String& title, const juce::String& message)
{
    juce::Logger::writeToLog (title + ": " + message);

    if (onError != nullptr)
    {
        onError (title, message);
        return;
    }

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton ("OK", 0)
                                      .withAssociatedComponent (dialogParent),
                                  static_cast<juce::ModalComponentManager::Callback*> (nullptr));
}

void ProjectSession::gatePluginStates (std::function<void (bool)> run)
{
    // Once those that said they changed have given their states, as a save asks them, the progress window showing a wait
    // of more than a second with Go On Without Them and Cancel, which drops what waits (known bug 166); not if another
    // project opened meanwhile. While it waits, the window takes no input, as it takes none once the progress window
    // shows: no mouse edit changes the song under it. What waits for the wait to be over (a close as the computer's session
    // ends) comes after it.
    const auto finish = [this, session = std::weak_ptr<bool> (alive)]
    {
        if (session.expired() || --gatesWaiting > 0)
            return;

        for (const auto& then : std::exchange (afterGates, {}))
            then();
    };

    if (project == nullptr)
    {
        run (true);
        return;
    }

    ++gatesWaiting;
    auto done = std::make_shared<bool> (false);
    auto hold = std::make_shared<std::unique_ptr<InputHold>>();
    withPluginStates (
        [run, done, hold, finish] (bool go)
        {
            *done = true;

            if (*hold != nullptr)
                (*hold)->release();

            hold->reset();
            run (go);
            finish();
        },
        false, true);

    if (! *done)
    {
        *hold = std::make_unique<InputHold>();
        inputHold = hold->get();
    }
}

bool ProjectSession::isHoldingInput (const juce::Component* modal) const noexcept
{
    return gatesWaiting > 0 && modal != nullptr && modal == inputHold.getComponent();
}

void ProjectSession::afterPluginStatesGate (std::function<void()> then)
{
    if (gatesWaiting > 0)
        afterGates.push_back (std::move (then));
    else
        then();
}

void ProjectSession::addCommands (CommandRegistry& registry, TrackRenderHost& host)
{
    renders = &host;

    // A command that reads the plug-ins' states runs once they are in, as everything does that reads them (the context's
    // gate: gatePluginStates()).
    registry.setPluginStatesGate ([this] (std::function<void (bool)> run) { context.withPluginStates (std::move (run)); });

    // Help > Example Songs, each as a new untitled project.
    for (const auto& song : getExampleSongs())
    {
        const juce::String id (song.id);
        registry.add (
            { .id = "help.example." + id,
              .name = "Example Song: " + juce::String (song.name),
              .category = "Help",
              .description = juce::String (song.description) + " It opens as a new project, to play, change and save as your own.",
              .synonyms = { "example", "demo", "demo song", "learn", "template", "song" },
              .perform = [this, id] { openExampleSong (id); } });
    }

    registry.add ({ .id = "project.new",
                    .name = "New Project",
                    .category = "Project",
                    .description = "Starts a new project, which its first save names and puts in a folder of its own.",
                    .synonyms = { "create", "song" },
                    .perform = [this] { newProject(); } });

    registry.add ({ .id = "project.open",
                    .name = "Open Project",
                    .category = "Project",
                    .description = "Opens a saved project.",
                    .synonyms = { "load", "song" },
                    .perform = [this] { openProject(); } });

    registry.add ({ .id = "project.openRecent",
                    .name = "Open Recent Project",
                    .category = "Project",
                    .description = "Opens one of the projects you worked on last.",
                    .synonyms = { "recent", "last", "history", "load" },
                    .perform = [this] { chooseRecentProject(); } });

    registry.add ({ .id = "project.saveAs",
                    .name = "Save Project As",
                    .category = "Project",
                    .description = "Saves the project under a new name, in a folder of its own where you choose, and continues in it. "
                                   "The original keeps its last saved version.",
                    .synonyms = { "copy", "duplicate", "rename", "version" },
                    .perform = [this] { saveProjectAs(); },
                    .isEnabled = [this] { return project != nullptr; },
                    .needsPluginStates = true });

    registry.add ({ .id = "project.save",
                    .name = "Save Project",
                    .category = "Project",
                    .description = "Saves the project; the previous version goes into its Backups folder. A new project's first save "
                                   "asks what to call it and where its folder goes.",
                    .synonyms = { "store", "write" },
                    .perform = [this] { saveProject(); },
                    .isEnabled = [this] { return project != nullptr; } });

    registry.add ({ .id = "project.autoSave",
                    .name = "Auto Save",
                    .category = "Project",
                    .description = "Saves the project a moment after each change, as VS Code's Auto Save does, so that its changes "
                                   "show in source control as they are made; a new project once its first save puts it somewhere.",
                    .synonyms = { "autosave", "save automatically", "keep saved" },
                    .perform = [this] { toggleAutoSave(); },
                    .isTicked = [this] { return isAutoSaving(); } });

    // Save Version: the project saved first, asking where for an untitled one, then a version of its song; a switch to
    // another branch saves it after (model/versions/VersionsModel.h).
    addVersionCommands (registry, context, [this] (std::function<void (bool)> then) { saveThen (std::move (then)); }, host);

    registry.add ({ .id = "project.export",
                    .name = "Export Song",
                    .category = "Project",
                    .description = "Renders the song to a WAV or FLAC file, or its stems to a file for each channel, in the project's "
                                   "Renders folder unless you choose another, and normalizes the song's loudness if you ask.",
                    .synonyms = { "render", "bounce", "mixdown", "wav", "flac", "audio file", "loudness", "lufs", "stems", "multitrack" },
                    .perform = [this] { exportSong(); },
                    .isEnabled = [this] { return project != nullptr && canExport (project->getSong()); } });

    registry.add ({ .id = "project.cleanUpAudio",
                    .name = "Clean Up Unused Audio...",
                    .category = "Project",
                    .description = "Lists the audio files in the project's folder that neither the song, nor its saved file or "
                                   "backups, nor a commit, nor a branch plays, to move those you tick to the recycle bin.",
                    .synonyms = { "unused", "delete", "free space", "takes", "recordings", "tidy", "purge" },
                    .perform = [this] { cleanUpAudio(); },
                    .isEnabled = [this] { return canCleanUpAudio (context) && ! isUntitled(); } });

    registry.add ({ .id = "project.importDawProject",
                    .name = "Import DAWproject...",
                    .category = "Project",
                    .description = "Opens a DAWproject file, from Bitwig Studio, Studio One, Cubase or another DAW, as a new project. One "
                                   "exported from a commit of this project comes back as a branch from that commit, with what changed.",
                    .synonyms = { "dawproject", "bitwig", "studio one", "cubase", "open", "import", "collaborate" },
                    .perform = [this] { importDawProject(); } });
}

void ProjectSession::cleanUpAudio()
{
    if (project != nullptr && onCleanUpAudio != nullptr)
        onCleanUpAudio();
}

void ProjectSession::importDawProject()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import a DAWproject", projectsFolder, "*.dawproject");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  if (chooser.getResult().existsAsFile())
                                      importDawProject (chooser.getResult());
                              });
}

void ProjectSession::importDawProject (const juce::File& file)
{
    const auto read = std::make_shared<DawProjectFile> (readDawProject (file));

    if (read->project == nullptr)
    {
        showError ("Phazite couldn't import " + file.getFileName(), read->error);
        return;
    }

    // One from a version of this project: a branch from that version, with what changed in it.
    const auto versions = getVersions (context);

    if (read->fromVersion.isNotEmpty() && project != nullptr
        && std::any_of (versions.begin(), versions.end(), [&read] (const VersionInfo& version) { return version.id == read->fromVersion; }))
    {
        // Once the plug-ins that said they changed have given their states, which the branch left keeps (known bug 166).
        context.withPluginStates (
            [this, session = std::weak_ptr<bool> (alive), read, file] (bool go)
            {
                if (! go || session.expired())
                    return;

                juce::StringArray said;

                if (importDawProjectAsBranch (
                        context, *read, [this] (std::function<void (bool)> then) { saveThen (std::move (then)); }, said))
                    showNotice ("Imported " + file.getFileName(),
                                "It came back as a new branch from the commit it was exported from."
                                    + (said.isEmpty() ? juce::String() : " Left out: " + said.joinIntoString ("; ") + "."));
                else
                    showError ("Phazite couldn't import " + file.getFileName(), said.joinIntoString (" "));
            });

        return;
    }

    // Any other: a new project of its own, in place of the open one, whose changes are saved first if the user wants.
    closeThen (
        [this, file, read] (bool closed)
        {
            if (! closed)
                return;

            // What it couldn't bring in, told by track (known bug 91: it was said nowhere).
            auto said = std::make_shared<juce::StringArray>();

            if (const auto result = createUntitled (file.getFileNameWithoutExtension(),
                                                    [read, said] (Song& song) { *said = takeDawProject (song, *read); });
                result.failed())
                showError ("Phazite couldn't import " + file.getFileName(), result.getErrorMessage());
            else if (! said->isEmpty())
                showNotice ("Imported " + file.getFileName(), "Left out: " + said->joinIntoString ("; ") + ".");
        });
}
} // namespace phazite

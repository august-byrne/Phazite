// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSession.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/base/BackgroundJobs.h"
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
#include <atomic>
#include <memory>
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
        bool isHold() const override { return wait->forCommand; }

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

    // A wait for the project's own work before a command changes the song (ProjectContext::holdForWork(): a branch's song
    // read): whether it is over, whether the progress window shows it, with what title, and what its Cancel does.
    struct WorkWait
    {
        bool over = false, shown = false;
        juce::String title;
        std::function<void (const juce::String& why)> cancel; // why: none for the user's Cancel
        std::function<float()> progress;                      // from 0 to 1, below 0 while it isn't known; none for none
    };

    // That wait as the progress window shows it after a second: its Cancel (the window going while the wait isn't over)
    // cancels the work, which then changes nothing.
    struct WaitForWork final : TrackRender::Impl
    {
        explicit WaitForWork (std::shared_ptr<WorkWait> shownWait)
            : wait (std::move (shownWait))
        {
            wait->shown = true;
        }

        ~WaitForWork() override
        {
            wait->shown = false;

            if (! wait->over && wait->cancel != nullptr)
                wait->cancel ({}); // which lets go of the hold below, the window going already
        }

        float getProgress() const override { return wait->progress != nullptr ? wait->progress() : -1.0f; }
        juce::String getTitle() const override { return wait->title; }
        bool isHold() const override { return true; }

        std::shared_ptr<WorkWait> wait;
    };

    // The wait itself, while the work holds the window (its input is the session's one hold, InputHold.h, which the context
    // holds: ProjectContext::holdForWork()): its progress window, once handed to the render queue, goes as it does, shown or
    // still waiting its turn.
    struct WorkShown final : ProjectContext::WorkHold
    {
        WorkShown (std::shared_ptr<TrackRenderHost*> host, std::shared_ptr<WorkWait> w)
            : renders (std::move (host)),
              wait (std::move (w))
        {
        }

        ~WorkShown() override
        {
            wait->over = true;

            // The host as it is now: none once the main window has gone (it may go first, at a quit: the wait's hold lives on in
            // what keeps it, a value typed into a plug-in's control in its instance's ask, which goes with the session).
            if (auto* host = *renders; wait->shown && host != nullptr)
                host->renderFinished (wait->title, juce::Result::ok()); // which deletes its window, or takes it out of the queue
        }

        std::shared_ptr<TrackRenderHost*> renders;
        std::shared_ptr<WorkWait> wait;
    };

    // The copies of a project whose file can't be read, as they come in from the thread that reads them (message thread):
    // its newest backup that reads, then its branch's last commit; and the window's hold meanwhile, which only the read and
    // the message that offers them hold (its Cancel holds it weakly), let go of as either ends it.
    struct CopiesRead
    {
        juce::File backupFile;
        juce::ValueTree backup;
        std::atomic<bool> stop { false }; // the read cancelled: the commit isn't read if it hasn't started
        bool over = false;                // offered, or cancelled
        std::unique_ptr<ProjectContext::WorkHold> hold;
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

    // And what waits for the project's own work holds the window as that does (holdForWork()).
    context.setWorkHolder ([this] (const juce::String& title, ProjectContext::Cancel cancel, ProjectContext::Progress progress)
                           { return holdForWork (title, std::move (cancel), std::move (progress)); });
}

ProjectSession::~ProjectSession()
{
    // A read of a project's copies under way ends here, saying nothing: its hold let go of while what it holds lives.
    if (const auto read = std::static_pointer_cast<CopiesRead> (copiesReading.lock()))
    {
        read->over = true;
        read->stop = true;
        read->hold = nullptr;
    }

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
    // commit as a new project opens, which may come while the rest is copied (known bug 169). Without a history, with the
    // rest.
    project->setVersionsCopier (keepsVersions (context) ? makeVersionsCopier (context) : Project::VersionsCopier());

    firstSave = nullptr;
    firstSaveHold = nullptr;
    const auto title = "Saving " + juce::File::createLegalFileName (getChosenName (chosen).trim());

    auto copying = project->moveTo (chosen.getParentDirectory(), getChosenName (chosen),
                                    [this, callback] (const juce::Result& result)
                                    {
                                        // The copy is calling back, and lets go of itself last: what comes after takes copies.
                                        const auto then = callback;
                                        const auto saved = result.wasOk();
                                        firstSaveHold = nullptr; // the window takes input again, its progress window gone
                                        firstSave = nullptr;

                                        if (saved)
                                        {
                                            // Its folder from now on; the untitled one goes when the project closes. It
                                            // saves itself from now on, with Auto Save.
                                            recent.add (project->getFile());
                                            updateAutoSave();

                                            if (onProjectChanged != nullptr)
                                                onProjectChanged();
                                        }
                                        else
                                        {
                                            showError ("Phazite couldn't save the project", result.getErrorMessage());
                                        }

                                        then (saved);
                                    });

    if (copying == nullptr)
        return; // it couldn't begin, and said why

    // In the background, however small, the window held as for any work it waits for (ProjectContext::holdForWork()): its
    // progress window shows after a second, so a small project's save looks as if it were done at once. Its Cancel, Escape
    // before it shows, or a quit stop the copy, leaving nothing of it, and the project stays where it was.
    const auto* render = copying.get();
    firstSave = std::move (copying);
    firstSaveHold = context.holdForWork (
        title,
        [this] (const juce::String& why)
        {
            firstSaveHold = nullptr;
            firstSave = nullptr;

            if (why.isNotEmpty())
                showError ("Phazite didn't save the project", why);
        },
        [render] { return render->getProgress(); });
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
    wait->title = "Finishing the versions of " + getProjectsBeingFinished().joinIntoString (", ");
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

    // A file there that doesn't read: its newest backup that does, or its branch's last commit, offered in its place, once
    // they are read in the background.
    if (file.existsAsFile())
        readCopiesThenOffer (file, error);
    else
        couldNotOpen (file, error);
}

void ProjectSession::couldNotOpen (const juce::File& file, const juce::String& error)
{
    recent.remove (file);
    showError ("Phazite couldn't open the project", error);

    if (project == nullptr)
        createFirstProject();
}

void ProjectSession::readCopiesThenOffer (const juce::File& file, const juce::String& error)
{
    // Known bug 184: the backups and the last commit were read on the message thread before the question showed, the commit's
    // whole song joined from the store's pieces (80-190 ms in the 32-track benchmark song, more with a long history). Now on
    // a thread of their own, the window held as for any work (InputHold.h), with a progress window after a second whose
    // Cancel, or Escape, offers what was read by then (the backup, if it was), or says the error; a quit stops it, saying
    // nothing. The project's own file and its autosave, read as it opened, are the open itself.
    const auto read = std::make_shared<CopiesRead>();
    const auto session = std::weak_ptr<bool> (alive);
    copiesReading = read;

    read->hold = context.holdForWork ("Reading the copies of \"" + file.getFileNameWithoutExtension() + "\"",
                                      [this, session, weakRead = std::weak_ptr<CopiesRead> (read), file, error] (const juce::String& why)
                                      {
                                          const auto reading = weakRead.lock();

                                          if (reading == nullptr || reading->over)
                                              return;

                                          reading->over = true;
                                          reading->stop = true;
                                          auto hold = std::move (reading->hold); // let go of at the end of this call

                                          // A quit (why given): nothing more. A cancel: what was read, offered a message
                                          // later, once the progress window has gone.
                                          if (why.isEmpty())
                                              juce::MessageManager::callAsync (
                                                  [this, session, reading, file, error]
                                                  {
                                                      if (! session.expired())
                                                          offerCopies (file, error, reading->backupFile, reading->backup, {}, {});
                                                  });
                                      });

    copyReads.add (
        [this, session, read, file, error, computer = getThisComputer()] (const BackgroundJobs::ShouldStop& shouldStop)
        {
            auto [backupFile, backup] = Project::readNewestBackup (file);
            juce::MessageManager::callAsync (
                [read, backupFile = backupFile, backup = backup]
                {
                    read->backupFile = backupFile;
                    read->backup = backup;
                });

            // Tests' seam: the commit's read held where it starts until let go, or cancelled (VersionHistory::holdReadsForTests).
            while (VersionHistory::holdReadsForTests.load() && ! shouldStop() && ! read->stop.load())
                juce::Thread::sleep (1);

            if (shouldStop() || read->stop.load())
                return;

            auto [commit, commitName] = readLastCommitSong (file, computer);
            juce::MessageManager::callAsync (
                [this, session, read, file, error, commit = commit, commitName = commitName]
                {
                    if (session.expired() || read->over)
                        return;

                    read->over = true;
                    read->hold = nullptr; // the window let go of (its progress window too) before the question shows
                    offerCopies (file, error, read->backupFile, read->backup, commit, commitName);
                });
        });
}

void ProjectSession::offerCopies (const juce::File& file, const juce::String& error, const juce::File& backupFile,
                                  const juce::ValueTree& backup, const juce::ValueTree& commit, const juce::String& commitName)
{
    if (! backup.isValid() && ! commit.isValid())
    {
        couldNotOpen (file, error);
        return;
    }

    const auto backupText =
        backup.isValid() ? "the backup of " + backupFile.getLastModificationTime().formatted ("%d %b %Y %H:%M") : juce::String();
    const auto commitText = commit.isValid() ? "the last version, \"" + commitName + "\"" : juce::String();

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
        options = options.withButton ("Open the Last Version", 2);

    // Escape, or closing the window, answers 0, as Cancel does.
    juce::AlertWindow::showAsync (options.withButton ("Cancel", 0), [callback] (int result) { callback (result); });
}

void ProjectSession::setProject (std::unique_ptr<Project> newProject, std::unique_ptr<UnsavedFolder> newUnsavedFolder)
{
    // A fall-back for a path that didn't keep it first (closeThen(), saveProjectAs()), before anything is dropped.
    if (project != nullptr)
        keepRecording();

    autosaveTimer = nullptr;
    firstSaveHold = nullptr; // a first save's copy of the project closing stops, and goes (it would move that project)
    firstSave = nullptr;

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
    auto done = std::make_shared<bool> (false), held = std::make_shared<bool> (false);
    withPluginStates (
        [this, run, done, held, finish, session = std::weak_ptr<bool> (alive)] (bool go)
        {
            *done = true;

            // Let go of before what runs, on purpose: the command runs as it would have with nothing to wait for, the window's
            // input and focus its own again; a hold it begins (an import it starts) holds the window again in this same call,
            // with nothing run between the two (InputHold.h).
            if (std::exchange (*held, false) && ! session.expired())
                inputHold.releaseInput();

            run (go);
            finish();
        },
        false, true);

    // The window's one hold (InputHold.h), the same as the work that reads audio files takes.
    if (! *done)
    {
        *held = true;
        inputHold.holdInput();
    }
}

bool ProjectSession::isHoldingInput (const juce::Component* modal) const noexcept
{
    return InputHold::isHold (modal); // whichever work holds it: a wait for the plug-ins' states, or audio files' work
}

void ProjectSession::cancelWork()
{
    // Each read cancelled, the song and the branch as they were, its hold let go (which lets go of its window).
    for (const auto& w : std::exchange (workWaits, {}))
        if (const auto wait = std::static_pointer_cast<WorkWait> (w.lock()); wait != nullptr && ! wait->over && wait->cancel != nullptr)
            wait->cancel ("Stopped, as Phazite was closing: nothing was changed.");

    // And an audio file's work (an import, Normalize), its reads not waited for (AudioWork.h): every kind of work that holds
    // the window, stopped by this one call, so that the close goes on.
    getAudioWork (context).giveUpAll ("Stopped, as Phazite was closing: nothing was changed.");
}

bool ProjectSession::cancelHeldWork()
{
    // Escape while the window holds, before a progress window shows (which takes its own Escape): the newest work that
    // holds it cancelled, as its window's Cancel would; a versions read's, else an audio file's.
    for (auto w = workWaits.rbegin(); w != workWaits.rend(); ++w)
        if (const auto wait = std::static_pointer_cast<WorkWait> (w->lock()); wait != nullptr && ! wait->over && wait->cancel != nullptr)
        {
            wait->cancel ({});
            return true;
        }

    return getAudioWork (context).cancelFirst();
}

std::unique_ptr<ProjectContext::WorkHold> ProjectSession::holdForWork (const juce::String& title, ProjectContext::Cancel cancel,
                                                                       ProjectContext::Progress progress)
{
    // As a command's wait for the plug-ins' states holds it (gatePluginStates()): the window held at once, and the progress
    // window shown once a second has gone, unless the work is over by then.
    auto wait = std::make_shared<WorkWait>();
    wait->title = title;
    wait->cancel = std::move (cancel);
    wait->progress = std::move (progress);
    auto hold = std::make_unique<WorkShown> (rendersNow, wait);
    std::erase_if (workWaits, [] (const std::weak_ptr<void>& w) { return w.expired(); });
    workWaits.push_back (wait);

    juce::Timer::callAfterDelay (workShownAfter,
                                 [this, session = std::weak_ptr<bool> (alive), wait]
                                 {
                                     if (! session.expired() && ! wait->over && renders != nullptr)
                                         renders->runRender (std::make_unique<TrackRender> (std::make_unique<WaitForWork> (wait)));
                                 });
    return hold;
}

void ProjectSession::afterPluginStatesGate (std::function<void()> then)
{
    if (gatesWaiting > 0)
        afterGates.push_back (std::move (then));
    else
        then();
}

void ProjectSession::letGoOfRenderHost()
{
    cancelWork(); // what holds the window told first, while the host is there to take the windows' progress back
    renders = nullptr;
    *rendersNow = nullptr;
}

void ProjectSession::addCommands (CommandRegistry& registry, TrackRenderHost& host)
{
    renders = &host;
    *rendersNow = &host;

    // A command that reads the plug-ins' states runs once they are in, as everything does that reads them (the context's
    // gate: gatePluginStates()).
    registry.setPluginStatesGate ([this] (std::function<void (bool)> run) { context.withPluginStates (std::move (run)); });
    registry.setBusyCheck ([this] { return context.isHoldingForWork(); });

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
                                   "show in the Versions view as they are made; a new project once its first save puts it somewhere.",
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
                                   "backups, nor a version, nor a branch plays, to move those you tick to the recycle bin.",
                    .synonyms = { "unused", "delete", "free space", "takes", "recordings", "tidy", "purge" },
                    .perform = [this] { cleanUpAudio(); },
                    .isEnabled = [this] { return canCleanUpAudio (context) && ! isUntitled(); } });

    registry.add ({ .id = "project.importDawProject",
                    .name = "Import DAWproject...",
                    .category = "Project",
                    .description = "Opens a DAWproject file, from Bitwig Studio, Studio One, Cubase or another DAW, as a new project. One "
                                   "exported from a version of this project comes back as a branch from that version, with what changed.",
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

                // Said once it is over (the commit read, the branch made and switched to, the window held meanwhile), or not at
                // all if it was cancelled.
                importDawProjectAsBranch (
                    context, read, [this] (std::function<void (bool)> then) { saveThen (std::move (then)); },
                    [this, session, file] (bool imported, const juce::StringArray& said)
                    {
                        if (session.expired())
                            return;

                        if (imported)
                            showNotice ("Imported " + file.getFileName(),
                                        "It came back as a new branch from the version it was exported from."
                                            + (said.isEmpty() ? juce::String() : " Left out: " + said.joinIntoString ("; ") + "."));
                        else if (said.joinIntoString ({}).isNotEmpty())
                            showError ("Phazite couldn't import " + file.getFileName(), said.joinIntoString (" "));
                    });
            });

        return;
    }

    // Any other: a new project of its own, in place of the open one: its audio unpacked first, on a thread of its own, in the
    // progress window, whose Cancel leaves the open project as it was (found in review of known bug 126: the message thread
    // unpacked it, for seconds with a large file); then the open one closed, its changes saved first if the user wants.
    unpackThenImport (file, read);
}

namespace
{
    // An import's audio unpacked into a folder of its own beside the new projects, on a thread of its own, as the progress
    // window shows it; its Cancel (the window going before it is done) stops it, and what was unpacked is taken away.
    struct UnpackForImport final : TrackRender::Impl
    {
        struct State
        {
            std::atomic<float> progress { 0.0f };
            std::atomic<bool> stop { false };
            bool over = false;
        };

        UnpackForImport (juce::String t, std::shared_ptr<State> s, std::shared_ptr<UnsavedFolder> staged)
            : title (std::move (t)),
              state (std::move (s)),
              staging (std::move (staged))
        {
        }

        ~UnpackForImport() override
        {
            state->stop = true;
            jobs.removeAll (true); // then the staging folder goes with the last of what holds it: cancelled, nothing left
        }

        float getProgress() const override { return state->progress.load(); }
        juce::String getTitle() const override { return title; }

        juce::String title;
        std::shared_ptr<State> state;
        std::shared_ptr<UnsavedFolder> staging; // locked while it lives, as a new project's folder is, and deleted as it goes
        BackgroundJobs jobs { "Phazite import", 1, juce::Thread::Priority::normal };
    };
} // namespace

void ProjectSession::unpackThenImport (const juce::File& file, std::shared_ptr<DawProjectFile> read)
{
    // A folder of its own beside the new projects' (on their drive, so that its files move into the project by a rename),
    // locked as theirs are, so that another Phazite starting leaves it alone; one a crash left, which holds no project, is
    // deleted at the next start (UnsavedFolder::findProjectToRestore()).
    const std::shared_ptr<UnsavedFolder> staging = UnsavedFolder::create (unsavedFolder);

    if (staging == nullptr)
    {
        showError ("Phazite couldn't import " + file.getFileName(),
                   "It couldn't make a folder in " + unsavedFolder.getFullPathName() + ".");
        return;
    }

    // Without a window to show it (a session of the tests' without one), unpacked here, at once.
    if (renders == nullptr)
    {
        closeThenImport (file, read, unpackDawProjectAudio (*read, staging->getFolder()), staging);
        return;
    }

    const auto title = "Unpacking the audio of " + file.getFileName();
    const auto state = std::make_shared<UnpackForImport::State>();
    auto unpacking = std::make_unique<UnpackForImport> (title, state, staging);
    unpacking->jobs.add (
        [this, session = std::weak_ptr<bool> (alive), state, staging, read, file, title] (const BackgroundJobs::ShouldStop& shouldStop)
        {
            auto audio = std::make_shared<UnpackedAudio> (unpackDawProjectAudio (
                *read, staging->getFolder(), [&shouldStop, state] { return shouldStop() || state->stop.load(); },
                [state] (float done) { state->progress = done; }));

            juce::MessageManager::callAsync (
                [this, session, state, staging, read, file, title, audio]
                {
                    if (session.expired() || state->stop.load())
                        return;

                    // The window goes (renderFinished() deletes it), what was unpacked kept: then into the new project.
                    state->over = true;

                    if (renders != nullptr)
                        renders->renderFinished (title, juce::Result::ok());

                    closeThenImport (file, read, *audio, staging);
                });
        });

    renders->runRender (std::make_unique<TrackRender> (std::move (unpacking)));
}

void ProjectSession::closeThenImport (const juce::File& file, std::shared_ptr<DawProjectFile> read, UnpackedAudio audio,
                                      std::shared_ptr<UnsavedFolder> staging)
{
    // Unpacked: now the open project closes, asking whether to save its changes; left open, what was unpacked goes with the
    // staging folder.
    closeThen (
        [this, file, read, audio, staging] (bool closed)
        {
            if (closed)
                importUnpacked (file, read, audio, staging->getFolder());
        });
}

void ProjectSession::importUnpacked (const juce::File& file, std::shared_ptr<DawProjectFile> read, UnpackedAudio audio,
                                     const juce::File& staging)
{
    // What it couldn't bring in, told by track (known bug 91: it was said nowhere). The audio moved from where it was
    // unpacked into the new project's Audio folder (a rename: the same drive), each by the name it has there.
    auto said = std::make_shared<juce::StringArray>();
    const auto result = createUntitled (file.getFileNameWithoutExtension(),
                                        [read, said, &audio] (Song& song)
                                        {
                                            const auto folder = song.getFile().getSiblingFile ("Audio");
                                            folder.createDirectory();
                                            UnpackedAudio moved;

                                            for (const auto& [path, unpacked] : audio.files)
                                            {
                                                const auto target = folder.getNonexistentChildFile (unpacked.getFileNameWithoutExtension(),
                                                                                                    unpacked.getFileExtension(), false);

                                                if (unpacked.moveFileTo (target))
                                                {
                                                    moved.files.emplace (path, target);
                                                    moved.made.push_back (target);
                                                }
                                            }

                                            *said = takeDawProject (song, *read, moved);
                                            forgetUnusedAudio (moved, song.getState(), song.getFile());
                                        });
    juce::ignoreUnused (staging); // deleted with its folder's holder, by the caller

    if (result.failed())
        showError ("Phazite couldn't import " + file.getFileName(), result.getErrorMessage());
    else if (! said->isEmpty())
        showNotice ("Imported " + file.getFileName(), "Left out: " + said->joinIntoString ("; ") + ".");
}
} // namespace phazite

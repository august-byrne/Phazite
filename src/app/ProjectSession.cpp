// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSession.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/project/ExampleSongs.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/NewProject.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "services/Library.h"
#include "ui/shell/NewProjectPanel.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // The name typed in the save dialog, without the project file's extension if the dialog added it.
    juce::String getChosenName (const juce::File& chosen)
    {
        return chosen.hasFileExtension (Project::fileExtension) ? chosen.getFileNameWithoutExtension() : chosen.getFileName();
    }
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

void ProjectSession::openAtStartup (bool afterCrash)
{
    // An untitled project that an earlier Phazite didn't close properly, such as in a crash, comes first: its changes are
    // nowhere else.
    const auto leftover = UnsavedFolder::findProjectToRestore (unsavedFolder);

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
        juce::String error;

        if (auto restored = Project::open (file, true, error))
        {
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
                           [this, callback] (const juce::File& chosen) { callback (chosen != juce::File() && moveUntitledTo (chosen)); });
        return;
    }

    const auto result = project->save();

    if (result.failed())
        showError ("Phazite couldn't save the project", result.getErrorMessage());

    if (onProjectChanged != nullptr)
        onProjectChanged();

    callback (result.wasOk());
}

bool ProjectSession::moveUntitledTo (const juce::File& chosen)
{
    if (! isUntitled())
        return false;

    // Its versions move with it, whole: the initial commit is written on the history's thread as a new project opens.
    waitForVersionsWritten (context);

    if (const auto result = project->moveTo (chosen.getParentDirectory(), getChosenName (chosen)); result.failed())
    {
        showError ("Phazite couldn't save the project", result.getErrorMessage());
        return false;
    }

    // Its folder from now on; the untitled one goes when the project closes. It saves itself from now on, with Auto Save.
    recent.add (project->getFile());
    updateAutoSave();

    if (onProjectChanged != nullptr)
        onProjectChanged();

    return true;
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

void ProjectSession::closeThen (std::function<void (bool)> callback)
{
    if (project == nullptr || ! project->hasUnsavedChanges())
    {
        callback (true);
        return;
    }

    // With Auto Save, as in VS Code: the changes saved, nothing asked.
    if (autoSave && ! isUntitled())
    {
        const auto result = project->save();

        if (result.failed())
            showError ("Phazite couldn't save the project", result.getErrorMessage());

        callback (result.wasOk());
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Save changes?")
                             .withMessage ("Save the changes to \"" + project->getFile().getFileNameWithoutExtension() + "\" first?")
                             .withButton ("Save", 1)
                             .withButton ("Don't save", 2)
                             .withButton ("Cancel", 0)
                             .withAssociatedComponent (dialogParent);

    // Escape, or closing the window, answers 0, as Cancel does. A save that fails, or an untitled project's save dialog
    // cancelled, leaves the project open.
    juce::AlertWindow::showAsync (options,
                                  [this, callback] (int result)
                                  {
                                      if (result == 1)
                                          saveThen (callback);
                                      else
                                          callback (result == 2); // "Don't save" closes; "Cancel" doesn't
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
    juce::String error;

    if (auto opened = Project::open (file, restoreAutosave, error))
    {
        setProject (std::move (opened));
        return;
    }

    recent.remove (file);
    showError ("Phazite couldn't open the project", error);

    if (project == nullptr)
        createFirstProject();
}

void ProjectSession::setProject (std::unique_ptr<Project> newProject, std::unique_ptr<UnsavedFolder> newUnsavedFolder)
{
    autosaveTimer = nullptr;

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
        autosaveTimer = std::make_unique<AutosaveTimer> (*project);
        autosaveTimer->onRepeatedFailures = [this]
        {
            showError ("Autosave failed",
                       "Phazite couldn't save your changes automatically, " + juce::String (AutosaveTimer::failuresToReport)
                           + " times in a row. Save the project to keep them, and check that its folder can be written to.");
        };

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

void ProjectSession::addCommands (CommandRegistry& registry, TrackRenderHost& renders)
{
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
                    .isEnabled = [this] { return project != nullptr; } });

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
    addVersionCommands (registry, context, [this] (std::function<void (bool)> then) { saveThen (std::move (then)); }, renders);

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
                    .description = "Lists the audio files in the project's folder that neither the song, nor a commit, nor a branch "
                                   "plays, to move those you tick to the recycle bin.",
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
        juce::StringArray said;

        if (importDawProjectAsBranch (context, *read, [this] (std::function<void (bool)> then) { saveThen (std::move (then)); }, said))
            showNotice ("Imported " + file.getFileName(),
                        "It came back as a new branch from the commit it was exported from."
                            + (said.isEmpty() ? juce::String() : " Left out: " + said.joinIntoString (", ") + "."));
        else
            showError ("Phazite couldn't import " + file.getFileName(), said.joinIntoString (" "));

        return;
    }

    // Any other: a new project of its own.
    if (const auto result = createUntitled (file.getFileNameWithoutExtension(), [read] (Song& song) { takeDawProject (song, *read); });
        result.failed())
        showError ("Phazite couldn't import " + file.getFileName(), result.getErrorMessage());
}
} // namespace phazite

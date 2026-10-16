// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSession.h"
#include "model/engine/LiveInput.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/StarterProject.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/NewProject.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/shell/NewProjectPanel.h"
#include "ui/theme/LookAndFeel.h"

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

ProjectSession::ProjectSession (te::Engine& e, UIBehaviour& ui, RecentProjects& recentProjects, juce::File projects, juce::File unsaved)
    : engine (e),
      projectsFolder (std::move (projects)),
      unsavedFolder (std::move (unsaved)),
      recent (recentProjects),
      engineContext (createEngineContext (e)),
      context (*engineContext, &ui)
{
    engine.getDeviceManager().addChangeListener (this);
    getSelection().addChangeListener (this);
}

ProjectSession::~ProjectSession()
{
    getSelection().removeChangeListener (this);
    engine.getDeviceManager().removeChangeListener (this);
    setProject (nullptr); // closes the project on the message thread, before the Engine goes
}

void ProjectSession::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (project == nullptr)
        return;

    if (source == &getSelection())
        followSelectedTrack (getSelection());
    else
        routeLiveMidiInput (project->getEdit()); // MIDI inputs show up a moment after startup, and when plugged in
}

bool ProjectSession::isUntitled() const
{
    return project != nullptr && projectUnsavedFolder != nullptr && project->getFile().isAChildOf (projectUnsavedFolder->getFolder());
}

void ProjectSession::openAtStartup()
{
    // An untitled project that an earlier Phazite didn't close properly, such as in a crash, comes first: its changes are
    // nowhere else.
    if (const auto leftover = UnsavedFolder::findProjectToRestore (engine, unsavedFolder); leftover != juce::File())
        askWhetherToRestore (leftover, true, [this, leftover] (bool restore) { restoreUntitled (leftover, restore); });
    else
        openRecentOrFirst();
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
    if (auto folder = UnsavedFolder::takeOver (engine, file); folder != nullptr && restore)
    {
        juce::String error;

        if (auto restored = Project::open (engine, file, true, error))
        {
            setProject (std::move (restored), std::move (folder));
            return;
        }

        showError ("Phazite couldn't restore the project", error);
    }

    openRecentOrFirst();
}

juce::Result ProjectSession::createUntitled (const juce::String& name, const std::function<void (te::Edit&)>& addContent)
{
    auto folder = UnsavedFolder::create (engine, unsavedFolder);

    if (folder == nullptr)
        return juce::Result::fail ("Phazite couldn't make a folder for a new project in " + unsavedFolder.getFullPathName() + ".");

    juce::String error;
    auto created = Project::create (engine, folder->getFolder(), name, error, addContent);

    if (created == nullptr)
        return juce::Result::fail (error);

    setProject (std::move (created), std::move (folder));
    return juce::Result::ok();
}

void ProjectSession::createFirstProject()
{
    if (const auto result = createUntitled ("My First Song", addStarterContent); result.failed())
    {
        showError ("Phazite couldn't create a project", result.getErrorMessage());
        return;
    }

    selectSynthTrack();
}

void ProjectSession::selectSynthTrack()
{
    // Live MIDI follows the selection, so the computer keyboard plays the synth, in its octave.
    if (project != nullptr)
        if (auto* synth = findSynthTrack (project->getEdit()))
            getSelection().selectOnly (synth);
}

void ProjectSession::newProject()
{
    closeThen (
        [this] (bool closed)
        {
            if (! closed)
                return;

            auto* panel = new ui::NewProjectPanel ([this] (const NewProjectSettings& settings) { return createProject (settings); });
            panel->setSize (540, 460);

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
    if (const auto result = createUntitled ("Untitled", [settings] (te::Edit& edit) { addNewProjectContent (edit, settings); });
        result.failed())
        return result;

    if (settings.withInstruments)
        selectSynthTrack();

    return juce::Result::ok();
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

    if (const auto result = project->moveTo (chosen.getParentDirectory(), getChosenName (chosen)); result.failed())
    {
        showError ("Phazite couldn't save the project", result.getErrorMessage());
        return false;
    }

    // Its folder from now on; the untitled one goes when the project closes.
    recent.add (project->getFile());

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

void ProjectSession::closeThen (std::function<void (bool)> callback)
{
    if (project == nullptr || ! project->hasUnsavedChanges())
    {
        callback (true);
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Save changes?")
                             .withMessage ("Save the changes to \"" + project->getFile().getFileNameWithoutExtension() + "\" first?")
                             .withButton ("Save")
                             .withButton ("Don't save")
                             .withButton ("Cancel")
                             .withAssociatedComponent (dialogParent);

    // With three buttons, JUCE reports the first as 1, the second as 2 and the third as 0. A save that fails, or an
    // untitled project's save dialog cancelled, leaves the project open.
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
            .withButton ("Restore")
            .withButton (untitled ? "Discard them" : "Open the last saved version")
            .withAssociatedComponent (dialogParent);

    // With two buttons, JUCE reports the first as 1 and the second as 0.
    juce::AlertWindow::showAsync (options, [callback] (int result) { callback (result == 1); });
}

void ProjectSession::open (const juce::File& file, bool restoreAutosave)
{
    juce::String error;

    if (auto opened = Project::open (engine, file, restoreAutosave, error))
    {
        setProject (std::move (opened));
        return;
    }

    // A newer Phazite's project stays in Open Recent, for the version that opens it (design question 17).
    const auto tooNew = Project::isFromNewerPhazite (engine, file);

    if (! tooNew)
        recent.remove (file);

    showError ("Phazite couldn't open the project", error);

    if (project == nullptr)
    {
        if (tooNew)
            openNextRecentProject (file);
        else
            createFirstProject();
    }
}

void ProjectSession::openNextRecentProject (const juce::File& failed)
{
    const auto projects = recent.getProjects();

    for (auto i = projects.indexOf (failed) + 1; i < projects.size(); ++i)
    {
        if (! Project::isFromNewerPhazite (engine, projects[i]))
        {
            openWithRestoreQuestion (projects[i]);
            return;
        }
    }

    createFirstProject();
}

void ProjectSession::setProject (std::unique_ptr<Project> newProject, std::unique_ptr<UnsavedFolder> newUnsavedFolder)
{
    autosaveTimer = nullptr;
    setEdit (context, nullptr);

    if (project != nullptr && onProjectClosing != nullptr)
        onProjectClosing();

    project = std::move (newProject);                    // the previous project closes here, deleting its autosave
    projectUnsavedFolder = std::move (newUnsavedFolder); // and then the folder it was made in, if it was untitled

    if (project != nullptr)
    {
        setEdit (context, &project->getEdit());
        autosaveTimer = std::make_unique<AutosaveTimer> (*project);

        if (! isUntitled()) // it is in Open Recent once it is saved
            recent.add (project->getFile());

        routeLiveMidiInput (project->getEdit()); // MIDI keyboards and the computer keyboard play it
    }

    if (onProjectChanged != nullptr)
        onProjectChanged();
}

void ProjectSession::showError (const juce::String& title, const juce::String& message)
{
    if (onError != nullptr)
    {
        onError (title, message);
        return;
    }

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton ("OK")
                                      .withAssociatedComponent (dialogParent),
                                  static_cast<juce::ModalComponentManager::Callback*> (nullptr));
}

void ProjectSession::addCommands (CommandRegistry& registry)
{
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

    registry.add ({ .id = "project.export",
                    .name = "Export Song",
                    .category = "Project",
                    .description = "Renders the song to a WAV or FLAC file, in the project's Renders folder unless you choose another, "
                                   "and normalizes its loudness if you ask.",
                    .synonyms = { "render", "bounce", "mixdown", "wav", "flac", "audio file", "loudness", "lufs" },
                    .perform = [this] { exportSong(); },
                    .isEnabled = [this] { return project != nullptr && canExport (project->getEdit()); } });
}
} // namespace phazite

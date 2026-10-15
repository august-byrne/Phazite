// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSession.h"
#include "model/engine/LiveInput.h"
#include "model/project/Export.h"
#include "model/project/StarterProject.h"
#include "ui/shell/NewProjectPanel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite
{
ProjectSession::ProjectSession (te::Engine& e, UIBehaviour& ui, RecentProjects& recentProjects, juce::File folder)
    : engine (e),
      projectsFolder (std::move (folder)),
      recent (recentProjects),
      context (e, &ui)
{
    engine.getDeviceManager().addChangeListener (this);
    context.getSelection().addChangeListener (this);
}

ProjectSession::~ProjectSession()
{
    context.getSelection().removeChangeListener (this);
    engine.getDeviceManager().removeChangeListener (this);
    setProject (nullptr); // closes the project on the message thread, before the Engine goes
}

void ProjectSession::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (project == nullptr)
        return;

    if (source == &context.getSelection())
        followSelectedTrack (context.getSelection());
    else
        routeLiveMidiInput (project->getEdit()); // MIDI inputs show up a moment after startup, and when plugged in
}

void ProjectSession::openAtStartup()
{
    if (const auto projects = recent.getProjects(); ! projects.isEmpty())
        openWithRestoreQuestion (projects.getFirst());
    else
        createFirstProject();
}

void ProjectSession::createFirstProject()
{
    auto folder = projectsFolder;
    folder.createDirectory();

    juce::String name ("My First Song");

    for (int n = 2; folder.getChildFile (name).exists(); ++n)
        name = "My First Song " + juce::String (n);

    juce::String error;
    auto created = Project::create (engine, folder, name, error, addStarterContent);

    if (created == nullptr)
    {
        showError ("Phazite couldn't create a project", error);
        return;
    }

    setProject (std::move (created));
    selectSynthTrack();
}

void ProjectSession::selectSynthTrack()
{
    // Live MIDI follows the selection, so the computer keyboard plays the synth, in its octave.
    if (project != nullptr)
        if (auto* synth = findSynthTrack (project->getEdit()))
            context.getSelection().selectOnly (synth);
}

void ProjectSession::newProject()
{
    closeThen (
        [this] (bool closed)
        {
            if (! closed)
                return;

            const auto folder = projectsFolder;
            auto* panel = new ui::NewProjectPanel (suggestNewName (folder), folder,
                                                   [this, folder] (const juce::String& name, const NewProjectSettings& settings)
                                                   { return createProject (folder, name, settings); });
            panel->setSize (540, 500);

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

juce::String ProjectSession::suggestNewName (const juce::File& folder)
{
    juce::String name ("Untitled");

    for (int n = 2; folder.getChildFile (name).exists(); ++n)
        name = "Untitled " + juce::String (n);

    return name;
}

juce::Result ProjectSession::createProject (const juce::File& folder, const juce::String& name, const NewProjectSettings& settings)
{
    folder.createDirectory();

    // What the project starts with is part of its first save, not undo steps.
    juce::String error;
    auto created = Project::create (engine, folder, name, error, [settings] (te::Edit& edit) { addNewProjectContent (edit, settings); });

    if (created == nullptr)
        return juce::Result::fail (error);

    setProject (std::move (created));

    if (settings.withInstruments)
        selectSynthTrack();

    return juce::Result::ok();
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

    const auto parentFolder = project->getFolder().getParentDirectory();
    auto* window = new juce::AlertWindow ("Save project as",
                                          "Name the copy. Phazite saves it in a folder of its own in " + parentFolder.getFullPathName()
                                              + " and continues in it. This project keeps its last saved version.",
                                          juce::MessageBoxIconType::NoIcon, dialogParent);
    window->addTextEditor ("name", project->getFile().getFileNameWithoutExtension() + " copy");
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
                             juce::ModalCallbackFunction::create (
                                 [this, window, parentFolder] (int result)
                                 {
                                     if (result != 1 || project == nullptr)
                                         return;

                                     juce::String error;
                                     const auto newFile = project->saveCopyAs (parentFolder, window->getTextEditorContents ("name"), error);

                                     if (newFile == juce::File())
                                     {
                                         showError ("Phazite couldn't save the project", error);
                                         return;
                                     }

                                     // Continue in the copy; the unsaved changes went into it.
                                     setProject (nullptr);
                                     open (newFile, false);
                                 }),
                             true);
}

void ProjectSession::saveProject()
{
    if (project == nullptr)
        return;

    if (const auto result = project->save(); result.failed())
        showError ("Phazite couldn't save the project", result.getErrorMessage());

    if (onProjectChanged != nullptr)
        onProjectChanged();
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

    // With three buttons, JUCE reports the first as 1, the second as 2 and the third as 0.
    juce::AlertWindow::showAsync (options,
                                  [this, callback] (int result)
                                  {
                                      if (result == 1)
                                      {
                                          if (project != nullptr)
                                          {
                                              if (const auto saved = project->save(); saved.failed())
                                              {
                                                  showError ("Phazite couldn't save the project", saved.getErrorMessage());
                                                  callback (false);
                                                  return;
                                              }
                                          }

                                          callback (true);
                                      }
                                      else
                                      {
                                          callback (result == 2); // "Don't save" closes; "Cancel" doesn't
                                      }
                                  });
}

void ProjectSession::openWithRestoreQuestion (const juce::File& file)
{
    if (! Project::hasNewerAutosave (file))
    {
        open (file, false);
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Restore unsaved changes?")
                             .withMessage ("Phazite didn't close properly the last time \"" + file.getFileNameWithoutExtension()
                                           + "\" was open, but it had saved your changes automatically. Restore them?")
                             .withButton ("Restore")
                             .withButton ("Open the last saved version")
                             .withAssociatedComponent (dialogParent);

    juce::AlertWindow::showAsync (options, [this, file] (int result) { open (file, result == 1); });
}

void ProjectSession::open (const juce::File& file, bool restoreAutosave)
{
    juce::String error;

    if (auto opened = Project::open (engine, file, restoreAutosave, error))
    {
        setProject (std::move (opened));
        return;
    }

    // A newer Phazite's project stays in Open Recent, for the version that opens it (design question 17's proposal).
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

void ProjectSession::setProject (std::unique_ptr<Project> newProject)
{
    autosaveTimer = nullptr;
    context.setEdit (nullptr);

    if (project != nullptr && onProjectClosing != nullptr)
        onProjectClosing();

    project = std::move (newProject); // the previous project closes here, deleting its autosave

    if (project != nullptr)
    {
        context.setEdit (&project->getEdit());
        autosaveTimer = std::make_unique<AutosaveTimer> (*project);
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
                    .description = "Makes a new project in its own folder.",
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
                    .description =
                        "Saves the project under a new name, in a folder of its own, and continues in it. The original keeps its last "
                        "saved version.",
                    .synonyms = { "copy", "duplicate", "rename", "version" },
                    .perform = [this] { saveProjectAs(); },
                    .isEnabled = [this] { return project != nullptr; } });

    registry.add ({ .id = "project.save",
                    .name = "Save Project",
                    .category = "Project",
                    .description = "Saves the project. The previous version goes into its Backups folder.",
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSession.h"
#include "model/LiveInput.h"
#include "model/Outlines.h"
#include "model/StarterProject.h"

namespace phazite
{
ProjectSession::ProjectSession (te::Engine& e, UIBehaviour& ui)
    : engine (e),
      uiBehaviour (ui),
      recent (e)
{
    engine.getDeviceManager().addChangeListener (this);
    selection.addChangeListener (this);
    uiBehaviour.setSelectionManager (&selection);
}

ProjectSession::~ProjectSession()
{
    uiBehaviour.setSelectionManager (nullptr);
    selection.removeChangeListener (this);
    engine.getDeviceManager().removeChangeListener (this);
    setProject (nullptr); // closes the project on the message thread, before the Engine goes
}

void ProjectSession::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (project == nullptr)
        return;

    if (source == &selection)
        followSelectedTrack (selection);
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
    auto folder = getDefaultProjectsFolder();
    folder.createDirectory();

    juce::String name ("My First Song");

    for (int n = 2; folder.getChildFile (name).exists(); ++n)
        name = "My First Song " + juce::String (n);

    juce::String error;
    auto created = Project::create (engine, folder, name, error);

    if (created == nullptr)
    {
        showError ("Phazite couldn't create a project", error);
        return;
    }

    addStarterContent (created->getEdit());

    if (const auto result = created->save(); result.failed())
        showError ("Phazite couldn't save the project", result.getErrorMessage());

    setProject (std::move (created));
}

void ProjectSession::newProject()
{
    closeThen (
        [this] (bool closed)
        {
            if (! closed)
                return;

            const auto folder = getDefaultProjectsFolder();
            auto* window = new juce::AlertWindow ("New project",
                                                  "Name the project. Phazite makes a folder for it in " + folder.getFullPathName()
                                                      + ". A song outline lays out its sections, which you can change later.",
                                                  juce::MessageBoxIconType::NoIcon, dialogParent);
            window->addTextEditor ("name", "Untitled");

            juce::StringArray outlineNames { "No outline" };

            for (const auto& outline : getBuiltInOutlines())
                outlineNames.add (outline.name);

            window->addComboBox ("outline", outlineNames, "Song outline");
            window->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
            window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            window->enterModalState (true,
                                     juce::ModalCallbackFunction::create (
                                         [this, window, folder] (int result)
                                         {
                                             if (result != 1)
                                                 return;

                                             folder.createDirectory();
                                             juce::String error;
                                             auto created = Project::create (engine, folder, window->getTextEditorContents ("name"), error);

                                             if (created == nullptr)
                                             {
                                                 showError ("Phazite couldn't create the project", error);
                                                 return;
                                             }

                                             // The outline is part of the new project, not a change to undo.
                                             const auto choice = window->getComboBoxComponent ("outline")->getSelectedItemIndex() - 1;

                                             if (juce::isPositiveAndBelow (choice, (int) getBuiltInOutlines().size()))
                                             {
                                                 applyOutline (created->getEdit(), getBuiltInOutlines()[(size_t) choice]);
                                                 created->getEdit().getUndoManager().clearUndoHistory();

                                                 if (const auto saved = created->save(); saved.failed())
                                                     showError ("Phazite couldn't save the project", saved.getErrorMessage());
                                             }

                                             setProject (std::move (created));
                                         }),
                                     true);
        });
}

void ProjectSession::openProject()
{
    fileChooser =
        std::make_unique<juce::FileChooser> ("Open a project", getDefaultProjectsFolder(), juce::String ("*") + Project::fileExtension);

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
                            if (juce::isPositiveAndNotGreaterThan (chosen, projects.size()))
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

    recent.remove (file);
    showError ("Phazite couldn't open the project", error);

    if (project == nullptr)
        createFirstProject();
}

void ProjectSession::setProject (std::unique_ptr<Project> newProject)
{
    autosaveTimer = nullptr;
    uiBehaviour.setFocusedEdit (nullptr);
    selection.deselectAll();
    selection.edit = nullptr;
    rangeSelection.clear();

    if (project != nullptr && onProjectClosing != nullptr)
        onProjectClosing();

    project = std::move (newProject); // the previous project closes here, deleting its autosave

    if (project != nullptr)
    {
        selection.edit = &project->getEdit();
        uiBehaviour.setFocusedEdit (&project->getEdit());
        autosaveTimer = std::make_unique<AutosaveTimer> (*project);
        recent.add (project->getFile());
        routeLiveMidiInput (project->getEdit()); // MIDI keyboards and the computer keyboard play it
    }

    if (onProjectChanged != nullptr)
        onProjectChanged();
}

void ProjectSession::showError (const juce::String& title, const juce::String& message)
{
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
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project and the dialogs around it: new, open, save, the unsaved-changes question, and
// restoring an autosave after a crash.
#pragma once

#include "model/commands/CommandRegistry.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/project/NewProject.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/project/internal/UnsavedProjects.h"
#include "services/RecentProjects.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class ProjectSession : private juce::ChangeListener
{
public:
    // New projects are kept in unsavedFolder (getUnsavedProjectsFolder() in the app) until their first save asks where
    // they go, starting in projectsFolder, where the Open dialog starts too.
    ProjectSession (te::Engine&, UIBehaviour&, RecentProjects&, juce::File projectsFolder, juce::File unsavedFolder);
    ~ProjectSession() override;

    Project* getProject() const noexcept { return project.get(); }

    // Whether the open project is a new one that hasn't been saved yet: its first save asks what it is called and where
    // it goes (model/project/internal/UnsavedProjects.h).
    bool isUntitled() const;

    // The open project's Edit and what is selected in it: tracks and clips (live MIDI input follows the selected
    // track), and a time range across tracks.
    ProjectContext& getContext() noexcept { return context; }
    te::SelectionManager& getSelection() noexcept { return phazite::getSelection (context); }
    RangeSelection& getRangeSelection() noexcept { return phazite::getRangeSelection (context); }

    std::function<void()> onProjectClosing;  // the open project is about to close: let go of its Edit
    std::function<void()> onProjectChanged;  // another project is open now (or none), or it was saved
    std::function<void()> onExport;          // shows the export window (the main window's), for Export Song
    juce::Component* dialogParent = nullptr; // dialogs appear over this
    std::function<void (const juce::String& title, const juce::String& message)> onError;  // shows an error; unset, a dialog
    std::function<void (const juce::String& title, const juce::String& message)> onNotice; // says what Phazite did; unset, a dialog

    // Asks what to call the project and where it goes, from a suggested file, for its first save and Save As, and calls
    // back with the file chosen, <folder>/<name> (the project's own folder is made there), or with no file if the user
    // cancelled. Unset, a file chooser asks; tests answer themselves.
    std::function<void (const juce::File& suggested, std::function<void (const juce::File&)>)> chooseSaveFile;

    // Asks whether to restore a project's changes that Phazite saved automatically before it didn't close properly, and
    // calls back with the answer; untitled if the project was never saved. Unset, a dialog asks; tests answer themselves.
    std::function<void (const juce::File& projectFile, bool untitled, std::function<void (bool restore)>)> askToRestore;

    // After Phazite didn't close properly (a crash, perhaps in a plug-in of the project that was open), asks how to open that
    // project (docs/performance-audit.md, R2): as usual, with its plug-ins off, or not, starting a new one. Unset, a dialog
    // asks; tests answer themselves.
    enum class AfterCrash
    {
        open,
        openWithoutPlugins,
        startNew
    };

    std::function<void (const juce::File& projectFile, std::function<void (AfterCrash)>)> askAfterCrash;

    // Restores the untitled project an earlier Phazite didn't close properly, if the user wants it, or opens the most
    // recent project (offering to restore its autosave), or makes a first project; after a crash, asking how first.
    void openAtStartup (bool afterCrash = false);

    void newProject(); // the new-project window
    void openProject();
    void openProject (const juce::File&);
    void chooseRecentProject(); // a menu of the recent projects
    void saveProject();         // asks where first if the project is untitled
    void saveProjectAs();       // a copy under a new name, which then stays open; the first save if untitled
    void exportSong();          // through onExport

    // Makes an untitled project with what the settings choose, and opens it, the computer keyboard on its synth track if
    // it has one: the new-project window's Create.
    juce::Result createProject (const NewProjectSettings&);

    // The name the save dialog suggests: the project's own, or with " 2" and so on if a project has it in the folder.
    static juce::String suggestName (const juce::File& folder, const juce::String& name);

    // Where Export Song puts the song unless another file is chosen: the project's Renders folder, or, while it is
    // untitled, Exports next to the projects folder (the untitled project's own folder goes when it closes).
    juce::File getExportFolder() const;

    // Asks whether to save unsaved changes first. Calls back with false if the user cancelled.
    void closeThen (std::function<void (bool closed)>);

    juce::Array<juce::File> getRecentProjects() const { return recent.getProjects(); }

    // The "project." commands.
    void addCommands (CommandRegistry&);

private:
    te::Engine& engine;
    const juce::File projectsFolder, unsavedFolder;
    RecentProjects& recent;
    std::unique_ptr<EngineContext> engineContext;        // the Engine, as the project context takes it
    ProjectContext context;                              // keeps Tracktion's focus in step
    std::unique_ptr<UnsavedFolder> projectUnsavedFolder; // where the project was made while untitled, until it closes
    std::unique_ptr<Project> project;
    std::unique_ptr<AutosaveTimer> autosaveTimer;
    std::unique_ptr<juce::FileChooser> fileChooser;

    // The project, and the folder it was made in if it was untitled, deleted once the project has closed.
    void setProject (std::unique_ptr<Project>, std::unique_ptr<UnsavedFolder> = {});
    juce::Result createUntitled (const juce::String& name, const std::function<void (te::Edit&)>& addContent);
    void openRecentOrFirst();
    void openWithRestoreQuestion (const juce::File&);
    void open (const juce::File&, bool restoreAutosave);
    void restoreUntitled (const juce::File&, bool restore);
    void askWhetherToRestore (const juce::File&, bool untitled, std::function<void (bool restore)>);
    void askHowToOpenAfterCrash (const juce::File&, std::function<void (AfterCrash)>);
    bool pluginsOffUntilOpen = false; // the project opening opens with its plug-ins off
    void createFirstProject();
    void selectSynthTrack(); // in a new project that has one (findSynthTrack())

    // Saves the project, asking where first if it is untitled, and calls back with whether it was saved.
    void saveThen (std::function<void (bool saved)>);
    void chooseWhereToSave (const juce::File& suggested, std::function<void (const juce::File&)>);
    bool moveUntitledTo (const juce::File& chosen);
    void showError (const juce::String& title, const juce::String& message);
    void showNotice (const juce::String& title, const juce::String& message);

    // The selection changed, so live MIDI follows it; or the audio or MIDI devices changed, so live
    // MIDI goes to the project again (see routeLiveMidiInput()).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project and the dialogs around it: new, open, save, the unsaved-changes question, and
// restoring an autosave after a crash.
#pragma once

#include "model/commands/CommandRegistry.h"
#include "model/engine/EngineSetup.h"
#include "model/project/Project.h"
#include "model/project/ProjectContext.h"
#include "services/RecentProjects.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class ProjectSession : private juce::ChangeListener
{
public:
    // New projects go into projectsFolder, and the Open dialog starts there.
    ProjectSession (te::Engine&, UIBehaviour&, RecentProjects&, juce::File projectsFolder);
    ~ProjectSession() override;

    Project* getProject() const noexcept { return project.get(); }

    // The open project's Edit and what is selected in it: tracks and clips (live MIDI input follows the selected
    // track), and a time range across tracks.
    ProjectContext& getContext() noexcept { return context; }
    te::SelectionManager& getSelection() noexcept { return context.getSelection(); }
    RangeSelection& getRangeSelection() noexcept { return context.getRangeSelection(); }

    std::function<void()> onProjectClosing;  // the open project is about to close: let go of its Edit
    std::function<void()> onProjectChanged;  // another project is open now (or none), or it was saved
    std::function<void()> onExport;          // shows the export window (the main window's), for Export Song
    juce::Component* dialogParent = nullptr; // dialogs appear over this
    std::function<void (const juce::String& title, const juce::String& message)> onError; // shows an error; unset, a dialog

    // Opens the most recent project (offering to restore its autosave), or makes a first project.
    void openAtStartup();

    void newProject();
    void openProject();
    void openProject (const juce::File&);
    void chooseRecentProject(); // a menu of the recent projects
    void saveProject();
    void saveProjectAs(); // a copy under a new name, which then stays open
    void exportSong();    // through onExport

    // Asks whether to save unsaved changes first. Calls back with false if the user cancelled.
    void closeThen (std::function<void (bool closed)>);

    juce::Array<juce::File> getRecentProjects() const { return recent.getProjects(); }

    // The "project." commands.
    void addCommands (CommandRegistry&);

private:
    te::Engine& engine;
    const juce::File projectsFolder;
    RecentProjects& recent;
    ProjectContext context; // keeps Tracktion's focus in step
    std::unique_ptr<Project> project;
    std::unique_ptr<AutosaveTimer> autosaveTimer;
    std::unique_ptr<juce::FileChooser> fileChooser;

    void setProject (std::unique_ptr<Project>);
    void openWithRestoreQuestion (const juce::File&);
    void open (const juce::File&, bool restoreAutosave);
    void createFirstProject();

    // Opens the first of the recent projects after one that failed to open that this Phazite can open, or else makes a
    // first project.
    void openNextRecentProject (const juce::File& failed);
    void showError (const juce::String& title, const juce::String& message);

    // The selection changed, so live MIDI follows it; or the audio or MIDI devices changed, so live
    // MIDI goes to the project again (see routeLiveMidiInput()).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};
} // namespace phazite

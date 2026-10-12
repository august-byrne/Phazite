// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project and the dialogs around it: new, open, save, the unsaved-changes question, and
// restoring an autosave after a crash.
#pragma once

#include "model/Commands.h"
#include "model/EngineSetup.h"
#include "model/Project.h"

namespace phazite
{
class ProjectSession : private juce::ChangeListener
{
public:
    ProjectSession (te::Engine&, UIBehaviour&);
    ~ProjectSession() override;

    Project* getProject() const noexcept { return project.get(); }
    te::Edit* getEdit() const noexcept { return project != nullptr ? &project->getEdit() : nullptr; }

    // What is selected in the open project (tracks, for now). Live MIDI input follows the selected track.
    te::SelectionManager& getSelection() noexcept { return selection; }

    std::function<void()> onProjectClosing;  // the open project is about to close: let go of its Edit
    std::function<void()> onProjectChanged;  // another project is open now (or none), or it was saved
    juce::Component* dialogParent = nullptr; // dialogs appear over this

    // Opens the most recent project (offering to restore its autosave), or makes a first project.
    void openAtStartup();

    void newProject();
    void openProject();
    void openProject (const juce::File&);
    void chooseRecentProject(); // a menu of the recent projects
    void saveProject();
    void saveProjectAs(); // a copy under a new name, which then stays open

    // Asks whether to save unsaved changes first. Calls back with false if the user cancelled.
    void closeThen (std::function<void (bool closed)>);

    juce::Array<juce::File> getRecentProjects() const { return recent.getProjects(); }

    // The "project." commands.
    void addCommands (CommandRegistry&);

private:
    te::Engine& engine;
    UIBehaviour& uiBehaviour;
    RecentProjects recent;
    te::SelectionManager selection { engine };
    std::unique_ptr<Project> project;
    std::unique_ptr<AutosaveTimer> autosaveTimer;
    std::unique_ptr<juce::FileChooser> fileChooser;

    void setProject (std::unique_ptr<Project>);
    void openWithRestoreQuestion (const juce::File&);
    void open (const juce::File&, bool restoreAutosave);
    void createFirstProject();
    void showError (const juce::String& title, const juce::String& message);

    // The selection changed, so live MIDI follows it; or the audio or MIDI devices changed, so live
    // MIDI goes to the project again (see routeLiveMidiInput()).
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};
} // namespace phazite

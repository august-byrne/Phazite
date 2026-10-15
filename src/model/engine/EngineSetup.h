// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's hooks into Tracktion Engine: settings storage, engine policies and UI callbacks.
#pragma once

#include "AppInfo.h"
#include "model/base/Tracktion.h"

namespace phazite
{
//==============================================================================
// Tracktion's settings storage (Settings.xml in the app's settings folder) without personal data.
// Tracktion writes getUserName() into every saved project ("modifiedBy"); by default that is the
// operating system user's full name, which must never end up in a Phazite project.
class PropertyStorage : public te::PropertyStorage
{
public:
    explicit PropertyStorage (juce::StringRef applicationName);

    juce::String getUserName() override;           // always empty
    juce::String getApplicationVersion() override; // getVersionString()
};

//==============================================================================
class EngineBehaviour : public te::EngineBehaviour
{
public:
    struct Options
    {
        bool openAudioDevice = true;         // false in tests, which don't need one
        bool scanPluginsOutOfProcess = true; // needs the "--PluginScan" check at app startup
        int numAudioThreads = 0;             // threads that process audio; 0 = one per CPU core
    };

    explicit EngineBehaviour (Options);

    bool autoInitialiseDeviceManager() override;
    bool canScanPluginsOutOfProcess() override;
    int getNumberOfCPUsToUseForAudio() override;
    bool shouldOpenAudioInputByDefault() override;
    te::EditLimits getEditLimits() override;
    juce::File getDefaultFolderForAudioRecordings (te::Edit&) override;

private:
    Options options;
};

//==============================================================================
// Tracktion's UI callbacks that need no window. The app extends this with devices' windows (AppUIBehaviour,
// src/app/AppUIBehaviour.h) and passes it to createEngine(); without it, as in the tests, devices have no windows.
class UIBehaviour : public te::UIBehaviour
{
public:
    // The Edit of the open project. Tracktion sends MIDI controller mappings and app commands to it. Kept in step by
    // ProjectContext (model/project/ProjectContext.h); Phazite's own code reads ProjectContext::getEdit().
    void setFocusedEdit (te::Edit*);

    te::Edit* getCurrentlyFocusedEdit() override;
    te::Edit* getLastFocusedEdit() override;
    juce::Array<te::Edit*> getAllOpenEdits() override;

    // What is selected in the open project. Tracktion's own commands (such as delete and copy) act on it. Kept in step
    // by ProjectContext too.
    void setSelectionManager (te::SelectionManager*);
    te::SelectionManager* getCurrentlyFocusedSelectionManager() override;

    // Runs the task to completion on the calling thread. Tracktion calls this for blocking jobs such as
    // freezing a track or rendering to a file. Phazite doesn't enable JUCE's modal loops, so the UI
    // can't stay responsive meanwhile; Phazite's own long jobs use Tracktion's asynchronous renderers.
    void runTaskWithProgressBar (te::ThreadPoolJobWithProgress&) override;

private:
    te::SafeSelectable<te::Edit> focusedEdit;
    juce::WeakReference<te::SelectionManager> selectionManager;
};

//==============================================================================
// Every member has a default, so designated initializers can leave any of them out (GCC's
// -Wmissing-field-initializers warns about left-out members without one).
struct EngineOptions
{
    juce::String applicationName { appName }; // names the settings folder
    EngineBehaviour::Options behaviour {};
    std::unique_ptr<PropertyStorage> propertyStorage {}; // nullptr = PropertyStorage (applicationName)
    std::unique_ptr<UIBehaviour> uiBehaviour {};         // nullptr = UIBehaviour; the app passes its own
};

// Creates the Engine with Phazite's hooks. Call on the message thread.
std::unique_ptr<te::Engine> createEngine (EngineOptions);

// The UIBehaviour of an Engine that createEngine() made.
UIBehaviour& getUIBehaviour (te::Engine&);

// Loads a saved Edit on the message thread, as Phazite's rules require (Tracktion's EditLoader builds
// Edits on a background thread), upgraded to the current project format (model/base/Migrations.h). Returns nullptr if
// the file can't be loaded, or was saved by a newer Phazite.
std::unique_ptr<te::Edit> loadEdit (te::Engine&, const juce::File&);

// Creates an Edit for editing from a saved state, already upgraded, that saves to a file. It keeps the state's saved
// ID, or makes a new one.
std::unique_ptr<te::Edit> createEditFromState (te::Engine&, const juce::ValueTree& state, const juce::File& file);
} // namespace phazite

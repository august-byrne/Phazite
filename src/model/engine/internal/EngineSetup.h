// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's hooks into Tracktion Engine: settings storage, engine policies and UI callbacks.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/engine/AppInfo.h"

#include <atomic>

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

    // How much of what came in each input keeps for Tracktion's retrospective record: none, so that an input makes no
    // buffer of 30 seconds on the audio thread as it is enabled (docs/realtime-audit.md, finding 1): Phazite records and
    // captures what comes in itself (model/transport/internal/Take.h, Capture.h).
    juce::var getProperty (te::SettingID, const juce::var& defaultValue) override;

    // The plug-ins scanning found are Phazite's to keep, in its own settings (the app's services: Settings::knownPlugins),
    // not Tracktion's: Tracktion's list is neither read nor written here.
    std::unique_ptr<juce::XmlElement> getXmlProperty (te::SettingID) override;
    void setXmlProperty (te::SettingID, const juce::XmlElement&) override;
};

//==============================================================================
class EngineBehaviour : public te::EngineBehaviour
{
public:
    struct Options
    {
        bool openAudioDevice = true;         // false in tests, which don't need one
        bool scanPluginsOutOfProcess = true; // needs the "--PluginScan" check at app startup
        int numAudioThreads = 0;             // threads that process audio; 0 = one per physical core but one
    };

    explicit EngineBehaviour (Options);

    bool autoInitialiseDeviceManager() override;
    bool canScanPluginsOutOfProcess() override;
    int getNumberOfCPUsToUseForAudio() override;
    bool shouldOpenAudioInputByDefault() override;
    te::EditLimits getEditLimits() override;
    juce::File getDefaultFolderForAudioRecordings (te::Edit&) override;
    bool shouldBypassedPluginsBeRemovedFromPlaybackGraph() override;
    bool areClipSlotsEnabled() override;
    bool shouldLoadPlugin (te::ExternalPlugin&) override;
    void doAdditionalInitialisation (te::ExternalPlugin&) override;
    void newClipCreated (te::Clip&, bool fromRecording) override;

    // Low latency's state (isLowLatency()), which devices read on the audio thread; and the buffer the output had before it.
    std::atomic<bool> lowLatency { false };
    int bufferBeforeLowLatency = 0, lowLatencyBuffer = 0;

    // Whether plug-ins load as projects open (setPluginsLoad()).
    std::atomic<bool> loadsPlugins { true };

private:
    Options options;
};

static_assert (std::atomic<bool>::is_always_lock_free);

// Low latency (docs/design.md, section 20), for the session: while it is on, what is played live has no delay
// compensation. Phazite's devices that look ahead by more than lowLatencyLimitSeconds are bypassed and report no latency,
// every Edit plays without delay compensation (so a plug-in's latency delays only its own track), and the output's buffer
// shrinks to chooseLowLatencyBufferSize()'s (model/engine/AudioDevices.h) where it offers one smaller. Turning it off puts
// the buffer back, unless it was changed meanwhile. Nothing is saved: the setup saved stays the one chosen in the settings.
bool isLowLatency (te::Engine&);
void setLowLatency (te::Engine&, bool);
inline constexpr double lowLatencyLimitSeconds = 0.001;

// Opening a project with its plug-ins off (docs/performance-audit.md, R2): while this is off, the plug-ins of a project that
// opens stay as they were saved, unloaded and silent, so that one that crashes as it loads can't take Phazite down again.
void setPluginsLoad (te::Engine&, bool);

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

    // Phazite keeps what is selected itself (model/base/internal/Selection.h): Tracktion's own commands, which act on its
    // focused selection manager, find none (getCurrentlyFocusedSelectionManager() stays Tracktion's, giving nullptr).

    // Runs the task to completion on the calling thread. Tracktion calls this for blocking jobs such as
    // freezing a track or rendering to a file. Phazite doesn't enable JUCE's modal loops, so the UI
    // can't stay responsive meanwhile; Phazite's own long jobs use Tracktion's asynchronous renderers.
    void runTaskWithProgressBar (te::ThreadPoolJobWithProgress&) override;

private:
    te::SafeSelectable<te::Edit> focusedEdit;
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

// Creates an Edit for editing from the state the mirror makes it from (model/mirror/internal/Mirror.h: makeEditState()),
// on the message thread, with no track of Tracktion's own; its files' paths start from file, and cacheFolder, if given,
// becomes its temporary folder, where its frozen tracks' files are (model/mixer/internal/Freeze.h). For
// createEditFromSong() (model/project/internal/Project.h), which then builds the rest.
std::unique_ptr<te::Edit> createEditFromState (te::Engine&, const juce::ValueTree& state, const juce::File& file,
                                               const juce::File& cacheFolder = {});
} // namespace phazite

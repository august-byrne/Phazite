// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/EngineSetup.h"
#include "AudioDevices.h"
#include "internal/LoudnessMeterPlugin.h"
#include "internal/PluginScanner.h"
#include "model/automation/internal/Automation.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Freeze.h"

// ThreadSanitizer builds (linux-tsan preset): GCC defines __SANITIZE_THREAD__, Clang has a feature check.
#if defined(__SANITIZE_THREAD__)
 #define PHAZITE_TSAN 1
#elif defined(__has_feature)
 #if __has_feature(thread_sanitizer)
  #define PHAZITE_TSAN 1
 #endif
#endif
#ifndef PHAZITE_TSAN
 #define PHAZITE_TSAN 0
#endif

namespace phazite
{
//==============================================================================
PropertyStorage::PropertyStorage (juce::StringRef applicationName)
    : te::PropertyStorage (applicationName)
{
}

juce::String PropertyStorage::getUserName()
{
    return {};
}

juce::String PropertyStorage::getApplicationVersion()
{
    return getVersionString();
}

//==============================================================================
EngineBehaviour::EngineBehaviour (Options o)
    : options (o)
{
}

bool EngineBehaviour::autoInitialiseDeviceManager()
{
    return options.openAudioDevice;
}

// Scan plug-ins in a child copy of the app, so a crashing plug-in can't take Phazite down. Needs the
// startChildProcessPluginScan() check at the top of the app's initialise(). Off under ThreadSanitizer:
// Tracktion's scan coordinator shares some state between threads without locks, which would bury real
// reports.
bool EngineBehaviour::canScanPluginsOutOfProcess()
{
    return options.scanPluginsOutOfProcess && ! PHAZITE_TSAN;
}

// Tracktion processes audio on the audio callback's thread plus (this number - 1) worker threads, which it wakes every
// block and which spin before they sleep. Not one per logical CPU, as Tracktion would: the second thread of a core adds
// little to audio, and the UI and the system need a core of their own (docs/performance-audit.md, A2).
int EngineBehaviour::getNumberOfCPUsToUseForAudio()
{
    return options.numAudioThreads > 0 ? options.numAudioThreads : std::max (1, juce::SystemStats::getNumPhysicalCpus() - 1);
}

// A device switched off leaves the playback graph, which Tracktion rebuilds when one is switched on or off: otherwise it
// would keep its latency in the delay compensation, holding back every other track (and what is played live) for nothing,
// while its own track's sound passed it undelayed (docs/performance-audit.md, A8).
bool EngineBehaviour::shouldBypassedPluginsBeRemovedFromPlaybackGraph()
{
    return true;
}

//==============================================================================
namespace
{
    // createEngine() always installs a phazite::EngineBehaviour.
    EngineBehaviour& getBehaviour (te::Engine& engine)
    {
        return static_cast<EngineBehaviour&> (engine.getEngineBehaviour());
    }

    // Sets the output's buffer outside what is saved (JUCE saves setups chosen as such only), with each Edit's playback
    // made again for it, as Tracktion's own low-latency monitoring does.
    void setBufferSize (te::Engine& engine, int bufferSize)
    {
        auto& devices = engine.getDeviceManager().deviceManager;
        auto setup = devices.getAudioDeviceSetup();
        setup.bufferSize = bufferSize;

        juce::Array<te::Edit*> playing;

        for (auto* edit : engine.getActiveEdits().getEdits())
        {
            if (edit->getTransport().isPlaying())
                playing.add (edit);

            edit->getTransport().freePlaybackContext();
        }

        devices.setAudioDeviceSetup (setup, false);

        for (auto* edit : engine.getActiveEdits().getEdits())
            edit->getTransport().ensureContextAllocated();

        for (auto* edit : playing)
            edit->getTransport().play (true);
    }
} // namespace

bool isLowLatency (te::Engine& engine)
{
    return getBehaviour (engine).lowLatency.load (std::memory_order_relaxed);
}

void setLowLatency (te::Engine& engine, bool on)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto& behaviour = getBehaviour (engine);

    if (behaviour.lowLatency.exchange (on) == on)
        return;

    // Every Edit plays without delay compensation, and its devices' latencies are read again as its graph is rebuilt.
    for (auto* edit : engine.getActiveEdits().getEdits())
    {
        edit->setLatencyCompensationEnabled (! on);
        edit->restartPlayback();
    }

    auto* device = engine.getDeviceManager().deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return;

    if (on)
    {
        const auto current = device->getCurrentBufferSizeSamples();

        if (const auto smaller = chooseLowLatencyBufferSize (device->getAvailableBufferSizes()); smaller.has_value() && *smaller < current)
        {
            behaviour.bufferBeforeLowLatency = current;
            behaviour.lowLatencyBuffer = *smaller;
            setBufferSize (engine, *smaller);
        }
    }
    else if (behaviour.bufferBeforeLowLatency > 0)
    {
        // Back as it was, unless the settings changed it meanwhile.
        if (device->getCurrentBufferSizeSamples() == behaviour.lowLatencyBuffer)
            setBufferSize (engine, behaviour.bufferBeforeLowLatency);

        behaviour.bufferBeforeLowLatency = behaviour.lowLatencyBuffer = 0;
    }
}

bool EngineBehaviour::shouldLoadPlugin (te::ExternalPlugin&)
{
    return loadsPlugins.load();
}

void setPluginsLoad (te::Engine& engine, bool load)
{
    getBehaviour (engine).loadsPlugins = load;
}

// Phazite has no clip launcher (until after v1: docs/design.md, section 26), so no track needs the nodes Tracktion adds
// to every one for its clip slots (docs/performance-audit.md, A3).
bool EngineBehaviour::areClipSlotsEnabled()
{
    return false;
}

// Don't open the microphone until the user asks for an input.
bool EngineBehaviour::shouldOpenAudioInputByDefault()
{
    return false;
}

// Tracktion's defaults (16 devices per track, 1500 clips per track, 4 master devices) are too tight for
// producers who place one-shots straight onto tracks and stack effects.
te::EditLimits EngineBehaviour::getEditLimits()
{
    te::EditLimits limits;
    limits.maxNumTracks = 1000;
    limits.maxClipsInTrack = 5000;
    limits.maxPluginsOnTrack = 64;
    limits.maxNumMasterPlugins = 16;
    return limits;
}

// Recordings go into the project's Audio folder. Edits without a file (none yet in Phazite) fall back
// to Documents/Phazite/Recordings, never to the working directory.
juce::File EngineBehaviour::getDefaultFolderForAudioRecordings (te::Edit& edit)
{
    if (auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File(); editFile != juce::File())
        return editFile.getSiblingFile ("Audio");

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Recordings");
}

//==============================================================================
void UIBehaviour::setFocusedEdit (te::Edit* edit)
{
    focusedEdit = edit;
}

te::Edit* UIBehaviour::getCurrentlyFocusedEdit()
{
    return focusedEdit.get();
}

te::Edit* UIBehaviour::getLastFocusedEdit()
{
    return focusedEdit.get();
}

juce::Array<te::Edit*> UIBehaviour::getAllOpenEdits()
{
    juce::Array<te::Edit*> edits;

    if (auto* edit = focusedEdit.get())
        edits.add (edit);

    return edits;
}

void UIBehaviour::setSelectionManager (te::SelectionManager* manager)
{
    selectionManager = manager;
}

te::SelectionManager* UIBehaviour::getCurrentlyFocusedSelectionManager()
{
    return selectionManager.get();
}

void UIBehaviour::runTaskWithProgressBar (te::ThreadPoolJobWithProgress& task)
{
    while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
    {
    }
}

//==============================================================================
std::unique_ptr<te::Engine> createEngine (EngineOptions o)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto storage = o.propertyStorage != nullptr ? std::move (o.propertyStorage) : std::make_unique<PropertyStorage> (o.applicationName);
    auto ui = o.uiBehaviour != nullptr ? std::move (o.uiBehaviour) : std::make_unique<UIBehaviour>();
    const bool openDevices = o.behaviour.openAudioDevice;
    const bool firstRun = openDevices && storage->getXmlProperty (te::SettingID::audio_device_setup) == nullptr;

    auto engine = std::make_unique<te::Engine> (std::move (storage), std::move (ui), std::make_unique<EngineBehaviour> (o.behaviour));

    // Plug-ins scanned in a child copy, with a time limit for each.
    if (engine->getPluginManager().usesSeparateProcessForScanning())
        usePluginScannerWithTimeLimit (*engine);

    // Phazite's own plug-in types, which projects can hold.
    engine->getPluginManager().createBuiltInType<LoudnessMeterPlugin>();
    registerDevices (*engine);

    // Tracktion's DeviceManager::initialise() asks for a MIDI device scan in 5 ms, but then restarts the
    // same timer with the 4-second hot-plug check, so MIDI inputs (and the computer keyboard, which
    // plays into "All MIDI Ins") would only work 4 seconds after startup. Ask again, once it is done.
    if (openDevices)
        engine->getDeviceManager().rescanMidiDeviceList();

    // With no audio setup saved yet, Tracktion opened the default output as Windows shares it; Low Latency Mode instead
    // where it offers a small buffer (docs/design.md, section 20).
    if (firstRun)
        chooseLowLatencySetup (engine->getDeviceManager().deviceManager);

    return engine;
}

UIBehaviour& getUIBehaviour (te::Engine& engine)
{
    // createEngine() always installs a phazite::UIBehaviour.
    return static_cast<UIBehaviour&> (engine.getUIBehaviour());
}

std::unique_ptr<te::Edit> loadEdit (te::Engine& engine, const juce::File& file)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! file.existsAsFile())
        return {};

    return createEditFromState (engine, te::loadEditFromFile (engine, file, te::ProjectItemID {}), file);
}

std::unique_ptr<te::Edit> createEditFromState (te::Engine& engine, const juce::ValueTree& state, const juce::File& file,
                                               const juce::File& cacheFolder)
{
    if (! state.isValid())
        return {};

    auto id = te::ProjectItemID::fromProperty (state, te::IDs::projectID);

    if (! id.isValid())
        id = te::ProjectItemID::createNewID (te::ProjectID {});

    // The values set by hand of parameters that automation set to others as the project was saved, and only the rack
    // types its containers play (model/mixer/internal/Containers.h); and the frozen tracks, which Tracktion would unfreeze
    // as it makes the Edit, not knowing about the Cache folder yet (model/mixer/internal/Freeze.h).
    auto editState = state;
    restoreValuesSetByHand (editState);
    dropUnusedContainerRacks (editState);
    const auto frozenTracks = takeFrozenTracks (editState, cacheFolder);

    auto edit = te::Edit::createEdit (
        { engine, state, id, te::Edit::forEditing, nullptr, te::Edit::getDefaultNumUndoLevels(), [file] { return file; }, {} });

    // While low latency is on, without delay compensation (setLowLatency()).
    if (edit != nullptr)
        edit->setLatencyCompensationEnabled (! isLowLatency (engine));

    if (edit != nullptr && cacheFolder != juce::File())
    {
        edit->setTempDirectory (cacheFolder);
        restoreFrozenTracks (*edit, frozenTracks);
    }

    return edit;
}
} // namespace phazite

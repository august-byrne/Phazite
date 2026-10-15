// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineSetup.h"
#include "LoudnessMeterPlugin.h"
#include "model/base/Migrations.h"
#include "model/devices/Devices.h"

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

// Tracktion processes audio on the audio callback's thread plus (this number - 1) worker threads.
int EngineBehaviour::getNumberOfCPUsToUseForAudio()
{
    return options.numAudioThreads > 0 ? options.numAudioThreads : te::EngineBehaviour::getNumberOfCPUsToUseForAudio();
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

    auto engine = std::make_unique<te::Engine> (std::move (storage), std::move (ui), std::make_unique<EngineBehaviour> (o.behaviour));

    // Phazite's own plug-in types, which projects can hold.
    engine->getPluginManager().createBuiltInType<LoudnessMeterPlugin>();
    registerDevices (*engine);

    // Tracktion's DeviceManager::initialise() asks for a MIDI device scan in 5 ms, but then restarts the
    // same timer with the 4-second hot-plug check, so MIDI inputs (and the computer keyboard, which
    // plays into "All MIDI Ins") would only work 4 seconds after startup. Ask again, once it is done.
    if (openDevices)
        engine->getDeviceManager().rescanMidiDeviceList();

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

    auto state = te::loadEditFromFile (engine, file, te::ProjectItemID {});

    if (! state.isValid() || ! migrateToCurrentSchema (state))
        return {};

    return createEditFromState (engine, state, file);
}

std::unique_ptr<te::Edit> createEditFromState (te::Engine& engine, const juce::ValueTree& state, const juce::File& file)
{
    if (! state.isValid())
        return {};

    auto id = te::ProjectItemID::fromProperty (state, te::IDs::projectID);

    if (! id.isValid())
        id = te::ProjectItemID::createNewID (te::ProjectID {});

    return te::Edit::createEdit (
        { engine, state, id, te::Edit::forEditing, nullptr, te::Edit::getDefaultNumUndoLevels(), [file] { return file; }, {} });
}
} // namespace phazite

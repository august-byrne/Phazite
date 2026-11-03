// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/EngineSetup.h"
#include "AudioDevices.h"
#include "internal/AudioIO.h"
#include "internal/LoudnessMeterPlugin.h"
#include "internal/PluginScanner.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/PluginParameters.h"

namespace phazite
{
namespace
{
    // Tracktion's list of the plug-ins scanning found, which Phazite keeps in its own settings.
    bool isPluginList (te::SettingID setting)
    {
        return setting == te::SettingID::knownPluginList || setting == te::SettingID::knownPluginList64;
    }
} // namespace

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

juce::var PropertyStorage::getProperty (te::SettingID setting, const juce::var& defaultValue)
{
    if (setting == te::SettingID::retrospectiveRecord)
        return 0.0;

    return te::PropertyStorage::getProperty (setting, defaultValue);
}

std::unique_ptr<juce::XmlElement> PropertyStorage::getXmlProperty (te::SettingID setting)
{
    return isPluginList (setting) ? nullptr : te::PropertyStorage::getXmlProperty (setting);
}

void PropertyStorage::setXmlProperty (te::SettingID setting, const juce::XmlElement& xml)
{
    if (! isPluginList (setting))
        te::PropertyStorage::setXmlProperty (setting, xml);
}

//==============================================================================
EngineBehaviour::EngineBehaviour (Options o)
    : options (o)
{
}

// Tracktion's device manager opens no device and no MIDI input: Phazite opens them (internal/AudioIO.h;
// docs/tracktion-migration.md, T4).
bool EngineBehaviour::autoInitialiseDeviceManager()
{
    return false;
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

    // Sets the output's buffer outside what is saved (JUCE saves setups chosen as such only): Phazite's engine is prepared
    // again for it as the device starts again.
    void setBufferSize (juce::AudioDeviceManager& devices, int bufferSize)
    {
        auto setup = devices.getAudioDeviceSetup();
        setup.bufferSize = bufferSize;
        devices.setAudioDeviceSetup (setup, false);
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

    auto* audio = AudioIO::findFor (engine);
    auto* device = audio != nullptr ? audio->getDevices().getCurrentAudioDevice() : nullptr;

    if (device == nullptr)
        return;

    if (on)
    {
        const auto current = device->getCurrentBufferSizeSamples();

        if (const auto smaller = chooseLowLatencyBufferSize (device->getAvailableBufferSizes()); smaller.has_value() && *smaller < current)
        {
            behaviour.bufferBeforeLowLatency = current;
            behaviour.lowLatencyBuffer = *smaller;
            setBufferSize (audio->getDevices(), *smaller);
        }
    }
    else if (behaviour.bufferBeforeLowLatency > 0)
    {
        // Back as it was, unless the settings changed it meanwhile.
        if (device->getCurrentBufferSizeSamples() == behaviour.lowLatencyBuffer)
            setBufferSize (audio->getDevices(), behaviour.bufferBeforeLowLatency);

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

// Called as soon as a plug-in's instance is made, before Tracktion makes its parameters: what the project names them by
// follows them through an update of the plug-in that moved them (model/devices/internal/PluginParameters.h).
void EngineBehaviour::doAdditionalInitialisation (te::ExternalPlugin& plugin)
{
    findMovedParameters (plugin);
}

// Phazite stretches and pitches audio clips itself (model/arrangement/internal/AudioClips.h; roadmap step 2.18), and
// Tracktion has no stretcher compiled in, so a new clip of a file with loop data plays the file as it is: Tracktion's own
// defaults for one (its auto-tempo, with the clip stretched to the loop's beats, and auto-pitch toward the song's key) are
// undone here, and the clip lasts as long as its file. The model makes it follow the tempo, as importFile() does a loop.
void EngineBehaviour::newClipCreated (te::Clip& clip, bool)
{
    auto* audio = dynamic_cast<te::AudioClipBase*> (&clip);

    if (audio == nullptr || ! (audio->getAutoTempo() || audio->getAutoPitch()))
        return;

    audio->setAutoTempo (false);
    audio->setAutoPitch (false);

    if (const auto length = te::AudioFile (clip.edit.engine, audio->getOriginalFile()).getLength(); length > 0.0)
        audio->setLength (te::TimeDuration::fromSeconds (length), true);
}

// Phazite has no clip launcher (until after v1: docs/design.md, section 26), so no track needs the nodes Tracktion adds
// to every one for its clip slots (docs/performance-audit.md, A3).
bool EngineBehaviour::areClipSlotsEnabled()
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
    auto engine = std::make_unique<te::Engine> (std::move (storage), std::move (ui), std::make_unique<EngineBehaviour> (o.behaviour));

    // Plug-ins scanned in Phazite's helper program, with a time limit for each.
    usePluginScannerWithTimeLimit (*engine);

    // Phazite's own plug-in types, which projects can hold.
    engine->getPluginManager().createBuiltInType<LoudnessMeterPlugin>();
    registerDevices (*engine);
    return engine;
}

UIBehaviour& getUIBehaviour (te::Engine& engine)
{
    // createEngine() always installs a phazite::UIBehaviour.
    return static_cast<UIBehaviour&> (engine.getUIBehaviour());
}

std::unique_ptr<te::Edit> createEditFromState (te::Engine& engine, const juce::ValueTree& state, const juce::File& file,
                                               const juce::File& cacheFolder)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! state.isValid())
        return {};

    // An ID of its own each time (the project's is Phazite's: the Cache folder below is its temporary folder).
    const auto id = te::ProjectItemID::createNewID (te::ProjectID {});
    te::Edit::Options options { engine, state, id };
    options.editFileRetriever = [file] { return file; };
    options.numAudioTracks = 0;
    auto edit = te::Edit::createEdit (std::move (options));

    // While low latency is on, without delay compensation (setLowLatency()).
    if (edit != nullptr)
        edit->setLatencyCompensationEnabled (! isLowLatency (engine));

    if (edit != nullptr && cacheFolder != juce::File())
        edit->setTempDirectory (cacheFolder);

    return edit;
}
} // namespace phazite

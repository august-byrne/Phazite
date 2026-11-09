// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppServices.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/Preview.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/versions/VersionsModel.h"

namespace phazite
{
struct AppServices::PluginLists final : private juce::ChangeListener
{
    PluginLists (PluginHost& h, Settings& s)
        : host (h),
          known (h.getKnownPlugins()),
          settings (s)
    {
        if (const auto xml = juce::parseXML (settings.get (Settings::knownPlugins)))
            known.recreateFromXml (*xml);

        host.setCrashedWhileLoading (settings.get (Settings::pluginsCrashedLoading));
        known.addChangeListener (this);
        host.addChangeListener (this);
    }

    ~PluginLists() override
    {
        known.removeChangeListener (this);
        host.removeChangeListener (this);
    }

    PluginHost& host;
    juce::KnownPluginList& known;
    Settings& settings;

    void changeListenerCallback (juce::ChangeBroadcaster* changed) override
    {
        if (changed == &known)
        {
            if (const auto xml = known.createXml())
                settings.set (Settings::knownPlugins, xml->toString (juce::XmlElement::TextFormat().withoutHeader().singleLine()));
        }
        else if (const auto crashed = host.getCrashedWhileLoading(); crashed != settings.get (Settings::pluginsCrashedLoading))
        {
            settings.set (Settings::pluginsCrashedLoading, crashed);
        }
    }
};

struct AppServices::DeviceSettings final : private juce::ChangeListener
{
    DeviceSettings (AudioIO& a, Settings& s)
        : audio (a),
          settings (s)
    {
        AudioIO::Saved saved;
        saved.audioSetup = juce::parseXML (settings.get (Settings::audioSetup));
        saved.midiInputsOff = settings.get (Settings::midiInputsOff);
        saved.overdub = settings.get (Settings::recordOverdub);
        saved.recordQuantize = settings.get (Settings::recordQuantize);
        saved.countIn = settings.get (Settings::recordCountIn);
        saved.recordingCorrection = settings.get (Settings::recordingCorrection);
        audio.open (saved);
        save(); // a first run's choice
        audio.addChangeListener (this);
    }

    ~DeviceSettings() override { audio.removeChangeListener (this); }

    AudioIO& audio;
    Settings& settings;

    void save()
    {
        const auto saved = audio.getSaved();
        settings.set (Settings::audioSetup, saved.audioSetup != nullptr
                                                ? saved.audioSetup->toString (juce::XmlElement::TextFormat().singleLine())
                                                : juce::String());
        settings.set (Settings::midiInputsOff, saved.midiInputsOff);
        settings.set (Settings::recordOverdub, saved.overdub);
        settings.set (Settings::recordQuantize, saved.recordQuantize);
        settings.set (Settings::recordCountIn, saved.countIn);
        settings.set (Settings::recordingCorrection, saved.recordingCorrection);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { save(); }
};

AppServices::AppServices (EngineContext& context, AppServicesOptions options)
    : engineContext (context),
      settingsFile (options.settingsInMemory ? nullptr : std::make_unique<juce::PropertiesFile> (Settings::getFileOptions())),
      settings (settingsFile.get()),
      recentProjects (settings),
      soundPack (options.packsFolder, options.soundPackRelease),
      updates (settings, options.updatesFolder, options.updateSource),
      library (engineContext, settings, soundPack.getFolder(), options.presetsFolder, options.factoryPresetsFolder,
               options.settingsInMemory ? juce::File() : Settings::getFolder().getChildFile ("Library facts.txt")),
      previewer (createPreviewer (engineContext))
{
    // Plug-ins in helper processes: the scan's list, and those that crashed as they loaded kept off.
    pluginLists = std::make_unique<PluginLists> (getPluginHost (engineContext), settings);

    // This computer, as projects' histories know it: an ID made once and kept, before any project opens.
    auto computer = settings.get (Settings::computer);

    if (computer.isEmpty())
    {
        computer = juce::Uuid().toString();
        settings.set (Settings::computer, computer);
    }

    setThisComputer (computer);
}

void AppServices::openAudioDevices()
{
    if (deviceSettings == nullptr)
        deviceSettings = std::make_unique<DeviceSettings> (getAudioIO (engineContext), settings);
}

AppServices::~AppServices() = default;
} // namespace phazite

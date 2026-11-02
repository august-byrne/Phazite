// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppServices.h"
#include "SandboxedEditor.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/Preview.h"
#include "model/plugins/internal/PluginHost.h"

namespace phazite
{
struct AppServices::PluginLists final : private juce::ChangeListener
{
    PluginLists (juce::KnownPluginList& list, PluginHost& h, Settings& s)
        : known (list),
          host (h),
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

    juce::KnownPluginList& known;
    PluginHost& host;
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

AppServices::AppServices (te::Engine& engine, AppServicesOptions options)
    : engineContext (createEngineContext (engine)),
      settings (options.settingsInMemory ? nullptr : &engine.getPropertyStorage().getPropertiesFile()),
      recentProjects (settings),
      soundPack (options.packsFolder, options.soundPackRelease),
      library (*engineContext, settings, soundPack.getFolder(), options.presetsFolder, options.factoryPresetsFolder),
      previewer (createPreviewer (engine))
{
    // Plug-ins in helper processes (docs/tracktion-migration.md, T3), with their editors in the app's windows; the scan's
    // list, and those that crashed as they loaded kept off.
    useSandboxedEditors (getPluginHost (*engineContext));
    pluginLists = std::make_unique<PluginLists> (engine.getPluginManager().knownPluginList, getPluginHost (*engineContext), settings);
}

AppServices::~AppServices() = default;
} // namespace phazite

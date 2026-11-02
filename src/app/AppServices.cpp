// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppServices.h"
#include "SandboxedEditor.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/Preview.h"
#include "model/plugins/internal/PluginHost.h"

namespace phazite
{
struct AppServices::CrashedPlugins final : private juce::ChangeListener
{
    CrashedPlugins (PluginHost& h, Settings& s)
        : host (h),
          settings (s)
    {
        host.setCrashedWhileLoading (settings.get (Settings::pluginsCrashedLoading));
        host.addChangeListener (this);
    }

    ~CrashedPlugins() override { host.removeChangeListener (this); }

    PluginHost& host;
    Settings& settings;

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (const auto crashed = host.getCrashedWhileLoading(); crashed != settings.get (Settings::pluginsCrashedLoading))
            settings.set (Settings::pluginsCrashedLoading, crashed);
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
    // Plug-ins in helper processes, while T3 lasts a setting (docs/tracktion-migration.md), with their editors in the
    // app's windows, and those that crashed as they loaded kept off.
    engineContext->setPluginsSandboxed (settings.get (Settings::sandboxPlugins));
    useSandboxedEditors (getPluginHost (*engineContext));
    crashedPlugins = std::make_unique<CrashedPlugins> (getPluginHost (*engineContext), settings);
}

AppServices::~AppServices() = default;
} // namespace phazite

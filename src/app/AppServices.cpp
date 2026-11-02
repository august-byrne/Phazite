// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppServices.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/Preview.h"

namespace phazite
{
AppServices::AppServices (te::Engine& engine, AppServicesOptions options)
    : engineContext (createEngineContext (engine)),
      settings (options.settingsInMemory ? nullptr : &engine.getPropertyStorage().getPropertiesFile()),
      recentProjects (settings),
      soundPack (options.packsFolder, options.soundPackRelease),
      library (*engineContext, settings, soundPack.getFolder(), options.presetsFolder, options.factoryPresetsFolder),
      previewer (createPreviewer (engine))
{
    // Plug-ins in helper processes, while T3 lasts a setting (docs/tracktion-migration.md).
    engineContext->setPluginsSandboxed (settings.get (Settings::sandboxPlugins));
}
} // namespace phazite

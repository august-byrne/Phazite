// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppServices.h"
#include "model/engine/internal/EngineContext.h"

namespace phazite
{
AppServices::AppServices (te::Engine& engine, AppServicesOptions options)
    : engineContext (createEngineContext (engine)),
      settings (options.settingsInMemory ? nullptr : &engine.getPropertyStorage().getPropertiesFile()),
      recentProjects (settings),
      soundPack (options.packsFolder, options.soundPackRelease),
      library (engine, settings, soundPack.getFolder()),
      previewer (engine)
{
}
} // namespace phazite

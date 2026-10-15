// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppServices.h"

namespace phazite
{
AppServices::AppServices (te::Engine& engine, AppServicesOptions options)
    : settings (options.settingsInMemory ? nullptr : &engine.getPropertyStorage().getPropertiesFile()),
      recentProjects (settings)
{
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// Making the EngineContext (model/engine/EngineContext.h) of an Engine, for the app's services and tests.
#pragma once

#include "model/base/Tracktion.h"

#include <memory>

namespace phazite
{
class EngineContext;

std::unique_ptr<EngineContext> createEngineContext (te::Engine&);
} // namespace phazite

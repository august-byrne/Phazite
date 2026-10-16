// SPDX-License-Identifier: AGPL-3.0-or-later
// The previewer's Tracktion side (model/engine/Preview.h), for the app and tests: one made for an Engine. Not for the UI.
#pragma once

#include "model/base/Tracktion.h"

#include <memory>

namespace phazite
{
class Previewer;

// Its preview Edit goes when it does, which must be before the Engine.
std::unique_ptr<Previewer> createPreviewer (te::Engine&);
} // namespace phazite

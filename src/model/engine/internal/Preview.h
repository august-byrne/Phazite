// SPDX-License-Identifier: AGPL-3.0-or-later
// The previewer (model/engine/Preview.h) for the app and tests: one made for an engine context, whose previews' engine
// plays it (model/playback/internal/PlaybackHost.h). Not for the UI.
#pragma once

#include <memory>

namespace phazite
{
class EngineContext;
class Previewer;

// What it plays goes before it does, which must be before the engine context.
std::unique_ptr<Previewer> createPreviewer (EngineContext&);
} // namespace phazite

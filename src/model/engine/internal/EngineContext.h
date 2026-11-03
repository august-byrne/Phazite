// SPDX-License-Identifier: AGPL-3.0-or-later
// Making the EngineContext (model/engine/EngineContext.h) of an Engine, for the app's services and tests.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <memory>

namespace phazite
{
class AudioIO;
class EngineContext;

std::unique_ptr<EngineContext> createEngineContext (te::Engine&);
te::Engine& getEngine (EngineContext&); // the one it was made for

// Its audio device and MIDI inputs (AudioIO.h), which the app opens as its settings saved them and keeps there.
AudioIO& getAudioIO (EngineContext&);
} // namespace phazite

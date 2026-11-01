// SPDX-License-Identifier: AGPL-3.0-or-later
// Example songs' Tracktion side (model/project/ExampleSongs.h), for the app and tests: what Project::create() adds to a new
// project's Edit for one. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <functional>

namespace phazite
{
class EngineContext;

// Project::create()'s content function for an example song (ExampleSongs.h: addExampleSong()), as the app opens one: it
// takes away the track Tracktion makes with the Edit and makes the song through a project context of its own on the
// Edit, before the first save. The engine context must outlive the function.
std::function<void (te::Edit&)> getExampleSongContent (EngineContext&, const juce::String& id, const juce::File& factoryPresets);
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// What Project::create() adds to a new project's song for an example song (model/project/ExampleSongs.h), for the app and
// tests. Not for the UI.
#pragma once

#include "model/base/Song.h"

#include <functional>

namespace phazite
{
class EngineContext;

// Project::create()'s content function for an example song (ExampleSongs.h: addExampleSong()), as the app opens one: it
// takes away the track a new song has and makes the song through a project context of its own, before the first save.
// The engine context must outlive the function.
std::function<void (Song&)> getExampleSongContent (EngineContext&, const juce::String& id, const juce::File& factoryPresets);
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// Making the EngineContext (model/engine/EngineContext.h), for the app and tests, and what it holds: the audio device and
// MIDI inputs Phazite opens (AudioIO.h), the plug-ins hosted in helper processes (model/plugins/internal/PluginHost.h) and
// Phazite's engine on the device, with the previews' beside it (model/playback/internal/PlaybackHost.h). The app makes one
// as it starts (src/app/Main.cpp), which outlives every project context; the tests make one for all of them. Message
// thread.
#pragma once

#include <memory>

namespace phazite
{
class AudioIO;
class EngineContext;
class PlaybackHost;
class PluginHost;

struct EngineOptions
{
    int numAudioThreads = 0; // threads that play audio, the audio device's among them; 0 = one per physical core but one
};

std::unique_ptr<EngineContext> createEngineContext (EngineOptions = {});

AudioIO& getAudioIO (EngineContext&);
PlaybackHost& getPlaybackHost (EngineContext&);
PluginHost& getPluginHost (EngineContext&);
} // namespace phazite

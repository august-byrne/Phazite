// SPDX-License-Identifier: AGPL-3.0-or-later
// Export's insides (model/project/Export.h), for the app and tests: the time an export covers, whether the song can be
// exported, the channels its stems are, and starting one. Not for the UI.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
struct ExportReport;
struct ExportSettings;
class PluginHost;
class SongExport;

// The song's time that an export covers: from its start to its end (model/transport/internal/Playback.h: getSongEnd()).
BeatRange getExportRange (const Song&);

// Whether the song can be exported: it has clips, and isn't recording.
bool canExport (const Song&);

// The channels that an export of stems writes a file for, by their tracks' IDs, in the mixer's order: each track outside a
// bus, each bus with the tracks in it, and each return, that the song plays (not muted, nor left silent by a solo) and that
// has something to play (clips, tracks with clips in a bus, a send to a return). A plain folder has no sound of its own:
// its tracks are stems.
std::vector<TrackID> getStemChannels (const Song&);

// A stem's file name: its number in that order and its channel's name, with the format's suffix (Export.h:
// getFileSuffix()): "01 Kick.wav".
juce::String getStemFileName (int index, const juce::String& channelName, const juce::String& suffix);

// Starts rendering the song to a file, or with settings.stems its stems into a folder (Export.h's SongExport says how),
// through Phazite's engine with the plug-ins of the song's session (model/playback/internal/PlaybackSession.h), which it
// lends them while it renders. Make it on the message thread; the song must outlive it.
std::unique_ptr<SongExport> startExport (Song&, const juce::File&, const ExportSettings&,
                                         std::function<void (const ExportReport&)> onFinished);

// The same of a song that no project's session plays (an old version's song: model/versions/), into a file, with plug-in
// instances of its own in the plug-in host, made as it starts, in the background: at a low priority, on one thread, while
// the open song plays on.
std::unique_ptr<SongExport> startBackgroundExport (Song&, PluginHost&, const juce::File&, const ExportSettings&,
                                                   std::function<void (const ExportReport&)> onFinished);
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// Export's Tracktion side (model/project/Export.h), for the app and tests: the time an export covers, whether the song
// can be exported, the channels its stems are, and starting one. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <functional>
#include <memory>

namespace phazite
{
struct ExportReport;
struct ExportSettings;
class SongExport;

// The song's time that an export covers: from its start to its end (Playback.h: getSongEnd()).
te::TimeRange getExportRange (const te::Edit&);

// Whether the song can be exported: it has clips, and isn't recording.
bool canExport (const te::Edit&);

// The channels that an export of stems writes a file for, in the mixer's order: each track outside a bus, each bus with
// the tracks in it, and each return, that the song plays (not muted, nor left silent by a solo) and that has something to
// play (clips, tracks with clips in a bus, a send to a return). A plain folder has no sound of its own: its tracks are
// stems.
juce::Array<te::Track*> getStemTracks (const te::Edit&);

// A stem's file name: its number in that order and its channel's name, with the format's suffix (Export.h:
// getFileSuffix()): "01 Kick.wav".
juce::String getStemFileName (int index, const te::Track&, const juce::String& suffix);

// Starts rendering the song to a file, or with settings.stems its stems into a folder (Export.h's SongExport says how).
// Make it on the message thread; the Edit must outlive it.
std::unique_ptr<SongExport> startExport (te::Edit&, const juce::File&, const ExportSettings&,
                                         std::function<void (const ExportReport&)> onFinished);
} // namespace phazite

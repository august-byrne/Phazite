// SPDX-License-Identifier: AGPL-3.0-or-later
// Export's Tracktion side (model/project/Export.h), for the app and tests: the time an export covers, whether the song
// can be exported, and starting one. Not for the UI.
#pragma once

#include "model/base/Tracktion.h"

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

// Starts rendering the song to a file (Export.h's SongExport says how). Make it on the message thread; the Edit must
// outlive it.
std::unique_ptr<SongExport> startExport (te::Edit&, const juce::File&, const ExportSettings&,
                                         std::function<void (const ExportReport&)> onFinished);
} // namespace phazite

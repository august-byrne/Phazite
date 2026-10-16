// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's waveforms (ui/playlist/ClipWaveform.h), drawn by Tracktion's thumbnails of what audio clips play: the
// app's, as they need both Tracktion and a component to repaint as they load. MainWindow gives the playlist the maker.
#pragma once

#include "ui/playlist/ClipWaveform.h"

namespace phazite
{
ui::ClipWaveformMaker getClipWaveformMaker();
} // namespace phazite

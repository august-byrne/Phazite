// SPDX-License-Identifier: AGPL-3.0-or-later
// The waveforms of the playlist's clips and the audio clip editor (ui/playlist/ClipWaveform.h), drawn by Tracktion's
// thumbnails of the files audio clips play: the app's, as they need both Tracktion and a component to repaint as they
// load. MainWindow gives the views the maker.
#pragma once

#include "ui/playlist/ClipWaveform.h"

namespace phazite
{
ui::ClipWaveformMaker getClipWaveformMaker();
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// The waveforms of the playlist's clips and the audio clip editor (ui/playlist/ClipWaveform.h), drawn from Phazite's own
// cache of the levels of the files audio clips play (JUCE's thumbnails: roadmap A5, 2026-10-30, in place of Tracktion's),
// read in the background and kept while waveforms show, one for each file however many clips play it. The app's, as they need
// the open Edit's clips and a component to repaint as they load. MainWindow gives the views the maker.
#pragma once

#include "ui/playlist/ClipWaveform.h"

namespace phazite
{
ui::ClipWaveformMaker getClipWaveformMaker();
} // namespace phazite

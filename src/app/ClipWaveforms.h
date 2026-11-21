// SPDX-License-Identifier: AGPL-3.0-or-later
// The waveforms of the playlist's clips and the audio clip editor (ui/playlist/ClipWaveform.h), drawn from Phazite's own
// cache of the levels of the files audio clips play (JUCE's thumbnails), read in the background and kept while waveforms
// show, one for each file however many clips play it. The app's, as they need the open song's clips and a component to
// repaint as they load. MainWindow gives the views the maker.
#pragma once

#include "ui/playlist/ClipWaveform.h"

#include <juce_core/juce_core.h>

#include <functional>

namespace phazite
{
ui::ClipWaveformMaker getClipWaveformMaker();

// Tests: what the thread that opens a clip's file for its waveform does before it opens it, given the file (held there
// until let go, as a drive that is slow to wake), or nothing. Any thread.
void setBeforeWaveformFileOpens (std::function<void (const juce::File&)>);

// Tests: how many times the waveforms opened a file to read it (each a reader made), how many of those were on the message
// thread, and how many of the readers are alive now. Any thread.
struct WaveformFiles
{
    int opens = 0, opensOnMessageThread = 0, readersAlive = 0;
};

WaveformFiles countWaveformFiles();
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// A busy song of any number of tracks, for what measures the engine's and the model's cost as a song grows: the 100-track
// benchmark (Benchmarks.cpp), the speed gates (BenchmarkGates.cpp, PerformanceGateTests.cpp) and the long run
// (LongRun.cpp). Built through the model as the UI builds a song (TestSong.h), its notes through TestClips.h for speed.
// Every track plays all the way through, so that none sleeps:
// - an instrument, in turn a Synth, an FM synth, a Drum Sampler and a Sampler, each playing a part that
//   suits it (chords on every beat, a bass line in eighths, a beat with sixteenth hats);
// - an EQ and a compressor after it; every third track a saturator too, and every fifth a chorus;
// - a send to a reverb on a return from every track, and to a delay on another from every other track;
// - its tracks in buses of up to ten, each bus with a compressor of its own;
// - the volume of every fourth track automated (a fade down and up over each 16 beats), and the cutoff of every Synth.
#pragma once

#include "TestSong.h"

#include <vector>

namespace phazite::test
{
struct BenchmarkSong
{
    std::vector<TrackID> tracks;       // the tracks that play, in their order
    std::vector<TrackID> buses;        // a bus for each ten
    std::vector<DeviceID> instruments; // the tracks', in their order
    std::vector<ClipID> clips;         // a clip on each track, the length of the song
    TrackID reverbReturn, delayReturn;
    double lengthBeats = 0.0;
};

// Builds it into a test song (whose first track becomes the first of them), at 120 BPM, its clips as long as asked, and
// clears its undo history.
BenchmarkSong buildBenchmarkSong (TestSong&, int numTracks, double lengthBeats = 64.0);

// Plays so many blocks of what Phazite's engine plays now (the open project, on a device an EnginePlayback opened, at its
// block size) one after another on the calling thread, as the audio device's thread would, and returns how long each
// took, in seconds.
std::vector<double> timeEngineBlocks (int count, int blockSize);

// A percentile of times (0.5 for the median, 1 for the most), and their mean.
double percentileOf (std::vector<double> times, double fraction);
double meanOf (const std::vector<double>& times);
} // namespace phazite::test

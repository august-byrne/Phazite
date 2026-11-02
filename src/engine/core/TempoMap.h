// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo map as the engine plays it (docs/tracktion-migration.md, T2): where each of the song's beats falls in
// samples, at a sample rate. The song keeps it in beats (model/transport/internal/Tempo.h): each tempo and time signature
// from a beat on, the first at the song's start. A beat is one of the time signature's own, a quarter note in 4/4 and an
// eighth in 6/8, and a tempo counts quarter notes in a minute, as Tracktion's tempo sequence counts them, which the song
// has played through so far.
#pragma once

#include <vector>

namespace phazite::engine
{
class TempoMap
{
public:
    struct Tempo
    {
        double beat = 0.0;
        double bpm = 120.0;
    };

    struct TimeSignature
    {
        double beat = 0.0;
        int numerator = 4, denominator = 4;
    };

    TempoMap(); // 120 BPM in 4/4, at 44.1 kHz

    // Each in the song's order, the first at its start (one is made there if there is none).
    TempoMap (std::vector<Tempo>, std::vector<TimeSignature>, double sampleRate);

    double getSampleRate() const noexcept { return sampleRate; }

    // Where a beat is, in samples from the song's start, and the other way round; before the song's start, as its first
    // tempo and time signature go on.
    double toSamples (double beat) const noexcept;
    double toBeats (double sample) const noexcept;

    // How long a beat is where a beat is, in samples; the tempo there, in beats per minute; and the time signature there.
    double getSamplesPerBeat (double beat) const noexcept;
    double getTempoAt (double beat) const noexcept;
    TimeSignature getTimeSignatureAt (double beat) const noexcept;

    // The first beat of the bar a beat is in.
    double getBarStart (double beat) const noexcept;

private:
    // A stretch of the song with one tempo and one time signature: where it starts, in beats and samples, and how long a
    // beat lasts in it.
    struct Segment
    {
        double beat = 0.0, sample = 0.0, samplesPerBeat = 0.0, bpm = 120.0;
        int signature = 0; // its time signature's index
    };

    std::vector<Segment> segments; // in order, the first at the song's start
    std::vector<TimeSignature> signatures;
    double sampleRate = 44100.0;

    const Segment& findByBeat (double beat) const noexcept;
    const Segment& findBySample (double sample) const noexcept;
};
} // namespace phazite::engine

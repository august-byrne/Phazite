// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo map as the engine plays it: where each of the song's beats falls in samples, at a sample rate. The
// song keeps it in beats (model/transport/internal/Tempo.h): each tempo and time signature from a beat on, the first at
// the song's start, and how each tempo goes on to the next: held and then jumping, or ramping to it, straight or bent, as
// an automation lane's line goes (automation/Curve.h). A beat is one of the time signature's own, a quarter note in 4/4
// and an eighth in 6/8, and a tempo counts quarter notes in a minute, whatever the time signature.
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

        // The line to the next tempo, as an automation point's (automation/Curve.h): 1 holds this tempo and then jumps to
        // the next, 0 ramps straight to it, between them it bends, and -1 jumps to it at once. The last tempo holds.
        float curve = 1.0f;
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
    // tempo and time signature go on. Real-time safe: a binary search and a formula, no allocation.
    double toSamples (double beat) const noexcept;
    double toBeats (double sample) const noexcept;

    // How long a beat is where a beat is, in samples; the tempo there, in beats per minute; and the time signature there.
    double getSamplesPerBeat (double beat) const noexcept;
    double getTempoAt (double beat) const noexcept;
    TimeSignature getTimeSignatureAt (double beat) const noexcept;

    // The first beat of the bar a beat is in.
    double getBarStart (double beat) const noexcept;

    // Whether the tempo is the same all the way through a range of beats.
    bool isSteadyBetween (double fromBeat, double toBeat) const noexcept;

private:
    // A stretch of the song with one time signature, over which the tempo is a straight line in beats, flat or not: where
    // it starts, in beats and samples, its tempo there and how much it changes a beat, and how many samples a beat lasts at
    // one beat per minute (sample rate x 60, and 4 / the time signature's note).
    struct Segment
    {
        double beat = 0.0, sample = 0.0, bpm = 120.0, bpmPerBeat = 0.0, scale = 0.0;
        int signature = 0; // its time signature's index
    };

    std::vector<Segment> segments; // in order, the first at the song's start
    std::vector<TimeSignature> signatures;
    double sampleRate = 44100.0;

    const Segment& findByBeat (double beat) const noexcept;
    const Segment& findBySample (double sample) const noexcept;
};
} // namespace phazite::engine

// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo map as the engine plays it: where each of the song's beats falls in samples, at a sample rate. The
// song keeps it in beats (model/transport/internal/Tempo.h): each tempo and time signature from a beat on, the first at
// the song's start, and how each tempo goes on to the next: held and then jumping, or ramping to it, straight or bent, as
// an automation lane's line goes (automation/Curve.h). A beat is a quarter note whatever the time signature, as MIDI,
// DAWproject and every major DAW count them, and a tempo counts quarter notes in a minute. A time signature sets two
// things only: how long its bar is, numerator x 4 / denominator quarter notes (3 in 6/8, 3.5 in 7/8, 4 in 2/2), and its
// own beat, the note it counts, 4 / denominator quarter notes (an eighth in 6/8, a half in 2/2), which the count-in and the
// metronome click on.
#pragma once

#include <algorithm>
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

        // How long one of its beats is, the note it counts, and its bar, numerator of them, in quarter notes.
        double getBeatLength() const noexcept { return 4.0 / std::max (1, denominator); }
        double getBarLength() const noexcept { return std::max (1, numerator) * getBeatLength(); }
    };

    TempoMap(); // 120 BPM in 4/4, at 44.1 kHz

    // Each in the song's order, the first at its start (one is made there if there is none).
    TempoMap (std::vector<Tempo>, std::vector<TimeSignature>, double sampleRate);

    double getSampleRate() const noexcept { return sampleRate; }

    // Where a beat is, in samples from the song's start, and the other way round; before the song's start, as its first
    // tempo and time signature go on. Real-time safe, as everything here is: a binary search and a formula, no allocation.
    double toSamples (double beat) const noexcept;
    double toBeats (double sample) const noexcept;

    // How long a quarter note is where a beat is, in samples; the tempo there, in quarter notes a minute; and the time
    // signature there.
    double getSamplesPerQuarterNote (double beat) const noexcept;
    double getTempoAt (double beat) const noexcept;
    TimeSignature getTimeSignatureAt (double beat) const noexcept;

    // The time signature's bar and its beat where a beat is, in quarter notes (getTimeSignatureAt()'s lengths).
    double getBarLength (double beat) const noexcept;
    double getBeatLength (double beat) const noexcept;

    // The first beat of the bar a beat is in, each change of the time signature starting a bar; and how far into the song a
    // beat is in bars, from 0 at its start, with the part of its bar it is through (2.5 half way through bar 2), as the
    // song's bars count them (model/transport/TempoValues.h: Bars).
    double getBarStart (double beat) const noexcept;
    double getBarsAt (double beat) const noexcept;

    // The first of the time signature's own beats at or after a beat (one a billionth of a beat before it counts), and the
    // next after one of them, each counted from where its time signature starts; a change of time signature starts on one.
    double getBeatAtOrAfter (double beat) const noexcept;
    double getNextBeat (double beat) const noexcept;

    // Whether the tempo is the same all the way through a range of beats.
    bool isSteadyBetween (double fromBeat, double toBeat) const noexcept;

private:
    // A stretch of the song with one time signature, over which the tempo is a straight line in beats, flat or not: where
    // it starts, in beats and samples, its tempo there and how much it changes a beat, and how many samples a beat lasts at
    // one beat per minute (sample rate x 60).
    struct Segment
    {
        double beat = 0.0, sample = 0.0, bpm = 120.0, bpmPerBeat = 0.0, scale = 0.0;
        int signature = 0; // its time signature's index
    };

    std::vector<Segment> segments; // in order, the first at the song's start
    std::vector<TimeSignature> signatures;
    std::vector<double> firstBars; // each time signature's first bar, counted from the song's start (getBarsAt())
    double sampleRate = 44100.0;

    const Segment& findByBeat (double beat) const noexcept;
    const Segment& findBySample (double sample) const noexcept;
};
} // namespace phazite::engine

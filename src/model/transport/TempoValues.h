// SPDX-License-Identifier: AGPL-3.0-or-later
// The tempo's limits, the time signature and the song's bars, as plain values (Tempo.h changes them; TransportModel.h
// shows them).
#pragma once

#include "model/base/Beats.h"

#include <array>
#include <vector>

namespace phazite
{
// The tempo's limits, in beats per minute.
inline constexpr double minTempo = 20.0, maxTempo = 300.0;

struct TimeSignature
{
    int numerator = 4;   // beats in a bar
    int denominator = 4; // the note that counts as a beat: 4 for a quarter note

    bool operator== (const TimeSignature&) const = default;
};

// Whether Phazite takes it: 1 to 32 beats of a half, quarter, eighth or sixteenth note.
bool isValid (TimeSignature);

// The time signatures that menus offer, as most DAWs do.
inline constexpr std::array<TimeSignature, 8> commonTimeSignatures {
    { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } }
};

// The tempo map: each tempo and time signature from a beat on, in the song's order, the first at its start. A song
// without one is at 120 BPM in 4/4.
struct TempoChange
{
    BeatPosition beat {};
    double bpm = 120.0;

    // The line to the next tempo, as an automation point's (AutomationValues.h): 1 holds this tempo and then jumps to the
    // next (a step, as tempo changes start), 0 ramps straight to it, and between the ramp bends; -1 jumps to it at once.
    float curve = 1.0f;

    bool operator== (const TempoChange&) const = default;
};

// A ramp's curve: straight; and the step that new tempo changes start as.
inline constexpr float tempoRamp = 0.0f, tempoJump = 1.0f;

struct TimeSignatureChange
{
    BeatPosition beat {};
    TimeSignature signature {};
};

// The song's bars, numbered from 0 at its start, as its time signatures lay them out: each change of the time signature
// starts a bar, and the bars before the song's start (below 0) go back in its first.
class Bars
{
public:
    Bars(); // 4/4 throughout
    explicit Bars (const std::vector<TimeSignatureChange>&);

    int getBarAt (BeatPosition) const;
    BeatPosition getBarStart (int bar) const;
    int getBeatsPerBar (BeatPosition) const; // in the bar that is there

    // How far into the song a beat is in bars, with the part of its bar it is through: 2.5 half way through bar 2.
    double getBarsAt (BeatPosition) const;

private:
    // From each time signature's start, bars of its length.
    struct Run
    {
        double startBeat = 0.0;
        int firstBar = 0;
        int beatsPerBar = 4;
    };

    std::vector<Run> runs;
    const Run& findRunAtBeat (double) const;
    const Run& findRunOfBar (int) const;
};

// What a change of the song's tempo stretches (docs/design.md, section 4): each sample (audio clip) that follows the
// tempo, and the song's patterns (MIDI clips) unless they keep their speed. A stretched clip plays faster or slower with
// the tempo; one that isn't keeps its speed. Either way each clip starts on the same beat.
struct TempoStretch
{
    int samples = 0;            // in the song
    int samplesFollowing = 0;   // of them, those that follow the tempo
    int patterns = 0;           // in the song
    bool patternsFollow = true; // or keep their speed

    bool operator== (const TempoStretch&) const = default;
};
} // namespace phazite

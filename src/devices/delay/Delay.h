// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's delay (roadmap steps 2.10 and S6.6): echoes of the sound, in time with the song (a note value at its tempo) or a
// time in milliseconds, each side its own time or the left's (Link), that come back with less of themselves each time (the
// feedback, up to 110 %, where they swell until the loop's curve holds them), each darker and thinner than the one before
// through a low and a high cut. Ping-pong bounces them between the left and the right. The loop's character: Tape bends
// every echo with a soft saturation; Clean is a straight line that bends only above full scale. The modulation wavers each
// line's time as a sine, the right a quarter of a cycle ahead, its phase running on from its rate and set from where the song
// is as playback starts; the duck turns the echoes down while sound comes in; the freeze holds what is in the lines, going
// round unchanged in whole samples, what is heard still the echo through the cuts. A change of time glides to the new one
// (Repitch, as a tape's would) or fades at equal power from a read at the old time to one at the new (Fade), rather than
// clicking. It puts out the echoes alone: its mix, which its runner applies, blends them with the sound. Every switch and
// every change is counted in samples, so that it plays the same in blocks of any size.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"

#include <array>
#include <atomic>
#include <span>

namespace phazite::devices
{
class Delay final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteDelay";
    static constexpr double maximumSeconds = 5.0;          // the longest echo: a bar of 4/4 at 48 BPM
    static constexpr double mostModulationSeconds = 0.004; // how far the modulation moves a time either way at 100 %
    static constexpr float mostDuckDecibels = 24.0f;       // how far the duck turns the echoes down at 100 %
    static constexpr float duckAllDecibels = -12.0f;       // sound this loud or louder ducks them all the way
    static constexpr float duckNoneDecibels = -36.0f;      // sound under this ducks them not at all
    static constexpr double longestTailSeconds = 60.0;     // the tail it reports at its longest

    enum Parameter
    {
        sync, // the times are note values at the song's tempo (on) or milliseconds (off)
        note, // the left's note value (both sides' while Link is on), a bar the song's (as long as its time signature makes it)
        time, // the left's milliseconds (both sides' while Link is on)
        link, // on, the right takes the left's time
        noteRight,
        timeRight,
        feedback, // 0 to 1.1
        pingPong,
        character, // Tape or Clean
        modRate,
        modDepth,
        duck,
        freeze,
        transition, // Repitch or Fade
        lowCut,
        highCut,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Character
    {
        tape,
        clean
    };

    enum class Transition
    {
        repitch,
        fade
    };

    static const DeviceInfo info;

    // How long a side's echo takes (0 the left, 1 the right) for the parameters' values at a tempo (in beats per minute, a beat
    // being a quarter note) and a bar so many beats long: the note value's length, or the milliseconds, the left's for both
    // while Link is on; at most maximumSeconds.
    static double getDelaySeconds (std::span<const float> parameters, double tempo, double barLength, int side = 0);

    // The Clean character's curve: a straight line to full scale, then rounding off toward 2 (1 + tanh of what is over 1, as
    // steep as the line where they join and as straight), so that a feedback over 100 % swells to a level and holds there.
    static float bendClean (float sample) noexcept;

    // How far the duck turns the echoes down, in decibels (0 or more), at its amount (0 to 1) with the sound coming in at a
    // level in dBFS: all of mostDuckDecibels times the amount at duckAllDecibels or louder, none under duckNoneDecibels, in a
    // straight line in decibels between.
    static float getDuckDecibels (float amount, float levelDecibels) noexcept;

    // The tempo it last played at, in beats per minute, and the length of the song's bar there, in beats (devices/core/
    // LiveValue.h), for its editor's note values; and how far its duck turns its echoes down now, in decibels.
    const LiveValue& getShownTempo() const noexcept { return shownTempo; }
    const LiveValue& getShownBarLength() const noexcept { return shownBarLength; }
    const LiveValue& getShownDuck() const noexcept { return shownDuck; }

    // How many fades it has started since it was prepared or reset (Transition: Fade), and how many are under way now, for
    // tests.
    int getFadesStarted() const noexcept { return fadesStarted; }
    int getFadesUnderWay() const noexcept;

    // How long the duck's follower holds the loudest the sound reached before it falls, in milliseconds: a cycle of a 50 Hz
    // bass, so that the level it hears doesn't fall between the half-cycles of a low note.
    static constexpr int duckHoldMilliseconds = 20;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

private:
    // A side's line and where it is read: at its time, which Repitch glides to a new one; and while Fade fades, also at the
    // time it fades from, which fades out as the new one fades in.
    struct Side
    {
        dsp::DelayLine line;
        dsp::Filter lowFilter, highFilter;
        float delaySamples = 1.0f;
        float fadingFrom = 1.0f;
        int fadeLeft = 0; // samples of the fade left; 0 while it doesn't fade
    };

    double sampleRate = 44100.0;
    float timeSmoothing = 0.001f; // of the way to a new time, per sample
    int fadeLength = 2205;        // a Fade's samples
    float smallestFade = 48.0f;   // a change of time shorter than this, in samples (1 ms), glides even with Fade
    float shareStep = 0.001f;     // of a switch's crossfade, per sample (the character's and the freeze's)
    float depthSmoothing = 0.001f, duckRise = 0.0f, duckFall = 0.0f, duckSmoothing = 0.001f;
    bool isFirstBlock = true;
    int fadesStarted = 0;
    std::array<Side, 2> sides;

    // The switches' crossfades: how much of the loop is Tape's, and how frozen it is, from 0 to 1.
    float tapeShare = 1.0f, frozenShare = 0.0f;

    // The modulation: its phase (0 to 1), built up a sample at a time from the rate, as an LFO in hertz runs; tied to where
    // the song is as playback starts, by a glide of what is left of the way there, which moves the phase no faster than the
    // rate itself; its depth now, in samples.
    double modPhase = 0.0, modIncrement = 0.0, modGlideLeft = 0.0;
    bool modFresh = true, wasPlaying = false;
    float depthNow = 0.0f;

    // The duck: the loudest the sound reached in each of the last milliseconds (a running maximum over the hold), what
    // follows it (a gain), and the gain on the echoes now.
    std::array<float, duckHoldMilliseconds> duckPeaks {};
    int duckPeakIndex = 0, duckPeakSamples = 0, samplesPerDuckPeak = 48;
    float duckPeakNow = 0.0f, duckPeakBefore = 0.0f;
    float duckLevel = 0.0f, duckGain = 1.0f;
    bool duckRests = true; // its follower and peaks at nothing, the Duck at 0 and its gain at 1

    std::atomic<double> tailSeconds { 2.0 };
    LiveValue shownTempo { 120.0f }, shownBarLength { 4.0f }, shownDuck { 0.0f };

    void startModulation (const ProcessContext&, float rate) noexcept;
    void advanceModulation() noexcept;
    void moveTime (Side&, float target, bool fades) noexcept;
    float readEcho (const Side&, float wave) const noexcept; // wave: the modulation's sine now, from -1 to 1
    float readLoop (const Side&) const noexcept;             // what goes round while frozen: whole samples
    float getSwing (float time) const noexcept;              // how far the modulation moves a read at a time, in samples
    float followDuck (float level) noexcept;                 // the duck's follower a sample on, from what comes in
    void restDuck() noexcept;
};
} // namespace phazite::devices

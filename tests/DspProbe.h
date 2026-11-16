// SPDX-License-Identifier: AGPL-3.0-or-later
// The measuring bench for device DSP (src/devices/): test signals, a device played as Phazite's engine plays it (through its
// runner, engine/nodes/DeviceRunner.h, the song playing from its start) or alone, and what is measured in what comes out:
// levels, a tone's level and a response at frequencies, the aliases under a tone, an offset, how late the sound comes out, and
// whether every sample is a number. DeviceConformanceTests.cpp holds every one of Phazite's devices to the same rules with
// it; a device's own tests measure what is the device's alone with it.
//
// A sound is a juce::AudioBuffer<float>, two channels unless said; places and lengths are in samples.
#pragma once

#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "engine/automation/Curve.h"
#include "engine/nodes/DeviceRunner.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace phazite::test::probe
{
using Sound = juce::AudioBuffer<float>;

//==============================================================================
// Signals. A sine and an impulse are the same on every channel; noise is white, each sample anywhere from -amplitude to
// amplitude, each channel with samples of its own, and the same every time for a seed.
Sound makeSilence (int numSamples, int numChannels = 2);
Sound makeSine (double hertz, int numSamples, double sampleRate, float amplitude = 0.5f, int numChannels = 2);
Sound makeImpulse (int numSamples, int at = 0, float amplitude = 1.0f, int numChannels = 2);
Sound makeNoise (int numSamples, std::uint32_t seed, float amplitude = 0.5f, int numChannels = 2);

// Noise with nothing above a frequency (a steep low-pass filter's), its largest sample as large as the amplitude: what a
// delay is measured with to a part of a sample (findDelay()).
Sound makeNoiseBelow (double hertz, int numSamples, double sampleRate, std::uint32_t seed, float amplitude = 0.5f, int numChannels = 2);

// One sound after another, on as many channels as the first; and a part of a sound.
Sound join (const Sound& first, const Sound& second);
Sound getPart (const Sound&, int from, int numSamples);

//==============================================================================
// Measurements. A stretch is given by its first sample and its length (to the sound's end where none is given).

// The first sample, on any channel, that isn't a finite number (a NaN or an infinity), or -1: every sample is one.
int findNotFinite (const Sound&);
inline bool isFinite (const Sound& sound)
{
    return findNotFinite (sound) < 0;
}

// The largest sample on any channel, whichever way it points; a channel's RMS; and its mean, which is its offset (DC).
double getPeak (const Sound&, int from = 0, int numSamples = -1);
double getRms (const Sound&, int channel, int from = 0, int numSamples = -1);
double getMean (const Sound&, int channel, int from = 0, int numSamples = -1);

// A channel's offset under a note that is held through the stretch, as a part of how loud the note is there: its mean over
// its RMS, both weighted by a Hann window, which leaves out the stretch's ends, where a part of a cycle would count as an
// offset. 0 for silence.
double getOffsetShare (const Sound&, int channel, int from = 0, int numSamples = -1);

// The largest difference between two sounds' samples, on the channels and over the stretch both have.
double getLargestDifference (const Sound&, const Sound&, int from = 0, int numSamples = -1);

// A gain in decibels (-240 for none).
double inDecibels (double gain);

// A tone's level in a stretch of a channel, as an amplitude: the sine at its frequency that fits the stretch best (least
// squares, so the stretch need not hold whole cycles).
double getToneLevel (const Sound&, int channel, double hertz, double sampleRate, int from = 0, int numSamples = -1);

// How many decibels louder (below 0, quieter) each of some frequencies comes out of something than it went in: a sine of each,
// `settle + measure` samples long, is played through `play`, and its level read in the last `measure` samples of what
// comes back.
std::vector<double> getResponse (const std::function<Sound (const Sound&)>& play, std::span<const double> hertz, double sampleRate,
                                 float amplitude, int settle, int measure);

// Aliases, measured as docs/archive/dsp-audit.md measured them: over aliasWindow samples, with a tone that is a whole, odd number of
// the transform's bins (onBin() gives the one nearest a frequency), so that each of its harmonics lands on a multiple of
// its bin and everything else in the audible band (up to 20 kHz) is an alias. How far the loudest alias is under the tone,
// in decibels, in aliasWindow samples of a channel.
inline constexpr int aliasWindow = 1 << 16;
double onBin (double hertz, double sampleRate);
double getAliasesUnder (const Sound&, int channel, int from, double hertz, double sampleRate);

// How late a sound comes out of something: the delay, from 0 to the longest given, at which a channel of what came out is
// most like the same channel of what went in (the peak of their cross-correlation, placed between samples by the parabola
// through the peak and its neighbours), and how alike they are there, from 0 to 1 (1: the same sound, only later and
// louder or quieter). Measure with noise: a tone is as like itself a cycle later.
struct Delay
{
    double samples = 0.0;
    double likeness = 0.0;
};

Delay findDelay (const Sound& in, const Sound& out, int channel, int longest);

//==============================================================================
// A device's values, each parameter at its default.
std::vector<float> getDefaultValues (const devices::DeviceInfo&);

// A device's DSP alone, without a runner: a sound played through it in place, in blocks of a size, on its first two
// channels, with its parameters at these values (nothing glides, and no mix is applied: DeviceInfo.h, mixParameterID) and
// notes at their samples from the sound's start, in time order. Prepare it first.
void playAlone (devices::Processor&, std::span<const float> values, Sound&, int blockSize, std::span<const devices::NoteEvent> notes = {});

// A device as Phazite's engine plays it, on its runner, the song playing from its start at 120 beats a minute, in 4/4.
class Device
{
public:
    // One of Phazite's own devices, made as the model makes it (model/devices/internal/Devices.h); or any other.
    explicit Device (const devices::DeviceInfo&);
    Device (const devices::DeviceInfo&, std::unique_ptr<devices::Processor>);

    const devices::DeviceInfo& info;

    // Its values set by hand, its defaults at first: what the next block plays (a continuous one that changed glides there
    // over that block, as a knob's does).
    std::vector<float> values;
    bool has (std::string_view parameterID) const;
    void set (std::string_view parameterID, float value); // the test fails where it has none
    void setAll (const std::vector<float>& newValues) { values = newValues; }

    // Automation, the song's: a parameter along a line from one value at the song's start to another so many samples in at
    // the rate it is prepared at, which the runner plays on the song's control points (engine/automation/Curve.h). Once
    // prepared; kept until clearAutomation().
    void automate (int parameter, float from, float to, std::int64_t overSamples);
    void clearAutomation();

    // A render (the default) plays everything; otherwise the runner lets the device sleep on silence, as it does live.
    bool isRendering = true;

    // Before it plays, and again for another rate: the song is back at its start. And as the engine's transport stops and
    // starts over: silent, and the song back at its start.
    void prepare (double sampleRate, int maximumBlockSize);
    void reset();

    // Plays a sound through it, in place, in blocks of a size (the last as long as what is left), the song playing on from
    // where the last call left it: an effect's input, or nothing an instrument uses. A sound of one channel plays as a mono
    // track does; the third and fourth channels of one with four are its sidechain (cleared as it plays: what comes out is
    // on the first two). The notes are at their samples from the sound's start, in time order.
    void play (Sound&, int blockSize, std::span<const devices::NoteEvent> notes = {});

    // The same on a copy, which is returned.
    Sound played (const Sound&, int blockSize, std::span<const devices::NoteEvent> notes = {});

    // A block of no samples at all, which a host may hand a device.
    void playNothing();

    // The song jumps to a sample, the device not reset, as when a loop goes round, playback starts elsewhere or a device
    // that slept on silence wakes: the next block plays from there.
    void moveTo (std::int64_t sample) noexcept { songSample = sample; }

    double getSampleRate() const noexcept { return sampleRate; }
    int getLatencySamples() const noexcept { return runner.getLatencySamples(); } // once prepared
    double getTailSeconds() const { return runner.getTailSeconds(); }
    std::int64_t getSongSample() const noexcept { return songSample; }
    engine::DeviceRunner& getRunner() noexcept { return runner; }
    devices::Processor& getProcessor() noexcept { return runner.getProcessor(); }

private:
    engine::DeviceRunner runner;
    double sampleRate = 48000.0;
    int longestBlock = 0;
    std::int64_t songSample = 0;
    std::vector<std::unique_ptr<engine::Curve>> curves;
    std::vector<engine::DeviceRunner::Automated> automation;
    std::vector<devices::NoteEvent> blockNotes;
    std::vector<float> valuesPlayed; // as the last block had them

    engine::DeviceRunner::Block makeBlock (bool hasSidechain);
};

// A note on, or off, at a sample.
devices::NoteEvent noteOn (int sample, int note, float velocity = 0.8f);
devices::NoteEvent noteOff (int sample, int note);
} // namespace phazite::test::probe

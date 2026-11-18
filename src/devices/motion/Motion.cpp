// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Motion.h"
#include "SafeBass.h"
#include "devices/dsp/Crossover.h"
#include "devices/dsp/NoteValues.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view modes[] = { "Chorus", "Flanger", "Phaser" };
    constexpr std::string_view shapes[] = { "Sine", "Triangle" };
    constexpr std::string_view notchCounts[] = { "1", "2", "3", "4", "5", "6" };
    constexpr std::string_view voiceCounts[] = { "1", "2", "3", "4", "5", "6", "7", "8" };

    static_assert (std::size (notchCounts) == (size_t) Phaser::maxNotches);
    static_assert (std::size (voiceCounts) == (size_t) Chorus::mostVoices);

    // The IDs are saved in projects. The defaults are the chorus's, so that a new Motion plays what a new Chorus played.
    constexpr ParameterInfo parameters[] = {
        { .id = "mode", .name = "Mode", .maximum = 2.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = modes },
        { .id = "delay",
          .name = "Delay",
          .minimum = 0.2f,
          .maximum = 25.0f,
          .defaultValue = 3.5f,
          .unit = Unit::milliseconds,
          .midpoint = 4.0f },
        { .id = "frequency",
          .name = "Frequency",
          .minimum = 50.0f,
          .maximum = 10000.0f,
          .defaultValue = 800.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "depth", .name = "Depth", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "sync", .name = "Sync", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "division",
          .name = "Division",
          .maximum = (float) std::size (dsp::noteValueNames) - 1.0f,
          .defaultValue = dsp::barNoteValue,
          .unit = Unit::choice,
          .choices = dsp::noteValueNames },
        { .id = "rate",
          .name = "Rate",
          .minimum = 0.05f,
          .maximum = 20.0f,
          .defaultValue = 0.5f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "shape", .name = "Shape", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = shapes },
        { .id = "feedback", .name = "Feedback", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "notches", .name = "Notches", .maximum = 5.0f, .defaultValue = 1.0f, .unit = Unit::choice, .choices = notchCounts },
        { .id = "spacing", .name = "Spacing", .defaultValue = 0.5f, .unit = Unit::percent },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 20000.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "voices",
          .name = "Voices",
          .maximum = (float) Chorus::mostVoices - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = voiceCounts },
        { .id = "voicePhase", .name = "Voice Phase", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "spread", .name = "Spread", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "shimmer", .name = "Shimmer", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "stereo", .name = "Stereo", .defaultValue = 1.0f, .unit = Unit::percent },
        { .id = "safeBass",
          .name = "Safe Bass",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 20.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 0.5f, .unit = Unit::percent },
    };

    constexpr double fadeSeconds = 0.01; // a mode fading out, then the next in
    constexpr int leastCapacity = 512;   // as its runner's stretches (engine/nodes/DeviceRunner.cpp)

    // A fade along a curve that starts and ends level, as the reverb's modes fade (reverb/Reverb.cpp).
    float fadeCurve (float level) noexcept
    {
        return level * level * (3.0f - 2.0f * level);
    }

    // What the runner plays, as a gain, of what the flanger or the phaser plays (its response, H) and the dry copy, as the one
    // meaning of Mix mixes them: (1 - m) D + m (2 H - D), D the dry copy as Safe Bass's split turns it (SafeBass.h).
    double mixWithDry (std::span<const float> values, std::complex<double> response, double frequency, double sampleRate)
    {
        std::complex<double> dry (1.0);

        if (SafeBass::isOn (values[Motion::safeBass]))
        {
            const auto bands = dsp::Crossover::getResponses (values[Motion::safeBass], frequency, sampleRate);
            dry = bands.low + bands.high;
        }

        const auto mixed = (double) values[Motion::mix];
        return std::abs ((1.0 - 2.0 * mixed) * dry + 2.0 * mixed * response);
    }
} // namespace

const DeviceInfo Motion::info { .typeID = typeID, .name = "Motion", .kind = DeviceKind::effect, .parameters = parameters };

Motion::Mode Motion::getMode (std::span<const float> values)
{
    switch (std::clamp ((int) std::lround (values[mode]), 0, 2))
    {
        case 1:
            return Mode::flanger;
        case 2:
            return Mode::phaser;
        default:
            return Mode::chorus;
    }
}

std::array<float, Chorus::mix + 1> Motion::getChorusValues (std::span<const float> values)
{
    std::array<float, Chorus::mix + 1> engine {};
    engine[Chorus::voices] = values[voices];
    engine[Chorus::delay] = values[delay];
    engine[Chorus::depth] = values[depth];
    engine[Chorus::sync] = values[sync];
    engine[Chorus::division] = values[division];
    engine[Chorus::rate] = values[rate];
    engine[Chorus::shape] = values[shape];
    engine[Chorus::shimmer] = values[shimmer];
    engine[Chorus::voicePhase] = values[voicePhase];
    engine[Chorus::spread] = values[spread];
    engine[Chorus::feedback] = chorusFeedback * values[feedback];
    engine[Chorus::stereo] = values[stereo];
    engine[Chorus::highCut] = values[highCut];
    engine[Chorus::safeBass] = values[safeBass];
    engine[Chorus::mix] = values[mix];
    return engine;
}

std::array<float, Flanger::mix + 1> Motion::getFlangerValues (std::span<const float> values)
{
    std::array<float, Flanger::mix + 1> engine {};
    engine[Flanger::delay] = values[delay];
    engine[Flanger::depth] = values[depth];
    engine[Flanger::sync] = values[sync];
    engine[Flanger::division] = values[division];
    engine[Flanger::rate] = values[rate];
    engine[Flanger::shape] = values[shape];
    engine[Flanger::feedback] = flangerFeedback * values[feedback];
    engine[Flanger::stereo] = values[stereo];
    engine[Flanger::safeBass] = values[safeBass];
    engine[Flanger::mix] = 2.0f * values[mix];
    return engine;
}

std::array<float, Phaser::mix + 1> Motion::getPhaserValues (std::span<const float> values)
{
    std::array<float, Phaser::mix + 1> engine {};
    engine[Phaser::notches] = values[notches];
    engine[Phaser::frequency] = values[frequency];
    engine[Phaser::depth] = values[depth];
    engine[Phaser::sync] = values[sync];
    engine[Phaser::division] = values[division];
    engine[Phaser::rate] = values[rate];
    engine[Phaser::shape] = values[shape];
    engine[Phaser::feedback] = phaserFeedback * values[feedback];
    engine[Phaser::spacing] = values[spacing];
    engine[Phaser::stereo] = values[stereo];
    engine[Phaser::safeBass] = values[safeBass];
    engine[Phaser::mix] = 2.0f * values[mix];
    return engine;
}

double Motion::getFlangerMagnitude (std::span<const float> values, double milliseconds, double frequency, double sampleRate)
{
    return mixWithDry (values, Flanger::getResponse (getFlangerValues (values), milliseconds, frequency, sampleRate), frequency,
                       sampleRate);
}

double Motion::getPhaserMagnitude (std::span<const float> values, double centre, double frequency, double sampleRate)
{
    return mixWithDry (values, Phaser::getResponse (getPhaserValues (values), centre, frequency, sampleRate), frequency, sampleRate);
}

float Motion::getFlangerNotchDepth (std::span<const float> values) noexcept
{
    return 1.0f - std::abs (1.0f - 2.0f * std::clamp (values[mix], 0.0f, 1.0f));
}

Processor& Motion::getEngine (int which) noexcept
{
    return which == (int) Mode::flanger  ? static_cast<Processor&> (flanger)
           : which == (int) Mode::phaser ? static_cast<Processor&> (phaser)
                                         : static_cast<Processor&> (chorus);
}

const Processor& Motion::getEngine (int which) const noexcept
{
    return which == (int) Mode::flanger  ? static_cast<const Processor&> (flanger)
           : which == (int) Mode::phaser ? static_cast<const Processor&> (phaser)
                                         : static_cast<const Processor&> (chorus);
}

double Motion::getTailSeconds() const
{
    return getEngine (shownMode.load (std::memory_order_relaxed)).getTailSeconds();
}

void Motion::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    capacity = std::max (maximumBlockSize, leastCapacity);
    asItCame.assign ((size_t) (4 * capacity), 0.0f);
    ownDry.assign ((size_t) (2 * capacity), 0.0f);
    gains.assign ((size_t) capacity, 0.0f);
    fadeLength = std::max (1, (int) std::lround (fadeSeconds * sampleRate));

    chorus.prepare (sampleRate, capacity);
    flanger.prepare (sampleRate, capacity);
    phaser.prepare (sampleRate, capacity);
    reset();
}

void Motion::reset()
{
    chorus.reset();
    flanger.reset();
    phaser.reset();
    playing = -1;
    faded = fadeLength;
}

void Motion::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0 || block.numSamples <= 0 || capacity <= 0)
        return;

    const auto& values = block.parameters;
    chorusValues = getChorusValues (values);
    flangerValues = getFlangerValues (values);
    phaserValues = getPhaserValues (values);

    // The mode that plays: the one asked for at once as it starts; then, asked for another, the one that plays fades out, and
    // the other, reset, fades in from the very sample the fade-out ended on.
    const auto wanted = (int) getMode (values);

    if (playing < 0)
    {
        playing = wanted;
        faded = fadeLength;
        shownMode.store (playing, std::memory_order_relaxed);
    }

    for (int start = 0; start < block.numSamples;)
    {
        if (playing != wanted && faded <= 0)
        {
            playing = wanted;
            getEngine (playing).reset();
            shownMode.store (playing, std::memory_order_relaxed);
        }

        const auto fadesOut = playing != wanted;
        auto length = std::min (block.numSamples - start, capacity);

        if (fadesOut)
            length = std::min (length, faded);

        processStretch (block, start, length, numChannels, fadesOut);
        start += length;
    }
}

void Motion::processStretch (const ProcessContext& block, int start, int length, int numChannels, bool fadesOut)
{
    const auto fades = fadesOut || faded < fadeLength;
    float* channels[2] {};
    float* dry[2] {};
    float* came[2] {};
    float* dryCame[2] {};

    for (int channel = 0; channel < numChannels; ++channel)
    {
        channels[channel] = block.channels[channel] + start;
        came[channel] = asItCame.data() + 2 * channel * capacity;
        dryCame[channel] = came[channel] + capacity;

        // Where its runner hands it no dry copy, one of its own: the flanger and the phaser take it from what they play.
        if (block.dry != nullptr)
        {
            dry[channel] = block.dry[channel] + start;
        }
        else
        {
            dry[channel] = ownDry.data() + channel * capacity;
            std::copy (channels[channel], channels[channel] + length, dry[channel]);
        }
    }

    // Fading, its engine is fed the sound faded as it fades, by g: cut in at full level, the sound would be a click of its
    // own, which its lines would then play back and feed back (as the reverb fades what a mode is fed, reverb/Reverb.cpp).
    if (fades)
    {
        for (int i = 0; i < length; ++i)
        {
            faded = std::clamp (faded + (fadesOut ? -1 : 1), 0, fadeLength);
            gains[(size_t) i] = fadeCurve ((float) faded / (float) fadeLength);
        }

        for (int channel = 0; channel < numChannels; ++channel)
        {
            std::copy (channels[channel], channels[channel] + length, came[channel]);
            std::copy (dry[channel], dry[channel] + length, dryCame[channel]);

            for (int i = 0; i < length; ++i)
            {
                channels[channel][i] *= gains[(size_t) i];
                dry[channel][i] *= gains[(size_t) i];
            }
        }
    }

    // The stretch as a block of its own, where the song is as it starts.
    auto part = block;
    part.channels = channels;
    part.dry = dry;
    part.numChannels = numChannels;
    part.numSamples = length;

    if (start > 0)
    {
        part.events = {};
        part.changes = {};
        part.outputOffset = block.outputOffset + start;

        if (block.isPlaying)
        {
            const auto beats = (double) start * block.tempo / 60.0 / sampleRate;
            const auto barLength = block.barLength > 0.0 ? block.barLength : 4.0;
            part.songSample = block.songSample + start;
            part.beat = block.beat + beats;
            part.bars = block.bars + beats / barLength;
            part.barStart = block.barStart + (std::floor (part.bars) - std::floor (block.bars)) * barLength;
        }
    }

    switch ((Mode) playing)
    {
        case Mode::chorus:
            part.parameters = chorusValues;
            chorus.process (part);
            break;
        case Mode::flanger:
            part.parameters = flangerValues;
            flanger.process (part);
            break;
        case Mode::phaser:
            part.parameters = phaserValues;
            phaser.process (part);
            break;
    }

    // The flanger and the phaser mixed the sound in themselves, half and half: what they add to it, the effect alone, is
    // twice what they played less the sound (the dry copy, turned as they turned it).
    if (playing != (int) Mode::chorus)
        for (int channel = 0; channel < numChannels; ++channel)
            for (int i = 0; i < length; ++i)
                channels[channel][i] = 2.0f * channels[channel][i] - dry[channel][i];

    if (! fades)
        return;

    // Then the sound as it came in, at what the fade leaves of it, with what the mode plays, its dry copy too: g of the
    // effect and 1 - g of the sound, as what the mode played was fed the sound times g already. Near the fade's bottom what
    // the mode plays is taken down a second time, by h = min (1, 4 g), so that at the bottom its runner plays the sound
    // untouched, at any mix, whatever still rings in the mode's lines: 1 - g + h g of a sound the effect is in phase with,
    // flat from a quarter of the way up and no more than 0.6 dB down below it (at g = 1/8), where the same gain twice
    // (1 - g + g squared) would dip 2.5 dB at the fade's middle.
    for (int channel = 0; channel < numChannels; ++channel)
    {
        for (int i = 0; i < length; ++i)
        {
            const auto gain = gains[(size_t) i];
            const auto second = std::min (1.0f, 4.0f * gain);
            channels[channel][i] = (1.0f - gain) * came[channel][i] + second * channels[channel][i];

            if (block.dry != nullptr)
                dry[channel][i] = (1.0f - gain) * dryCame[channel][i] + second * dry[channel][i];
        }
    }
}
} // namespace phazite::devices

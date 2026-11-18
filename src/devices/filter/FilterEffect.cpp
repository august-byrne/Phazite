// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FilterEffect.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr float lowestCutoff = 20.0f, highestCutoff = 20000.0f;
    constexpr std::string_view characters[] = { "Clean", "Driven" };
    constexpr std::string_view qualities[] = { "2x", "4x", "8x" };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "filterType",
          .name = "Type",
          .maximum = (float) std::size (dsp::filterModeNames) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = dsp::filterModeNames },
        { .id = "cutoff",
          .name = "Cutoff",
          .minimum = lowestCutoff,
          .maximum = highestCutoff,
          .defaultValue = highestCutoff,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "resonance", .name = "Resonance", .defaultValue = 0.1f, .unit = Unit::percent },
        { .id = "filterSlope",
          .name = "Slope",
          .maximum = 1.0f,
          .defaultValue = 1.0f,
          .unit = Unit::choice,
          .choices = dsp::filterSlopeNames },
        { .id = "character",
          .name = "Character",
          .maximum = (float) std::size (characters) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = characters },
        { .id = "drive", .name = "Drive", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "quality",
          .name = "Quality",
          .maximum = (float) std::size (qualities) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = qualities },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double glideSeconds = 0.01; // how long the cutoff takes to follow a change, about
    constexpr double driveSeconds = 0.02;
    constexpr double fadeSeconds = 0.01; // a change of Character or Quality

    // How long a new way plays unheard before it is faded to: 5 ms at least, for the half-band filters and the curves'
    // averaging; and for a filter started from rest, four cycles of its cutoff, up to 100 ms.
    constexpr double warmSeconds = 0.005, settlingCycles = 4.0, longestWarmSeconds = 0.1;

    // What the export takes for silence (model/playback/OfflineRender.cpp), 70 dB under full scale: the Driven filter's tail
    // is how long its ring takes to fall that far.
    constexpr double ringDecibels = 70.0;

    dsp::FilterSettings makeSettings (std::span<const float> values, double cutoffHz)
    {
        return { .mode = dsp::toFilterMode (values[FilterEffect::filterType]),
                 .frequency = cutoffHz,
                 .resonance = values[FilterEffect::resonance],
                 .steep = dsp::isSteep (values[FilterEffect::filterSlope]),
                 .q = dsp::getResonanceQ (values[FilterEffect::resonance]) };
    }

    // The Clean filter at a cutoff.
    dsp::MultimodeCoefficients makeCoefficients (std::span<const float> values, double cutoffHz, double sampleRate)
    {
        return dsp::makeMultimode (makeSettings (values, cutoffHz), sampleRate);
    }

    float toOctaves (float hertz) noexcept
    {
        return std::log2 (std::max (hertz, lowestCutoff) / lowestCutoff);
    }

    double toHertz (float octavesAbove20) noexcept
    {
        return (double) lowestCutoff * std::pow (2.0, (double) octavesAbove20);
    }

    int getQualityIndex (float value) noexcept
    {
        return juce::jlimit (0, (int) std::size (qualities) - 1, (int) std::lround (value));
    }

    bool hasDriven (dsp::FilterMode mode) noexcept
    {
        return mode == dsp::FilterMode::lowPass || mode == dsp::FilterMode::highPass || mode == dsp::FilterMode::bandPass
               || mode == dsp::FilterMode::notch;
    }

    // A fade from 0 to 1 in a straight line, heard along an S (half a cosine), so that it starts and ends without a corner.
    float getEased (float along) noexcept
    {
        if (along <= 0.0f)
            return 0.0f;

        if (along >= 1.0f)
            return 1.0f;

        return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * along);
    }

    // How late a Quality comes, in the sound's samples, at any rate: the filters to its rate and back, whole samples late, and
    // the curve's averaging and levelling filter, 4 samples at its rate, made a whole number of the sound's samples (at 8
    // times the rate, half of one: 4 more make it whole). As the saturator's (Saturator.cpp).
    int getOwnLatency (int q)
    {
        static const std::array<int, 3> latencies = []
        {
            std::array<int, 3> found {};

            for (int each = 0; each < (int) found.size(); ++each)
            {
                juce::dsp::Oversampling<float> halfBands (2, (size_t) each + 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                                          true, true);
                halfBands.initProcessing (1);
                const auto factor = 2 << each;
                const auto padding = (factor - dsp::AntialiasedCurve::delay % factor) % factor;
                found[(size_t) each] =
                    (int) std::lround (halfBands.getLatencyInSamples()) + (dsp::AntialiasedCurve::delay + padding) / factor;
            }

            return found;
        }();

        return latencies[(size_t) q];
    }

    // As many samples later as make a way a whole number of the sound's samples: the padding's ring.
    float pad (std::array<float, 8>& padded, unsigned at, int padding, float y) noexcept
    {
        padded[at & 7u] = y;
        return padded[(at - (unsigned) padding) & 7u];
    }
} // namespace

const DeviceInfo FilterEffect::info { .typeID = typeID, .name = "Filter", .kind = DeviceKind::effect, .parameters = parameters };

bool FilterEffect::isDriven (std::span<const float> values) noexcept
{
    return values[character] > 0.5f && hasDriven (dsp::toFilterMode (values[filterType]));
}

double FilterEffect::getMagnitude (std::span<const float> values, double frequency, double sampleRate)
{
    if (const auto response = getResponse (values, frequency, sampleRate))
        return std::abs (*response);

    return dsp::getMagnitude (makeCoefficients (values, values[cutoff], sampleRate), frequency, sampleRate);
}

std::optional<std::complex<double>> FilterEffect::getResponse (std::span<const float> values, double frequency, double sampleRate)
{
    // Driven: the loop at its rate, the sound in it times the drive's gain and the square root of that given back after it.
    if (isDriven (values))
    {
        const auto rate = sampleRate * (2 << getQualityIndex (values[quality]));
        const auto gain = std::sqrt ((double) dsp::decibelsToGain (values[drive]));
        return gain * dsp::getResponse (dsp::makeDriven (makeSettings (values, values[cutoff]), rate), frequency, rate);
    }

    const auto coefficients = makeCoefficients (values, values[cutoff], sampleRate);

    if (! dsp::hasResponse (coefficients.mode))
        return std::nullopt;

    return dsp::getResponse (coefficients, frequency, sampleRate);
}

double FilterEffect::getHeardMagnitude (std::span<const float> values, double frequency, double sampleRate)
{
    const auto response = getResponse (values, frequency, sampleRate);

    if (! response.has_value())
        return getMagnitude (values, frequency, sampleRate);

    const auto wet = (double) std::clamp (values[mix], 0.0f, 1.0f);
    return std::abs ((1.0 - wet) + wet * *response);
}

int FilterEffect::getLatency()
{
    auto latest = 0;

    for (int q = 0; q < numQualities; ++q)
        latest = std::max (latest, getOwnLatency (q));

    return latest;
}

void FilterEffect::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    capacity = std::max (1, maximumBlockSize);
    glide = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    fadePerSample = (float) (1.0 / (fadeSeconds * sampleRate));
    warmUpLeast = std::max (256, (int) std::lround (warmSeconds * sampleRate));
    latency = getLatency();

    // Every Quality made ready at once, so that a change of it while the song plays allocates nothing: filters to 2, 4 or 8
    // times the rate and back, polyphase half-band ones that keep the latency short and whole, for the Clean drive, the Driven
    // filter and the copy for the mix.
    for (int q = 0; q < numQualities; ++q)
    {
        auto& path = paths[(size_t) q];
        path.factor = 2 << q;
        path.rate = sampleRate * path.factor;
        path.padding = (path.factor - dsp::AntialiasedCurve::delay % path.factor) % path.factor;
        path.ownLatency = getOwnLatency (q);
        path.delay = std::max (0, latency - path.ownLatency);
        path.smoothing = (float) (1.0 - std::exp (-1.0 / (driveSeconds * path.rate)));

        for (auto* halfBands : { &path.clean.filters, &path.driven.filters, &path.dryFilters })
        {
            *halfBands = std::make_unique<juce::dsp::Oversampling<float>> (
                2, (size_t) q + 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
            (*halfBands)->initProcessing ((size_t) capacity);
        }

        for (auto* lines : { &path.cleanLate, &path.cleanDryLate, &path.drivenLate, &path.drivenDryLate })
            for (auto& line : *lines)
                line.prepare (path.delay + 1);

        for (auto* buffer : { &path.cleanOut, &path.cleanDryOut, &path.drivenOut, &path.drivenDryOut, &path.scratch, &path.dryOut })
            buffer->setSize (2, capacity);

        path.blends.assign ((size_t) capacity, 0.0f);
    }

    for (auto* lines : { &undriven, &dryUndriven })
        for (auto& line : *lines)
            line.prepare (latency);

    undrivenOut.setSize (2, capacity);
    cleanIn.setSize (2, capacity);
    steps.assign ((size_t) capacity + 1, Step {});
    fades.assign ((size_t) capacity, 0.0f);

    for (auto& filter : filters)
        filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));

    reset();
}

void FilterEffect::reset()
{
    for (auto& path : paths)
    {
        for (auto* halfBands : { &path.clean.filters, &path.driven.filters, &path.dryFilters })
            if (*halfBands != nullptr)
                (*halfBands)->reset();

        for (auto* curves : { &path.clean.curves, &path.driven.ceilings, &path.dryStraight })
            for (auto& curve : *curves)
                curve.reset();

        for (auto& loop : path.driven.loops)
            loop.resetDriven();

        for (auto* lines : { &path.cleanLate, &path.cleanDryLate, &path.drivenLate, &path.drivenDryLate })
            for (auto& line : *lines)
                line.reset();

        for (auto* padded : { &path.cleanPadded, &path.drivenPadded, &path.dryPadded })
            for (auto& channel : *padded)
                channel.fill (0.0f);

        path.clean.driveBlend = 0.0f;
        path.clean.wasDriven = false;
        path.driven.madeOctaves = -1.0f;
        path.dryInStep = false;
    }

    for (auto& filter : filters)
        filter.reset();

    for (auto* lines : { &undriven, &dryUndriven })
        for (auto& line : *lines)
            line.reset();

    lastMix = 1.0f;
    isFirstBlock = true;
    stepLeft = 0;
    nextSongSample = -1;
    active = 0;
    next = -1;
    fade = 0.0f;
}

double FilterEffect::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

bool FilterEffect::sleepsOnSilence() const noexcept
{
    return ! playedDriven.load (std::memory_order_relaxed);
}

void FilterEffect::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (paths[0].clean.filters == nullptr || numChannels <= 0 || block.numSamples <= 0)
        return;

    playedDriven.store (isDriven (values), std::memory_order_relaxed);

    // The way it plays from the start, from rest, its drive where it is set.
    if (isFirstBlock)
    {
        isFirstBlock = false;
        octaves = toOctaves (values[cutoff]);
        const auto targetDrive = dsp::decibelsToGain (values[drive]);
        active = getQualityIndex (values[quality]) + (isDriven (values) ? numQualities : 0);
        auto& path = paths[(size_t) (active % numQualities)];
        path.clean.driveGain = targetDrive;
        path.driven.driveGain = targetDrive;
        path.driven.giveBack = 1.0f / std::sqrt (targetDrive);

        if (isDrivenWay (active))
            makeDrivenCoefficients (path, values, octaves);
    }

    // What came in, for the mix: through the half-band filters only while the mix uses it (from silence then, as the mix
    // leaves 100 %).
    const auto usesDry = block.dry != nullptr && (values[mix] < 1.0f || lastMix < 1.0f);
    lastMix = values[mix];

    // In stretches as long as the block it was prepared for, at most, each ending where a fade to another way ends, so that a
    // change waiting for it starts on the same sample whatever the blocks.
    for (int done = 0; done < block.numSamples;)
        done += play (block, done, std::min (capacity, block.numSamples - done), usesDry);

    // How long it rings once its input stops: at the lower of where its cutoff is and where it is gliding to. A comb rings as
    // its feedback says; the Driven filter as its damping for a quiet sound does, and on for ever where it sings; the others
    // fall silent within a few milliseconds.
    const auto lowestHertz = std::min (toHertz (octaves), (double) values[cutoff]);
    const auto lowest = makeCoefficients (values, lowestHertz, sampleRate);
    auto tail = 0.0;

    if (lowest.mode == dsp::FilterMode::combPlus || lowest.mode == dsp::FilterMode::combMinus)
    {
        tail = dsp::getRingSeconds (lowest.comb, sampleRate);
    }
    else if (isDriven (values) || isDrivenWay (active) || (next >= 0 && isDrivenWay (next)))
    {
        const auto mode = dsp::toFilterMode (values[filterType]);
        const auto damping =
            hasDriven (mode) ? (double) dsp::getDrivenDamping (mode, dsp::isSteep (values[filterSlope]), values[resonance]) : 1.0;
        tail = damping <= 0.0 ? longestTailSeconds
                              : std::min (longestTailSeconds, std::log (10.0) * ringDecibels / 20.0 * 2.0
                                                                  / (damping * juce::MathConstants<double>::twoPi * lowestHertz));
    }

    tailSeconds.store ((float) tail, std::memory_order_relaxed);
}

int FilterEffect::workOutFades (int most) noexcept
{
    if (next < 0)
        return most;

    for (int i = 0; i < most; ++i)
    {
        if (warmed < warmUp)
        {
            ++warmed;
            fades[(size_t) i] = 0.0f;
            continue;
        }

        fade = std::min (1.0f, fade + fadePerSample);
        fades[(size_t) i] = getEased (fade);

        // The fade ends the stretch, so that the way faded to plays alone from the next sample, whatever the blocks.
        if (fade >= 1.0f)
            return i + 1;
    }

    return most;
}

void FilterEffect::workOutSteps (const ProcessContext& block, int start, int length, float targetOctaves) noexcept
{
    // New coefficients on the song's steps of coefficientStep samples, each made as the step starts for where the cutoff
    // will be at its end; a step a block's end cuts goes on in the next block with them. So a cutoff that moves (its
    // automation, on the song's control points, which are whole steps apart) plays the same in blocks of any size. Where the
    // song didn't go on from the last block (a jump, the song stopped), a step starts afresh.
    const auto from = block.songSample + start;
    numSteps = 0;

    if (from != nextSongSample)
        stepLeft = 0;

    nextSongSample = from + length;

    for (int at = 0, end = 0; at < length; at = end)
    {
        if (stepLeft == 0)
        {
            const auto sinceStep = (int) (((from + at) % coefficientStep + coefficientStep) % coefficientStep);
            stepLeft = coefficientStep - sinceStep;
            auto& step = steps[(size_t) numSteps++];
            step.at = at;
            step.length = stepLeft;
            step.moved = false;

            // The cutoff glides in octaves, so a sweep sounds even all the way; a comb's delay glides between the coefficients
            // too, over the step.
            if (std::abs (targetOctaves - octaves) > 1.0e-4f)
            {
                octaves += (targetOctaves - octaves) * (1.0f - std::pow (1.0f - glide, (float) stepLeft));
                step.moved = true;
            }

            step.octaves = octaves;
        }

        end = std::min (length, at + stepLeft);
        stepLeft -= end - at;
    }
}

void FilterEffect::makeDrivenCoefficients (Path& path, std::span<const float> values, float atOctaves) noexcept
{
    auto& stage = path.driven;

    if (juce::exactlyEqual (atOctaves, stage.madeOctaves) && juce::exactlyEqual (values[filterType], stage.madeType)
        && juce::exactlyEqual (values[resonance], stage.madeResonance) && juce::exactlyEqual (values[filterSlope], stage.madeSlope))
        return;

    stage.madeOctaves = atOctaves;
    stage.madeType = values[filterType];
    stage.madeResonance = values[resonance];
    stage.madeSlope = values[filterSlope];
    stage.coefficients = dsp::makeDriven (makeSettings (values, toHertz (atOctaves)), path.rate);

    // Both channels, whatever the block has, so that what the cache says it made, each has.
    for (auto& loop : stage.loops)
        loop.setDriven (stage.coefficients);
}

void FilterEffect::startWay (int way, std::span<const float> values, float targetDrive) noexcept
{
    next = way;
    warmed = 0;
    fade = 0.0f;
    warmUp = std::max (warmUpLeast, (int) std::lround (std::min (longestWarmSeconds, settlingCycles / toHertz (octaves)) * sampleRate));

    auto& path = paths[(size_t) (way % numQualities)];
    const auto& from = paths[(size_t) (active % numQualities)];

    if (! isDrivenWay (way))
    {
        // The Clean drive from rest, where the one playing has its drive, or coming on from the plain delay.
        auto& stage = path.clean;
        stage.filters->reset();

        for (auto& curve : stage.curves)
            curve.reset();

        for (auto& channel : path.cleanPadded)
            channel.fill (0.0f);

        // Its plain delays hold what the plain sound and its copy would have left in them, so that the Clean filter, which
        // may take what comes out of it at once (from Driven), goes on from where the plain sound was.
        for (size_t channel = 0; channel < 2; ++channel)
        {
            for (int d = path.delay; d >= 1; --d)
            {
                path.cleanLate[channel].push (undriven[channel].read (path.ownLatency + d - 1));
                path.cleanDryLate[channel].push (dryUndriven[channel].read (path.ownLatency + d - 1));
            }
        }

        stage.driveGain = isDrivenWay (active) ? from.driven.driveGain : from.clean.driveGain;
        stage.driveBlend = isDrivenWay (active) ? 0.0f : from.clean.driveBlend;
        stage.wasDriven = false;
        return;
    }

    // The Driven filter: from rest, or taking the loop of the Driven filter playing, at its own rate.
    auto& stage = path.driven;
    stage.filters->reset();

    for (auto& ceiling : stage.ceilings)
        ceiling.reset();

    for (auto& channel : path.drivenPadded)
        channel.fill (0.0f);

    for (auto* lines : { &path.drivenLate, &path.drivenDryLate })
        for (auto& line : *lines)
            line.reset();

    stage.driveGain = isDrivenWay (active) ? from.driven.driveGain : targetDrive;
    stage.giveBack = 1.0f / std::sqrt (stage.driveGain);
    stage.madeOctaves = -1.0f;
    makeDrivenCoefficients (path, values, octaves);

    for (size_t channel = 0; channel < 2; ++channel)
    {
        if (isDrivenWay (active))
        {
            const auto& playing = from.driven.loops[channel];
            stage.loops[channel].takeDrivenState (playing, stage.coefficients.g / std::max (1.0e-9f, playing.getDriven().g));
        }
        else
        {
            stage.loops[channel].resetDriven();
        }
    }
}

int FilterEffect::play (const ProcessContext& block, int start, int most, bool usesDry)
{
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto targetDrive = dsp::decibelsToGain (values[drive]);
    const auto wanted = getQualityIndex (values[quality]) + (isDriven (values) ? numQualities : 0);

    // Another Character or Quality: it starts beside the way playing, unheard, and the sound fades to it; another change
    // meanwhile waits for the fade's end.
    if (next < 0 && wanted != active)
        startWay (wanted, values, targetDrive);

    const auto length = workOutFades (most);
    const auto startOctaves = octaves;
    workOutSteps (block, start, length, toOctaves (values[cutoff]));

    const int ways[] = { active, next };
    const auto numWaysPlaying = next >= 0 ? 2 : 1;
    const auto isClean = [] (int way) { return way >= 0 && ! isDrivenWay (way); };
    const auto bothClean = isClean (active) && isClean (next);

    // What came in, plain, as late as each way's own filters make it, and as late as every Quality (for the Clean filter, to
    // keep it warm while no Clean way plays).
    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto& line = undriven[(size_t) channel];
        const auto* in = block.channels[channel] + start;

        for (int i = 0; i < length; ++i)
        {
            line.push (in[i]);
            undrivenOut.setSample (channel, i, line.read (latency));

            for (int w = 0; w < numWaysPlaying; ++w)
                if (isClean (ways[w]))
                    paths[(size_t) ways[w]].cleanOut.setSample (channel, i, line.read (paths[(size_t) ways[w]].ownLatency));
        }

        if (block.dry == nullptr)
            continue;

        auto& dryLine = dryUndriven[(size_t) channel];
        const auto* came = block.dry[channel] + start;

        for (int i = 0; i < length; ++i)
        {
            dryLine.push (came[i]);

            for (int w = 0; w < numWaysPlaying; ++w)
            {
                auto& path = paths[(size_t) (ways[w] % numQualities)];
                (isClean (ways[w]) ? path.cleanDryOut : path.drivenDryOut).setSample (channel, i, dryLine.read (path.ownLatency));
            }
        }
    }

    // The copy through each playing Quality's half-band filters, where a way uses it: the Driven filter whenever the mix uses
    // the copy, the Clean drive while the sound goes through it too.
    for (auto& path : paths)
        path.dryNeeded = false;

    for (int w = 0; w < numWaysPlaying; ++w)
    {
        auto& path = paths[(size_t) (ways[w] % numQualities)];
        const auto through = isDrivenWay (ways[w]) || targetDrive > 1.0001f || path.clean.driveBlend > 0.0f;
        path.dryNeeded = path.dryNeeded || (usesDry && through);
    }

    for (auto& path : paths)
    {
        if (path.dryNeeded)
            playDry (path, block, start, length);

        path.dryInStep = path.dryNeeded;
    }

    for (int w = 0; w < numWaysPlaying; ++w)
    {
        auto& path = paths[(size_t) (ways[w] % numQualities)];

        if (isDrivenWay (ways[w]))
            playDriven (path, block, start, length, targetDrive, usesDry);
        else
            playClean (path, block, start, length, targetDrive, usesDry);
    }

    // The Clean filter, on the Clean drive (two faded from one to the other, as a Quality changes), or on the plain delay.
    const auto* activeClean = isClean (active) ? &paths[(size_t) active].cleanOut : nullptr;
    const auto* nextClean = isClean (next) ? &paths[(size_t) next].cleanOut : nullptr;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* into = cleanIn.getWritePointer (channel);

        if (activeClean != nullptr && nextClean != nullptr)
        {
            const auto* a = activeClean->getReadPointer (channel);
            const auto* b = nextClean->getReadPointer (channel);

            for (int i = 0; i < length; ++i)
                into[i] = a[i] + fades[(size_t) i] * (b[i] - a[i]);
        }
        else
        {
            const auto* source = activeClean != nullptr ? activeClean : (nextClean != nullptr ? nextClean : &undrivenOut);
            std::copy (source->getReadPointer (channel), source->getReadPointer (channel) + length, into);
        }
    }

    playCleanFilter (values, length, startOctaves, numChannels);

    // What goes out: the way playing, faded to the next (two Clean ways were faded before the filter).
    const auto soundOf = [this] (int way) -> const juce::AudioBuffer<float>&
    { return isDrivenWay (way) ? paths[(size_t) (way % numQualities)].drivenOut : cleanIn; };
    const auto copyOf = [this] (int way) -> const juce::AudioBuffer<float>&
    {
        auto& path = paths[(size_t) (way % numQualities)];
        return isDrivenWay (way) ? path.drivenDryOut : path.cleanDryOut;
    };

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* out = block.channels[channel] + start;
        const auto* a = soundOf (active).getReadPointer (channel);

        if (next >= 0 && ! bothClean)
        {
            const auto* b = soundOf (next).getReadPointer (channel);

            for (int i = 0; i < length; ++i)
                out[i] = a[i] + fades[(size_t) i] * (b[i] - a[i]);
        }
        else
        {
            std::copy (a, a + length, out);
        }

        if (block.dry == nullptr)
            continue;

        auto* copy = block.dry[channel] + start;
        const auto* dryA = copyOf (active).getReadPointer (channel);

        if (next >= 0)
        {
            const auto* dryB = copyOf (next).getReadPointer (channel);

            for (int i = 0; i < length; ++i)
                copy[i] = dryA[i] + fades[(size_t) i] * (dryB[i] - dryA[i]);
        }
        else
        {
            std::copy (dryA, dryA + length, copy);
        }
    }

    if (next >= 0 && fade >= 1.0f)
    {
        active = next;
        next = -1;
        fade = 0.0f;
    }

    return length;
}

void FilterEffect::playClean (Path& path, const ProcessContext& block, int start, int length, float targetDrive, bool turned) noexcept
{
    auto& stage = path.clean;
    const auto numChannels = std::min (block.numChannels, 2);
    const auto driveOn = targetDrive > 1.0001f;
    const auto through = driveOn || stage.driveBlend > 0.0f;

    if (! through)
    {
        // Undriven: the sound and its copy as they came, plain, already in cleanOut and cleanDryOut.
        stage.driveGain = targetDrive;
    }
    else
    {
        // Coming on: from silence, faded in.
        if (stage.driveBlend <= 0.0f)
        {
            stage.filters->reset();

            for (auto& curve : stage.curves)
                curve.reset();

            for (auto& channel : path.cleanPadded)
                channel.fill (0.0f);
        }

        // Faster, the drive saturating what goes in, its quiet parts keeping their level (near 0 dB, straight).
        const float* in[2] = { block.channels[0] + start, numChannels > 1 ? block.channels[1] + start : nullptr };
        const juce::dsp::AudioBlock<const float> audio (in, (size_t) numChannels, (size_t) length);
        auto fast = stage.filters->processSamplesUp (audio);

        for (size_t i = 0; i < fast.getNumSamples(); ++i)
        {
            stage.driveGain += (targetDrive - stage.driveGain) * path.smoothing;
            const auto driven = stage.driveGain > 1.0001f;

            if (driven != stage.wasDriven)
            {
                for (auto& curve : stage.curves)
                    curve.restartAveraging();

                stage.wasDriven = driven;
            }

            ++path.cleanPaddedAt;

            for (size_t channel = 0; channel < fast.getNumChannels(); ++channel)
            {
                auto& sample = fast.getChannelPointer (channel)[i];
                sample = driven ? stage.curves[channel].process (sample * stage.driveGain, dsp::SoftCurve {}) / stage.driveGain
                                : stage.curves[channel].process (sample, dsp::StraightCurve {});

                if (path.padding > 0)
                    sample = pad (path.cleanPadded[channel], path.cleanPaddedAt, path.padding, sample);
            }
        }

        juce::dsp::AudioBlock<float> driven (path.scratch.getArrayOfWritePointers(), (size_t) numChannels, (size_t) length);
        stage.filters->processSamplesDown (driven);

        // Faded over 64 samples from the plain delay to the drive, or back.
        constexpr auto blendStep = 1.0f / 64.0f;
        const auto blendTarget = driveOn ? 1.0f : 0.0f;
        auto blend = stage.driveBlend;

        for (int i = 0; i < length; ++i)
        {
            blend += std::clamp (blendTarget - blend, -blendStep, blendStep);
            path.blends[(size_t) i] = blend;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const auto plain = path.cleanOut.getSample (channel, i);
                path.cleanOut.setSample (channel, i, plain + blend * (path.scratch.getSample (channel, i) - plain));
            }
        }

        stage.driveBlend = blend;

        // Its copy for the mix, through the drive's half-band filters while the mix uses it, faded as the sound is.
        if (block.dry != nullptr && turned)
        {
            for (int channel = 0; channel < numChannels; ++channel)
            {
                for (int i = 0; i < length; ++i)
                {
                    const auto plain = path.cleanDryOut.getSample (channel, i);
                    path.cleanDryOut.setSample (channel, i, plain + path.blends[(size_t) i] * (path.dryOut.getSample (channel, i) - plain));
                }
            }
        }
    }

    late (path, path.cleanOut, path.cleanLate, numChannels, length);

    if (block.dry != nullptr)
        late (path, path.cleanDryOut, path.cleanDryLate, numChannels, length);
}

void FilterEffect::playDriven (Path& path, const ProcessContext& block, int start, int length, float targetDrive, bool usesDry) noexcept
{
    auto& stage = path.driven;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    const float* in[2] = { block.channels[0] + start, numChannels > 1 ? block.channels[1] + start : nullptr };
    const juce::dsp::AudioBlock<const float> audio (in, (size_t) numChannels, (size_t) length);
    auto fast = stage.filters->processSamplesUp (audio);
    const auto factor = path.factor;
    auto stepIndex = 0;

    for (int i = 0; i < length; ++i)
    {
        // New coefficients on the song's steps, as the Clean filter takes them, worked out for this rate.
        if (stepIndex < numSteps && steps[(size_t) stepIndex].at == i)
            makeDrivenCoefficients (path, values, steps[(size_t) stepIndex++].octaves);

        for (int k = 0; k < factor; ++k)
        {
            const auto j = (size_t) (i * factor + k);

            // The drive glides; half of it, in decibels, is given back after the loop.
            if (! juce::exactlyEqual (stage.driveGain, targetDrive))
            {
                stage.driveGain += (targetDrive - stage.driveGain) * path.smoothing;

                if (std::abs (stage.driveGain - targetDrive) < 1.0e-6f * targetDrive)
                    stage.driveGain = targetDrive;

                stage.giveBack = 1.0f / std::sqrt (stage.driveGain);
            }

            ++path.drivenPaddedAt;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto& sample = fast.getChannelPointer ((size_t) channel)[j];
                const auto looped = stage.loops[(size_t) channel].processDriven (sample * stage.driveGain) * stage.giveBack;
                sample = stage.ceilings[(size_t) channel].process (looped, dsp::KneeCurve {});

                if (path.padding > 0)
                    sample = pad (path.drivenPadded[(size_t) channel], path.drivenPaddedAt, path.padding, sample);
            }
        }
    }

    juce::dsp::AudioBlock<float> out (path.drivenOut.getArrayOfWritePointers(), (size_t) numChannels, (size_t) length);
    stage.filters->processSamplesDown (out);
    late (path, path.drivenOut, path.drivenLate, numChannels, length);

    // Its copy for the mix: through the same half-band filters while the mix uses it, plain otherwise.
    if (block.dry == nullptr)
        return;

    if (usesDry)
        for (int channel = 0; channel < numChannels; ++channel)
            path.drivenDryOut.copyFrom (channel, 0, path.dryOut, channel, 0, length);

    late (path, path.drivenDryOut, path.drivenDryLate, numChannels, length);
}

void FilterEffect::playDry (Path& path, const ProcessContext& block, int start, int length) noexcept
{
    const auto numChannels = std::min (block.numChannels, 2);

    if (! path.dryInStep)
    {
        path.dryFilters->reset();

        for (auto& straight : path.dryStraight)
            straight.reset();

        for (auto& channel : path.dryPadded)
            channel.fill (0.0f);
    }

    const float* came[2] = { block.dry[0] + start, numChannels > 1 ? block.dry[1] + start : nullptr };
    const juce::dsp::AudioBlock<const float> copy (came, (size_t) numChannels, (size_t) length);
    auto fast = path.dryFilters->processSamplesUp (copy);

    for (size_t i = 0; i < fast.getNumSamples(); ++i)
    {
        ++path.dryPaddedAt;

        for (size_t channel = 0; channel < fast.getNumChannels(); ++channel)
        {
            auto& sample = fast.getChannelPointer (channel)[i];
            sample = path.dryStraight[channel].process (sample, dsp::StraightCurve {});

            if (path.padding > 0)
                sample = pad (path.dryPadded[channel], path.dryPaddedAt, path.padding, sample);
        }
    }

    juce::dsp::AudioBlock<float> out (path.dryOut.getArrayOfWritePointers(), (size_t) numChannels, (size_t) length);
    path.dryFilters->processSamplesDown (out);
}

void FilterEffect::playCleanFilter (std::span<const float> values, int length, float startOctaves, int numChannels) noexcept
{
    // The coefficients as the stretch starts, and new ones where a step glided; each step sets them, a comb's delay gliding
    // over it.
    auto coefficients = makeCoefficients (values, toHertz (startOctaves), sampleRate);
    auto stepIndex = 0;

    for (int i = 0; i < length;)
    {
        if (stepIndex < numSteps && steps[(size_t) stepIndex].at == i)
        {
            const auto& step = steps[(size_t) stepIndex++];

            if (step.moved)
                coefficients = makeCoefficients (values, toHertz (step.octaves), sampleRate);

            for (int channel = 0; channel < numChannels; ++channel)
                filters[(size_t) channel].set (coefficients, step.length);
        }

        const auto end = stepIndex < numSteps ? steps[(size_t) stepIndex].at : length;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* samples = cleanIn.getWritePointer (channel);

            for (int j = i; j < end; ++j)
                samples[j] = filters[(size_t) channel].process (samples[j]);
        }

        i = end;
    }
}

void FilterEffect::late (const Path& path, juce::AudioBuffer<float>& buffer, std::array<dsp::DelayLine, 2>& lines, int numChannels,
                         int length) noexcept
{
    if (path.delay <= 0)
        return;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto& line = lines[(size_t) channel];
        auto* samples = buffer.getWritePointer (channel);

        for (int i = 0; i < length; ++i)
        {
            line.push (samples[i]);
            samples[i] = line.read (path.delay);
        }
    }
}
} // namespace phazite::devices

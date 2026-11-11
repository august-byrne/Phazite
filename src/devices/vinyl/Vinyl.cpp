// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Vinyl.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "wow", .name = "Wow", .maximum = 25.0f, .defaultValue = 4.0f, .unit = Unit::cents, .midpoint = 6.0f },
        { .id = "wowRate",
          .name = "Wow Rate",
          .minimum = 0.25f,
          .maximum = 4.0f,
          .defaultValue = 0.5555556f, // 33 1/3 rpm
          .unit = Unit::hertz,
          .midpoint = 1.0f },
        { .id = "flutter", .name = "Flutter", .maximum = 10.0f, .defaultValue = 0.0f, .unit = Unit::cents, .midpoint = 2.5f },
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 30.0f,
          .unit = Unit::hertz,
          .midpoint = 120.0f },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 14000.0f,
          .unit = Unit::hertz,
          .midpoint = 5000.0f },
        { .id = "distortion", .name = "Distortion", .defaultValue = 0.15f, .unit = Unit::percent },
        { .id = "width", .name = "Width", .defaultValue = 0.9f, .unit = Unit::percent },
        { .id = "bassMono",
          .name = "Bass Mono",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .midpoint = 120.0f },
        { .id = "hiss",
          .name = "Hiss",
          .minimum = -96.0f,
          .maximum = -30.0f,
          .defaultValue = -64.0f,
          .unit = Unit::decibels,
          .midpoint = -60.0f },
        { .id = "crackle",
          .name = "Crackle",
          .minimum = -96.0f,
          .maximum = -6.0f,
          .defaultValue = -30.0f,
          .unit = Unit::decibels,
          .midpoint = -36.0f },
        { .id = "density",
          .name = "Density",
          .minimum = 0.5f,
          .maximum = 500.0f,
          .defaultValue = 5.0f,
          .unit = Unit::hertz,
          .midpoint = 15.0f },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr double butterworthQ = 0.70710678;
    constexpr double centsToRatio = 0.00057778950655485; // ln 2 / 1200
    constexpr double highsHertz = 2000.0;                // the curve bends what is above this
    constexpr double flutterHertz = 6.0;                 // the flutter's noise is under this
    constexpr double flutterMostRms = 4.5;               // the flutter moves the delay at most this many RMS either way
    constexpr double residueLowCut = 20.0;               // what the curve adds, without anything under this
    constexpr double wowGlideSeconds = 0.05, glideSeconds = 0.02, bandGlideSeconds = 0.005, flutterGlideSeconds = 0.3;
    constexpr double mostSwoop = 0.004;   // how far a change of the wow may bend the pitch as it glides: 7 cents
    constexpr int splitStep = 32;         // samples between the split's moves as it glides
    constexpr float soundFloor = 3.2e-5f; // -90 dBFS: anything over this coming in is sound, for the noise's hold
    constexpr float silence = 1.0e-6f;    // -120 dBFS: under this, nothing comes in (as its runner hears it)
    constexpr double restSeconds = 0.1;   // and after the delay line, this long for the filters to ring out, before it rests

    // The groove's curve: softClip (Math.h) about a point a little off its middle, so that it is lopsided (a second harmonic
    // first, a third as it is pushed), less what the point itself gives and as steep there as a straight line. With its
    // antiderivative, for the curve's antiderivative anti-aliasing (dsp/Saturation.h).
    struct GrooveCurve
    {
        static constexpr float bias = 0.25f;

        float rest = 0.0f, steep = 1.0f;
        double restIntegral = 0.0;

        GrooveCurve() noexcept
            : rest (dsp::softClip (bias)),
              restIntegral (dsp::SoftCurve::integral (bias))
        {
            const auto root = (9.0f - bias * bias) / (3.0f * (3.0f + bias * bias));
            steep = 1.0f / (root * root); // one over softClip's slope at the point
        }

        float value (float x) const noexcept { return (dsp::softClip (x + bias) - rest) * steep; }

        double integral (double x) const noexcept
        {
            return (dsp::SoftCurve::integral (x + bias) - restIntegral - (double) rest * x) * (double) steep;
        }
    };

    const GrooveCurve& getGrooveCurve()
    {
        static const GrooveCurve curve;
        return curve;
    }

    // How hard the curve is driven at a distortion: from as loud as the sound at 0 to 12 dB louder at 100 %.
    float getDrive (float distortionValue) noexcept
    {
        return 1.0f + 3.0f * distortionValue;
    }

    // A ratio a little over 1 from cents: e^x - 1, near enough for the few cents a wow and a flutter reach.
    double getSwing (double cents) noexcept
    {
        const auto x = cents * centsToRatio;
        return x * (1.0 + x * (0.5 + x / 6.0));
    }

    // A filter of the band's a step nearer where it is going, its frequency gliding there (as the tangent the filter keeps of
    // it); its mix of outputs stays its shape's.
    void glide (dsp::FilterCoefficients& now, const dsp::FilterCoefficients& target, float smoothing) noexcept
    {
        if (juce::exactlyEqual (now.g, target.g))
            return;

        const auto g = now.g + (target.g - now.g) * smoothing;

        if (std::abs (target.g - g) <= 1.0e-6f * target.g)
        {
            now = target;
            return;
        }

        now = target;
        now.g = g;
        now.a1 = 1.0f / (1.0f + g * (g + now.k));
        now.a2 = g * now.a1;
        now.a3 = g * now.a2;
    }

    // A value taken softly back inside a reach: the same up to 80 % of it, then bent to stop at it.
    double softLimit (double value, double reach) noexcept
    {
        const auto knee = 0.8 * reach, size = std::abs (value);

        if (size <= knee)
            return value;

        const auto bent = knee + 0.2 * reach * std::tanh ((size - knee) / (0.2 * reach));
        return value < 0.0 ? -bent : bent;
    }
} // namespace

const DeviceInfo Vinyl::info { .typeID = typeID, .name = "Vinyl", .kind = DeviceKind::effect, .parameters = parameters };

bool Vinyl::isOff (std::span<const float> values, Parameter which)
{
    const auto& parameter = parameters[(size_t) which];
    return values[(size_t) which] <= parameter.minimum + (which == bassMono ? 0.5f : 0.01f);
}

float Vinyl::getWowCents (float wowCents, float turnPart) noexcept
{
    const auto angle = twoPi * (double) turnPart;
    const auto swing = getSwing (std::max (0.0f, wowCents));
    return (float) (1200.0 * std::log2 (1.0 + swing * (std::cos (angle) - 0.25 * std::cos (2.0 * angle))));
}

double Vinyl::getBandGain (std::span<const float> values, double frequency, double rate)
{
    return dsp::getMagnitude (dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], rate, butterworthQ), frequency, rate)
           * dsp::getMagnitude (dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], rate, butterworthQ), frequency, rate);
}

float Vinyl::shapeSample (float distortionValue, float sample) noexcept
{
    const auto amount = std::clamp (distortionValue, 0.0f, 1.0f);
    const auto drive = getDrive (amount);
    return sample + amount * (getGrooveCurve().value (drive * sample) / drive - sample);
}

double Vinyl::getTailSeconds() const
{
    return holdSeconds + 0.05 + latency / sampleRate;
}

void Vinyl::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    const auto fastRate = 2.0 * sampleRate;
    getGrooveCurve(); // made now, off the audio thread

    // The delay line's middle: as far as the deepest wow at the slowest rate swings it (its turn's sine and the quarter as
    // much again at twice the rate, an eighth as far), and the deepest flutter, with room for the interpolation; then made the
    // device's latency in whole samples, with the oversampling's and its curve's few.
    const auto& wowInfo = parameters[wow];
    const auto slowest = (double) parameters[wowRate].minimum;
    const auto wowReach = getSwing (wowInfo.maximum) / (twoPi * slowest) * 1.125 * fastRate;
    const auto flutterRms = getSwing (parameters[flutter].maximum) / std::sqrt (2.0);
    flutterReach = flutterMostRms * flutterRms / (twoPi * flutterHertz) * fastRate;
    const auto shaperLatency = (double) dsp::AntialiasedShaper::getLatencySamples();
    latency = (int) std::ceil ((wowReach + flutterReach + 4.0) / 2.0 + shaperLatency);
    centre = 2.0 * (latency - shaperLatency);

    for (auto& channel : channels)
        channel.line.prepare ((int) std::ceil (centre + wowReach + flutterReach) + 4);

    highsShape = dsp::makeFilter (dsp::FilterShape::highPass, highsHertz, fastRate, butterworthQ);
    residuePole = (float) std::exp (-twoPi * residueLowCut / fastRate);

    // The flutter: noise, low-passed at its corner, made a delay whose slope swings the pitch as far as its depth says. Even
    // noise has a variance of a third; through a Butterworth low-pass at w its variance is that times w / (2 sqrt 2 fs), and
    // its slope's RMS w times its own.
    flutterShape = dsp::makeFilter (dsp::FilterShape::lowPass, flutterHertz, sampleRate, butterworthQ);
    const auto corner = twoPi * flutterHertz;
    const auto noiseRms = std::sqrt ((1.0 / 3.0) * corner / (2.0 * std::sqrt (2.0) * sampleRate));
    flutterScale = (float) (fastRate / (corner * noiseRms));

    wowSmoothing = (float) (1.0 - std::exp (-1.0 / (wowGlideSeconds * sampleRate)));
    flutterSmoothing = (float) (1.0 - std::exp (-1.0 / (flutterGlideSeconds * sampleRate)));
    swingStep = mostSwoop * 2.0; // the delay moves this many samples at twice the rate a sample: mostSwoop of a second a second
    smoothing = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    bandSmoothing = (float) (1.0 - std::exp (-1.0 / (bandGlideSeconds * sampleRate)));
    splitSmoothing = (float) (1.0 - std::exp (-(double) splitStep / (glideSeconds * sampleRate)));
    holdSamples = (int) std::lround (holdSeconds * sampleRate);
    restAfter = 2 * latency + (int) std::lround (restSeconds * sampleRate);
    readingEvery = std::max (1, (int) std::lround (sampleRate / pitchReadingsPerSecond));

    noise.prepare (sampleRate);
    reset();
}

void Vinyl::restCurves() noexcept
{
    for (auto& channel : channels)
    {
        channel.bent.reset();
        channel.straight.reset();
        channel.highs.reset();
        channel.residueLast = channel.residueOut = 0.0f;
    }

    curveRests = true;
}

void Vinyl::reset()
{
    for (auto& channel : channels)
    {
        channel.oversampler.reset();
        channel.line.reset();
        channel.late.fill (0.0f);
        channel.lateAt = 0;
        channel.lowCut.reset();
        channel.highCut.reset();
    }

    restCurves();
    sideSplit.reset();
    middlePhase.reset();
    flutterFilter.reset();
    flutterNoise.setSeed (4507u);
    turnCos = 1.0;
    turnSin = 0.0;
    stepRate = -1.0;
    sinceNormalised = sinceSplitMoved = sinceReading = 0;
    noise.reset();
    wasPlaying = heardSound = false;
    silentFor = restingFor = 0;
    isFirstBlock = true;
}

void Vinyl::setSplit (double frequency) noexcept
{
    splitFrequency = frequency;
    sideSplit.setFrequency (frequency, sampleRate);
    middlePhase.setFrequency (frequency, sampleRate);
}

// The record a part of a turn further, by a pair that turns it as far each sample, worked out again only as the rate moves.
void Vinyl::turnBy (double samples) noexcept
{
    const auto angle = twoPi * (double) rateNow / sampleRate * samples;
    const auto c = std::cos (angle), s = std::sin (angle);
    const auto turnedCos = turnCos * c - turnSin * s;
    turnSin = turnSin * c + turnCos * s;
    turnCos = turnedCos;
}

float Vinyl::getTurnNow() const noexcept
{
    const auto part = std::atan2 (turnSin, turnCos) / twoPi;
    return (float) (part < 0.0 ? part + 1.0 : part);
}

// Nothing comes in and nothing plays: the output is silence, the record turns on, and its readings go on.
void Vinyl::rest (int numSamples) noexcept
{
    if (restingFor == restAfter) // its first resting block: what it holds is nothing now, exactly
    {
        for (auto& channel : channels)
        {
            channel.oversampler.reset();
            channel.line.reset();
            channel.late.fill (0.0f);
            channel.lowCut.reset();
            channel.highCut.reset();
        }

        restCurves();
        sideSplit.reset();
        middlePhase.reset();
        ++restingFor;
    }

    turnBy (numSamples);
    sinceReading += numSamples;

    if (sinceReading >= readingEvery)
    {
        sinceReading %= readingEvery;
        const auto reading = getWowCents (wowNow, getTurnNow());
        pitchTap.push (&reading, &reading, 1);
    }
}

void Vinyl::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0 || block.numSamples <= 0)
        return;

    // Where each parameter is going.
    const auto targetWow = std::max (0.0f, values[wow]);
    const auto targetRate = std::max (0.01f, values[wowRate]);
    const auto targetFlutter = (float) (getSwing (std::max (0.0f, values[flutter])) / std::sqrt (2.0));
    const auto targetDistortion = std::clamp (values[distortion], 0.0f, 1.0f);
    const auto targetWidth = std::clamp (values[width], 0.0f, 1.0f);
    const auto targetBass = isOff (values, bassMono) ? 0.0f : 1.0f;
    const auto hissGain = isOff (values, hiss) ? 0.0f : dsp::decibelsToGain (values[hiss]);
    const auto crackleGain = isOff (values, crackle) ? 0.0f : dsp::decibelsToGain (values[crackle]);
    const auto makesNoise = hissGain > 0.0f || crackleGain > 0.0f;
    lowCutTarget = dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], sampleRate, butterworthQ);
    highCutTarget = dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], sampleRate, butterworthQ);
    noise.setLevels (hissGain, crackleGain, std::max (0.0f, values[density]));

    // The record plays while the song does: its noise starts over with the song, and waits for sound only once some came.
    const auto playing = block.isPlaying || block.isRendering;

    if (playing && ! wasPlaying)
    {
        heardSound = false;
        silentFor = 0;
    }

    wasPlaying = playing;
    // The noise is on the record, as late as the sound: a moment of it is where the song was the latency ago.
    const auto beatsPerSample = playing ? block.tempo / 60.0 / sampleRate : 0.0;
    noise.setPosition (block.beat, beatsPerSample, -latency * beatsPerSample);

    if (isFirstBlock)
    {
        wowNow = targetWow;
        rateNow = targetRate;
        swingNow = getSwing (targetWow) / (twoPi * (double) targetRate) * 2.0 * sampleRate;
        flutterNow = targetFlutter;
        distortionNow = targetDistortion;
        widthNow = targetWidth;
        bassAmount = targetBass;
        setSplit (std::max ((double) values[bassMono], 20.0));
        lowCutNow = lowCutTarget;
        highCutNow = highCutTarget;
        noise.jumpTo (playing && makesNoise);
        lastDelay = centre;
        isFirstBlock = false;
    }

    // Resting: nothing coming in, no noise playing or about to, and its delay line long empty.
    auto quietIn = true;

    for (int channel = 0; channel < numChannels && quietIn; ++channel)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (block.channels[channel], block.numSamples);
        quietIn = range.getEnd() <= silence && range.getStart() >= -silence;
    }

    const auto noiseWanted = playing && makesNoise && (! heardSound || silentFor < holdSamples);

    if (quietIn && ! noiseWanted && ! noise.isSounding() && restingFor >= restAfter)
    {
        for (int channel = 0; channel < numChannels; ++channel)
            juce::FloatVectorOperations::clear (block.channels[channel], block.numSamples);

        silentFor = std::min (silentFor + block.numSamples, holdSamples);
        rest (block.numSamples);
        noise.skip (block.numSamples);
        turn.set (getTurnNow());
        pitchNow.set (getWowCents (wowNow, turn.get()));
        return;
    }

    if (! quietIn)
        restingFor = 0;

    const auto& groove = getGrooveCurve();
    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;
    auto lastPitch = 0.0f;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The parameters glide.
        if (! juce::exactlyEqual (rateNow, targetRate))
        {
            rateNow += (targetRate - rateNow) * wowSmoothing;

            if (std::abs (targetRate - rateNow) <= 1.0e-6f * targetRate)
                rateNow = targetRate;
        }

        wowNow += (targetWow - wowNow) * wowSmoothing;
        flutterNow += (targetFlutter - flutterNow) * flutterSmoothing;
        distortionNow += (targetDistortion - distortionNow) * smoothing;
        widthNow += (targetWidth - widthNow) * smoothing;
        bassAmount += (targetBass - bassAmount) * smoothing;

        if (targetDistortion <= 0.0f && distortionNow < 1.0e-6f)
            distortionNow = 0.0f;

        glide (lowCutNow, lowCutTarget, bandSmoothing);
        glide (highCutNow, highCutTarget, bandSmoothing);

        // What comes in, for the noise's hold.
        auto l = left[i], r = right != nullptr ? right[i] : left[i];

        if (std::abs (l) > soundFloor || std::abs (r) > soundFloor)
        {
            heardSound = true;
            silentFor = 0;
        }
        else if (silentFor < holdSamples)
        {
            ++silentFor;
        }

        // The cut: the lows in mono, the split gliding to its frequency in octaves.
        if (right != nullptr && (bassAmount > 0.0f || targetBass > 0.0f))
        {
            if (++sinceSplitMoved >= splitStep)
            {
                sinceSplitMoved = 0;
                const auto wanted = std::max ((double) values[bassMono], 20.0);

                if (targetBass > 0.0f && std::abs (std::log (wanted / splitFrequency)) > 1.0e-4)
                    setSplit (splitFrequency * std::pow (wanted / splitFrequency, (double) splitSmoothing));
            }

            auto middle = 0.5f * (l + r), side = 0.5f * (l - r);
            const auto highSide = sideSplit.process (side).high;
            const auto phasedMiddle = middlePhase.processAllPass (middle);
            side += bassAmount * (highSide - side);
            middle += bassAmount * (phasedMiddle - middle);
            l = middle + side;
            r = middle - side;

            if (targetBass <= 0.0f && bassAmount < 1.0e-6f)
            {
                bassAmount = 0.0f;
                sideSplit.reset();
                middlePhase.reset();
            }
        }

        // The wow: the record's turn, its swing and a quarter as much at twice the rate; and the flutter's noise, as delays.
        if (! juce::exactlyEqual ((double) rateNow, stepRate))
        {
            stepRate = rateNow;
            const auto step = twoPi * stepRate / sampleRate;
            stepCos = std::cos (step);
            stepSin = std::sin (step);
        }

        const auto turnedCos = turnCos * stepCos - turnSin * stepSin;
        turnSin = turnSin * stepCos + turnCos * stepSin;
        turnCos = turnedCos;

        if (++sinceNormalised >= 256)
        {
            sinceNormalised = 0;
            const auto size = 1.0 / std::sqrt (turnCos * turnCos + turnSin * turnSin);
            turnCos *= size;
            turnSin *= size;
        }

        // How far the wow swings the delay: its depth over the rate, gliding there no faster than swingStep a sample.
        const auto swingTarget = getSwing (wowNow) / (twoPi * (double) rateNow) * 2.0 * sampleRate;
        swingNow += std::clamp (swingTarget - swingNow, -swingStep, swingStep);
        const auto wowDelay = -swingNow * turnSin + 0.125 * swingNow * 2.0 * turnSin * turnCos;
        const auto flutterDelay = softLimit (
            (double) (flutterNow * flutterScale * flutterFilter.process (flutterNoise.nextBipolar(), flutterShape)), flutterReach);
        const auto delay = centre + wowDelay + flutterDelay;
        const auto halfway = 0.5 * (lastDelay + delay);
        const auto moved = delay - lastDelay; // in samples at twice the rate: the read moves 2 less this, a sample
        lastDelay = delay;

        // The pitch it plays, every 10 ms for its editor's history, and as the block ends.
        const auto isReading = ++sinceReading >= readingEvery;

        if (isReading || i == block.numSamples - 1)
            lastPitch = (float) (1200.0 * std::log2 (std::max (0.5, 1.0 - 0.5 * moved)));

        if (isReading)
        {
            sinceReading = 0;
            pitchTap.push (&lastPitch, &lastPitch, 1);
        }

        // The curve wakes as Distortion turns up, and rests once it is down.
        if (curveRests && distortionNow > 0.0f)
            curveRests = false;
        else if (! curveRests && distortionNow <= 0.0f)
            restCurves();

        const auto drive = getDrive (distortionNow);
        const auto back = distortionNow / drive;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& state = channels[(size_t) channel];

            // At twice the rate, through the moving delay and the groove's curve.
            const auto [first, second] = state.oversampler.up (channel == 0 ? l : r);
            float played[2];

            for (int k = 0; k < 2; ++k)
            {
                state.line.push (k == 0 ? first : second);
                const auto heard = state.line.read ((float) (k == 0 ? halfway : delay));
                state.lateAt = (state.lateAt + 1) & 7;
                state.late[(size_t) state.lateAt] = heard;
                auto added = 0.0f;

                if (! curveRests)
                {
                    const auto driven = drive * state.highs.process (heard, highsShape);
                    const auto residue =
                        (state.bent.process (driven, groove) - state.straight.process (driven, dsp::StraightCurve {})) * back;
                    state.residueOut = residuePole * (state.residueOut + residue - state.residueLast);
                    state.residueLast = residue;
                    added = state.residueOut;
                }

                played[k] = state.late[(size_t) ((state.lateAt - dsp::AntialiasedCurve::delay) & 7)] + added;
            }

            const auto out = state.oversampler.down (played[0], played[1]);

            if (channel == 0)
                l = out;
            else
                r = out;
        }

        // The surface's noise, then the player: its two sides, and its band.
        noise.addSample (l, r, playing && makesNoise && (! heardSound || silentFor < holdSamples));

        if (right != nullptr)
        {
            const auto middle = 0.5f * (l + r), side = 0.5f * (l - r) * widthNow;
            l = middle + side;
            r = middle - side;
        }

        left[i] = channels[0].highCut.process (channels[0].lowCut.process (l, lowCutNow), highCutNow);

        if (right != nullptr)
            right[i] = channels[1].highCut.process (channels[1].lowCut.process (r, lowCutNow), highCutNow);
    }

    // Resting comes after its delay line has emptied of what last came in.
    restingFor = quietIn && ! noise.isSounding() ? std::min (restingFor + block.numSamples, restAfter) : 0;

    turn.set (getTurnNow());
    pitchNow.set (lastPitch);
}
} // namespace phazite::devices

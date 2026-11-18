// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Age.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view media[] = { "Record", "Tape" };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "medium", .name = "Medium", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = media },
        { .id = "wobble", .name = "Wobble", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "wow", .name = "Wow", .maximum = 25.0f, .defaultValue = 4.0f, .unit = Unit::cents, .midpoint = 6.0f },
        { .id = "wowRate",
          .name = "Wow Rate",
          .minimum = 0.25f,
          .maximum = 4.0f,
          .defaultValue = 0.5555556f, // 33 1/3 rpm
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "flutter", .name = "Flutter", .maximum = 10.0f, .defaultValue = 0.0f, .unit = Unit::cents, .midpoint = 2.5f },
        { .id = "noise", .name = "Noise", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
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
          .scale = Scale::logarithmic },
        { .id = "dropouts", .name = "Dropouts", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "drive", .name = "Drive", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "distortion", .name = "Distortion", .defaultValue = 0.15f, .unit = Unit::percent },
        { .id = "headBump", .name = "Head Bump", .maximum = 6.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "stereo", .name = "Stereo", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "width", .name = "Width", .defaultValue = 0.9f, .unit = Unit::percent },
        { .id = "bassMono",
          .name = "Bass Mono",
          .minimum = 20.0f,
          .maximum = 500.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "band", .name = "Band", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 20.0f,
          .maximum = 1000.0f,
          .defaultValue = 30.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 14000.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr double butterworthQ = 0.70710678;
    constexpr double centsToRatio = 0.00057778950655485;                 // ln 2 / 1200
    constexpr double highsHertz = 2000.0;                                // the record's curve bends what is above this
    constexpr double flutterHertz = 6.0;                                 // the record's flutter's noise is under this
    constexpr double tapeFlutterLowest = 4.0, tapeFlutterHighest = 14.0; // the tape's flutter's band, in hertz
    constexpr double tapeFlutterLeakHertz = 0.1;                         // what keeps the tape's flutter's delay from wandering off
    constexpr double flutterMostRms = 4.5;                               // the flutter moves the delay at most this many RMS either way
    constexpr double residueLowCut = 20.0;                               // what the record's curve adds, without anything under this
    constexpr double tapeLowCut =
        2.0; // what the tape's adds: its curve takes the whole sound, so only the DC goes (a bass keeps its saturation)
    constexpr double wowGlideSeconds = 0.05, glideSeconds = 0.02, bandGlideSeconds = 0.005, flutterGlideSeconds = 0.3;
    constexpr double fadeSeconds = 0.02, flutterFadeSeconds = 0.25; // a part's switch and a change of medium; the flutter's
    constexpr double mostSwoop = 0.004;                             // how far a change of the wow may bend the pitch as it glides: 7 cents
    constexpr int splitStep = 32;                                   // samples between the split's moves as it glides
    constexpr float soundFloor = 3.2e-5f;                           // -90 dBFS: anything over this coming in is sound, for the noise's hold
    constexpr float silence = 1.0e-6f;                              // -120 dBFS: under this, nothing comes in (as its runner hears it)
    constexpr double restSeconds = 0.1;    // and after the delay line, this long for the filters to ring out, before it rests
    constexpr int dryReplaySamples = 128;  // the delay line's reads that make the copy's way back again (after 128 samples its
                                           // filters' silent start is 94 dB under a 10 kHz tone)
    constexpr double dryHoldSeconds = 1.0; // how long the copy goes on after the Mix reaches 100 %, before it rests

    // The tape's wow: a sine at the rate, and a drift, as loud as a sine of that depth.
    constexpr double tapeSine = 0.63, tapeDrift = 0.27;

    // The tape's drive: its shelf, before and after the curve, and how far into the curve it goes at 100 %: 9 dB, the highs
    // 15 dB with the shelf. Pushed 18 dB (the highs 24), a loud 9 kHz tone's harmonics folded back as aliases 39 dB under it
    // at twice the rate, and pushed 12, 51 dB under, where the record's are more than 60 dB under.
    constexpr double tapeShelfHertz = 4000.0, tapeShelfDecibels = 6.0, tapeMostDrive = 2.8183829; // 9 dB

    // The head bump.
    constexpr double bumpHertz = 60.0, bumpQ = 1.4;

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

    // How hard the record's curve is driven at a distortion: from as loud as the sound at 0 to 12 dB louder at 100 %.
    float getDrive (float distortionValue) noexcept
    {
        return 1.0f + 3.0f * distortionValue;
    }

    // And the tape's: to 9 dB louder at 100 %, its highs 15 with its shelf.
    float getTapeDrive (float distortionValue) noexcept
    {
        return 1.0f + (float) (tapeMostDrive - 1.0) * distortionValue;
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

    // A share a step nearer 0 or 1.
    float fade (float share, bool on, float step) noexcept
    {
        return on ? std::min (1.0f, share + step) : std::max (0.0f, share - step);
    }

    // What makes noise through a filter's chain RMS 1: even noise from -1 to 1 has a variance of a third, which the chain
    // multiplies by the energy of its answer to an impulse.
    float getNoiseScale (const dsp::FilterCoefficients& first, const dsp::FilterCoefficients& second, int samples)
    {
        dsp::Filter a, b;
        auto energy = 0.0;

        for (int i = 0; i < samples; ++i)
        {
            const auto out = (double) b.process (a.process (i == 0 ? 1.0f : 0.0f, first), second);
            energy += out * out;
        }

        return (float) (1.0 / std::sqrt (energy / 3.0));
    }
} // namespace

const DeviceInfo Age::info { .typeID = typeID, .name = "Age", .kind = DeviceKind::effect, .parameters = parameters };

Age::Medium Age::getMedium (std::span<const float> values) noexcept
{
    return values[medium] > 0.5f ? Medium::tape : Medium::record;
}

bool Age::isOn (std::span<const float> values, Parameter part) noexcept
{
    return values[(size_t) part] > 0.5f;
}

bool Age::isOff (std::span<const float> values, Parameter which)
{
    const auto& parameter = parameters[(size_t) which];
    return values[(size_t) which] <= parameter.minimum + (which == bassMono ? 0.5f : 0.01f);
}

float Age::getWowCents (float wowCents, float turnPart) noexcept
{
    const auto angle = twoPi * (double) turnPart;
    const auto swing = getSwing (std::max (0.0f, wowCents));
    return (float) (1200.0 * std::log2 (1.0 + swing * (std::cos (angle) - 0.25 * std::cos (2.0 * angle))));
}

double Age::getBandGain (std::span<const float> values, double frequency, double rate)
{
    return dsp::getMagnitude (dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], rate, butterworthQ), frequency, rate)
           * dsp::getMagnitude (dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], rate, butterworthQ), frequency, rate);
}

float Age::shapeSample (float distortionValue, float sample) noexcept
{
    const auto amount = std::clamp (distortionValue, 0.0f, 1.0f);
    const auto into = getDrive (amount);
    return sample + amount * (getGrooveCurve().value (into * sample) / into - sample);
}

double Age::getTailSeconds() const
{
    return holdSeconds + 0.05 + latency / sampleRate;
}

void Age::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    const auto fastRate = 2.0 * sampleRate;
    getGrooveCurve(); // made now, off the audio thread

    // The delay line's middle: as far as the deepest wow at the slowest rate swings it (a record's turn's sine and the quarter
    // as much again at twice the rate, an eighth as far; the tape's is held inside it), and the deepest flutter, with room for
    // the interpolation; then made the device's latency in whole samples, with the oversampling's and its curve's few.
    const auto& wowInfo = parameters[wow];
    const auto slowest = (double) parameters[wowRate].minimum;
    wowReach = getSwing (wowInfo.maximum) / (twoPi * slowest) * 1.125 * fastRate;
    const auto flutterRms = getSwing (parameters[flutter].maximum) / std::sqrt (2.0);
    flutterReach = flutterMostRms * flutterRms / (twoPi * flutterHertz) * fastRate;
    const auto shaperLatency = (double) dsp::AntialiasedShaper::getLatencySamples();
    latency = (int) std::ceil ((wowReach + flutterReach + 4.0) / 2.0 + shaperLatency);
    centre = 2.0 * (latency - shaperLatency);

    for (auto& channel : channels)
        channel.line.prepare ((int) std::ceil (centre + wowReach + flutterReach) + 4);

    for (auto& way : dryWays)
    {
        way.line.prepare ((int) std::ceil (centre) + dsp::AntialiasedCurve::delay + 2 * dryReplaySamples + 4); // and read again
        way.plain.prepare (latency);
    }

    highsShape = dsp::makeFilter (dsp::FilterShape::highPass, highsHertz, fastRate, butterworthQ);
    liftShape = dsp::makeFilter (dsp::FilterShape::highShelf, tapeShelfHertz, fastRate, butterworthQ, tapeShelfDecibels);
    lowerShape = dsp::makeFilter (dsp::FilterShape::highShelf, tapeShelfHertz, fastRate, butterworthQ, -tapeShelfDecibels);
    bumpShape = dsp::makeFilter (dsp::FilterShape::bandPass, bumpHertz, sampleRate, bumpQ);
    residuePole = (float) std::exp (-twoPi * residueLowCut / fastRate);
    tapePole = (float) std::exp (-twoPi * tapeLowCut / fastRate);

    // The record's flutter: noise, low-passed at its corner, made a delay whose slope swings the pitch as far as its depth
    // says. Even noise has a variance of a third; through a Butterworth low-pass at w its variance is that times
    // w / (2 sqrt 2 fs), and its slope's RMS w times its own.
    flutterShape = dsp::makeFilter (dsp::FilterShape::lowPass, flutterHertz, sampleRate, butterworthQ);
    const auto corner = twoPi * flutterHertz;
    const auto noiseRms = std::sqrt ((1.0 / 3.0) * corner / (2.0 * std::sqrt (2.0) * sampleRate));
    flutterScale = (float) (fastRate / (corner * noiseRms));

    // The tape's: its pitch, noise in its band, made RMS 1, which its depth scales; gathered into a delay as it plays.
    tapeFlutterLowShape = dsp::makeFilter (dsp::FilterShape::highPass, tapeFlutterLowest, sampleRate, butterworthQ);
    tapeFlutterHighShape = dsp::makeFilter (dsp::FilterShape::lowPass, tapeFlutterHighest, sampleRate, butterworthQ);
    tapeFlutterScale = getNoiseScale (tapeFlutterLowShape, tapeFlutterHighShape, (int) (2.0 * sampleRate));
    tapeFlutterLeak = std::exp (-twoPi * tapeFlutterLeakHertz / sampleRate);

    wowSmoothing = (float) (1.0 - std::exp (-1.0 / (wowGlideSeconds * sampleRate)));
    flutterSmoothing = (float) (1.0 - std::exp (-1.0 / (flutterGlideSeconds * sampleRate)));
    swingStep = mostSwoop * 2.0; // the delay moves this many samples at twice the rate a sample: mostSwoop of a second a second
    smoothing = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    bandSmoothing = (float) (1.0 - std::exp (-1.0 / (bandGlideSeconds * sampleRate)));
    splitSmoothing = (float) (1.0 - std::exp (-(double) splitStep / (glideSeconds * sampleRate)));
    fadeStep = (float) (1.0 / (fadeSeconds * sampleRate));
    flutterFadeStep = (float) (1.0 / (flutterFadeSeconds * sampleRate));
    holdSamples = (int) std::lround (holdSeconds * sampleRate);
    dryHold = (int) std::lround (dryHoldSeconds * sampleRate);
    restAfter = 2 * latency + (int) std::lround (restSeconds * sampleRate);
    readingEvery = std::max (1, (int) std::lround (sampleRate / pitchReadingsPerSecond));

    noiseOfRecord.prepare (sampleRate);
    wear.prepare (sampleRate);
    reset();
}

void Age::restCurves() noexcept
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

void Age::restTapeCurves() noexcept
{
    for (auto& channel : channels)
    {
        channel.tapeBent.reset();
        channel.tapeStraight.reset();
        channel.lifted.reset();
        channel.lowered.reset();
        channel.tapeLast = channel.tapeOut = 0.0f;
    }

    tapeCurveRests = true;
}

void Age::restDrift() noexcept
{
    driftNoise.setSeed (6211u);
    driftFilter.reset();
    driftRate = -1.0;
}

void Age::restTapeFlutter() noexcept
{
    tapeFlutterNoise.setSeed (8093u);
    tapeFlutterLow.reset();
    tapeFlutterHigh.reset();
    tapeFlutterDelay = 0.0;
}

void Age::restBand() noexcept
{
    for (auto& channel : channels)
    {
        channel.lowCut.reset();
        channel.highCut.reset();
    }
}

void Age::reset()
{
    for (auto& channel : channels)
    {
        channel.oversampler.reset();
        channel.line.reset();
        channel.late.fill (0.0f);
        channel.lateAt = 0;
        channel.bump.reset();
    }

    restBand();
    restDry();
    dryRan = false;
    lastMix = 1.0f;
    sinceMixed = dryHold; // resting
    restCurves();
    restTapeCurves();
    restDrift();
    restTapeFlutter();
    sideSplit.reset();
    middlePhase.reset();
    flutterFilter.reset();
    flutterNoise.setSeed (4507u);
    turnCos = 1.0;
    turnSin = 0.0;
    stepRate = -1.0;
    sinceNormalised = sinceSplitMoved = sinceReading = 0;
    noiseOfRecord.reset();
    wear.reset();
    wasPlaying = heardSound = false;
    silentFor = restingFor = 0;
    isFirstBlock = true;
}

int Age::getDryRebuildSamples() const noexcept
{
    return dryReplaySamples;
}

void Age::restDry() noexcept
{
    for (auto& way : dryWays)
    {
        way.restTurn();
        way.phase.reset();
        way.plain.reset();
    }

    dryRan = false;
}

void Age::DryWay::restTurn() noexcept
{
    upper.reset();
    lower.reset();
    line.reset();
}

void Age::setSplit (double frequency) noexcept
{
    splitFrequency = frequency;
    sideSplit.setFrequency (frequency, sampleRate);
    middlePhase.setFrequency (frequency, sampleRate);

    for (auto& way : dryWays)
        way.phase.setFrequencyAs (middlePhase);
}

void Age::DryWay::feed (float phasedSample) noexcept
{
    const auto [first, second] = upper.up (phasedSample);
    line.push (first);
    line.push (second);
}

float Age::DryWay::turn (float phasedSample, double back) noexcept
{
    // The rest of the sound's way after Bass Mono's all-pass: twice the rate through the delay line, read as far back as its
    // middle and as late again as the curve, read between samples as the sound is read there while nothing moves it, so that
    // the two stay in phase to the top (which the read dulls a little, both alike), and back.
    const auto [first, second] = upper.up (phasedSample);
    line.push (first);
    const auto firstRead = line.read ((float) back);
    line.push (second);
    return lower.down (firstRead, line.read ((float) back));
}

void Age::DryWay::rebuild (int samples, double back) noexcept
{
    // Each of the last samples' two reads as they were made, a push apart, two pushes a sample.
    lower.reset();

    for (auto k = samples - 1; k >= 0; --k)
        lower.down (line.read ((float) (back + 2.0 * k + 1.0)), line.read ((float) (back + 2.0 * k)));
}

// The record a part of a turn further, by a pair that turns it as far each sample, worked out again only as the rate moves.
void Age::turnBy (double samples) noexcept
{
    const auto angle = twoPi * (double) rateNow / sampleRate * samples;
    const auto c = std::cos (angle), s = std::sin (angle);
    const auto turnedCos = turnCos * c - turnSin * s;
    turnSin = turnSin * c + turnCos * s;
    turnCos = turnedCos;
}

float Age::getTurnNow() const noexcept
{
    const auto part = std::atan2 (turnSin, turnCos) / twoPi;
    return (float) (part < 0.0 ? part + 1.0 : part);
}

// Nothing comes in and nothing plays: the output is silence, the record turns on, and its readings go on.
void Age::rest (int numSamples) noexcept
{
    if (restingFor == restAfter) // its first resting block: what it holds is nothing now, exactly
    {
        for (auto& channel : channels)
        {
            channel.oversampler.reset();
            channel.line.reset();
            channel.late.fill (0.0f);
            channel.bump.reset();
        }

        restBand();
        restCurves();
        restTapeCurves();
        restDry();
        sideSplit.reset();
        middlePhase.reset();
        ++restingFor;
    }

    turnBy (numSamples);
    sinceReading += numSamples;

    if (sinceReading >= readingEvery)
    {
        sinceReading %= readingEvery;
        const auto reading = wowShape == Medium::record ? getWowCents (wowNow, getTurnNow()) : 0.0f;
        pitchTap.push (&reading, &reading, 1);
    }
}

void Age::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0 || block.numSamples <= 0)
        return;

    // Where each part is going: the medium, and each part on or off.
    const auto targetMedium = getMedium (values);
    const auto onTape = targetMedium == Medium::tape;
    const auto wobbleOn = isOn (values, wobble), noiseOn = isOn (values, noise), driveOn = isOn (values, drive);
    const auto stereoOn = isOn (values, stereo), bandOn = isOn (values, band);

    // Where each parameter is going.
    const auto targetWow = wobbleOn ? std::max (0.0f, values[wow]) : 0.0f;
    const auto targetRate = std::max (0.01f, values[wowRate]);
    const auto targetFlutter = wobbleOn ? (float) (getSwing (std::max (0.0f, values[flutter])) / std::sqrt (2.0)) : 0.0f;
    const auto targetDistortion = driveOn ? std::clamp (values[distortion], 0.0f, 1.0f) : 0.0f;
    const auto targetBump = driveOn && onTape ? dsp::decibelsToGain (std::clamp (values[headBump], 0.0f, 6.0f)) - 1.0f : 0.0f;
    const auto targetWidth = stereoOn ? std::clamp (values[width], 0.0f, 1.0f) : 1.0f;
    const auto targetBass = stereoOn && ! isOff (values, bassMono) ? 1.0f : 0.0f;
    const auto targetTape = onTape ? 1.0f : 0.0f;
    const auto hissGain = isOff (values, hiss) ? 0.0f : dsp::decibelsToGain (values[hiss]);
    const auto crackleGain = isOff (values, crackle) ? 0.0f : dsp::decibelsToGain (values[crackle]);
    const auto dropoutAmount = std::clamp (values[dropouts], 0.0f, 1.0f);
    const auto recordNoise = ! onTape && noiseOn && (hissGain > 0.0f || crackleGain > 0.0f);
    const auto tapeHiss = onTape && noiseOn && hissGain > 0.0f;
    const auto tapeDips = onTape && noiseOn && dropoutAmount > 0.0f;
    const auto makesNoise = recordNoise || tapeHiss;
    lowCutTarget = dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], sampleRate, butterworthQ);
    highCutTarget = dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], sampleRate, butterworthQ);
    noiseOfRecord.setLevels (hissGain, crackleGain, std::max (0.0f, values[density]));
    wear.setLevels (hissGain, dropoutAmount);

    // The medium plays while the song does: its noise starts over with the song, and waits for sound only once some came.
    const auto playing = block.isPlaying || block.isRendering;

    if (playing && ! wasPlaying)
    {
        heardSound = false;
        silentFor = 0;
    }

    wasPlaying = playing;
    // The noise is on the medium, as late as the sound: a moment of it is where the song was the latency ago.
    const auto beatsPerSample = playing ? block.tempo / 60.0 / sampleRate : 0.0;
    noiseOfRecord.setPosition (block.beat, beatsPerSample, -latency * beatsPerSample);
    wear.setPosition (block.beat, beatsPerSample, -latency * beatsPerSample);

    if (isFirstBlock)
    {
        wowNow = targetWow;
        rateNow = targetRate;
        swingNow = getSwing (targetWow) / (twoPi * (double) targetRate) * 2.0 * sampleRate;
        flutterNow = targetFlutter;
        distortionNow = targetDistortion;
        bumpNow = targetBump;
        widthNow = targetWidth;
        bassAmount = targetBass;
        bandShare = bandOn ? 1.0f : 0.0f;
        tapeShare = flutterTapeShare = targetTape;
        wowShape = targetMedium;
        setSplit (std::max ((double) values[bassMono], 20.0));
        lowCutNow = lowCutTarget;
        highCutNow = highCutTarget;
        noiseOfRecord.jumpTo (playing && recordNoise);
        wear.jumpTo (playing && tapeHiss, playing && tapeDips);
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

    if (quietIn && ! noiseWanted && ! noiseOfRecord.isSounding() && ! wear.isSounding() && restingFor >= restAfter)
    {
        for (int channel = 0; channel < numChannels; ++channel)
        {
            juce::FloatVectorOperations::clear (block.channels[channel], block.numSamples);

            if (block.dry != nullptr) // the copy for its mix, as late, is as silent
                juce::FloatVectorOperations::clear (block.dry[channel], block.numSamples);
        }

        rested = true;
        silentFor = std::min (silentFor + block.numSamples, holdSamples);
        rest (block.numSamples);
        noiseOfRecord.skip (block.numSamples);
        wear.skip (block.numSamples);
        turn.set (getTurnNow());
        pitchNow.set (wowShape == Medium::record ? getWowCents (wowNow, turn.get()) : 0.0f);
        wear.getDip().set (0.0f);
        return;
    }

    rested = false;

    if (! quietIn)
        restingFor = 0;

    const auto& groove = getGrooveCurve();
    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;
    auto lastPitch = 0.0f, deepestDip = 0.0f;
    auto resetsDryPhase = false;

    // The copy for its mix: at every mix, 0 % too, the sound's linear way (the same oversampling and delay, and Bass Mono's
    // all-pass as far as the sound's own split has come in), so that the two meet in phase at any mix; at no mix, what came in
    // with its phase turned, its level kept within a few tenths of a decibel to 18 kHz. A copy merely delayed cancelled the
    // top octave against the sound (5 to 18 dB at 15 and 18 kHz, half and half), and one merely delayed at 0 % but turned
    // above it cancelled the bass at Bass Mono's frequency by tens of decibels at a Mix resting under 5 %. Made whole only while
    // the mix uses it, and for a second after the Mix reaches 100 % (so that a Mix moved through 100 % and back, or automation
    // that touches it, goes on with it); meanwhile only its way into its delay line, at twice the rate. As the Mix leaves 100 %
    // after a rest, its way back is made again from the line's last reads, so that its first samples are as if it had never
    // stopped: 128 samples' work at any rate, where making it all again from what came in took its latency's and 256 more
    // (14 times a 32-sample call's work at 48 kHz, 37 times at 192).
    const auto mixed = values[mix] < 1.0f || lastMix < 1.0f;
    const auto dryRuns = block.dry != nullptr && (mixed || sinceMixed < dryHold);
    sinceMixed = mixed ? 0 : std::min (sinceMixed + block.numSamples, dryHold);
    lastMix = values[mix];

    if (dryRuns && ! dryRan)
    {
        ++dryRebuilds;

        for (auto& way : dryWays)
            way.rebuild (dryReplaySamples, centre + dsp::AntialiasedCurve::delay);
    }

    dryRan = dryRuns;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The parameters glide, and the parts and the medium fade.
        if (! juce::exactlyEqual (rateNow, targetRate))
        {
            auto glided = rateNow + (targetRate - rateNow) * wowSmoothing;

            if (std::abs (targetRate - glided) <= 1.0e-6f * targetRate)
                glided = targetRate;

            // Rising, the rate outruns no swing: how far the wow swings the delay falls only so fast (swingStep), and at a rate
            // much faster than it is set for, a swing that wide bends the pitch far past the Wow. So it rises only as far as
            // turns the swing there is 1.5 times as fast as the Wow's (a record's second hump on it, 1.25 times that: under
            // twice the Wow); at a steady rate, or falling, it is never held.
            if (glided > rateNow && swingNow > 0.0)
            {
                const auto most = 1.5 * getSwing (wowNow) * 2.0 * sampleRate / (twoPi * swingNow);
                glided = std::max (rateNow, (float) std::min ((double) glided, most));
            }

            rateNow = glided;
        }

        wowNow += (targetWow - wowNow) * wowSmoothing;
        flutterNow += (targetFlutter - flutterNow) * flutterSmoothing;
        distortionNow += (targetDistortion - distortionNow) * smoothing;
        widthNow += (targetWidth - widthNow) * smoothing;
        bassAmount += (targetBass - bassAmount) * smoothing;

        if (targetDistortion <= 0.0f && distortionNow < 1.0e-6f)
            distortionNow = 0.0f;

        if (targetWow <= 0.0f && wowNow < 1.0e-6f)
            wowNow = 0.0f;

        if (targetFlutter <= 0.0f && flutterNow < 1.0e-9f)
            flutterNow = 0.0f;

        if (std::abs (targetWidth - widthNow) < 1.0e-6f)
            widthNow = targetWidth;

        if (! juce::exactlyEqual (bumpNow, targetBump))
        {
            bumpNow += (targetBump - bumpNow) * smoothing;

            if (std::abs (targetBump - bumpNow) < 1.0e-6f)
                bumpNow = targetBump;
        }

        tapeShare = fade (tapeShare, onTape, fadeStep);
        flutterTapeShare = fade (flutterTapeShare, onTape, flutterFadeStep);
        bandShare = fade (bandShare, bandOn, fadeStep);

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
        const auto turnsBass = right != nullptr && (bassAmount > 0.0f || targetBass > 0.0f);
        const auto bassShare = bassAmount;

        if (turnsBass)
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
                resetsDryPhase = true;
            }
        }

        // The wobble: the turn (the record's, or the tape's sine), its swing; and the flutter's noise, as delays.
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

        // How far the wow swings the delay: its depth over the rate, gliding there no faster than swingStep a sample. A change
        // of medium takes it down to nothing in the old one's shape first, then up in the new one's.
        const auto swingTarget = wowShape == targetMedium ? getSwing (wowNow) / (twoPi * (double) rateNow) * 2.0 * sampleRate : 0.0;
        swingNow += std::clamp (swingTarget - swingNow, -swingStep, swingStep);

        if (wowShape != targetMedium && juce::exactlyEqual (swingNow, 0.0))
        {
            wowShape = targetMedium;
            restDrift();
        }

        auto wowDelay = 0.0;

        if (wowShape == Medium::record)
        {
            wowDelay = -swingNow * turnSin + 0.125 * swingNow * 2.0 * turnSin * turnCos;
        }
        else
        {
            // The tape's: 0.63 of the sine, and 0.27 of a drift, held inside the record's reach. The drift is noise low-passed at
            // half the rate, made RMS 1, a delay whose slope's RMS is its corner's (pi times the rate) times its own; a sine of
            // the swing has a slope 2 pi times the rate over the square root of two times the swing, RMS. So a drift whose
            // slope is as loud as that sine's is the square root of two times the swing, of which 0.27 (with 0.63 of the sine, its
            // 99th percentile over a minute is at the Wow).
            if (! juce::exactlyEqual ((double) rateNow, driftRate))
            {
                driftRate = rateNow;
                const auto driftHertz = 0.5 * driftRate;
                // makeFilter() keeps a frequency at 1 Hz or more, and the drift's is as low as an eighth of a hertz: the same
                // filter, both scaled up alike (its tangent, the filter's one number of them, is their ratio's).
                driftShape = dsp::makeFilter (dsp::FilterShape::lowPass, 64.0 * driftHertz, 64.0 * sampleRate, butterworthQ);
                const auto driftCorner = twoPi * driftHertz;
                driftScale = (float) (1.0 / std::sqrt ((1.0 / 3.0) * driftCorner / (2.0 * std::sqrt (2.0) * sampleRate)));
            }

            // Its noise scaled as it goes in, so that what the filter holds is RMS 1 at any corner: as the rate moves, the drift
            // keeps its size, rather than jumping with a scale applied after the filter.
            const auto drift = (double) driftFilter.process (driftScale * driftNoise.nextBipolar(), driftShape);
            const auto driftShare = tapeDrift * std::sqrt (2.0);
            wowDelay = softLimit (swingNow * (-tapeSine * turnSin + driftShare * drift), wowReach);
        }

        auto flutterDelay = softLimit (
            (double) (flutterNow * flutterScale * flutterFilter.process (flutterNoise.nextBipolar(), flutterShape)), flutterReach);

        if (flutterTapeShare > 0.0f)
        {
            // The tape's: a pitch of a unit's depth in its band, gathered into a delay (two samples at twice the rate a sample, less
            // its pitch), then made as deep as the flutter: so that it is nothing the moment the flutter is (Wobble off).
            const auto pitch =
                (double) (tapeFlutterScale
                          * tapeFlutterHigh.process (tapeFlutterLow.process (tapeFlutterNoise.nextBipolar(), tapeFlutterLowShape),
                                                     tapeFlutterHighShape));
            tapeFlutterDelay = tapeFlutterLeak * tapeFlutterDelay - 2.0 * pitch;
            const auto tapeFlutter = softLimit ((double) flutterNow * tapeFlutterDelay, flutterReach);
            flutterDelay = flutterTapeShare >= 1.0f ? tapeFlutter : flutterDelay + (double) flutterTapeShare * (tapeFlutter - flutterDelay);
        }
        else if (! onTape && ! juce::exactlyEqual (tapeFlutterDelay, 0.0))
        {
            restTapeFlutter(); // faded out: the next starts from its seed
        }

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

        // The curves wake as Distortion turns up in their medium, and rest once it is down or the medium is the other's.
        const auto recordCurve = distortionNow > 0.0f && tapeShare < 1.0f;
        const auto tapeCurve = distortionNow > 0.0f && tapeShare > 0.0f;

        if (curveRests && recordCurve)
            curveRests = false;
        else if (! curveRests && ! recordCurve)
            restCurves();

        if (tapeCurveRests && tapeCurve)
            tapeCurveRests = false;
        else if (! tapeCurveRests && ! tapeCurve)
            restTapeCurves();

        const auto grooveDrive = getDrive (distortionNow);
        const auto back = distortionNow / grooveDrive;
        const auto tapeDrive = getTapeDrive (distortionNow);
        const auto tapeBack = distortionNow / tapeDrive;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& state = channels[(size_t) channel];

            // At twice the rate, through the moving delay and the medium's curve.
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
                    const auto driven = grooveDrive * state.highs.process (heard, highsShape);
                    const auto residue =
                        (state.bent.process (driven, groove) - state.straight.process (driven, dsp::StraightCurve {})) * back;
                    state.residueOut = residuePole * (state.residueOut + residue - state.residueLast);
                    state.residueLast = residue;
                    added = state.residueOut;
                }

                if (! tapeCurveRests)
                {
                    // The tape's: its top lifted into a symmetric curve, and what the curve bends lowered again.
                    const auto driven = tapeDrive * state.lifted.process (heard, liftShape);
                    const auto residue =
                        (state.tapeBent.process (driven, dsp::SoftCurve {}) - state.tapeStraight.process (driven, dsp::StraightCurve {}))
                        * tapeBack;
                    const auto unshelved = state.lowered.process (residue, lowerShape);
                    state.tapeOut = tapePole * (state.tapeOut + unshelved - state.tapeLast);
                    state.tapeLast = unshelved;
                    const auto tapeAdded = state.tapeOut;
                    added = tapeShare >= 1.0f ? tapeAdded : added + tapeShare * (tapeAdded - added);
                }

                played[k] = state.late[(size_t) ((state.lateAt - dsp::AntialiasedCurve::delay) & 7)] + added;
            }

            auto out = state.oversampler.down (played[0], played[1]);

            // The tape's head bump: the band around 60 Hz lifted, as far as it says.
            if (bumpNow > 0.0f || targetBump > 0.0f)
                out += bumpNow * state.bump.process (out, bumpShape);

            if (channel == 0)
                l = out;
            else
                r = out;
        }

        if (juce::exactlyEqual (bumpNow, 0.0f) && targetBump <= 0.0f)
            for (auto& channel : channels)
                channel.bump.reset();

        // The medium's noise (a tape's dropouts dip the sound first), then the player: its two sides, and its band.
        const auto held = playing && (! heardSound || silentFor < holdSamples);
        noiseOfRecord.addSample (l, r, held && recordNoise);
        wear.process (l, r, held && tapeHiss, playing && tapeDips);
        deepestDip = std::max (deepestDip, wear.getLastDip());

        if (right != nullptr)
        {
            const auto middle = 0.5f * (l + r), side = 0.5f * (l - r) * widthNow;
            l = middle + side;
            r = middle - side;
        }

        if (bandShare >= 1.0f)
        {
            left[i] = channels[0].highCut.process (channels[0].lowCut.process (l, lowCutNow), highCutNow);

            if (right != nullptr)
                right[i] = channels[1].highCut.process (channels[1].lowCut.process (r, lowCutNow), highCutNow);
        }
        else if (bandShare > 0.0f || bandOn)
        {
            // The band fading in or out: what it keeps, faded with the sound as it came.
            const auto lBand = channels[0].highCut.process (channels[0].lowCut.process (l, lowCutNow), highCutNow);
            left[i] = l + bandShare * (lBand - l);

            if (right != nullptr)
            {
                const auto rBand = channels[1].highCut.process (channels[1].lowCut.process (r, lowCutNow), highCutNow);
                right[i] = r + bandShare * (rBand - r);
            }
        }
        else
        {
            left[i] = l;

            if (right != nullptr)
                right[i] = r;
        }

        if (bandShare <= 0.0f && ! bandOn)
            restBand();

        // The copy of what came in, for its mix: the sound's way, but for what moves it and colours it (the wobble, the drive,
        // the sides of the lows, the noise, the width and the band), so that the two meet in phase at any mix; merely delayed
        // at no mix, which leaves what came in exactly.
        if (block.dry != nullptr)
        {
            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto& way = dryWays[(size_t) channel];
                auto& came = block.dry[channel][i];
                way.plain.push (came);
                const auto delayed = way.plain.read (latency);

                // Bass Mono's all-pass, always, cheap: what makes the copy again is through it, and settled.
                auto phased = came;

                if (turnsBass)
                    phased += bassShare * (way.phase.processAllPass (came) - came);

                if (dryRuns)
                {
                    came = way.turn (phased, centre + dsp::AntialiasedCurve::delay);
                }
                else
                {
                    way.feed (phased); // the way the copy is made again from; as late either way, and at 100 % nothing hears it
                    came = delayed;
                }
            }
        }

        if (std::exchange (resetsDryPhase, false))
            for (auto& way : dryWays)
                way.phase.reset();
    }

    // Resting comes after its delay line has emptied of what last came in.
    restingFor = quietIn && ! noiseOfRecord.isSounding() && ! wear.isSounding() ? std::min (restingFor + block.numSamples, restAfter) : 0;

    turn.set (getTurnNow());
    pitchNow.set (lastPitch);
    wear.getDip().set (deepestDip / TapeWear::deepestDip);
}
} // namespace phazite::devices

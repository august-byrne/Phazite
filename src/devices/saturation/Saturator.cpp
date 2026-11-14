// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Saturator.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view shapes[] = { "Soft", "Tape", "Tube", "Hard", "Fold" };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "drive", .name = "Drive", .minimum = -24.0f, .maximum = 36.0f, .defaultValue = 6.0f, .unit = Unit::decibels },
        { .id = "shape",
          .name = "Shape",
          .maximum = (float) std::size (shapes) - 1.0f,
          .defaultValue = 0.0f,
          .unit = Unit::choice,
          .choices = shapes },
        { .id = "tone", .name = "Tone", .minimum = -1.0f, .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "output", .name = "Output", .minimum = -24.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        { .id = "autoGain", .name = "Auto Gain", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = Unit::percent },
    };

    constexpr double gainSeconds = 0.02;        // how long a change of the drive or the output takes, about
    constexpr double tiltFrequency = 1000.0;    // where the tone tilts around
    constexpr float tiltDecibels = 6.0f;        // how far the tone tilts each end, at its ends
    constexpr double dcBlockerFrequency = 10.0; // under what the tube's lopsided curve leaves
} // namespace

const DeviceInfo Saturator::info { .typeID = typeID, .name = "Saturator", .kind = DeviceKind::effect, .parameters = parameters };

float Saturator::shapeSample (Shape shape, float x) noexcept
{
    switch (shape)
    {
        case Shape::tape:
            return dsp::TapeCurve::value (x);
        case Shape::tube:
            return dsp::TubeCurve::value (x);
        case Shape::hard:
            return dsp::HardCurve::value (x);
        case Shape::fold:
            return dsp::FoldCurve::value (x);
        case Shape::soft:
        default:
            return dsp::SoftCurve::value (x);
    }
}

void Saturator::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    dsp::TubeCurve::prepare();

    // Twice the rate, through half-band filters that keep the latency short and whole; the curves' averaging and the
    // filter that levels it again add 4 samples at that rate, 2 at the sound's.
    oversampling =
        std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampling->initProcessing ((size_t) std::max (1, maximumBlockSize));
    latency = (int) std::lround (oversampling->getLatencyInSamples()) + dsp::AntialiasedCurve::delay / 2;

    // The same again for the copy of what came in, which its mix blends with the sound.
    dryOversampling =
        std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    dryOversampling->initProcessing ((size_t) std::max (1, maximumBlockSize));

    const auto rate = 2.0 * sampleRate;
    smoothing = (float) (1.0 - std::exp (-1.0 / (gainSeconds * rate)));
    reset();
}

void Saturator::reset()
{
    for (auto* filters : { &oversampling, &dryOversampling })
        if (*filters != nullptr)
            (*filters)->reset();

    for (auto* both : { &curves, &dryCurves })
        for (auto& curve : *both)
            curve.reset();

    lastMix = 1.0f;
    dryInStep = false;

    for (auto* filters : { &dcBlockers, &lowTilts, &highTilts })
        for (auto& filter : *filters)
            filter.reset();

    isFirstBlock = true;
}

void Saturator::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (oversampling == nullptr || numChannels <= 0 || block.numSamples <= 0)
        return;

    const auto curve = (Shape) juce::jlimit (0, (int) std::size (shapes) - 1, (int) std::lround (values[shape]));
    const auto targetDrive = dsp::decibelsToGain (values[drive]);

    // Auto gain takes the drive back off: quiet parts, which the curve leaves straight, keep their level.
    const auto targetOutput = dsp::decibelsToGain (values[output]) / (values[autoGain] > 0.5f ? targetDrive : 1.0f);

    if (isFirstBlock)
    {
        driveGain = targetDrive;
        outputGain = targetOutput;
        isFirstBlock = false;
    }

    const auto rate = 2.0 * sampleRate;
    const auto dcBlocker = dsp::makeFilter (dsp::FilterShape::highPass, dcBlockerFrequency, rate, 0.70710678);
    const auto lowTilt = dsp::makeFilter (dsp::FilterShape::lowShelf, tiltFrequency, rate, 0.5, -tiltDecibels * values[tone]);
    const auto highTilt = dsp::makeFilter (dsp::FilterShape::highShelf, tiltFrequency, rate, 0.5, tiltDecibels * values[tone]);

    // Another curve: its averaging starts again.
    if (curve != lastShape)
    {
        for (auto& antialiased : curves)
            antialiased.restartAveraging();

        lastShape = curve;
    }

    juce::dsp::AudioBlock<float> audio (block.channels, (size_t) numChannels, (size_t) block.numSamples);
    auto fast = oversampling->processSamplesUp (audio);

    // Each sample driven into the curve, played by its antiderivative.
    const auto play = [&] (auto curveType)
    {
        using Curve = decltype (curveType);

        for (size_t i = 0; i < fast.getNumSamples(); ++i)
        {
            driveGain += (targetDrive - driveGain) * smoothing;
            outputGain += (targetOutput - outputGain) * smoothing;

            for (size_t channel = 0; channel < fast.getNumChannels(); ++channel)
            {
                auto* samples = fast.getChannelPointer (channel);
                auto y = curves[channel].process (samples[i] * driveGain, Curve {});

                if (curve == Shape::tube)
                    y = dcBlockers[channel].process (y, dcBlocker);

                y = highTilts[channel].process (lowTilts[channel].process (y, lowTilt), highTilt);
                samples[i] = y * outputGain;
            }
        }
    };

    switch (curve)
    {
        case Shape::tape:
            play (dsp::TapeCurve {});
            break;
        case Shape::tube:
            play (dsp::TubeCurve {});
            break;
        case Shape::hard:
            play (dsp::HardCurve {});
            break;
        case Shape::fold:
            play (dsp::FoldCurve {});
            break;
        case Shape::soft:
            play (dsp::SoftCurve {});
            break;
    }

    oversampling->processSamplesDown (audio);

    // What came in, for the mix: through the same filters, as late as the sound and its phase turned alike, a straight line
    // in the curve's place. Only while the mix uses it (it starts from silence then, as the mix leaves 100 %): with the mix
    // all the saturator's own sound, nothing reads it.
    const auto usesDry = block.dry != nullptr && (values[mix] < 1.0f || lastMix < 1.0f);
    lastMix = values[mix];

    if (usesDry)
    {
        if (! dryInStep)
        {
            dryOversampling->reset();

            for (auto& straight : dryCurves)
                straight.reset();
        }

        juce::dsp::AudioBlock<float> came (block.dry, (size_t) numChannels, (size_t) block.numSamples);
        auto fastDry = dryOversampling->processSamplesUp (came);

        for (size_t channel = 0; channel < fastDry.getNumChannels(); ++channel)
        {
            auto* samples = fastDry.getChannelPointer (channel);

            for (size_t i = 0; i < fastDry.getNumSamples(); ++i)
                samples[i] = dryCurves[channel].process (samples[i], dsp::StraightCurve {});
        }

        dryOversampling->processSamplesDown (came);
    }

    dryInStep = usesDry;
}
} // namespace phazite::devices

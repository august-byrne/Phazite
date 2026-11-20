// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LinearPhase.h"
#include "Equalizer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::devices
{
namespace
{
    using Placement = Equalizer::Placement;
    using BandType = Equalizer::BandType;

    static_assert (LinearPhase::numBands == Equalizer::numBands);
    static_assert (LinearPhase::fieldsCompared == (int) Equalizer::placement + 1);

    constexpr int responseChunks = 64; // a design's response is worked out in so many steps
    constexpr int windowParts = 4, fillParts = 4, zeroHertzParts = 4;
    constexpr int bandCost = 64;    // a band's sections, in units of work
    constexpr double flat = 1.0e-9; // a response within this of 0 everywhere is none, of 1 a delay alone

    int log2Of (int n)
    {
        auto bits = 0;

        while ((1 << bits) < n)
            ++bits;

        return bits;
    }

    // The modified Bessel function of the first kind, of order 0, for the Kaiser window.
    double besselI0 (double x)
    {
        auto sum = 1.0, term = 1.0;

        for (int k = 1; k < 200 && term > 1.0e-17 * sum; ++k)
        {
            const auto factor = x / (2.0 * k);
            term *= factor * factor;
            sum += term;
        }

        return sum;
    }

    template <typename Values>
    bool differ (const Values& a, const Values& b) noexcept
    {
        for (size_t i = 0; i < a.size(); ++i)
            if (! juce::exactlyEqual (a[i], b[i]))
                return true;

        return false;
    }

    bool isShelfOrBell (int type)
    {
        return type == (int) BandType::lowShelf || type == (int) BandType::bell || type == (int) BandType::highShelf;
    }

    int toPlacement (float value)
    {
        return std::clamp ((int) std::lround (value), (int) Placement::stereo, (int) Placement::right);
    }
} // namespace

int LinearPhase::getRateFactor (double rate) noexcept
{
    return rate <= 50000.0 ? 1 : rate <= 100000.0 ? 2 : 4;
}

void LinearPhase::prepare (double newSampleRate, std::span<const float> values, int numChannels)
{
    prepare (newSampleRate, values, numChannels, Design {});
}

void LinearPhase::prepare (double newSampleRate, std::span<const float> values, int numChannels, Design design)
{
    sampleRate = newSampleRate;
    const auto r = getRateFactor (sampleRate);
    latency = latencyAtBaseRate * r;
    half = latency / 2;
    const auto partition = partitionAtBaseRate * r;
    const auto middle = latency - partition; // taps to each side of the middle one
    taps = 2 * middle + 1;
    const auto numPartitions = (taps + partition - 1) / partition;
    convolution.prepare (partition, numPartitions, middle / partition);

    for (auto& set : sets)
    {
        convolution.allocate (set);
        set.kinds.fill (Kernels::Kind::none);
        set.routes.fill (-1);
    }

    gridPoints = std::max (2, design.gridTimesLatency) * latency;
    bins = gridPoints / 2 + 1;
    tangents.resize ((size_t) bins);

    for (int k = 0; k < bins; ++k)
    {
        const auto frequency = std::min ((double) k * sampleRate / gridPoints, 0.4999 * sampleRate);
        tangents[(size_t) k] = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate);
    }

    // A Kaiser window (none at a beta of 0), and the last part of each side tapered by a raised cosine to a small end tap.
    window.resize ((size_t) middle + 1);
    const auto beta = design.windowBeta;
    const auto tapered = std::clamp ((int) std::lround (design.taperFraction * middle), 0, middle);

    for (int j = 0; j <= middle; ++j)
    {
        const auto x = (double) j / (double) middle;
        auto w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - x * x))) / besselI0 (beta);

        if (const auto into = j - (middle - tapered); into > 0)
            w *= 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * into / (tapered + 1)));

        window[(size_t) j] = (float) w;
    }

    for (auto& response : responses)
        response.assign ((size_t) bins, 0.0f);

    for (auto& kernel : kernelTaps)
        kernel.assign ((size_t) taps, 0.0f);

    bigFft.prepare (log2Of (gridPoints), true);
    smallFft.prepare (log2Of (2 * partition), false);

    for (auto* buffer : { &bigIn, &bigWork, &bigOut })
        buffer->assign ((size_t) gridPoints, {});

    for (auto* buffer : { &smallIn, &smallWork, &smallOut })
        buffer->assign ((size_t) (2 * partition), {});

    spare.assign ((size_t) partition + 1, {});
    steps.resize ((size_t) (responseChunks + 1 + 2 * (fillParts + bigFft.getNumParts() + windowParts + zeroHertzParts)
                            + 4 * ((numPartitions + 1) / 2) * (2 + smallFft.getNumParts())));

    auto historySize = 1;

    while (historySize < latency + maxLength + 1)
        historySize *= 2;

    for (auto& side : history)
        side.assign ((size_t) historySize, 0.0f);

    historyMask = historySize - 1;
    grid = dsp::ControlGrid (half);

    // Its first kernels, designed whole.
    played = 0;
    gliding = -1;
    startDesign (compare (values, numChannels), values, 0, true, half);
    advanceDesign (half);
    workDone = 0;
    designsStarted = 0;
    reset();
}

void LinearPhase::reset() noexcept
{
    if (! isPrepared())
        return;

    convolution.reset();

    if (gliding >= 0)
    {
        convolution.endGlide();
        played = std::exchange (gliding, -1);
    }

    designing = droppedLast = false;

    // Its history forgotten by its count, nothing of it written (playStretch()).
    historyAt = 0;
    historyHeld = {};
    grid.reset();
    afterReset = true;
    placeKnown = jumped = false;
}

void LinearPhase::startBlock (bool isPlaying, std::int64_t songSample) noexcept
{
    grid.startBlock (isPlaying, songSample);

    if (! isPlaying)
    {
        placeKnown = false;
        return;
    }

    // Started, or elsewhere than where the song was due: a jump.
    if (! placeKnown || songSample != nextSongSample)
        jumped = true;

    placeKnown = true;
    nextSongSample = songSample;
}

LinearPhase::Compared LinearPhase::compare (std::span<const float> values, int numChannels) const noexcept
{
    Compared compared {};

    for (int band = 0; band < numBands; ++band)
        for (int field = 0; field < fieldsCompared; ++field)
            compared[(size_t) (band * fieldsCompared + field)] =
                values[(size_t) Equalizer::getParameterIndex (band, (Equalizer::Field) field)];

    compared.back() = numChannels < 2 ? 1.0f : 0.0f;
    return compared;
}

int LinearPhase::getFreeSet() const noexcept
{
    for (int set = 0; set < (int) sets.size(); ++set)
        if (set != played && set != gliding)
            return set;

    return 0;
}

void LinearPhase::atJump (const Compared& now, std::span<const float> values) noexcept
{
    // A design under way for values the jump left behind is dropped (one put in whole after a reset goes on), and one for the
    // values here starts at once, spread over the samples to the grid's next line, so that it glides in there as a render's
    // design started at the line before the jump does (a glide under way ends as it would). Then the line's own design comes
    // in at the line after, as a render's does: the jump's sound is the render's from 1.5 L after it at the latest. With fewer
    // than L / 8 samples to the line, which would crowd a design's work into them, the line's design follows as at any line.
    //
    // Two things keep a loop shorter than the grid's step from holding the kernels still for ever. A design done by the jump,
    // for the values here, glides in from the jump: with no line of the grid inside the loop, no line would bring it in. And of
    // two designs in a row only the first is dropped: where each pass of a loop starts a design at its line and the jump at
    // its wrap finds other values, every second one ends and comes in.
    const auto forOthers = designing && ! putInWhole && differ (now, setValues[(size_t) designSet]);

    if (! forOthers && glideInIfDone())
        return;

    if (forOthers && ! std::exchange (droppedLast, true))
        designing = false;

    const auto intoStep = (int) (((nextSongSample % half) + half) % half);
    const auto toLine = intoStep == 0 ? half : half - intoStep;

    if (! designing && toLine >= half / 4 && differ (now, setValues[(size_t) (gliding >= 0 ? gliding : played)]))
        startDesign (now, values, getFreeSet(), false, toLine);
}

void LinearPhase::atLine (const Compared& now, std::span<const float> values) noexcept
{
    // A design done glides in from a line, once a glide under way has ended; then one starts for the values here.
    glideInIfDone();

    if (! designing && differ (now, setValues[(size_t) (gliding >= 0 ? gliding : played)]))
        startDesign (now, values, getFreeSet(), false, half);
}

bool LinearPhase::glideInIfDone() noexcept
{
    if (! designing || putInWhole || designPlayed < designSpread || gliding >= 0)
        return false;

    gliding = designSet;
    designing = droppedLast = false;
    glideDone = 0;
    convolution.setGliding (&sets[(size_t) gliding]);
    return true;
}

void LinearPhase::startDesign (const Compared& compared, std::span<const float> values, int set, bool whole, int spread) noexcept
{
    designing = true;
    putInWhole = whole;
    designSet = set;
    designSpread = std::max (1, spread);
    designPlayed = 0;
    nextStep = 0;
    ++designsStarted;
    setValues[(size_t) set] = compared;
    const auto mono = compared.back() > 0.5f;

    // Each band's sections as the minimum phase plays them, those that play.
    auto anySides = false, anyMiddleOrSide = false;
    auto numSections = 0, numPlayed = 0;

    for (int index = 0; index < numBands; ++index)
    {
        auto& band = bands[(size_t) index];
        const auto shape = Equalizer::getBandShape (values, index, sampleRate);
        const auto type = (int) std::lround (values[(size_t) Equalizer::getParameterIndex (index, Equalizer::type)]);
        const auto gain = values[(size_t) Equalizer::getParameterIndex (index, Equalizer::gain)];
        band.placement = toPlacement (values[(size_t) Equalizer::getParameterIndex (index, Equalizer::placement)]);
        band.numSections = shape.numSections;
        band.sections = shape.sections;
        band.played = shape.numSections > 0 && ! (isShelfOrBell (type) && std::abs (gain) < 0.001f);

        if (mono && (band.placement == (int) Placement::side || band.placement == (int) Placement::right))
            band.played = false;

        if (band.played)
        {
            anySides = anySides || band.placement == (int) Placement::left || band.placement == (int) Placement::right;
            anyMiddleOrSide = anyMiddleOrSide || band.placement == (int) Placement::mid || band.placement == (int) Placement::side;
            numSections += band.numSections;
            ++numPlayed;
        }
    }

    // The kernels there are: one for both sides; the middle's and the side's, or the sides' and the crossings' (which of the
    // two is known once the responses are: finishResponse()); or all four.
    mayPlayMidSide = false;

    if (mono)
    {
        numKernels = numResponses = 1;
        entries = { 4, 0, 0, 0 };
        routes = { 0, -1, -1, 0 };
    }
    else if (anySides)
    {
        numKernels = numResponses = 4;
        entries = { 0, 1, 2, 3 };
        routes = { 0, 1, 2, 3 };
    }
    else if (anyMiddleOrSide)
    {
        numKernels = 2;
        numResponses = 4;
        entries = { 0, 1, 5, 6 };
        routes = { 0, 1, 1, 0 };
        mayPlayMidSide = true;
    }
    else
    {
        numKernels = numResponses = 1;
        entries = { 0, 0, 0, 0 };
        routes = { 0, -1, -1, 0 };
    }

    auto& kernels = sets[(size_t) set];
    kernels.routes = routes;
    kernels.midSide = false;

    for (int k = 0; k < 4; ++k)
        kernels.kinds[(size_t) k] = k < numKernels ? Kernels::Kind::filter : Kernels::Kind::none;

    largest.fill (0.0);
    offOne.fill (0.0);

    // Its steps, each with its work.
    numSteps = 0;
    const auto add = [this] (StepKind kind, int a, int b, int cost) { steps[(size_t) numSteps++] = { kind, a, b, cost }; };
    const auto chunk = (bins + responseChunks - 1) / responseChunks;

    for (int from = 0; from < bins; from += chunk)
        add (StepKind::response, from, std::min (bins, from + chunk),
             std::min (chunk, bins - from) * (3 * numSections + 2 * numPlayed + 2 * numResponses + 2));

    const auto middle = (taps - 1) / 2;

    for (int pair = 0; pair < (numKernels + 1) / 2; ++pair)
    {
        for (int part = 0; part < fillParts; ++part)
            add (StepKind::fill, pair, part, gridPoints / fillParts);

        for (int part = 0; part < bigFft.getNumParts(); ++part)
            add (StepKind::transform, pair, part, bigFft.getPartCost (part));

        for (int part = 0; part < windowParts; ++part)
            add (StepKind::window, pair, part, 2 * (middle + 1) / windowParts);

        for (int part = 0; part < zeroHertzParts; ++part)
            add (StepKind::zeroHertz, pair, part, 2 * taps / zeroHertzParts);
    }

    const auto partition = convolution.getPartitionSize();
    const auto partitionPairs = (convolution.getNumPartitions() + 1) / 2;
    const auto smallParts = smallFft.getNumParts();

    for (int kernel = 0; kernel < numKernels; ++kernel)
        for (int pair = 0; pair < partitionPairs; ++pair)
        {
            add (StepKind::partitionFill, kernel, pair, partition);

            for (int part = 0; part < smallParts; ++part)
                add (StepKind::partitionTransform, kernel, pair * smallParts + part, smallFft.getPartCost (part));

            add (StepKind::partitionTakeApart, kernel, pair, 2 * (partition + 1));
        }

    designCost = bandCost * numBands;
    designDone = designCost; // the bands' sections, worked out as it starts
    workDone += designCost;

    for (int i = 0; i < numSteps; ++i)
        designCost += steps[(size_t) i].cost;
}

void LinearPhase::advanceDesign (int samples) noexcept
{
    if (! designing)
        return;

    // So much of its work for each sample played: all of it in the samples it is spread over (half the latency, or after a
    // jump those to the grid's next line).
    designPlayed += samples;
    const auto owed = designPlayed >= designSpread ? designCost : (designCost * designPlayed + designSpread - 1) / designSpread;

    while (nextStep < numSteps && designDone < owed)
    {
        const auto& step = steps[(size_t) nextStep++];

        if (doStep (step))
            workDone += step.cost;

        designDone += step.cost;
    }

    if (putInWhole && designPlayed >= designSpread)
    {
        played = designSet;
        designing = false;
        convolution.setPlayed (&sets[(size_t) played]);
    }
}

bool LinearPhase::doStep (const Step& step) noexcept
{
    auto& kernels = sets[(size_t) designSet];
    const auto isFilter = [&kernels, this] (int kernel)
    { return kernel < numKernels && kernels.kinds[(size_t) kernel] == Kernels::Kind::filter; };
    const auto middle = (taps - 1) / 2;
    const auto partition = convolution.getPartitionSize();

    switch (step.kind)
    {
        case StepKind::response:
        {
            for (int k = step.a; k < step.b; ++k)
            {
                // The bands in their order, as a matrix from (left, right) in to (left, right) out, and the mono channel's gain.
                const auto tangent = tangents[(size_t) k];
                double m00 = 1.0, m01 = 0.0, m10 = 0.0, m11 = 1.0, mono = 1.0;

                for (const auto& band : bands)
                {
                    if (! band.played)
                        continue;

                    auto squared = 1.0;

                    for (int section = 0; section < band.numSections; ++section)
                    {
                        const auto& c = band.sections[(size_t) section];
                        const auto w = tangent / (double) c.g;
                        const auto denominatorReal = 1.0 - w * w, denominatorImaginary = (double) c.k * w;
                        const auto numeratorReal = (double) c.m0 * denominatorReal + (double) c.m2;
                        const auto numeratorImaginary = ((double) c.m0 * (double) c.k + (double) c.m1) * w;
                        squared *= (numeratorReal * numeratorReal + numeratorImaginary * numeratorImaginary)
                                   / (denominatorReal * denominatorReal + denominatorImaginary * denominatorImaginary);
                    }

                    const auto g = std::sqrt (squared);

                    switch ((Placement) band.placement)
                    {
                        case Placement::stereo:
                            m00 *= g;
                            m01 *= g;
                            m10 *= g;
                            m11 *= g;
                            mono *= g;
                            break;

                        case Placement::mid:
                        case Placement::side:
                        {
                            // The identity plus (g - 1) times the half matrix of ones, or of (1, -1; -1, 1).
                            const auto a = 0.5 * (1.0 + g);
                            const auto b = band.placement == (int) Placement::mid ? 0.5 * (g - 1.0) : 0.5 * (1.0 - g);
                            const auto n00 = a * m00 + b * m10, n01 = a * m01 + b * m11;
                            const auto n10 = b * m00 + a * m10, n11 = b * m01 + a * m11;
                            m00 = n00;
                            m01 = n01;
                            m10 = n10;
                            m11 = n11;

                            if (band.placement == (int) Placement::mid)
                                mono *= g;

                            break;
                        }

                        case Placement::left:
                            m00 *= g;
                            m01 *= g;
                            mono *= g;
                            break;

                        case Placement::right:
                            m10 *= g;
                            m11 *= g;
                            break;
                    }
                }

                // The middle's and the side's: with no band on Left or Right, m00 = m11 and m01 = m10.
                const double entry[] = { m00, m01, m10, m11, mono, m00 + m01, m00 - m01 };

                for (int kernel = 0; kernel < numResponses; ++kernel)
                {
                    const auto value = entry[entries[(size_t) kernel]];
                    responses[(size_t) kernel][(size_t) k] = (float) value;
                    largest[(size_t) kernel] = std::max (largest[(size_t) kernel], std::abs (value));
                    offOne[(size_t) kernel] = std::max (offOne[(size_t) kernel], std::abs (value - 1.0));
                }
            }

            if (step.b >= bins)
                finishResponse();

            return true;
        }

        case StepKind::fill:
        {
            if (! isFilter (2 * step.a) && ! isFilter (2 * step.a + 1))
                return false;

            // Two kernels' responses at once, the first real and the second imaginary: both real and even, so each comes
            // out of the inverse transform real, and apart.
            const auto& first = responses[(size_t) (2 * step.a)];
            const auto* second = 2 * step.a + 1 < numKernels ? responses[(size_t) (2 * step.a + 1)].data() : nullptr;
            const auto part = gridPoints / fillParts;

            for (int k = step.b * part; k < (step.b + 1) * part; ++k)
            {
                const auto at = (size_t) std::min (k, gridPoints - k);
                bigIn[(size_t) k] = { first[at], second != nullptr ? second[at] : 0.0f };
            }

            return true;
        }

        case StepKind::transform:
            if (! isFilter (2 * step.a) && ! isFilter (2 * step.a + 1))
                return false;

            bigFft.perform (step.b, bigIn.data(), bigWork.data(), bigOut.data());
            return true;

        case StepKind::window:
        {
            if (! isFilter (2 * step.a) && ! isFilter (2 * step.a + 1))
                return false;

            // Each tap as the mean of its two sides, so that the kernel is symmetric to the last bit, under the window; and the
            // kernels' sums.
            auto& first = kernelTaps[(size_t) (2 * step.a)];
            auto* second = 2 * step.a + 1 < numKernels ? kernelTaps[(size_t) (2 * step.a + 1)].data() : nullptr;
            const auto part = (middle + windowParts) / windowParts;

            if (step.b == 0)
                tapSums.fill (0.0);

            for (int j = step.b * part; j < std::min (middle + 1, (step.b + 1) * part); ++j)
            {
                const auto h = 0.5f * (bigOut[(size_t) j] + bigOut[(size_t) ((gridPoints - j) % gridPoints)]);
                const auto w = window[(size_t) j];
                const auto times = j == 0 ? 1.0 : 2.0;
                first[(size_t) (middle + j)] = first[(size_t) (middle - j)] = w * h.real();
                tapSums[0] += times * (double) first[(size_t) (middle + j)];

                if (second != nullptr)
                {
                    second[middle + j] = second[middle - j] = w * h.imag();
                    tapSums[1] += times * (double) second[middle + j];
                }
            }

            return true;
        }

        case StepKind::zeroHertz:
        {
            if (! isFilter (2 * step.a) && ! isFilter (2 * step.a + 1))
                return false;

            // The cut and the window dropped some of each kernel's sum, its gain at 0 Hz: the difference spread evenly over its
            // taps, so that its sum is its response there.
            const auto pairSize = std::min (2, numKernels - 2 * step.a);

            if (step.b == 0)
                for (int k = 0; k < pairSize; ++k)
                    tapsMoveBy[(size_t) k] =
                        (float) (((double) responses[(size_t) (2 * step.a + k)][0] - tapSums[(size_t) k]) / (double) taps);

            const auto part = (taps + zeroHertzParts - 1) / zeroHertzParts;

            for (int k = 0; k < pairSize; ++k)
            {
                auto& kernel = kernelTaps[(size_t) (2 * step.a + k)];

                for (int i = step.b * part; i < std::min (taps, (step.b + 1) * part); ++i)
                    kernel[(size_t) i] += tapsMoveBy[(size_t) k];
            }

            return true;
        }

        case StepKind::partitionFill:
        {
            if (! isFilter (step.a))
                return false;

            const auto& kernel = kernelTaps[(size_t) step.a];
            const auto first = 2 * step.b * partition, second = first + partition;

            for (int i = 0; i < partition; ++i)
            {
                smallIn[(size_t) i] = { first + i < taps ? kernel[(size_t) (first + i)] : 0.0f,
                                        second + i < taps ? kernel[(size_t) (second + i)] : 0.0f };
                smallIn[(size_t) (partition + i)] = {};
            }

            return true;
        }

        case StepKind::partitionTransform:
            if (! isFilter (step.a))
                return false;

            smallFft.perform (step.b % smallFft.getNumParts(), smallIn.data(), smallWork.data(), smallOut.data());
            return true;

        case StepKind::partitionTakeApart:
        {
            if (! isFilter (step.a))
                return false;

            auto* spectra = kernels.spectra[(size_t) step.a].data();
            const auto first = 2 * step.b, second = first + 1;
            dsp::PartitionedConvolution::takeApart (smallOut.data(), partition, spectra + first * (partition + 1),
                                                    second < convolution.getNumPartitions() ? spectra + second * (partition + 1)
                                                                                            : spare.data());
            return true;
        }
    }

    return false;
}

void LinearPhase::finishResponse() noexcept
{
    auto& kernels = sets[(size_t) designSet];

    // With bands on Mid or Side and none on Left or Right: where the middle or the side is a delay alone, the two are played
    // (their responses moved into the first two places), else the sides' and the crossings'.
    if (mayPlayMidSide && (offOne[2] <= flat || offOne[3] <= flat))
    {
        for (size_t from = 0; from < 2; ++from)
        {
            std::swap (responses[from], responses[from + 2]);
            std::swap (largest[from], largest[from + 2]);
            std::swap (offOne[from], offOne[from + 2]);
        }

        kernels.routes = { 0, -1, -1, 1 };
        kernels.midSide = true;
    }

    // A kernel that is nothing at every frequency is skipped, and one that is 1 at every frequency is a delay alone.
    for (int kernel = 0; kernel < numKernels; ++kernel)
        kernels.kinds[(size_t) kernel] = largest[(size_t) kernel] <= flat  ? Kernels::Kind::none
                                         : offOne[(size_t) kernel] <= flat ? Kernels::Kind::delay
                                                                           : Kernels::Kind::filter;
}

void LinearPhase::process (float* const* channels, int numChannels, int length, std::span<const float> values) noexcept
{
    if (! isPrepared())
        return;

    length = std::clamp (length, 0, maxLength);
    Compared now {};
    auto compared = false;
    const auto getNow = [&]() -> const Compared&
    {
        if (! std::exchange (compared, true))
            now = compare (values, numChannels);

        return now;
    };

    for (int done = 0; done < length;)
    {
        // After a reset, the history is silence: kernels that differ from the values are designed at once and put in whole.
        if (std::exchange (afterReset, false) && ! designing && differ (getNow(), setValues[(size_t) played]))
            startDesign (getNow(), values, getFreeSet(), true, half);

        const auto jumpedHere = std::exchange (jumped, false);

        if (jumpedHere)
            atJump (getNow(), values);

        // Its own grid's lines, wherever they fall among the EQ's steps. A stretch never runs past the step under way, so that
        // where a jump cuts the step short (the song elsewhere on its grid, though not on the EQ's), its next line is still met.
        // The step a jump starts in the middle of the grid's step is no line: the jump has done what one does.
        if (grid.startsStep())
        {
            const auto isLine = grid.startStep() == half;

            if (isLine || ! jumpedHere)
                atLine (getNow(), values);
        }

        const auto n = grid.take (length - done);
        playStretch (channels, numChannels, done, n);
        grid.advance (n);
        done += n;
    }

    if (placeKnown)
        nextSongSample += length;
}

void LinearPhase::playStretch (float* const* channels, int numChannels, int from, int length) noexcept
{
    if (numChannels <= 0)
    {
        advanceDesign (length);
        return;
    }

    numChannels = std::min (numChannels, 2);

    // What came in, kept for its copy as late as the latency: silence for what came in before the reset (from the first
    // sample, a latency after the reset, that what came in since reaches back to).
    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto& kept = history[(size_t) channel];
        auto& held = historyHeld[(size_t) channel];
        const auto firstHeld = std::clamp (latency - held, 0, length);

        for (int i = 0; i < length; ++i)
        {
            kept[(size_t) ((historyAt + i) & historyMask)] = channels[channel][from + i];
            delayed[(size_t) channel][(size_t) (from + i)] =
                i >= firstHeld ? kept[(size_t) ((historyAt + i - latency) & historyMask)] : 0.0f;
        }

        held = std::min (held + length, historyMask + 1);
    }

    // A side not played keeps nothing of this stretch: what it held is from before it, and silence when it plays again.
    for (int channel = numChannels; channel < 2; ++channel)
        historyHeld[(size_t) channel] = 0;

    if (numChannels < 2)
        std::copy (delayed[0].begin() + from, delayed[0].begin() + from + length, delayed[1].begin() + from);

    historyAt = (historyAt + length) & historyMask;

    // Through the kernels, cut where a glide ends or kernels designed after a reset come in.
    for (int at = from; at < from + length;)
    {
        auto n = from + length - at;

        if (designing && putInWhole)
            n = std::min (n, (int) (designSpread - designPlayed));

        if (gliding >= 0)
            n = std::min (n, half - glideDone);

        n = std::max (1, n);
        play (channels, numChannels, at, n);
        advanceDesign (n);
        at += n;
    }
}

void LinearPhase::finish (const Kernels& set, float* const* out, int numChannels, int from, int length) const noexcept
{
    // A side whose one way through is a delay alone plays what came in as late as the latency, to the bit (through the
    // transforms it would come back a float's rounding off).
    const auto isDelayAlone = [&set] (int side)
    {
        const auto self = set.routes[(size_t) (side * 2 + side)], other = set.routes[(size_t) (side * 2 + 1 - side)];
        return self >= 0 && set.kinds[(size_t) self] == Kernels::Kind::delay
               && (other < 0 || set.kinds[(size_t) other] == Kernels::Kind::none);
    };

    const auto* left = delayed[0].data() + from;
    const auto* right = delayed[1].data() + from;

    if (! set.midSide || numChannels < 2)
    {
        for (int side = 0; side < numChannels; ++side)
            if (isDelayAlone (side))
                std::copy (delayed[(size_t) side].begin() + from, delayed[(size_t) side].begin() + from + length, out[side]);

        return;
    }

    // The middle and the side, played, put back together as the left and the right.
    const auto middleDelayed = isDelayAlone (0), sideDelayed = isDelayAlone (1);

    for (int i = 0; i < length; ++i)
    {
        const auto middle = middleDelayed ? (left[i] + right[i]) * 0.5f : out[0][i];
        const auto side = sideDelayed ? (left[i] - right[i]) * 0.5f : out[1][i];
        out[0][i] = middle + side;
        out[1][i] = middle - side;
    }
}

void LinearPhase::play (float* const* channels, int numChannels, int from, int length) noexcept
{
    float* io[2] = { channels[0] + from, channels[numChannels - 1] + from };
    float* to[2] = { glided[0].data() + from, glided[1].data() + from };
    convolution.process (io, numChannels, length, io, gliding >= 0 ? to : nullptr);
    finish (sets[(size_t) played], io, numChannels, from, length);

    if (gliding < 0)
        return;

    finish (sets[(size_t) gliding], to, numChannels, from, length);

    // Both sets' outputs crossfaded in a straight line.
    for (int channel = 0; channel < numChannels; ++channel)
        for (int i = 0; i < length; ++i)
        {
            const auto t = (float) (glideDone + i + 1) / (float) half;
            io[channel][i] += (to[channel][i] - io[channel][i]) * t;
        }

    glideDone += length;

    if (glideDone >= half)
    {
        convolution.endGlide();
        played = std::exchange (gliding, -1);
    }
}

int LinearPhase::getNumKernelsPlayed() const noexcept
{
    auto count = 0;

    for (const auto kind : sets[(size_t) played].kinds)
        if (kind == Kernels::Kind::filter)
            ++count;

    return count;
}

int LinearPhase::getMultiplyAddsPerSample (int numChannels) const noexcept
{
    return dsp::PartitionedConvolution::getMultiplyAddsPerSample (sets[(size_t) played], numChannels, convolution.getPartitionSize(),
                                                                  convolution.getNumPartitions());
}

std::vector<float> LinearPhase::getPlayedTaps (int kernel) const
{
    return kernelTaps[(size_t) std::clamp (kernel, 0, 3)];
}
} // namespace phazite::devices

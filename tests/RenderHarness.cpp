// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RenderHarness.h"
#include "EnginePlayback.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/base/internal/Schema.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace phazite::test
{
namespace
{
    template <size_t size>
    juce::String formatNumbers (const std::array<float, size>& numbers, int decimals)
    {
        juce::StringArray texts;

        for (const auto number : numbers)
            texts.add (juce::String ((double) number, decimals));

        return "[" + texts.joinIntoString (", ") + "]";
    }

    juce::String formatBandEdges()
    {
        juce::StringArray texts;

        for (const auto edge : getBandEdges())
            texts.add (juce::String (edge, 1));

        return "[" + texts.joinIntoString (", ") + "]";
    }

    juce::String getChannelName (int channel)
    {
        if (channel == 0)
            return "left";

        if (channel == 1)
            return "right";

        return "channel " + juce::String (channel + 1);
    }

    // A band as a difference names it: "below 44 Hz", "62.5 Hz", "8 kHz", "above 11.3 kHz".
    juce::String getBandName (int band)
    {
        const auto edges = getBandEdges();
        const auto name = [] (double hertz, int decimals)
        { return hertz < 1000.0 ? juce::String (hertz, decimals) + " Hz" : juce::String (hertz / 1000.0, decimals) + " kHz"; };

        if (band == 0)
            return "below " + juce::String (juce::roundToInt (edges.front())) + " Hz";

        if (band == numBands - 1)
            return "above " + name (edges.back(), 1);

        const auto middle = edges[(size_t) band - 1] * juce::MathConstants<double>::sqrt2;
        return middle < 100.0    ? name (middle, 1)
               : middle < 1000.0 ? juce::String (juce::roundToInt (middle)) + " Hz"
                                 : juce::String (juce::roundToInt (middle / 1000.0)) + " kHz";
    }

    // A number of a block of a parsed fingerprint (property is "rms", "peak", "bands", "hertz" or "centre"), or nothing if
    // the block has none there.
    std::optional<double> findNumber (const juce::var& block, const char* property, int index)
    {
        if (const auto* numbers = block[property].getArray(); numbers != nullptr && juce::isPositiveAndBelow (index, numbers->size()))
            if (const auto& number = numbers->getReference (index); number.isDouble() || number.isInt())
                return (double) number;

        return std::nullopt;
    }

    //==============================================================================
    // A second-order section of a filter, with what it remembers of the sound.
    struct Section
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    };

    // A Butterworth low-pass or high-pass at a frequency, 12 dB an octave (the bilinear transform's, as in Robert
    // Bristow-Johnson's cookbook).
    Section makeEdge (double hertz, double sampleRate, bool highPass)
    {
        const auto omega = juce::MathConstants<double>::twoPi * std::min (hertz, 0.45 * sampleRate) / sampleRate;
        const auto cosine = std::cos (omega), alpha = std::sin (omega) / juce::MathConstants<double>::sqrt2;
        const auto a0 = 1.0 + alpha;
        Section section;
        section.b0 = (highPass ? 1.0 + cosine : 1.0 - cosine) / 2.0 / a0;
        section.b1 = (highPass ? -(1.0 + cosine) : 1.0 - cosine) / a0;
        section.b2 = section.b0;
        section.a1 = -2.0 * cosine / a0;
        section.a2 = (1.0 - alpha) / a0;
        return section;
    }

    // A band's filter: each of its edges twice over, 24 dB an octave; the lowest band has no lower edge and the highest no
    // upper one.
    std::vector<Section> makeBandFilter (int band, double sampleRate)
    {
        const auto edges = getBandEdges();
        std::vector<Section> sections;

        if (band > 0)
            sections.insert (sections.end(), 2, makeEdge (edges[(size_t) band - 1], sampleRate, true));

        if (band < numBands - 1)
            sections.insert (sections.end(), 2, makeEdge (edges[(size_t) band], sampleRate, false));

        return sections;
    }

    // Adds to each block's sum the squares of a channel's samples through a band's filter, so many times over.
    void addBandSquares (const float* samples, int numBlocks, int blockSamples, std::vector<Section> sections, double weight,
                         double* blockSums)
    {
        auto* const first = sections.data();
        auto* const end = first + sections.size();

        for (int block = 0; block < numBlocks; ++block)
        {
            auto sum = 0.0;

            for (int i = 0; i < blockSamples; ++i)
            {
                auto value = (double) samples[block * blockSamples + i];

                for (auto* section = first; section != end; ++section)
                {
                    const auto out = section->b0 * value + section->b1 * section->x1 + section->b2 * section->x2 - section->a1 * section->y1
                                     - section->a2 * section->y2;
                    section->x2 = section->x1;
                    section->x1 = value;
                    section->y2 = section->y1;
                    section->y1 = out;
                    value = out;
                }

                sum += value * value;
            }

            blockSums[block] += weight * sum;
        }
    }

    // Each band's level in each block, of both sides together, in dBFS.
    void findBands (const juce::AudioBuffer<float>& audio, int blockSamples, Fingerprint& fingerprint)
    {
        const auto numBlocks = (int) fingerprint.size();
        const auto sampleRate = 10.0 * blockSamples;
        const auto isMono = audio.getNumChannels() < renderChannels;
        std::vector<double> sums ((size_t) numBlocks);

        for (int band = 0; band < numBands; ++band)
        {
            const auto filter = makeBandFilter (band, sampleRate);
            std::fill (sums.begin(), sums.end(), 0.0);

            // A mono sound is on both sides.
            addBandSquares (audio.getReadPointer (0), numBlocks, blockSamples, filter, isMono ? 2.0 : 1.0, sums.data());

            if (! isMono)
                addBandSquares (audio.getReadPointer (1), numBlocks, blockSamples, filter, 1.0, sums.data());

            for (int block = 0; block < numBlocks; ++block)
                fingerprint[(size_t) block].bands[(size_t) band] =
                    (float) (10.0 * std::log10 (std::max (sums[(size_t) block] / blockSamples, 1.0e-10)));
        }
    }

    // The frequency a side of a block's power lies around, in hertz, and where in the block it lies, in milliseconds from
    // its start. The mean square of the differences between one sample and the next, against the samples' own, is the
    // mean of (2 sin (pi f / rate)) squared over the sound's power: for a tone, at its own frequency.
    void findFrequencyAndCentre (const float* samples, int start, int blockSamples, double sampleRate, float& hertz, float& centre)
    {
        auto squares = 0.0, differences = 0.0, moment = 0.0;
        auto previous = start > 0 ? (double) samples[start - 1] : 0.0;

        for (int i = 0; i < blockSamples; ++i)
        {
            const auto value = (double) samples[start + i];
            squares += value * value;
            differences += (value - previous) * (value - previous);
            moment += (i + 0.5) * value * value;
            previous = value;
        }

        const auto sounds = squares > 0.0;
        hertz = sounds ? (float) (sampleRate / juce::MathConstants<double>::pi
                                  * std::asin (std::min (1.0, 0.5 * std::sqrt (differences / squares))))
                       : 0.0f;
        centre = sounds ? (float) (moment / squares / sampleRate * 1000.0) : 0.0f;
    }

    //==============================================================================
    // Phazite's engine, playing a song.
    juce::AudioBuffer<float> renderSong (Song& song, double seconds, double sampleRate, int blockSize)
    {
        REQUIRE (renderMissingClipAudio (song)); // as an export does: a clip whose render isn't ready would play nothing
        return renderWithOwnEngine (song, (int) std::lround (seconds * sampleRate), sampleRate, blockSize);
    }

    // The open project's.
    class OwnEngineRender final : public RenderEngine
    {
    public:
        juce::String getName() const override { return "reference"; }

        juce::AudioBuffer<float> render (ProjectContext& context, double seconds) override
        {
            return renderOffline (context, seconds, renderSampleRate, renderBlockSize);
        }
    };

    // A project opened afresh from a copy of the song alone, as a save would hold it, in a context of its own as a project
    // opens in: so that nothing a render leaves behind (where its modulators were) plays in the next, and the song alone
    // makes what it plays.
    class OwnEngineOnACopy final : public RenderEngine
    {
    public:
        juce::String getName() const override { return "candidate"; }

        juce::AudioBuffer<float> render (ProjectContext& context, double seconds) override
        {
            auto* song = findSong (context);
            REQUIRE (song != nullptr);
            song->storeDeviceStates(); // as a save does
            const auto copy = makeSong (song->getState().createCopy(), song->getFile(), song->getCacheFolder());
            REQUIRE (copy != nullptr);

            ProjectContext opened (getEngineContext());
            setSong (opened, copy.get());
            settle();
            auto audio = renderSong (*copy, seconds, renderSampleRate, renderBlockSize);
            setSong (opened, nullptr);
            return audio;
        }
    };

    // Checks a render (check() first, keeping it if it fails), and returns its fingerprint's text.
    juce::String checkRender (const juce::AudioBuffer<float>& audio, double sampleRate, const GoldenRender& golden,
                              const juce::String& keptName)
    {
        const auto blockSamples = (int) std::lround (sampleRate / 10.0);
        const auto fingerprint = getFingerprint (audio, blockSamples);
        const auto soundsRight = golden.check == nullptr || golden.check (fingerprint, audio);

        const auto text = writeFingerprint (fingerprint, golden.about, (int) sampleRate, blockSamples);

        if (! soundsRight)
        {
            keepRender (audio, sampleRate, keptName);
            getActualFolder().getChildFile ("renders").getChildFile (keptName + ".json").replaceWithText (text, false, false, "\n");
        }

        REQUIRE_MESSAGE (soundsRight,
                         ("The render of " + golden.name + " doesn't sound as it should (the checks above say why)").toStdString());
        return text;
    }

    // Compares a fingerprint with its golden file, keeping the render if they differ.
    void compareWithGolden (const juce::String& text, const juce::AudioBuffer<float>& audio, double sampleRate, const GoldenRender& golden,
                            const juce::String& keptName)
    {
        auto differs = false;
        checkGoldenText ("renders/" + golden.name + ".json", text,
                         [&differs] (const juce::String& expectedText, const juce::String& actualText)
                         {
                             const auto found = compareFingerprints (expectedText, actualText);
                             differs = ! found.isEmpty();
                             return found;
                         });

        if (differs)
            keepRender (audio, sampleRate, keptName);
    }

    // Checks what another way of playing the song played (a copy of it rendered, the song played live), and that it sounds
    // as the reference's render does.
    void checkAgainstReference (const juce::AudioBuffer<float>& audio, const juce::AudioBuffer<float>& referenceAudio,
                                const juce::String& referenceText, const GoldenRender& golden, const juce::String& player,
                                const juce::String& what)
    {
        const auto text = checkRender (audio, renderSampleRate, golden, golden.name + "-" + player);
        const auto differences = compareFingerprints (referenceText, text);

        if (! differences.isEmpty())
            keepRender (audio, renderSampleRate, golden.name + "-" + player);

        const auto apart = getDifference (referenceAudio, audio, renderSampleRate);
        INFO ((what + " plays " + golden.name + " otherwise than a render of the song itself (sample by sample, up to "
               + formatLevel (apart.peakDb) + " dBFS apart, at " + juce::String (apart.seconds, 4) + " s):\n  "
               + differences.joinIntoString ("\n  "))
                  .toStdString());
        CHECK (differences.isEmpty());
    }
} // namespace

//==============================================================================
std::array<double, numBands - 1> getBandEdges()
{
    std::array<double, numBands - 1> edges {};
    auto edge = 62.5 / juce::MathConstants<double>::sqrt2;

    for (auto& hertz : edges)
    {
        hertz = edge;
        edge *= 2.0;
    }

    return edges;
}

Fingerprint getFingerprint (const juce::AudioBuffer<float>& audio, int blockSamples)
{
    Fingerprint fingerprint;

    if (audio.getNumChannels() == 0 || blockSamples <= 0)
        return fingerprint;

    const auto sampleRate = 10.0 * blockSamples;

    for (int start = 0; start + blockSamples <= audio.getNumSamples(); start += blockSamples)
    {
        BlockLevels levels;

        for (int channel = 0; channel < renderChannels; ++channel)
        {
            const auto source = std::min (channel, audio.getNumChannels() - 1);
            levels.rms[(size_t) channel] = juce::Decibels::gainToDecibels (audio.getRMSLevel (source, start, blockSamples), -100.0f);
            levels.peak[(size_t) channel] = juce::Decibels::gainToDecibels (audio.getMagnitude (source, start, blockSamples), -100.0f);
            findFrequencyAndCentre (audio.getReadPointer (source), start, blockSamples, sampleRate, levels.hertz[(size_t) channel],
                                    levels.centre[(size_t) channel]);
        }

        fingerprint.push_back (levels);
    }

    findBands (audio, blockSamples, fingerprint);
    return fingerprint;
}

juce::String writeFingerprint (const Fingerprint& fingerprint, const juce::String& about, int sampleRate, int blockSamples)
{
    juce::String text;
    text << "{\n"
         << "  \"about\": \"" << juce::JSON::escapeString (about) << "\",\n"
         << "  \"sampleRate\": " << sampleRate << ",\n"
         << "  \"blockSamples\": " << blockSamples << ",\n"
         << "  \"channels\": " << renderChannels << ",\n"
         << "  \"bandEdges\": " << formatBandEdges() << ",\n"
         << "  \"blocks\": [\n";

    for (size_t i = 0; i < fingerprint.size(); ++i)
    {
        const auto& block = fingerprint[i];
        const auto start = (double) i * blockSamples / sampleRate;
        text << "    { \"start\": " << juce::String (start, 1) << ", \"rms\": " << formatNumbers (block.rms, 2)
             << ", \"peak\": " << formatNumbers (block.peak, 2) << ", \"bands\": " << formatNumbers (block.bands, 2)
             << ", \"hertz\": " << formatNumbers (block.hertz, 1) << ", \"centre\": " << formatNumbers (block.centre, 2) << " }"
             << (i + 1 < fingerprint.size() ? "," : "") << "\n";
    }

    text << "  ]\n"
         << "}\n";
    return text;
}

juce::StringArray compareFingerprints (const juce::String& expectedText, const juce::String& actualText)
{
    const auto expected = juce::JSON::parse (expectedText);
    const auto actual = juce::JSON::parse (actualText);

    if (! expected.isObject())
        return { "the golden file isn't a JSON object" };

    if (! actual.isObject())
        return { "Phazite's fingerprint isn't a JSON object" };

    juce::StringArray differences;

    for (const auto* property : { "sampleRate", "blockSamples", "channels", "bandEdges" })
    {
        const auto expectedValue = juce::JSON::toString (expected[property], true);
        const auto actualValue = juce::JSON::toString (actual[property], true);

        if (expectedValue != actualValue)
            differences.add (juce::String (property) + ": expected "
                             + (expected[property].isVoid() ? juce::String ("nothing") : expectedValue) + ", got "
                             + (actual[property].isVoid() ? juce::String ("nothing") : actualValue));
    }

    const auto* expectedBlocks = expected["blocks"].getArray();
    const auto* actualBlocks = actual["blocks"].getArray();
    const auto numExpected = expectedBlocks != nullptr ? expectedBlocks->size() : 0;
    const auto numActual = actualBlocks != nullptr ? actualBlocks->size() : 0;

    if (numExpected != numActual)
        differences.add ("blocks: expected " + juce::String (numExpected) + ", got " + juce::String (numActual));

    if (! differences.isEmpty())
        return differences;

    const auto blockSeconds = (double) expected["blockSamples"] / (double) expected["sampleRate"];
    const auto channels = (int) expected["channels"];
    const auto describe = [] (std::optional<double> number, int decimals, const char* unit)
    { return number.has_value() ? juce::String (*number, decimals) + " " + unit : juce::String ("nothing"); };

    for (int block = 0; block < numExpected; ++block)
    {
        const auto& expectedBlock = expectedBlocks->getReference (block);
        const auto& actualBlock = actualBlocks->getReference (block);
        const auto when = juce::String (block * blockSeconds, 2) + "-" + juce::String ((block + 1) * blockSeconds, 2) + " s";

        // A level, where both have one: the same within the tolerance, above the floor.
        const auto compareLevel = [&] (const char* property, int index, const juce::String& name)
        {
            const auto expectedLevel = findNumber (expectedBlock, property, index);
            const auto actualLevel = findNumber (actualBlock, property, index);

            if (! expectedLevel.has_value() || ! actualLevel.has_value()
                || std::abs (std::max (*actualLevel, (double) floorDb) - std::max (*expectedLevel, (double) floorDb)) > toleranceDb)
                differences.add (when + ", " + name + ": expected " + describe (expectedLevel, 2, "dBFS") + ", got "
                                 + describe (actualLevel, 2, "dBFS"));
        };

        for (const auto& [property, measure] : { std::pair { "rms", "RMS" }, std::pair { "peak", "peak" } })
            for (int channel = 0; channel < channels; ++channel)
                compareLevel (property, channel, getChannelName (channel) + " " + measure);

        for (int band = 0; band < numBands; ++band)
            compareLevel ("bands", band, getBandName (band) + " band");

        // A side's frequency and centre, where it sounds in both: what is under the floor is noise, anywhere.
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto expectedRms = findNumber (expectedBlock, "rms", channel);
            const auto actualRms = findNumber (actualBlock, "rms", channel);
            const auto sounds = expectedRms.has_value() && actualRms.has_value() && std::min (*expectedRms, *actualRms) > floorDb;

            const auto compare = [&] (const char* property, const char* measure, int decimals, const char* unit, double share, double fixed)
            {
                const auto expectedNumber = findNumber (expectedBlock, property, channel);
                const auto actualNumber = findNumber (actualBlock, property, channel);
                const auto both = expectedNumber.has_value() && actualNumber.has_value();

                if (! both || (sounds && std::abs (*actualNumber - *expectedNumber) > share * std::abs (*expectedNumber) + fixed + 1.0e-9))
                    differences.add (when + ", " + getChannelName (channel) + " " + measure + ": expected "
                                     + describe (expectedNumber, decimals, unit) + ", got " + describe (actualNumber, decimals, unit));
            };

            compare ("hertz", "frequency", 1, "Hz", toleranceHertzShare, toleranceHertz);
            compare ("centre", "centre", 2, "ms", 0.0, toleranceMilliseconds);
        }
    }

    limitDifferences (differences, maxListedDifferences);
    return differences;
}

juce::StringArray compareAudio (const juce::AudioBuffer<float>& expected, const juce::AudioBuffer<float>& actual, double sampleRate)
{
    const auto blockSamples = (int) std::lround (sampleRate / 10.0);
    return compareFingerprints (writeFingerprint (getFingerprint (expected, blockSamples), "expected", (int) sampleRate, blockSamples),
                                writeFingerprint (getFingerprint (actual, blockSamples), "actual", (int) sampleRate, blockSamples));
}

juce::String formatLevel (double level)
{
    return juce::String (level, 2);
}

juce::String describeLevels (const std::array<float, renderChannels>& levels)
{
    return "left " + formatLevel (levels[0]) + " dBFS, right " + formatLevel (levels[1]) + " dBFS";
}

float getLouder (const BlockLevels& block)
{
    return std::max (block.rms[0], block.rms[1]);
}

float getLoudest (const Fingerprint& fingerprint, size_t first, size_t end)
{
    auto loudest = -100.0f;

    for (auto i = first; i < std::min (end, fingerprint.size()); ++i)
        loudest = std::max (loudest, getLouder (fingerprint[i]));

    return loudest;
}

bool differSomewhere (const Fingerprint& a, const Fingerprint& b, float decibels)
{
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        for (size_t channel = 0; channel < (size_t) renderChannels; ++channel)
            if (std::abs (std::max (a[i].rms[channel], floorDb) - std::max (b[i].rms[channel], floorDb)) > decibels)
                return true;

    return a.size() != b.size();
}

float getPower (const BlockLevels& block)
{
    const auto power = std::pow (10.0f, block.rms[0] / 10.0f) + std::pow (10.0f, block.rms[1] / 10.0f);
    return 10.0f * std::log10 (std::max (power, 1.0e-10f));
}

namespace
{
    juce::String describeBlock (size_t block)
    {
        return juce::String ((double) block / 10.0, 1) + "-" + juce::String ((double) (block + 1) / 10.0, 1) + " s";
    }
} // namespace

bool isLouder (const Fingerprint& fingerprint, size_t louder, size_t quieter, float decibels, const juce::String& why)
{
    REQUIRE (std::max (louder, quieter) < fingerprint.size());
    const auto a = getPower (fingerprint[louder]), b = getPower (fingerprint[quieter]);
    const auto is = a >= b + decibels;
    CHECK_MESSAGE (is, (why + ": " + describeBlock (louder) + " should be at least " + formatLevel (decibels) + " dB louder than "
                        + describeBlock (quieter) + "; they are at " + formatLevel (a) + " and " + formatLevel (b) + " dBFS")
                           .toStdString());
    return is;
}

bool isBetween (const Fingerprint& fingerprint, size_t block, float low, float high, const juce::String& why)
{
    REQUIRE (block < fingerprint.size());
    const auto level = getPower (fingerprint[block]);
    const auto is = level >= low && level <= high;
    CHECK_MESSAGE (is, (why + ": " + describeBlock (block) + " should be between " + formatLevel (low) + " and " + formatLevel (high)
                        + " dBFS; it is at " + formatLevel (level))
                           .toStdString());
    return is;
}

bool swings (const Fingerprint& fingerprint, size_t first, size_t end, float decibels, const juce::String& why)
{
    auto low = 100.0f, high = -100.0f;

    for (auto i = first; i < std::min (end, fingerprint.size()); ++i)
    {
        low = std::min (low, getPower (fingerprint[i]));
        high = std::max (high, getPower (fingerprint[i]));
    }

    const auto is = high - low >= decibels;
    CHECK_MESSAGE (is, (why + ": from " + describeBlock (first) + " to " + describeBlock (end - 1) + " the level should swing by at least "
                        + formatLevel (decibels) + " dB; it goes from " + formatLevel (low) + " to " + formatLevel (high) + " dBFS")
                           .toStdString());
    return is;
}

Difference getDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, double sampleRate)
{
    const auto length = std::min (a.getNumSamples(), b.getNumSamples());
    auto loudest = 0.0f;
    auto where = 0;

    for (int channel = 0; channel < renderChannels; ++channel)
    {
        const auto* x = a.getReadPointer (std::min (channel, a.getNumChannels() - 1));
        const auto* y = b.getReadPointer (std::min (channel, b.getNumChannels() - 1));

        for (int i = 0; i < length; ++i)
        {
            if (const auto difference = std::abs (x[i] - y[i]); difference > loudest)
            {
                loudest = difference;
                where = i;
            }
        }
    }

    if (a.getNumSamples() != b.getNumSamples())
        return { 0.0f, length / sampleRate };

    return { juce::Decibels::gainToDecibels (loudest, -100.0f), where / sampleRate };
}

double estimateFrequency (const juce::AudioBuffer<float>& audio, int channel, double sampleRate, double startSeconds, double seconds)
{
    const auto start = (int) (startSeconds * sampleRate);
    const auto end = std::min (audio.getNumSamples(), start + (int) (seconds * sampleRate));
    auto crossings = 0;

    for (int i = start + 1; i < end; ++i)
        if ((audio.getSample (channel, i - 1) < 0.0f) != (audio.getSample (channel, i) < 0.0f))
            ++crossings;

    return end > start ? crossings / 2.0 / ((end - start) / sampleRate) : 0.0;
}

juce::File writeHit (const juce::File& file)
{
    constexpr double sampleRate = 44100.0;
    constexpr int length = 2205, fade = 220;
    juce::AudioBuffer<float> buffer (1, length);

    for (int i = 0; i < length; ++i)
    {
        const auto gain = std::min ({ 1.0, (double) i / fade, (double) (length - 1 - i) / fade });
        buffer.setSample (0, i, (float) (0.5 * gain * std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate)));
    }

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    REQUIRE (writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()));
    return file;
}

void keepRender (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name)
{
    const auto copy = getActualFolder().getChildFile ("renders").getChildFile (name + ".wav");
    auto written = false;

    if (copy.getParentDirectory().createDirectory().wasOk() && copy.deleteFile())
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (copy);

        if (auto writer = juce::WavAudioFormat().createWriterFor (
                stream, juce::AudioFormatWriterOptions {}
                            .withSampleRate (sampleRate)
                            .withNumChannels (audio.getNumChannels())
                            .withBitsPerSample (32)
                            .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)))
            written = writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    MESSAGE (((written ? "The render is in " : "The render couldn't be written to ") + copy.getFullPathName()).toStdString());
}

//==============================================================================
juce::AudioBuffer<float> renderOffline (ProjectContext& context, double seconds, double sampleRate, int blockSize)
{
    auto* song = findSong (context);
    REQUIRE (song != nullptr);
    return renderSong (*song, seconds, sampleRate, blockSize);
}

juce::AudioBuffer<float> playLive (ProjectContext& context, double seconds, double sampleRate, int blockSize)
{
    auto* song = findSong (context);
    auto* session = getSession (context);
    REQUIRE (song != nullptr);
    REQUIRE (session != nullptr);
    REQUIRE (renderMissingClipAudio (*song)); // as for a render: a clip whose render isn't ready would play nothing

    // The app's engine on a device at this rate, stopped at the song's start for half a second: what was awake is asleep.
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::realTime, sampleRate, blockSize);
    REQUIRE_MESSAGE (session->isPlayingProject(), "the engine plays another project than the one to play live");
    session->stop();
    session->locate (BeatPosition());
    EnginePlayback::waitForFiles();
    playback.play (0.5);

    // What a render doesn't play, and how late the song comes out, which a render plays ahead by.
    auto late = 0;
    {
        const auto* plan = session->getEngine().getPlan();
        REQUIRE (plan != nullptr);
        REQUIRE_MESSAGE (! plan->loop.has_value(), "a song that loops doesn't play live as a render plays it");
        REQUIRE_MESSAGE (! plan->metronome, "a song with its metronome on doesn't play live as a render plays it");
        late = engine::Engine::getOutputLatency (*plan);
    }

    // All in one go, without a turn of the message loop between, in which the session would stop it at the song's end.
    const auto numSamples = (int) std::lround (seconds * sampleRate);
    session->play();
    const auto played = playback.playSamples (late + numSamples);
    session->stop();

    juce::AudioBuffer<float> audio (renderChannels, numSamples);

    for (int channel = 0; channel < renderChannels; ++channel)
        audio.copyFrom (channel, 0, played, channel, late, numSamples);

    return audio;
}

Fingerprint renderFingerprint (ProjectContext& context, double seconds)
{
    return getFingerprint (makeReferenceEngine()->render (context, seconds));
}

std::unique_ptr<RenderEngine> makeReferenceEngine()
{
    return std::make_unique<OwnEngineRender>();
}

std::unique_ptr<RenderEngine> makeCandidateEngine()
{
    return std::make_unique<OwnEngineOnACopy>();
}

void checkGoldenRender (ProjectContext& context, const GoldenRender& golden, const juce::String& liveDiffers)
{
    const auto reference = makeReferenceEngine();
    const auto referenceAudio = reference->render (context, golden.seconds);
    const auto referenceText = checkRender (referenceAudio, renderSampleRate, golden, golden.name + "-" + reference->getName());
    compareWithGolden (referenceText, referenceAudio, renderSampleRate, golden, golden.name + "-" + reference->getName());

    const auto candidate = makeCandidateEngine();
    checkAgainstReference (candidate->render (context, golden.seconds), referenceAudio, referenceText, golden, candidate->getName(),
                           "A copy of the song");

    const auto liveAudio = playLive (context, golden.seconds);

    if (liveDiffers.isEmpty())
        checkAgainstReference (liveAudio, referenceAudio, referenceText, golden, "live", "The song played live");
    else
        checkRender (liveAudio, renderSampleRate, golden, golden.name + "-live");
}

void checkGoldenAudio (const juce::AudioBuffer<float>& audio, double sampleRate, const GoldenRender& golden)
{
    const auto text = checkRender (audio, sampleRate, golden, golden.name);
    compareWithGolden (text, audio, sampleRate, golden, golden.name);
}

void checkGoldenChange (ProjectContext& context, const std::function<void()>& change, GoldenRender golden, const ChangeCheck& check,
                        const juce::String& liveDiffers)
{
    settle();
    const auto before = renderFingerprint (context, golden.seconds);
    change();
    settle();
    golden.check = [&before, &check] (const Fingerprint& after, const juce::AudioBuffer<float>&) { return check (before, after); };
    checkGoldenRender (context, golden, liveDiffers);
}

bool changesLevel (const Fingerprint& before, const Fingerprint& after, size_t block, float decibels, const juce::String& why)
{
    REQUIRE (block < before.size());
    const auto was = getPower (before[block]);
    return decibels >= 0.0f ? isBetween (after, block, was + decibels, 6.0f, why) : isBetween (after, block, -100.0f, was + decibels, why);
}
} // namespace phazite::test

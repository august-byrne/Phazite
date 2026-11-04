// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RenderHarness.h"
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
    juce::String formatLevels (const std::array<float, renderChannels>& levels)
    {
        juce::StringArray texts;

        for (const auto level : levels)
            texts.add (formatLevel (level));

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

    // A level of a block of a parsed fingerprint (property is "rms" or "peak"), or nothing if the block has none there.
    std::optional<double> findLevel (const juce::var& block, const char* property, int channel)
    {
        if (const auto* levels = block[property].getArray(); levels != nullptr && juce::isPositiveAndBelow (channel, levels->size()))
            if (const auto& level = levels->getReference (channel); level.isDouble() || level.isInt())
                return (double) level;

        return std::nullopt;
    }

    //==============================================================================
    // Phazite's engine, playing a song.
    juce::AudioBuffer<float> renderSong (Song& song, double seconds)
    {
        REQUIRE (renderMissingClipAudio (song)); // as an export does: a clip whose render isn't ready would play nothing
        return renderWithOwnEngine (song, (int) std::lround (seconds * renderSampleRate), renderSampleRate, renderBlockSize);
    }

    // The open project's.
    class OwnEngineRender final : public RenderEngine
    {
    public:
        juce::String getName() const override { return "reference"; }

        juce::AudioBuffer<float> render (ProjectContext& context, double seconds) override
        {
            auto* song = findSong (context);
            REQUIRE (song != nullptr);
            return renderSong (*song, seconds);
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
            auto audio = renderSong (*copy, seconds);
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
} // namespace

//==============================================================================
Fingerprint getFingerprint (const juce::AudioBuffer<float>& audio, int blockSamples)
{
    Fingerprint fingerprint;

    for (int start = 0; start + blockSamples <= audio.getNumSamples(); start += blockSamples)
    {
        BlockLevels levels;

        for (int channel = 0; channel < renderChannels; ++channel)
        {
            const auto source = std::min (channel, audio.getNumChannels() - 1);
            levels.rms[(size_t) channel] = juce::Decibels::gainToDecibels (audio.getRMSLevel (source, start, blockSamples), -100.0f);
            levels.peak[(size_t) channel] = juce::Decibels::gainToDecibels (audio.getMagnitude (source, start, blockSamples), -100.0f);
        }

        fingerprint.push_back (levels);
    }

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
         << "  \"blocks\": [\n";

    for (size_t i = 0; i < fingerprint.size(); ++i)
    {
        const auto start = (double) i * blockSamples / sampleRate;
        text << "    { \"start\": " << juce::String (start, 1) << ", \"rms\": " << formatLevels (fingerprint[i].rms)
             << ", \"peak\": " << formatLevels (fingerprint[i].peak) << " }" << (i + 1 < fingerprint.size() ? "," : "") << "\n";
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

    for (const auto* property : { "sampleRate", "blockSamples", "channels" })
        if (expected[property] != actual[property])
            differences.add (juce::String (property) + ": expected " + expected[property].toString() + ", got "
                             + actual[property].toString());

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
    const auto describe = [] (std::optional<double> level)
    { return level.has_value() ? formatLevel (*level) + " dBFS" : juce::String ("nothing"); };

    for (int block = 0; block < numExpected; ++block)
    {
        const auto& expectedBlock = expectedBlocks->getReference (block);
        const auto& actualBlock = actualBlocks->getReference (block);
        const auto when = juce::String (block * blockSeconds, 2) + "-" + juce::String ((block + 1) * blockSeconds, 2) + " s";

        for (const auto& [property, measure] : { std::pair { "rms", "RMS" }, std::pair { "peak", "peak" } })
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto expectedLevel = findLevel (expectedBlock, property, channel);
                const auto actualLevel = findLevel (actualBlock, property, channel);

                if (! expectedLevel.has_value() || ! actualLevel.has_value()
                    || std::abs (std::max (*actualLevel, (double) floorDb) - std::max (*expectedLevel, (double) floorDb)) > toleranceDb)
                    differences.add (when + ", " + getChannelName (channel) + " " + measure + ": expected " + describe (expectedLevel)
                                     + ", got " + describe (actualLevel));
            }
        }
    }

    limitDifferences (differences, maxListedDifferences);
    return differences;
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

void checkGoldenRender (ProjectContext& context, const GoldenRender& golden)
{
    const auto reference = makeReferenceEngine();
    const auto referenceAudio = reference->render (context, golden.seconds);
    const auto referenceText = checkRender (referenceAudio, renderSampleRate, golden, golden.name + "-" + reference->getName());
    compareWithGolden (referenceText, referenceAudio, renderSampleRate, golden, golden.name + "-" + reference->getName());

    const auto candidate = makeCandidateEngine();
    const auto candidateAudio = candidate->render (context, golden.seconds);
    const auto candidateText = checkRender (candidateAudio, renderSampleRate, golden, golden.name + "-" + candidate->getName());
    const auto differences = compareFingerprints (referenceText, candidateText);

    if (! differences.isEmpty())
        keepRender (candidateAudio, renderSampleRate, golden.name + "-" + candidate->getName());

    const auto apart = getDifference (referenceAudio, candidateAudio, renderSampleRate);
    INFO (("A copy of the song plays " + golden.name + " otherwise than the song itself (sample by sample, up to "
           + formatLevel (apart.peakDb) + " dBFS apart, at " + juce::String (apart.seconds, 4) + " s):\n  "
           + differences.joinIntoString ("\n  "))
              .toStdString());
    CHECK (differences.isEmpty());
}

void checkGoldenAudio (const juce::AudioBuffer<float>& audio, double sampleRate, const GoldenRender& golden)
{
    const auto text = checkRender (audio, sampleRate, golden, golden.name);
    compareWithGolden (text, audio, sampleRate, golden, golden.name);
}

void checkGoldenChange (ProjectContext& context, const std::function<void()>& change, GoldenRender golden, const ChangeCheck& check)
{
    settle();
    const auto before = renderFingerprint (context, golden.seconds);
    change();
    settle();
    golden.check = [&before, &check] (const Fingerprint& after, const juce::AudioBuffer<float>&) { return check (before, after); };
    checkGoldenRender (context, golden);
}

bool changesLevel (const Fingerprint& before, const Fingerprint& after, size_t block, float decibels, const juce::String& why)
{
    REQUIRE (block < before.size());
    const auto was = getPower (before[block]);
    return decibels >= 0.0f ? isBetween (after, block, was + decibels, 6.0f, why) : isBetween (after, block, -100.0f, was + decibels, why);
}
} // namespace phazite::test

// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden renders (tests/golden/renders): a short mix with a bus, a send to a reverb return, volume automation and pan is
// rendered offline, and the RMS and peak level of each 100 ms block of the result are compared with a committed
// fingerprint, so that a change to how Phazite plays a project fails a test. Levels may be toleranceDb apart, and a
// level below floorDb counts as floorDb: that absorbs the differences between compilers' maths libraries, and where a
// reverb's tail is cut off. A fingerprint doesn't notice a change of phase only, or of frequency at the same level.
// Golden renders never use 4OSC (its oscillators start at random phases) or anything else random. GoldenFiles.h says
// how to regenerate the golden files.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Tracks.h"
#include "model/mixer/Mixer.h"
#include "model/project/Project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace tracktion::literals;

namespace
{
constexpr float toleranceDb = 0.05f; // how far a level may be from the golden one
constexpr float floorDb = -70.0f;    // a lower level counts as this (Tracktion's reverb stops below about -78 dBFS)
constexpr int renderSampleRate = 44100;
constexpr int fingerprintBlockSamples = 4410; // 100 ms
constexpr int renderChannels = 2;
constexpr int fingerprintBlocks = 40;    // 4 seconds
constexpr int maxListedDifferences = 20; // the most lines a comparison lists

constexpr const char* aboutTheMix = "Golden render (tests/GoldenRenderTests.cpp): two tones through a bus, a send to a reverb return, "
                                    "volume automation and pan. Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";

te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

//==============================================================================
// The RMS and peak level of a block, in dBFS, on each side (left, right).
struct BlockLevels
{
    std::array<float, renderChannels> rms {}, peak {};
};

// A render's fingerprint: the levels of each block, in order.
using Fingerprint = std::vector<BlockLevels>;

Fingerprint getFingerprint (const juce::AudioBuffer<float>& audio)
{
    Fingerprint fingerprint;

    for (int start = 0; start + fingerprintBlockSamples <= audio.getNumSamples(); start += fingerprintBlockSamples)
    {
        BlockLevels levels;

        for (int channel = 0; channel < renderChannels; ++channel)
        {
            levels.rms[(size_t) channel] =
                juce::Decibels::gainToDecibels (audio.getRMSLevel (channel, start, fingerprintBlockSamples), -100.0f);
            levels.peak[(size_t) channel] =
                juce::Decibels::gainToDecibels (audio.getMagnitude (channel, start, fingerprintBlockSamples), -100.0f);
        }

        fingerprint.push_back (levels);
    }

    return fingerprint;
}

// A fingerprint as long as the render's, with the same levels in every block and on both sides.
Fingerprint makeFingerprint (float rms, float peak)
{
    BlockLevels levels;
    levels.rms.fill (rms);
    levels.peak.fill (peak);
    return Fingerprint ((size_t) fingerprintBlocks, levels);
}

// A level as golden files write it: in dBFS, with two decimals.
juce::String formatLevel (double level)
{
    return juce::String (level, 2);
}

juce::String formatLevels (const std::array<float, renderChannels>& levels)
{
    juce::StringArray texts;

    for (const auto level : levels)
        texts.add (formatLevel (level));

    return "[" + texts.joinIntoString (", ") + "]";
}

// A fingerprint as its golden file holds it: JSON, with one block on each line.
juce::String writeFingerprint (const Fingerprint& fingerprint)
{
    juce::String text;
    text << "{\n"
         << "  \"about\": \"" << juce::JSON::escapeString (aboutTheMix) << "\",\n"
         << "  \"sampleRate\": " << renderSampleRate << ",\n"
         << "  \"blockSamples\": " << fingerprintBlockSamples << ",\n"
         << "  \"channels\": " << renderChannels << ",\n"
         << "  \"blocks\": [\n";

    for (size_t i = 0; i < fingerprint.size(); ++i)
    {
        const auto start = (double) i * fingerprintBlockSamples / renderSampleRate;
        text << "    { \"start\": " << juce::String (start, 1) << ", \"rms\": " << formatLevels (fingerprint[i].rms)
             << ", \"peak\": " << formatLevels (fingerprint[i].peak) << " }" << (i + 1 < fingerprint.size() ? "," : "") << "\n";
    }

    text << "  ]\n"
         << "}\n";
    return text;
}

//==============================================================================
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

// The differences between the texts of two fingerprints, the golden one and Phazite's, at most maxListedDifferences lines:
//   sampleRate: expected 44100, got 48000
//   1.20-1.30 s, left RMS: expected -14.20 dBFS, got -13.10 dBFS
// Two levels are the same if they are at most toleranceDb apart, a level below floorDb counting as floorDb. Fingerprints
// that differ in their sample rate, block size, channels or number of blocks aren't compared block by block.
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

    phazite::test::limitDifferences (differences, maxListedDifferences);
    return differences;
}

//==============================================================================
// The mix, at 120 BPM (a beat is 0.5 s): Low, a 220 Hz tone from 0 to 2 s whose volume falls from 0 to -12 dB, and
// High, a quieter 660 Hz tone from 1 to 2.5 s, panned halfway left, with a send after its fader to a reverb return; both
// in a bus at -3 dB. (No digits in the files' names: Phazite would read a tempo in them and stretch the clips.)
void addMix (te::Edit& edit, const juce::File& scratch)
{
    edit.tempoSequence.getTempos()[0]->setBpm (120.0);

    auto* low = te::getAudioTracks (edit)[0];
    REQUIRE (low != nullptr);
    low->setName ("Low");
    auto* high = phazite::insertTrack (edit, low, nullptr);
    REQUIRE (high != nullptr);
    high->setName ("High");

    const auto lowTone = phazite::test::writeSound (scratch.getChildFile ("Low Tone.wav"), 2.0, 220.0, 0.5f);
    const auto highTone = phazite::test::writeSound (scratch.getChildFile ("High Tone.wav"), 1.5, 660.0, 0.25f);
    REQUIRE (phazite::importFile (*low, lowTone, beats (0.0)) != nullptr);
    REQUIRE (phazite::importFile (*high, highTone, beats (2.0)) != nullptr);

    auto* lowVolume = low->getVolumePlugin();
    REQUIRE (lowVolume != nullptr);
    auto& lowCurve = lowVolume->volParam->getCurve();
    lowCurve.addPoint (0_tp, te::decibelsToVolumeFaderPosition (0.0f), 0.0f, nullptr);
    lowCurve.addPoint (2_tp, te::decibelsToVolumeFaderPosition (-12.0f), 0.0f, nullptr);

    auto* highVolume = high->getVolumePlugin();
    REQUIRE (highVolume != nullptr);
    highVolume->setPan (-0.5f);

    auto* bus = phazite::groupIntoFolder ({ low, high }, nullptr);
    REQUIRE (bus != nullptr);
    bus->setName ("Bus");
    phazite::setMixedAsBus (*bus, true);
    auto* busVolume = dynamic_cast<te::VolumeAndPanPlugin*> (phazite::getVolumeDevice (*bus));
    REQUIRE (busVolume != nullptr);
    busVolume->setVolumeDb (-3.0f);

    REQUIRE (phazite::addReturnWithEffect (*high, phazite::test::createBuiltInDevice (edit, phazite::devices::Reverb::typeID)) != nullptr);
}

// Renders the first 4 seconds of an Edit to a WAV file, always in the same way: at 44.1 kHz, in blocks of 512 samples,
// as 32-bit floats without dither, in stereo, with the master's devices. Tracktion runs the render with the
// UIBehaviour's runTaskWithProgressBar(), which Phazite's runs to the end on this thread.
void renderMix (te::Edit& edit, const juce::File& file)
{
    te::Renderer::Parameters params (edit);
    params.destFile = file;
    params.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
    params.bitDepth = 32;
    params.sampleRateForAudio = renderSampleRate;
    params.blockSizeForAudio = 512;
    params.time = te::TimeRange (0_tp, 4_tp);
    params.tracksToDo = te::toBitSet (te::getAllTracks (edit));
    params.useMasterPlugins = true;
    params.canRenderInMono = false;
    params.ditheringEnabled = false;

    REQUIRE (te::Renderer::renderToFile ("Golden render", params) == params.destFile);
}

// The audio of a render, which must be 4 seconds of stereo.
juce::AudioBuffer<float> readRender (te::Engine& engine, const juce::File& file)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == (unsigned int) renderChannels);
    REQUIRE (reader->lengthInSamples == (juce::int64) fingerprintBlocks * fingerprintBlockSamples);

    juce::AudioBuffer<float> audio (renderChannels, fingerprintBlocks * fingerprintBlockSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    return audio;
}

juce::String describeLevels (const std::array<float, renderChannels>& levels)
{
    return "left " + formatLevel (levels[0]) + " dBFS, right " + formatLevel (levels[1]) + " dBFS";
}

// Checks that a render sounds like the mix before it is compared with the golden file, so that a broken render (silent,
// or with its pan, send or reverb not working) fails here and never becomes the golden file. Returns whether it does.
bool checkSoundsLikeTheMix (const Fingerprint& fingerprint)
{
    REQUIRE (fingerprint.size() == (size_t) fingerprintBlocks);

    // 0.5-0.6 s: Low alone, in the middle.
    const auto& lowAlone = fingerprint[5].rms;
    const auto lowInTheMiddle = lowAlone[0] > -30.0f && lowAlone[1] > -30.0f && std::abs (lowAlone[0] - lowAlone[1]) < 0.01f;
    CHECK_MESSAGE (lowInTheMiddle,
                   ("0.50-0.60 s: Low alone should be above -30 dBFS and as loud on each side; its RMS is " + describeLevels (lowAlone))
                       .toStdString());

    // 1.5-1.6 s: High too, panned left.
    const auto& withHigh = fingerprint[15].rms;
    const auto highOnTheLeft = withHigh[0] > withHigh[1] + 0.5f;
    CHECK_MESSAGE (
        highOnTheLeft,
        ("1.50-1.60 s: with High, panned left, the left side should be louder; the RMS is " + describeLevels (withHigh)).toStdString());

    // 2.6-2.7 s: both tones have ended, and the reverb's tail is left.
    const auto& tail = fingerprint[26].rms;
    const auto hasTail = std::max (tail[0], tail[1]) > floorDb + 10.0f;
    CHECK_MESSAGE (hasTail, ("2.60-2.70 s: the reverb's tail should be above " + formatLevel (floorDb + 10.0f) + " dBFS; its RMS is "
                             + describeLevels (tail))
                                .toStdString());

    // 3.9-4.0 s: the tail has faded, on each side.
    const auto& faded = fingerprint[39].rms;
    auto tailFades = true;

    for (size_t channel = 0; channel < faded.size(); ++channel)
        tailFades = tailFades && std::max (faded[channel], floorDb) < std::max (tail[channel], floorDb) - 6.0f;

    CHECK_MESSAGE (
        tailFades,
        ("3.90-4.00 s: the reverb's tail should have faded by 6 dB since 2.60-2.70 s; its RMS is " + describeLevels (faded)).toStdString());

    return lowInTheMiddle && highOnTheLeft && hasTail && tailFades;
}

// Keeps a render that doesn't sound as it should, or doesn't match its golden file, as
// <temp>/PhaziteGoldenActual/renders/mix.wav, to listen to.
void keepRender (const juce::File& file)
{
    const auto copy = phazite::test::getActualFolder().getChildFile ("renders").getChildFile ("mix.wav");

    if (copy.getParentDirectory().createDirectory().wasOk() && file.copyFileTo (copy))
        MESSAGE (("The render is in " + copy.getFullPathName()).toStdString());
    else
        MESSAGE (("The render couldn't be copied to " + copy.getFullPathName()).toStdString());
}
} // namespace

//==============================================================================
TEST_SUITE ("golden")
{
    TEST_CASE ("Golden files: render fingerprints match within a tolerance, above a floor")
    {
        const auto golden = makeFingerprint (-20.0f, -17.0f);
        const auto goldenText = writeFingerprint (golden);

        // One block on each line, with two decimals; a fingerprint matches itself.
        CHECK (goldenText.contains ("\n    { \"start\": 1.2, \"rms\": [-20.00, -20.00], \"peak\": [-17.00, -17.00] },\n"));
        CHECK (compareFingerprints (goldenText, goldenText).isEmpty());

        // Within the tolerance: a level 0.04 dB louder.
        auto louder = golden;
        louder[12].rms[0] = -19.96f;
        CHECK (compareFingerprints (goldenText, writeFingerprint (louder)).isEmpty());

        // Beyond it: 0.1 dB louder is a difference, listed with its block, side and levels.
        louder[12].rms[0] = -19.9f;
        const auto differences = compareFingerprints (goldenText, writeFingerprint (louder));
        REQUIRE (differences.size() == 1);
        CHECK (differences[0] == juce::String ("1.20-1.30 s, left RMS: expected -20.00 dBFS, got -19.90 dBFS"));

        // Below the floor, levels are all silence: -80 and -95 dBFS are the same.
        auto quiet = golden;
        auto quieter = golden;
        quiet[30].peak.fill (-80.0f);
        quieter[30].peak.fill (-95.0f);
        CHECK (compareFingerprints (writeFingerprint (quiet), writeFingerprint (quieter)).isEmpty());

        // A render of another length differs in its number of blocks, which aren't compared one by one.
        auto shorter = golden;
        shorter.pop_back();
        const auto lengthChange = compareFingerprints (goldenText, writeFingerprint (shorter));
        REQUIRE (lengthChange.size() == 1);
        CHECK (lengthChange[0] == juce::String ("blocks: expected 40, got 39"));

        // A render that differs everywhere lists only the first differences.
        CHECK (compareFingerprints (goldenText, writeFingerprint (makeFingerprint (-10.0f, -7.0f))).size() == maxListedDifferences);
    }

    TEST_CASE ("Golden render: two tones through a bus, a send to a reverb return, automation and pan")
    {
        phazite::test::TemporaryFolder temp;
        juce::String error;
        const auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Mix", error);
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());

        auto& edit = project->getEdit();
        addMix (edit, temp.folder.getChildFile ("Sounds"));
        phazite::test::settle();

        // In the temporary folder (never in the repository), and deleted at the end.
        const juce::TemporaryFile wav (".wav");
        renderMix (edit, wav.getFile());
        const auto fingerprint = getFingerprint (readRender (edit.engine, wav.getFile()));

        const auto soundsLikeTheMix = checkSoundsLikeTheMix (fingerprint);

        if (! soundsLikeTheMix)
            keepRender (wav.getFile());

        REQUIRE (soundsLikeTheMix); // a broken render never becomes the golden file

        auto differs = false;
        phazite::test::checkGoldenText ("renders/mix.json", writeFingerprint (fingerprint),
                                        [&differs] (const juce::String& expectedText, const juce::String& actualText)
                                        {
                                            const auto found = compareFingerprints (expectedText, actualText);
                                            differs = ! found.isEmpty();
                                            return found;
                                        });

        if (differs)
            keepRender (wav.getFile());
    }
}

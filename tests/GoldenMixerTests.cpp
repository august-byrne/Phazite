// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden renders of the mixer (tests/golden/renders, RenderHarness.h): a sidechain, a send before the fader, mute and
// solo, delay compensation across two paths, each of Phazite's effects, a frozen track and an export's stems, in songs
// made through the model (TestSong.h). Most are renders of a change, whose check compares the song before and after it.
#include "RenderHarness.h"
#include "TestSong.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "devices/vocoder/Vocoder.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/project/Export.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

using phazite::test::Fingerprint;
using phazite::test::TestSong;

namespace
{
using phazite::test::changesLevel;
using phazite::test::differSomewhere;
using phazite::test::floorDb;
using phazite::test::isBetween;
using phazite::test::isLouder;

juce::String aboutGolden (const juce::String& about)
{
    return "Golden render (tests/GoldenMixerTests.cpp): " + about + " Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";
}

void checkChange (TestSong& song, const std::function<void()>& change, const char* name, const juce::String& about,
                  const phazite::test::ChangeCheck& check)
{
    phazite::test::checkGoldenChange (song.context, change, { name, aboutGolden (about), 4.0, {} }, check);
}

juce::File writeTone (TestSong& song, const char* name, double frequency)
{
    return phazite::test::writeSound (song.getSoundsFolder().getChildFile (juce::String (name) + ".wav"), 4.0, frequency, 0.25f);
}

// Clicks every half second for 4 s, each a single sample at half of full scale, written as writeSound() writes tones. Its
// name has no tempo in it, so its clip plays it as it is.
juce::File writeTicks (const juce::File& file)
{
    constexpr double sampleRate = 44100.0;
    juce::AudioBuffer<float> buffer (1, (int) (4.0 * sampleRate));
    buffer.clear();

    for (int tick = 0; tick < 8; ++tick)
        buffer.setSample (0, (int) (tick * 0.5 * sampleRate) + 100, 0.5f);

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return file;
}

// The source that each effect's golden render plays through the effect, on the master: at 120 BPM, two bars of kick and
// snare on the drum sampler, and stabs of a chord on the synth, panned a little right, off the beat with silence between.
void addEffectSource (TestSong& song)
{
    song.setTempo (120.0);
    const auto drums = song.firstTrack;
    song.rename (drums, "Drums");
    song.setInstrument (drums, phazite::devices::DrumSampler::typeID);
    const auto beat = song.addMidiClip (drums, 0.0, 8.0);

    for (int beatNumber = 0; beatNumber < 8; ++beatNumber)
        song.addNote (beat, beatNumber % 2 == 0 ? 36 : 38, beatNumber, 0.25);

    const auto synth = song.addTrack ("Synth");
    song.setInstrument (synth, phazite::devices::Synth::typeID);
    song.setPan (synth, 0.3f);
    const auto stabs = song.addMidiClip (synth, 0.0, 8.0);

    for (int beatNumber = 0; beatNumber < 8; beatNumber += 2)
        song.addChord (stabs, { 60, 64, 67 }, beatNumber + 0.5, 0.5);
}

// One of Phazite's effects on the master, as it is made but for the settings given, which make it audible on the source.
void checkEffect (const char* typeID, const char* name, std::initializer_list<std::pair<const char*, float>> settings,
                  const juce::String& about)
{
    TestSong song;
    addEffectSource (song);
    checkChange (
        song,
        [&song, typeID, settings]
        {
            const auto effect = song.addEffect ({}, typeID);

            for (const auto& [parameter, value] : settings)
                song.setParameter (effect, parameter, value);
        },
        name, about,
        [] (const Fingerprint& before, const Fingerprint& after)
        {
            const auto changes = differSomewhere (before, after, 1.0f);
            CHECK_MESSAGE (changes, "the effect should change the sound somewhere by more than 1 dB");
            return isBetween (after, 1, -60.0f, 6.0f, "the song plays through it") && changes;
        });
}

// Reads the first seconds of a WAV file in stereo (a mono file on both sides).
juce::AudioBuffer<float> readStart (const juce::File& file, double seconds)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (file.createInputStream().release(), true));
    REQUIRE (reader != nullptr);
    const auto samples = (int) (seconds * reader->sampleRate);
    REQUIRE (reader->lengthInSamples >= samples);
    juce::AudioBuffer<float> audio (2, samples);
    REQUIRE (reader->read (&audio, 0, samples, 0, true, true));
    return audio;
}
} // namespace

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden render: a tone ducks under a kick it listens to through its compressor's sidechain")
    {
        TestSong song;
        song.setTempo (120.0);
        const auto bass = song.firstTrack;
        song.rename (bass, "Bass");
        song.addAudioClip (bass, writeTone (song, "Bass", 110.0), 0.0);

        const auto kick = song.addTrack ("Kick");
        song.setInstrument (kick, phazite::devices::DrumSampler::typeID);
        const auto clip = song.addMidiClip (kick, 0.0, 8.0);

        for (int beatNumber = 0; beatNumber < 8; ++beatNumber)
            song.addNote (clip, 36, beatNumber, 0.25);

        phazite::toggleMute (song.context, kick); // only the ducking is heard: a sidechain hears its source muted too
        checkChange (
            song, [&song, bass, kick] { REQUIRE (phazite::duckUnder (song.context, bass, kick).isValid()); }, "sidechain",
            "a 110 Hz tone ducking under a kick, muted, that its compressor's sidechain listens to.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                auto ok = changesLevel (before, after, 10, -3.0f, "the tone ducks as the kick hits");
                return isLouder (after, 8, 10, 3.0f, "the tone comes back up between the kicks") && ok;
            });
    }

    TEST_CASE ("Golden render: a send before the fader feeds its return with the fader down")
    {
        TestSong song;
        song.setTempo (120.0);
        const auto track = song.firstTrack;
        song.addAudioClip (track, writeTone (song, "Tone", 440.0), 0.0);
        song.addReturn (track, phazite::devices::Reverb::typeID, 0.0f);
        song.setVolume (track, -100.0f);
        const auto sends = phazite::getSends (song.context, track);
        REQUIRE (sends.size() == 1);

        checkChange (
            song, [&song, send = sends.front().id] { phazite::setPreFader (song.context, send, true); }, "send-pre-fader",
            "a tone with its fader down, its send to a reverb return switched to before the fader.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                auto ok = isBetween (before, 20, -100.0f, floorDb, "after the fader, the send sends nothing");
                return isBetween (after, 20, -40.0f, 6.0f, "before the fader, it sends the tone to the reverb") && ok;
            });
    }

    TEST_CASE ("Golden render: a soloed track plays alone, and a muted one never")
    {
        TestSong song;
        song.setTempo (120.0);
        const auto left = song.firstTrack;
        song.rename (left, "Left");
        song.addAudioClip (left, writeTone (song, "Low", 220.0), 0.0);
        song.setPan (left, -1.0f);
        const auto right = song.addTrack ("Right");
        song.addAudioClip (right, writeTone (song, "High", 440.0), 0.0);
        song.setPan (right, 1.0f);
        const auto middle = song.addTrack ("Middle");
        song.addAudioClip (middle, writeTone (song, "Higher", 660.0), 0.0);

        checkChange (
            song,
            [&song, right, middle]
            {
                phazite::toggleSolo (song.context, right);
                phazite::toggleMute (song.context, middle);
            },
            "mute-solo", "three tones, left, right and in the middle: the right one soloed, the middle one muted.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                const auto leftBefore = before[20].rms[0] > -40.0f;
                const auto leftSilent = after[20].rms[0] < floorDb;
                const auto rightPlays = after[20].rms[1] > -40.0f;
                CHECK_MESSAGE (leftBefore, "before, the left side plays");
                CHECK_MESSAGE (leftSilent, "with the right track soloed and the middle one muted, the left side is silent");
                CHECK_MESSAGE (rightPlays, "the soloed track plays");
                return leftBefore && leftSilent && rightPlays;
            });
    }

    TEST_CASE ("Golden render: delay compensation lines up a path through a device that looks ahead with one without")
    {
        TestSong song;
        song.setTempo (120.0);
        const auto ticks = writeTicks (song.getSoundsFolder().getChildFile ("Ticks.wav"));
        const auto through = song.firstTrack;
        song.rename (through, "Through");
        song.addAudioClip (through, ticks, 0.0);
        const auto straight = song.addTrack ("Straight");
        song.addAudioClip (straight, ticks, 0.0);

        checkChange (
            song, [&song, through] { song.addEffect (through, phazite::devices::Limiter::typeID); }, "latency",
            "clicks on two tracks, one through Phazite's limiter, which looks ahead, lined up by delay compensation.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                const auto lined = ! differSomewhere (before, after, 0.2f);
                CHECK_MESSAGE (lined, "the two paths' clicks should add up as they did without the limiter");
                return lined;
            });

        // Both clicks come out together, and on time: all of the song waits for the limiter, and a render plays ahead by as
        // much (Engine::render()).
        CHECK (phazite::getDelayCompensation (song.context).seconds > 0.001);
        const auto audio = phazite::test::makeReferenceEngine()->render (song.context, 4.0);
        auto first = 0;

        while (first < audio.getNumSamples() && std::abs (audio.getSample (0, first)) < 1.0e-3f)
            ++first;

        CHECK (first == 100);
    }

    TEST_CASE ("Golden render: Phazite's delay on a beat and stabs")
    {
        checkEffect (phazite::devices::Delay::typeID, "effect-delay", {}, "Phazite's delay as it is made, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's compressor on a beat and stabs")
    {
        // Its curve turning down from -36 dB, its corner there.
        const auto corner = phazite::devices::curves::axisForDecibels (-36.0f);
        checkEffect (phazite::devices::Compressor::typeID, "effect-compressor", { { "point2X", corner }, { "point2Y", corner } },
                     "Phazite's compressor turning down from -36 dB, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's limiter on a beat and stabs")
    {
        checkEffect (phazite::devices::Limiter::typeID, "effect-limiter", { { "gain", 18.0f } },
                     "Phazite's limiter, 18 dB of gain into it, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's multiband on a beat and stabs")
    {
        checkEffect (phazite::devices::Multiband::typeID, "effect-multiband", { { "masterPre", 12.0f } },
                     "Phazite's multiband, 12 dB into its master band's limiter at 0 dB, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's EQ on a beat and stabs")
    {
        checkEffect (phazite::devices::Equalizer::typeID, "effect-eq", { { "band2On", 1.0f }, { "band2Gain", 12.0f } },
                     "Phazite's EQ, its second band raised 12 dB, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's filter on a beat and stabs")
    {
        checkEffect (phazite::devices::FilterEffect::typeID, "effect-filter", { { "cutoff", 500.0f } },
                     "Phazite's filter at 500 Hz, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's flanger on a beat and stabs")
    {
        checkEffect (phazite::devices::Flanger::typeID, "effect-flanger", {},
                     "Phazite's flanger as it is made, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's chorus on a beat and stabs")
    {
        checkEffect (phazite::devices::Chorus::typeID, "effect-chorus", { { "mode", 1.0f } },
                     "Phazite's chorus in Ensemble, its three copies a side, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's phaser on a beat and stabs")
    {
        checkEffect (phazite::devices::Phaser::typeID, "effect-phaser", {}, "Phazite's phaser as it is made, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's reverb on a beat and stabs")
    {
        checkEffect (phazite::devices::Reverb::typeID, "effect-reverb", {}, "Phazite's reverb as it is made, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's saturator on a beat and stabs")
    {
        checkEffect (phazite::devices::Saturator::typeID, "effect-saturator", { { "drive", 24.0f } },
                     "Phazite's saturator driven 24 dB, on a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: Phazite's stereo expander on a beat and stabs")
    {
        checkEffect (phazite::devices::StereoExpander::typeID, "effect-stereo-expander", { { "width", 2.0f } },
                     "Phazite's stereo expander at twice the width, on a beat and chord stabs panned a little right.");
    }

    TEST_CASE ("Golden render: Phazite's vocoder on a beat and stabs")
    {
        checkEffect (phazite::devices::Vocoder::typeID, "effect-vocoder", {},
                     "Phazite's vocoder as it is made, its buzz speaking with a beat and chord stabs.");
    }

    TEST_CASE ("Golden render: a frozen track plays its file as it played live")
    {
        TestSong song;
        song.setTempo (120.0);
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        song.addEffect (track, phazite::devices::Reverb::typeID);
        const auto clip = song.addMidiClip (track, 0.0, 4.0);
        song.addChord (clip, { 60, 64, 67 }, 0.0, 2.0);
        song.addChord (clip, { 62, 65, 69 }, 2.0, 2.0);

        checkChange (
            song,
            [&song, track]
            {
                song.context.getSongSelection().selectOnly (track);
                song.run ("track.freeze");
                REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }, 60000));
                CHECK (song.renders.finished[0].endsWith (": ok"));
                REQUIRE (phazite::isFrozen (song.context, track));
            },
            "frozen", "two chords on Phazite's synth through its reverb, frozen: the track plays the file of its sound.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                // Phazite's engine renders the frozen file as it plays the track, its tail cut below -70 dB: as loud, a tenth
                // of a second at a time, to within 0.5 dB, where it is above -60 dB.
                auto same = before.size() == after.size();

                for (size_t i = 0; i < std::min (before.size(), after.size()); ++i)
                    for (size_t channel = 0; channel < 2; ++channel)
                        if (before[i].rms[channel] > -60.0f)
                            same = same && std::abs (before[i].rms[channel] - after[i].rms[channel]) <= 0.5f;

                CHECK_MESSAGE (same, "the frozen track should sound as it did live");
                return isBetween (after, 5, -40.0f, 6.0f, "the frozen track plays") && same;
            });
    }

    TEST_CASE ("Golden render: an export's stems, each its channel's sound, adding up to the song")
    {
        TestSong song;
        addEffectSource (song);
        const auto synth = phazite::getAllPlaylistTracks (song.context).back();
        song.addReturn (synth, phazite::devices::Reverb::typeID, -6.0f);
        phazite::test::settle();

        // The song exported, and then its stems (both Phazite's engine's renders).
        const auto exportSong = [&song] (bool stems, const char* name)
        {
            phazite::ExportSettings settings;
            settings.stems = stems;
            settings.bitDepth = 32;
            settings.sampleRate = phazite::test::renderSampleRate;
            std::optional<phazite::ExportReport> report;
            auto* exported = phazite::findSong (song.context);
            REQUIRE (exported != nullptr);
            auto running = phazite::startExport (*exported, song.getSoundsFolder().getChildFile (name), settings,
                                                 [&report] (const phazite::ExportReport& finished) { report = finished; });
            REQUIRE (running != nullptr);
            REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 60000));
            REQUIRE_MESSAGE (report->result.wasOk(), report->result.getErrorMessage().toStdString());
            return *report;
        };

        exportSong (false, "Mix.wav");
        const auto mix = readStart (song.getSoundsFolder().getChildFile ("Mix.wav"), 4.0);
        const auto report = exportSong (true, "Song.wav");
        REQUIRE (report.stems.size() == 3);

        const char* names[] = { "stems-drums", "stems-synth", "stems-reverb" };
        const char* abouts[] = { "the stem of a beat on Phazite's drum sampler.", "the stem of chord stabs on Phazite's synth.",
                                 "the stem of a reverb return the synth's chords are sent to." };
        juce::AudioBuffer<float> sum (2, mix.getNumSamples());
        sum.clear();

        for (int i = 0; i < 3; ++i)
        {
            const auto stem = readStart (report.stems[i], 4.0);

            for (int channel = 0; channel < 2; ++channel)
                sum.addFrom (channel, 0, stem, channel, 0, stem.getNumSamples());

            phazite::test::checkGoldenAudio (stem, phazite::test::renderSampleRate,
                                             { names[i], aboutGolden (abouts[i]), 4.0,
                                               [] (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
                                               { return isBetween (fingerprint, 15, -60.0f, 6.0f, "the stem has its channel's sound"); } });
        }

        // Stems leave out the master's devices, its fader too (at -3 dB in a new song): at its level, they add up to the song.
        const auto master = phazite::getMasterVolume (song.context);
        sum.applyGain (juce::Decibels::decibelsToGain (phazite::faderPositionToDecibels (master.getValue())));
        const auto differences =
            phazite::test::compareFingerprints (phazite::test::writeFingerprint (phazite::test::getFingerprint (mix), "the song"),
                                                phazite::test::writeFingerprint (phazite::test::getFingerprint (sum), "its stems"));
        INFO (("The stems, at the master's fader's level, should add up to the song:\n  " + differences.joinIntoString ("\n  "))
                  .toStdString());
        CHECK (differences.isEmpty());
    }
}

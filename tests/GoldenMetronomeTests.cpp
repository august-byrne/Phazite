// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden renders of the metronome and the count-in (tests/golden/renders, RenderHarness.h; docs/tracktion-migration.md,
// T0), which sound only as the song plays live, what played kept from its first click on, through Phazite's own engine,
// driven a block at a time (EnginePlayback.h): the metronome (T2 step 1), and the count-in that recording plays (T2 step
// 9). Both start between beats.
#include "EnginePlayback.h"
#include "RenderHarness.h"
#include "TestSong.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/TimelineModel.h"
#include "model/engine/internal/LiveInput.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Metronome.h"

#include <cmath>

using phazite::test::EnginePlayback;
using phazite::test::Fingerprint;
using phazite::test::TestSong;

namespace
{
using phazite::test::isBetween;
using phazite::test::isLouder;

constexpr double clickSeconds = 2.5; // how long a golden render keeps after the first click

juce::String aboutGolden (const char* about)
{
    return juce::String ("Golden render (tests/GoldenMetronomeTests.cpp), played from its first click: ") + about
           + " Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";
}

// What played, from its first click on, for clickSeconds; fails if nothing clicked, or too late.
juce::AudioBuffer<float> fromFirstClick (const juce::AudioBuffer<float>& played)
{
    auto first = -1;

    for (int i = 0; i < played.getNumSamples() && first < 0; ++i)
        if (std::abs (played.getSample (0, i)) > 1.0e-4f || std::abs (played.getSample (1, i)) > 1.0e-4f)
            first = i;

    REQUIRE_MESSAGE (first >= 0, "nothing clicked");
    const auto length = (int) (clickSeconds * EnginePlayback::sampleRate);
    REQUIRE (first + length <= played.getNumSamples());

    juce::AudioBuffer<float> clicks (2, length);

    for (int channel = 0; channel < 2; ++channel)
        clicks.copyFrom (channel, 0, played, channel, first, length);

    return clicks;
}
} // namespace

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden render: the metronome clicks every beat at 90 BPM in 3/4, louder on each bar's first")
    {
        TestSong song;
        song.setTempo (90.0);
        song.setTimeSignature (3, 4);
        song.setSwitch ("transport.metronome", true);
        song.setSwitch ("transport.countIn", false);
        song.addMidiClip (song.firstTrack, 0.0, 12.0); // a silent song of four bars: playback stops at the song's end
        phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (2.5)); // the clicks: beat 3 (a bar's first), 4, 5, 6 (one)

        // Phazite's engine plays the metronome (docs/tracktion-migration.md, T2 step 1), with a click of its own.
        juce::AudioBuffer<float> played;
        {
            EnginePlayback playback;
            song.run ("transport.playStop");
            played = playback.play (3.5);
            song.run ("transport.playStop");
        }

        // A beat is 0.667 s: the clicks at 0 (a bar's first), 0.667, 1.333 and 2.0 s (a bar's first again).
        phazite::test::checkGoldenAudio (fromFirstClick (played), EnginePlayback::sampleRate,
                                         { "metronome", aboutGolden ("the metronome on every beat at 90 BPM in 3/4."), clickSeconds,
                                           [] (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
                                           {
                                               auto ok = isLouder (fingerprint, 6, 3, 20.0f, "a click on each beat");
                                               ok = isLouder (fingerprint, 13, 10, 20.0f, "a click on each beat") && ok;
                                               ok = isLouder (fingerprint, 20, 6, 1.0f, "a bar's first click is louder") && ok;
                                               return isLouder (fingerprint, 0, 13, 1.0f, "a bar's first click is louder") && ok;
                                           } });
    }

    TEST_CASE ("Golden render: the count-in clicks a bar before recording starts, the metronome off")
    {
        auto& engine = phazite::test::getEngine();

        if (phazite::getAllMidiInputs (engine) == nullptr)
        {
            engine.getDeviceManager().rescanMidiDeviceList();
            phazite::test::runTimersUntil ([&engine] { return phazite::getAllMidiInputs (engine) != nullptr; });
        }

        REQUIRE (phazite::getAllMidiInputs (engine) != nullptr);

        TestSong song;
        song.setTempo (120.0);
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        phazite::routeLiveMidiInput (*phazite::getEdit (song.context)); // live MIDI plays the track with an instrument
        song.setSwitch ("transport.metronome", false);
        song.setSwitch ("transport.countIn", true);
        const juce::ScopeGuard countInOff {
            [&song] { phazite::setCountInOn (*phazite::getEdit (song.context), false); }
        }; // the app's setting, for every project
        phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (0.5));

        // Phazite's engine plays the count-in (T2 step 9): on the song's beats in the bar before where it starts.
        juce::AudioBuffer<float> played;
        {
            EnginePlayback playback;
            song.run ("transport.record");
            played = playback.play (3.5);
            song.run ("transport.record");
        }

        // A beat is half a second: four clicks, a bar, then silence while recording, the metronome off.
        phazite::test::checkGoldenAudio (
            fromFirstClick (played), EnginePlayback::sampleRate,
            { "count-in", aboutGolden ("a bar of count-in at 120 BPM in 4/4 before recording, the metronome off."), clickSeconds,
              [] (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
              {
                  auto ok = true;

                  for (size_t click = 1; click < 4; ++click)
                      ok = isLouder (fingerprint, click * 5, click * 5 - 2, 20.0f, "a click on each beat") && ok;

                  return isBetween (fingerprint, 22, -100.0f, phazite::test::floorDb, "silence once recording starts") && ok;
              } });
    }
}

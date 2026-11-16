// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden renders of what rings on in silence (tests/golden/renders, RenderHarness.h): a hit through Phazite's delay, whose
// echoes come further apart than a device stays awake in silence. Rendered, and played live as every golden render is
// (checkGoldenRender()), where the delay must stay awake for its echoes; and what only live playback plays of the same
// song, through Phazite's engine driven a block at a time (EnginePlayback.h), each with a golden file of its own: the song
// looped, and stopped and started again.
#include "EnginePlayback.h"
#include "RenderHarness.h"
#include "TestSong.h"
#include "devices/delay/Delay.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"

using phazite::test::EnginePlayback;
using phazite::test::Fingerprint;
using phazite::test::TestSong;

namespace
{
using phazite::test::floorDb;
using phazite::test::getPower;
using phazite::test::isBetween;
using phazite::test::isLouder;

juce::String aboutGolden (const char* about)
{
    return juce::String ("Golden render (tests/GoldenLiveTests.cpp): ") + about + " Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";
}

// At 120 BPM, a hit of 50 ms at the song's start (and another at 2.5 s, if asked) through Phazite's delay on its track: an
// echo every dotted quarter note, 0.75 s, each 4.4 dB quieter than the one before, as loud as the hit itself at first. A
// song of one bar, 2 s, or of two with the second hit.
void addEchoSong (TestSong& song, bool secondHit)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.rename (track, "Hit");
    const auto hit = phazite::test::writeHit (song.getSoundsFolder().getChildFile ("Hit.wav"));
    song.addAudioClip (track, hit, 0.0);

    if (secondHit)
        song.addAudioClip (track, hit, 5.0);

    const auto delay = song.addEffect (track, phazite::devices::Delay::typeID);
    song.setParameter (delay, "time", 9.0f); // 1/4D
    song.setParameter (delay, "feedback", 0.6f);
    song.setParameter (delay, "mix", 0.5f);
    phazite::test::dispatchPendingMessages();
}

// A hit in a block, nothing in the six after it, 0.6 s, and its first echo in the seventh.
bool hasFirstEcho (const Fingerprint& fingerprint, size_t hitBlock)
{
    auto ok = isBetween (fingerprint, hitBlock, -40.0f, 0.0f, "the hit plays");

    for (size_t block = hitBlock + 1; block < hitBlock + 7; ++block)
        ok = isBetween (fingerprint, block, -100.0f, floorDb,
                        "nothing sounds between the hit and its first echo, for longer than a device stays awake in silence")
             && ok;

    return isBetween (fingerprint, hitBlock + 7, -50.0f, 0.0f, "the hit's first echo, 0.75 s after it") && ok;
}

// The song's start: the hit, its first echo at 0.75 s, and its second at 1.5 s, quieter.
bool startsWithEchoes (const Fingerprint& fingerprint)
{
    const auto ok = hasFirstEcho (fingerprint, 0);
    return isLouder (fingerprint, 7, 15, 3.0f, "the second echo, at 1.5 s, is quieter than the first") && ok;
}

// A block as loud as another was, within half a decibel.
bool isAsLoudAs (const Fingerprint& fingerprint, size_t block, size_t other, const juce::String& why)
{
    const auto level = getPower (fingerprint[other]);
    return isBetween (fingerprint, block, level - 0.5f, level + 0.5f, why);
}

// The app's engine on a device at the golden renders' rate, in their blocks, playing the song's project; with its
// session, whose transport a test drives as the app's commands do.
struct LivePlayback
{
    explicit LivePlayback (TestSong& song)
        : session (*phazite::getSession (song.context))
    {
        REQUIRE (session.isPlayingProject());
        EnginePlayback::waitForFiles();
    }

    EnginePlayback playback { EnginePlayback::Input::silence, EnginePlayback::Timing::realTime, phazite::test::renderSampleRate,
                              phazite::test::renderBlockSize };
    phazite::PlaybackSession& session;
};

// What was played, one stretch after another.
juce::AudioBuffer<float> join (std::initializer_list<const juce::AudioBuffer<float>*> stretches)
{
    auto length = 0;

    for (const auto* stretch : stretches)
        length += stretch->getNumSamples();

    juce::AudioBuffer<float> joined (2, length);
    auto done = 0;

    for (const auto* stretch : stretches)
    {
        for (int channel = 0; channel < 2; ++channel)
            joined.copyFrom (channel, done, *stretch, channel, 0, stretch->getNumSamples());

        done += stretch->getNumSamples();
    }

    return joined;
}
} // namespace

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden render: a delay's echoes come through the silence after a hit, played live as rendered")
    {
        // The second hit comes at 2.5 s, between the first's echoes at 2.25 and 3 s, with an echo of its own at 3.25 s.
        TestSong song;
        addEchoSong (song, true);
        phazite::test::checkGoldenRender (
            song.context,
            { "delay-echoes",
              aboutGolden ("two hits through Phazite's delay on their track, their echoes 0.75 s apart with silence between."), 4.0,
              [] (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
              {
                  auto ok = startsWithEchoes (fingerprint);
                  ok = isBetween (fingerprint, 20, -100.0f, floorDb, "nothing sounds between the second echo and the third") && ok;
                  ok = isLouder (fingerprint, 15, 22, 3.0f, "the third echo, at 2.25 s, is quieter than the second") && ok;
                  ok = isBetween (fingerprint, 25, -40.0f, 0.0f, "the second hit plays, at 2.5 s") && ok;
                  return isBetween (fingerprint, 32, -50.0f, 0.0f, "the second hit's first echo, at 3.25 s") && ok;
              } });
    }

    TEST_CASE (
        "Golden render: a looped song's echoes ring on over the loop's end as it plays live, and its hit plays again each time round")
    {
        // The song of one bar, looped: twice round and a half, 5 s, in one go.
        TestSong song;
        addEchoSong (song, false);
        song.setSwitch ("transport.loop", true);
        phazite::test::dispatchPendingMessages();

        juce::AudioBuffer<float> played;
        {
            LivePlayback live (song);
            live.session.locate (phazite::BeatPosition());
            live.session.play();
            played = live.playback.play (5.0);
            live.session.stop();
        }

        phazite::test::checkGoldenAudio (
            played, phazite::test::renderSampleRate,
            { "live-loop",
              aboutGolden ("a hit through Phazite's delay in a song of one bar, looped and played live twice round and a half."), 5.0,
              [] (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
              {
                  auto ok = startsWithEchoes (fingerprint);
                  ok = isLouder (fingerprint, 20, 19, 20.0f, "the hit plays again as the loop starts again, at 2 s") && ok;
                  ok = isLouder (fingerprint, 22, 2, 20.0f,
                                 "the first round's echoes ring on over the loop's end: its third, at 2.25 s, where the first round "
                                 "had silence")
                       && ok;
                  ok = isAsLoudAs (fingerprint, 27, 7, "the second round's hit has its first echo as the first round's had") && ok;
                  return isLouder (fingerprint, 40, 39, 20.0f, "the hit plays a third time, at 4 s") && ok;
              } });
    }

    TEST_CASE ("Golden render: an echo rings on after a stop as the song plays live, and not into what plays next")
    {
        // The song of one bar played for a second, stopped for a second, and played from its start again for a second.
        TestSong song;
        addEchoSong (song, false);

        juce::AudioBuffer<float> played;
        {
            LivePlayback live (song);
            live.session.locate (phazite::BeatPosition());
            live.session.play();
            const auto first = live.playback.play (1.0);
            live.session.stop();
            const auto stopped = live.playback.play (1.0);
            live.session.locate (phazite::BeatPosition());
            live.session.play();
            const auto second = live.playback.play (1.0);
            live.session.stop();
            played = join ({ &first, &stopped, &second });
        }

        phazite::test::checkGoldenAudio (
            played, phazite::test::renderSampleRate,
            { "live-stop-start",
              aboutGolden ("a hit through Phazite's delay played live for a second, stopped for a second, and played again from its "
                           "start."),
              3.0, [] (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
              {
                  auto ok = startsWithEchoes (fingerprint);
                  ok = isBetween (fingerprint, 15, -60.0f, 0.0f, "the second echo rings on while the song is stopped, at 1.5 s") && ok;
                  ok = hasFirstEcho (fingerprint, 20) && ok;
                  ok = isBetween (fingerprint, 22, -100.0f, floorDb,
                                  "what rang on from before doesn't sound in what plays next: no third echo at 2.25 s")
                       && ok;
                  return isAsLoudAs (fingerprint, 27, 7, "the hit played again has its first echo as it had the first time") && ok;
              } });
    }
}

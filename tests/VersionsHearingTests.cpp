// SPDX-License-Identifier: AGPL-3.0-or-later
// A version's bounce, and hearing it in place of the song (docs/versions.md: Bounces and hearing them; step V3): a bounce
// made as Export Song renders the song, deleted with the room it took, and heard through Phazite's engine as the app plays
// the song, a block at a time: at once, on the same beat, and as loud as the song.
#include "EnginePlayback.h"
#include "TestSong.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/commands/CommandRegistry.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
using namespace phazite::test;

void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isBusy();
        }));
}

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    waitForVersions (context);
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }));
    REQUIRE (result->wasOk());
    waitForVersions (context);
    return phazite::getLastVersion (context)->id;
}

// Bounce, as the command runs it in its host, waited for: it must work.
void bounce (TestSong& s, const juce::String& version)
{
    phazite::bounceVersion (s.context, version, s.renders);
    REQUIRE (dispatchMessagesUntil ([&s] { return ! s.renders.finished.isEmpty(); }, 60000));
    INFO (s.renders.finished.joinIntoString ("\n").toStdString());
    REQUIRE (s.renders.finished[s.renders.finished.size() - 1].endsWith (": ok"));
    s.renders.finished.clear();
    waitForVersions (s.context);
}

// A hit at the start of each beat at 120 BPM, a burst of 64 samples, for so many seconds: a file at 44.1 kHz whose name
// doesn't follow the song's tempo.
juce::File writeHits (const juce::File& file, double seconds, float level)
{
    constexpr double rate = 44100.0;
    constexpr int beat = 22050;
    juce::AudioBuffer<float> buffer (1, (int) (seconds * rate));
    buffer.clear();

    for (int start = 0; start < buffer.getNumSamples(); start += beat)
        for (int i = start; i < std::min (start + 64, buffer.getNumSamples()); ++i)
            buffer.setSample (0, i, level);

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24)))
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());

    return file;
}

// Where each hit starts in what was played: the first sample over a level after a stretch of silence.
std::vector<int> findHits (const juce::AudioBuffer<float>& audio)
{
    std::vector<int> hits;
    auto quietFor = 1000;

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto loud = std::abs (audio.getSample (0, i)) > 0.01f;

        if (loud && quietFor >= 1000)
            hits.push_back (i);

        quietFor = loud ? 0 : quietFor + 1;
    }

    return hits;
}

juce::AudioBuffer<float> join (std::initializer_list<const juce::AudioBuffer<float>*> parts)
{
    auto length = 0;

    for (const auto* part : parts)
        length += part->getNumSamples();

    juce::AudioBuffer<float> all (2, length);
    auto at = 0;

    for (const auto* part : parts)
    {
        for (int channel = 0; channel < 2; ++channel)
            all.copyFrom (channel, at, *part, channel, 0, part->getNumSamples());

        at += part->getNumSamples();
    }

    return all;
}

// The integrated loudness of a stretch of what was played, in LUFS.
float measureLoudness (const juce::AudioBuffer<float>& audio, int start, int length)
{
    phazite::engine::LoudnessMeter meter;
    meter.prepare (EnginePlayback::sampleRate, 512);

    for (int done = 0; done < length; done += 512)
    {
        const auto count = std::min (512, length - done);
        const float* channels[] = { audio.getReadPointer (0, start + done), audio.getReadPointer (1, start + done) };
        meter.process (channels, 2, count);
    }

    return meter.getReadings().integrated.value_or (-100.0f);
}
} // namespace

TEST_CASE ("Bounce renders the version the song is now, once, as Export Song does, and Delete Bounce frees its room")
{
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Hits");
    s.addAudioClip (track, writeHits (sounds.folder.getChildFile ("Hits.wav"), 4.0, 0.5f), 0.0);
    const auto first = keepVersion (s.context, "First");
    CHECK (phazite::getVersionsRoom (s.context).bounces == 0);
    CHECK (phazite::getVersionsRoom (s.context).songs > 0);
    REQUIRE (phazite::canBounce (s.context, first));
    bounce (s, first);

    CHECK (phazite::getLastVersion (s.context)->hasBounce);
    CHECK_FALSE (phazite::canBounce (s.context, first)); // once
    const auto file = phazite::getVersionHistory (s.context)->getBounceFile (first);
    REQUIRE (file.existsAsFile());
    CHECK (phazite::getVersionsRoom (s.context).bounces == file.getSize());

    // FLAC of 16 bits, at the rate the song plays at, from the song's start to its end and its tails.
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        REQUIRE (reader != nullptr);
        CHECK (reader->getFormatName() == "FLAC file");
        CHECK (reader->bitsPerSample == 16);
        CHECK (juce::exactlyEqual (reader->sampleRate, phazite::getSession (s.context)->getEngine().getSampleRate()));
        CHECK (reader->lengthInSamples >= (juce::int64) std::lround (4.0 * reader->sampleRate));
    }

    // Only the version the song is now: not once the song changed, nor an older one.
    const auto second = keepVersion (s.context, "Second");
    CHECK (phazite::canBounce (s.context, second));
    s.setVolume (track, -12.0f);
    CHECK_FALSE (phazite::canBounce (s.context, second));
    CHECK_FALSE (phazite::canBounce (s.context, first));

    // Delete Bounce: its file, and the room it took, go; the version stays.
    REQUIRE (phazite::deleteBounce (s.context, first));
    CHECK (dispatchMessagesUntil ([&file] { return ! file.existsAsFile(); }));
    CHECK (phazite::getVersionsRoom (s.context).bounces == 0);
    REQUIRE (phazite::getVersions (s.context).size() == 2);
    CHECK_FALSE (phazite::getVersions (s.context).back().hasBounce);
}

TEST_CASE ("Save Version with a bounce bounces the version it saves, in the host, as the box says")
{
    TestSong s;
    TemporaryFolder sounds;
    s.addAudioClip (s.addTrack ("Hits"), writeHits (sounds.folder.getChildFile ("Hits.wav"), 2.0, 0.5f), 0.0);
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (
        commands, s.context, [&s] (std::function<void (bool)> then) { then (s.getProject().save().wasOk()); },
        [&s] (const juce::ValueTree& song) { s.openSong (song); }, s.renders);
    waitForVersions (s.context);

    phazite::setBounceWithVersion (s.context, true);
    phazite::setVersionDraft (s.context, "Take one", {});
    REQUIRE (commands.invoke ("version.save"));
    REQUIRE (dispatchMessagesUntil ([&s] { return ! s.renders.finished.isEmpty(); }, 60000));
    CHECK (s.renders.finished[0] == "Bouncing \"Take one\": ok");
    CHECK (phazite::getLastVersion (s.context)->hasBounce);

    // The commands on the version selected: Hear It, and Delete Bounce.
    phazite::selectVersion (s.context, phazite::getLastVersion (s.context)->id);
    CHECK (commands.find ("version.hear")->isEnabled());
    CHECK_FALSE (commands.find ("version.bounce")->isEnabled()); // it has one
    REQUIRE (commands.invoke ("version.deleteBounce"));
    CHECK_FALSE (phazite::getLastVersion (s.context)->hasBounce);
    CHECK_FALSE (commands.find ("version.hear")->isEnabled());
}

TEST_CASE ("Hearing a version switches at once while the song plays, and every hit stays on its beat")
{
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Hits");
    s.addAudioClip (track, writeHits (sounds.folder.getChildFile ("Hits.wav"), 8.0, 0.5f), 0.0);
    const auto loud = keepVersion (s.context, "Loud");
    bounce (s, loud);
    s.setVolume (track, -12.0f); // the song quieter than the version now

    // The version first, playing from where playback starts; then Now, the song itself; then the version again.
    EnginePlayback playback;
    REQUIRE (phazite::hearVersion (s.context, loud));
    CHECK (phazite::isSongPlaying (*phazite::findSong (s.context)));
    CHECK (phazite::getHeardVersion (s.context) == loud);
    CHECK (phazite::describeHearing (s.context) == "Hearing: Loud");
    EnginePlayback::waitForFiles();
    const auto first = playback.play (1.25);

    REQUIRE (phazite::hearVersion (s.context, {}));
    CHECK (phazite::getHeardVersion (s.context).isEmpty());
    CHECK (phazite::describeHearing (s.context).isEmpty());
    const auto second = playback.play (1.0);

    REQUIRE (phazite::hearVersion (s.context, loud));
    const auto third = playback.play (1.0);

    // A hit every half second, 24,000 samples apart at 48 kHz, through all three, the switches between beats.
    const auto hits = findHits (join ({ &first, &second, &third }));
    REQUIRE (hits.size() == 7);

    for (size_t i = 0; i < hits.size(); ++i)
    {
        INFO ("hit " << i << " at " << hits[i]);
        CHECK (std::abs (hits[i] - (int) i * 24000) <= 4);
    }

    // Playback from another beat: the version from the same beat of its own.
    phazite::stopAndReturn (*phazite::findSong (s.context));
    phazite::movePlayhead (*phazite::findSong (s.context), phazite::BeatPosition::fromBeats (3.0));
    phazite::playOrStop (*phazite::findSong (s.context));
    const auto later = findHits (playback.play (1.2));
    REQUIRE (later.size() >= 2);
    CHECK (later[0] <= 4);
    CHECK (std::abs (later[1] - 24000) <= 4);
}

TEST_CASE ("After a switch, the version and the song are heard within half a LU of each other")
{
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Tone");
    s.addAudioClip (track, writeSound (sounds.folder.getChildFile ("Tone.wav"), 10.0, 220.0, 0.5f), 0.0);
    const auto loud = keepVersion (s.context, "Loud");
    bounce (s, loud);
    s.setVolume (track, -9.0f);

    EnginePlayback playback;
    REQUIRE (phazite::hearVersion (s.context, loud));
    EnginePlayback::waitForFiles();
    const auto version = playback.play (3.0);
    REQUIRE (phazite::hearVersion (s.context, {}));
    const auto song = playback.play (3.0);

    // Measured as they play, before their gains, they are 9 LU apart; heard, over the last two seconds of each, as loud.
    const auto* comparison = phazite::getSession (s.context)->getPlayer().getComparisonState();
    REQUIRE (comparison != nullptr);
    REQUIRE (comparison->getSongLoudness().integrated.has_value());
    REQUIRE (comparison->getVersionLoudness().integrated.has_value());
    CHECK (*comparison->getVersionLoudness().integrated - *comparison->getSongLoudness().integrated
           == doctest::Approx (9.0f).epsilon (0.05));

    const auto heardVersion = measureLoudness (version, 48000, 96000), heardSong = measureLoudness (song, 48000, 96000);
    INFO ("version " << heardVersion << " LUFS, song " << heardSong << " LUFS");
    CHECK (std::abs (heardVersion - heardSong) < 0.5f);
}

TEST_CASE ("An edit to the song switches back to it, so that what is heard is what is shown; moving the playhead doesn't")
{
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Hits");
    s.addAudioClip (track, writeHits (sounds.folder.getChildFile ("Hits.wav"), 4.0, 0.5f), 0.0);
    const auto first = keepVersion (s.context, "First");
    bounce (s, first);

    REQUIRE (phazite::hearVersion (s.context, first));
    phazite::movePlayhead (*phazite::findSong (s.context), phazite::BeatPosition::fromBeats (2.0));
    CHECK (phazite::getHeardVersion (s.context) == first);

    s.setVolume (track, -3.0f);
    CHECK (phazite::getHeardVersion (s.context).isEmpty());
    CHECK (phazite::describeHearing (s.context).isEmpty());
    CHECK_FALSE (phazite::getSession (s.context)->getPlayer().getComparisonState()->isHearingVersion());

    // A version without a bounce isn't heard.
    const auto second = keepVersion (s.context, "Second");
    CHECK_FALSE (phazite::hearVersion (s.context, second));
    phazite::stopAndReturn (*phazite::findSong (s.context));
}

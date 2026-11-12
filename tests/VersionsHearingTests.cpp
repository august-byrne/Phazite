// SPDX-License-Identifier: AGPL-3.0-or-later
// A version's bounce, and hearing it in place of the song (docs/versions.md: Bounces and hearing them; step V3): a bounce
// made as Export Song renders the song, deleted with the room it took, and heard through Phazite's engine as the app plays
// the song, a block at a time: at once, on the same beat, and as loud as the song.
#include "EnginePlayback.h"
#include "RealTimePlayback.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/commands/CommandRegistry.h"
#include "model/mixer/MixerModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/Export.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
using namespace phazite::test;

// Whether a commit, by its id, has its bounce.
bool hasBounceOf (const phazite::ProjectContext& context, const juce::String& id)
{
    for (const auto& version : phazite::getVersions (context))
        if (version.id == id)
            return version.hasBounce;

    return false;
}

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
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
    REQUIRE (waitForVersionHistory (s.context));
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

// A file's audio, whole, on two channels.
juce::AudioBuffer<float> readAudio (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio (2, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio;
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

    // Once the song changed, a version is bounced from its own song (an old version's bounce, below); one with a bounce, not
    // again.
    const auto second = keepVersion (s.context, "Second");
    CHECK (phazite::canBounce (s.context, second));
    s.setVolume (track, -12.0f);
    CHECK (phazite::canBounce (s.context, second));
    CHECK_FALSE (phazite::canBounce (s.context, first));

    // Delete Bounce: its file, and the room it took, go; the version stays.
    REQUIRE (phazite::deleteBounce (s.context, first));
    CHECK (dispatchMessagesUntil ([&file] { return ! file.existsAsFile(); }));
    CHECK (phazite::getVersionsRoom (s.context).bounces == 0);
    REQUIRE (phazite::getVersions (s.context).size() == 3); // and the initial commit
    CHECK_FALSE (hasBounceOf (s.context, first));
}

TEST_CASE ("An old version's bounce is its song alone, rendered in the background with plug-ins of its own, as an export of it")
{
    TestPlugin plugin; // before the songs that hold it
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Hits");
    s.addAudioClip (track, writeHits (sounds.folder.getChildFile ("Hits.wav"), 4.0, 0.5f), 0.0);
    const auto effect = TestPlugin::add (*phazite::findSong (s.context), track);
    s.setParameter (effect, TestPlugin::gainID, 0.25f);
    const auto quiet = keepVersion (s.context, "Quiet");
    phazite::removeDevice (s.context, effect);
    keepVersion (s.context, "Loud");

    // While the song plays on, the session keeping its plug-ins: the old version's own, in their helper.
    phazite::playOrStop (*phazite::findSong (s.context));
    REQUIRE (phazite::canBounce (s.context, quiet));
    REQUIRE (phazite::startBackgroundBounce (s.context, quiet));
    CHECK (phazite::getBounceProgress (s.context, quiet).has_value());
    CHECK (phazite::isSongPlaying (*phazite::findSong (s.context)));
    CHECK_FALSE (phazite::getSession (s.context)->isLentToRender());
    CHECK_FALSE (phazite::canBounce (s.context, phazite::getLastVersion (s.context)->id)); // one at a time

    REQUIRE (dispatchMessagesUntil ([&s, &quiet] { return hasBounceOf (s.context, quiet); }, 60000));
    CHECK (phazite::getBounceProblem (s.context).isEmpty());
    CHECK_FALSE (phazite::getBounceProgress (s.context, quiet).has_value());
    CHECK (phazite::isSongPlaying (*phazite::findSong (s.context)));
    phazite::stopAndReturn (*phazite::findSong (s.context));

    // The same as an export of that version's song, brought back, written as a bounce is.
    REQUIRE (phazite::bringBackVersion (s.context, quiet));
    phazite::ExportSettings settings;
    settings.format = phazite::ExportSettings::Format::flac;
    settings.bitDepth = 16;
    settings.sampleRate = phazite::getSession (s.context)->getEngine().getSampleRate();
    const auto exported = sounds.folder.getChildFile ("Quiet.flac");
    std::optional<phazite::ExportReport> report;
    const auto exporting = phazite::startExport (*phazite::findSong (s.context), exported, settings,
                                                 [&report] (const phazite::ExportReport& r) { report = r; });
    REQUIRE (dispatchMessagesUntil ([&report] { return report.has_value(); }, 60000));
    REQUIRE (report->result.wasOk());

    const auto bounced = readAudio (phazite::getVersionHistory (s.context)->getBounceFile (quiet)), wanted = readAudio (exported);
    REQUIRE (bounced.getNumSamples() == wanted.getNumSamples());
    CHECK (bounced.getMagnitude (0, 0, bounced.getNumSamples()) > 0.01f); // through the plug-in at its gain, as the export

    for (int channel = 0; channel < 2; ++channel)
    {
        auto differs = 0;

        for (int i = 0; i < bounced.getNumSamples(); ++i)
            differs += juce::exactlyEqual (bounced.getSample (channel, i), wanted.getSample (channel, i)) ? 0 : 1;

        CHECK (differs == 0);
    }
}

TEST_CASE ("The song plays in real time while an old version renders in the background, no block held up")
{
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Hits");
    s.addAudioClip (track, writeHits (sounds.folder.getChildFile ("Hits.wav"), 90.0, 0.5f), 0.0);
    const auto old = keepVersion (s.context, "Old");
    s.setVolume (track, -6.0f);
    keepVersion (s.context, "New");

    RealTimePlayback playback;
    auto& engine = phazite::getSession (s.context)->getEngine();
    phazite::playOrStop (*phazite::findSong (s.context));
    RealTimePlayback::wait (300);
    engine.takeBlockTimes(); // from here on
    playback.takeLongestCall();

    REQUIRE (phazite::startBackgroundBounce (s.context, old));

    // A second and a half of the render, or all of it where it ends sooner: an optimised build renders the song faster than
    // it plays (CI's windows-release did, 2026-11-06).
    const auto done = dispatchMessagesUntil ([&s, &old] { return hasBounceOf (s.context, old); }, 1500);
    const auto times = engine.takeBlockTimes();
    const auto longestCall = playback.takeLongestCall();
    INFO ("longest call " << longestCall * 1000.0 << " ms, longest block " << times.longestBlock * 1000.0 << " ms, longest gap "
                          << times.longestGap * 1000.0 << " ms, " << times.late << " late");
    CHECK ((done || phazite::getBounceProgress (s.context, old).has_value())); // rendering all the while, or done

    // The render held no block up: each of the device's calls played its block, with whatever it waited on, in less than a
    // block's length. A call that came late was the machine's: CI's windows-debug, running four tests at once, kept the
    // device's thread from a core for 33 ms while no block took a millisecond (2026-11-12), counted as late blocks.
    CHECK (times.longestBlock > 0.0); // blocks played meanwhile
    CHECK (longestCall < times.blockLength);
    phazite::stopAndReturn (*phazite::findSong (s.context));
    REQUIRE (phazite::deleteBounce (s.context, old)); // stops it
}

TEST_CASE ("No bounce starts while the song records, and one going on in the background stops as recording starts")
{
    TestSong s;
    TemporaryFolder sounds;
    const auto track = s.addTrack ("Hits");
    s.addAudioClip (track, writeHits (sounds.folder.getChildFile ("Hits.wav"), 90.0, 0.5f), 0.0); // long, to be rendering still
    const auto old = keepVersion (s.context, "Old");
    s.setVolume (track, -6.0f);
    keepVersion (s.context, "New");
    auto& song = *phazite::findSong (s.context);
    phazite::getSession (s.context)->setLiveTrack (track); // live MIDI plays it: it can record

    // Recording: none starts.
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    CHECK_FALSE (phazite::canBounce (s.context, old));
    phazite::stopRecording (song);
    REQUIRE (phazite::canBounce (s.context, old));

    // One going on stops as recording starts, and says so.
    REQUIRE (phazite::startBackgroundBounce (s.context, old));
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    CHECK (dispatchMessagesUntil ([&s, &old] { return ! phazite::getBounceProgress (s.context, old).has_value(); }));
    CHECK (phazite::getBounceProblem (s.context) == "Bouncing \"Old\" stopped for recording.");
    CHECK_FALSE (hasBounceOf (s.context, old));
    phazite::stopRecording (song);

    // Deleting a bounce that renders stops it, leaving nothing.
    REQUIRE (phazite::startBackgroundBounce (s.context, old));
    REQUIRE (phazite::deleteBounce (s.context, old));
    CHECK_FALSE (phazite::getBounceProgress (s.context, old).has_value());
    CHECK_FALSE (phazite::getVersionHistory (s.context)->getBounceFile (old).existsAsFile());
}

TEST_CASE ("Save Version with a bounce bounces the version it saves, in the host, as the box says")
{
    TestSong s;
    TemporaryFolder sounds;
    s.addAudioClip (s.addTrack ("Hits"), writeHits (sounds.folder.getChildFile ("Hits.wav"), 2.0, 0.5f), 0.0);
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (
        commands, s.context, [&s] (std::function<void (bool)> then) { then (s.getProject().save().wasOk()); }, s.renders);
    REQUIRE (waitForVersionHistory (s.context));

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

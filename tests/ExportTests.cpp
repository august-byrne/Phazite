// SPDX-License-Identifier: AGPL-3.0-or-later
// Export (roadmap step 1.8): the song rendered to WAV or FLAC, normalized to a loudness under a true-peak ceiling, and
// written whole or not at all (src/model/project/Export.h), and the window that does it (src/ui/shell/ExportPanel.h). And
// its stems (step 2.11): a file for each channel that reaches the master, lined up, into a folder written whole or not
// at all.
#include "TestClips.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/Export.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "services/Settings.h"
#include "ui/shell/ExportPanel.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <optional>

using phazite::ExportReport;
using phazite::ExportSettings;

namespace
{
// A song at 120 BPM in 4/4 (a bar lasts 2 s) that is a steady A (220 Hz) at a quarter of full scale, in a clip from its
// start to 1.5 s: the song ends with its first bar, at 2 s.
struct SineSong
{
    SineSong()
    {
        clip = phazite::test::addAudioClip (*song, phazite::ensureTracks (*song, 1)[0], "Sine", sound, 0.0, 1.5);
        REQUIRE (clip.isValid());
    }

    phazite::test::TemporaryFolder temp;
    const juce::File sound = phazite::test::writeSound (temp.folder.getChildFile ("Sine.wav"), 2.0);
    const juce::File renders = temp.folder.getChildFile ("Renders");
    phazite::Song::Ptr song = phazite::createSong();
    phazite::ClipID clip;
};

// Exports, and waits for the report.
ExportReport exportAndWait (phazite::Song& song, const juce::File& file, const ExportSettings& settings)
{
    std::optional<ExportReport> report;
    const auto running = phazite::startExport (song, file, settings, [&report] (const ExportReport& r) { report = r; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }));
    return *report;
}

std::unique_ptr<juce::AudioFormatReader> openFile (const juce::File& file)
{
    return std::unique_ptr<juce::AudioFormatReader> (phazite::getAudioFormats().createReaderFor (file));
}

// The files in a folder, by name.
juce::StringArray getFileNames (const juce::File& folder)
{
    juce::StringArray names;

    for (const auto& file : folder.findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no))
        names.add (file.getFileName());

    names.sort (true);
    return names;
}

// The names of the channels an export of stems writes a file for, in their order.
juce::StringArray getStemNames (const phazite::Song& song)
{
    juce::StringArray names;

    for (const auto channel : phazite::getStemChannels (song))
        names.add (phazite::getTrackName (song, channel));

    return names;
}

// The RMS level of a stretch of the left channel, in seconds.
double getLevel (const juce::AudioBuffer<float>& audio, double sampleRate, double start, double end)
{
    const auto first = juce::roundToInt (start * sampleRate);
    return audio.getRMSLevel (0, first, juce::roundToInt (end * sampleRate) - first);
}

// How often the left channel goes from below zero to zero or above, per second, in a stretch of it.
double getFrequency (const juce::AudioBuffer<float>& audio, double sampleRate, double start, double end)
{
    const auto first = juce::roundToInt (start * sampleRate), last = juce::roundToInt (end * sampleRate);
    int rises = 0;

    for (int i = first + 1; i < last; ++i)
        if (audio.getSample (0, i - 1) < 0.0f && audio.getSample (0, i) >= 0.0f)
            ++rises;

    return rises / (end - start);
}
} // namespace

TEST_CASE ("Export settings are checked")
{
    ExportSettings settings; // 44.1 kHz, 24-bit WAV, as it plays
    CHECK (phazite::isValid (settings));

    settings.sampleRate = 22050.0;
    CHECK_FALSE (phazite::isValid (settings));
    settings.sampleRate = 96000.0;
    CHECK (phazite::isValid (settings));

    settings.bitDepth = 32; // floating point, in WAV only
    CHECK (phazite::isValid (settings));
    settings.format = ExportSettings::Format::flac;
    CHECK_FALSE (phazite::isValid (settings));
    settings.bitDepth = 16;
    CHECK (phazite::isValid (settings));
    settings.bitDepth = 8;
    CHECK_FALSE (phazite::isValid (settings));
    settings.bitDepth = 24;

    settings.loudnessTarget = -14.0;
    CHECK (phazite::isValid (settings));
    settings.loudnessTarget = -31.0;
    CHECK_FALSE (phazite::isValid (settings));
    settings.loudnessTarget = -4.0;
    CHECK_FALSE (phazite::isValid (settings));
    settings.loudnessTarget = -23.0;

    settings.truePeakCeiling = 0.5;
    CHECK_FALSE (phazite::isValid (settings));
    settings.truePeakCeiling = -6.0;
    CHECK (phazite::isValid (settings));

    CHECK (phazite::getFileSuffix (ExportSettings::Format::wav) == ".wav");
    CHECK (phazite::getFileSuffix (ExportSettings::Format::flac) == ".flac");
}

TEST_CASE ("An export covers the song to its end, and needs a song")
{
    SineSong sine;
    CHECK (phazite::getExportRange (*sine.song) == phazite::BeatRange (phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0)));
    CHECK (phazite::canExport (*sine.song));

    phazite::deleteClips (*sine.song, { sine.clip }, false);
    CHECK_FALSE (phazite::canExport (*sine.song));

    const auto file = sine.renders.getChildFile ("Song.wav");
    const auto report = exportAndWait (*sine.song, file, {});
    CHECK (report.result.failed());
    CHECK (report.file == file);
    CHECK_FALSE (file.exists());
}

TEST_CASE ("A song exports to a WAV file that plays it")
{
    SineSong sine;
    const auto file = sine.renders.getChildFile ("Song.wav");
    const auto report = exportAndWait (*sine.song, file, {});
    REQUIRE (report.result.wasOk());
    CHECK (report.file == file);

    // The song to the end of its bar; the clip's end leaves no tail.
    CHECK (report.seconds == doctest::Approx (2.0).epsilon (0.001));

    const auto reader = openFile (file);
    REQUIRE (reader != nullptr);
    CHECK (reader->getFormatName() == "WAV file");
    CHECK (reader->sampleRate == doctest::Approx (44100.0));
    CHECK (reader->bitsPerSample == 24);
    CHECK (reader->numChannels == 2);
    CHECK (reader->lengthInSamples == juce::roundToInt (report.seconds * 44100.0));

    // The sine while the clip plays, then silence.
    juce::AudioBuffer<float> audio (2, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    CHECK (getLevel (audio, 44100.0, 0.1, 1.4) > 0.1);
    CHECK (getFrequency (audio, 44100.0, 0.1, 1.1) == doctest::Approx (220.0).epsilon (0.01));
    CHECK (getLevel (audio, 44100.0, 1.55, 2.0) < 0.0001);

    // Measured as it was written: about -16 LUFS for a sine 12 dB under full scale, 3 dB quieter in each channel of a
    // track panned to the center. Nothing else is left in the folder.
    CHECK (report.integratedLufs > -18.0);
    CHECK (report.integratedLufs < -14.0);
    CHECK (report.truePeakDb < -12.0);
    CHECK (getFileNames (sine.renders) == juce::StringArray { "Song.wav" });
}

TEST_CASE ("An export plays a delay's echoes out past the song's end, through the silence between them")
{
    // At 120 BPM, a click on the last beat of the song's one bar, at 1.5 s, through a delay of a second, its echoes alone,
    // each 0.3 of the one before: the song ends at 2 s in silence, and the echoes come at 2.5 s, 3.5 s and on. Until
    // 2026-11-13 the tail ended with its first silent block, the song's last: no echo was written.
    phazite::test::TestSong song;
    song.setTempo (120.0);
    song.addAudioClip (song.firstTrack, phazite::test::writeClicks (song.getSoundsFolder(), 1, 120.0), 3.0);
    const auto delay = song.addEffect (song.firstTrack, phazite::devices::Delay::typeID);
    song.setParameter (delay, "sync", 0.0f);
    song.setParameter (delay, "time", 1000.0f);
    song.setParameter (delay, "feedback", 0.3f);
    song.setParameter (delay, "mix", 1.0f);
    phazite::test::dispatchPendingMessages();

    const auto file = song.getSoundsFolder().getChildFile ("Renders").getChildFile ("Echoes.wav");
    const auto report = exportAndWait (*phazite::findSong (song.context), file, {});
    REQUIRE (report.result.wasOk());

    // Both echoes and those after them, down to those too quiet to hear, and no more than the tail may last (ten seconds).
    CHECK (report.seconds > 4.5);
    CHECK (report.seconds <= 12.0 + 0.001);

    const auto reader = openFile (file);
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio (2, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    const auto loudest = [&audio] (double start, double end)
    { return audio.getMagnitude (0, juce::roundToInt (start * 44100.0), juce::roundToInt ((end - start) * 44100.0)); };

    CHECK (loudest (1.6, 2.45) < 1.0e-4f); // the click's echoes alone are heard
    CHECK (loudest (2.45, 2.6) > 0.05f);
    CHECK (loudest (2.6, 3.45) < 1.0e-4f);
    CHECK (loudest (3.45, 3.6) > 0.005f);
    CHECK (loudest (3.45, 3.6) < loudest (2.45, 2.6));
}

TEST_CASE ("An export meets the loudness target it was given")
{
    SineSong sine;

    ExportSettings streaming;
    streaming.loudnessTarget = -14.0;
    const auto loud = exportAndWait (*sine.song, sine.renders.getChildFile ("Streaming.wav"), streaming);
    REQUIRE (loud.result.wasOk());
    CHECK (loud.integratedLufs == doctest::Approx (-14.0).epsilon (0.005)); // within 0.07 LU
    CHECK (loud.truePeakDb <= -1.0);
    CHECK (phazite::getLoudnessShortfall (streaming, loud) == doctest::Approx (0.0));

    // FLAC, at 48 kHz in 16 bits, for broadcast.
    ExportSettings broadcast;
    broadcast.format = ExportSettings::Format::flac;
    broadcast.sampleRate = 48000.0;
    broadcast.bitDepth = 16;
    broadcast.loudnessTarget = -23.0;
    const auto quiet = exportAndWait (*sine.song, sine.renders.getChildFile ("Broadcast.flac"), broadcast);
    REQUIRE (quiet.result.wasOk());
    CHECK (quiet.integratedLufs == doctest::Approx (-23.0).epsilon (0.005));
    CHECK (quiet.seconds == doctest::Approx (2.0).epsilon (0.001));

    const auto reader = openFile (quiet.file);
    REQUIRE (reader != nullptr);
    CHECK (reader->getFormatName() == "FLAC file");
    CHECK (reader->sampleRate == doctest::Approx (48000.0));
    CHECK (reader->bitsPerSample == 16);
    CHECK (reader->numChannels == 2);
}

TEST_CASE ("The ceiling holds an export's true peak, under its target if it must")
{
    // A sine's true peak is about 1 dB over its loudness, so -5 LUFS would take it over a ceiling of -6 dBTP.
    SineSong sine;
    ExportSettings settings;
    settings.loudnessTarget = -5.0;
    settings.truePeakCeiling = -6.0;

    const auto report = exportAndWait (*sine.song, sine.renders.getChildFile ("Song.wav"), settings);
    REQUIRE (report.result.wasOk());
    CHECK (report.truePeakDb <= -6.0 + 0.01);
    CHECK (report.truePeakDb > -6.5);
    CHECK (report.integratedLufs < -5.5);
    CHECK (phazite::getLoudnessShortfall (settings, report) == doctest::Approx (-5.0 - report.integratedLufs));
}

TEST_CASE ("An export replaces a file only once it is complete")
{
    SineSong sine;
    const auto file = sine.renders.getChildFile ("Song.wav");
    REQUIRE (file.create().wasOk());
    REQUIRE (file.replaceWithText ("an earlier export"));

    // Cancelled: the file stays as it was, and nothing else is left in the folder.
    {
        const auto running =
            phazite::startExport (*sine.song, file, {}, [] (const ExportReport&) { FAIL ("a cancelled export reports nothing"); });
    }

    phazite::test::dispatchPendingMessages();
    CHECK (file.loadFileAsString() == "an earlier export");
    CHECK (getFileNames (sine.renders) == juce::StringArray { "Song.wav" });

    // Complete: replaced.
    const auto report = exportAndWait (*sine.song, file, {});
    REQUIRE (report.result.wasOk());
    CHECK (openFile (file) != nullptr);
    CHECK (getFileNames (sine.renders) == juce::StringArray { "Song.wav" });
}

TEST_CASE ("The project's engine plays nothing of it while an export has its plug-ins, and plays it again after")
{
    SineSong sine;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::setSong (context, sine.song.get());
    const auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    REQUIRE (engine.getPlan() != nullptr);

    std::optional<ExportReport> report;
    auto running = phazite::startExport (*sine.song, sine.renders.getChildFile ("Song.wav"), ExportSettings {},
                                         [&report] (const ExportReport& r) { report = r; });
    CHECK (engine.getPlan() == nullptr); // the render has it
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }));
    CHECK (report->result.wasOk());
    CHECK (engine.getPlan() != nullptr);

    // And after one that is cancelled.
    running = phazite::startExport (*sine.song, sine.renders.getChildFile ("Again.wav"), ExportSettings {}, [] (const ExportReport&) {});
    CHECK (engine.getPlan() == nullptr);
    running = nullptr;
    CHECK (engine.getPlan() != nullptr);
    phazite::setSong (context, nullptr);
}
//==============================================================================
namespace
{
// A band at 120 BPM (a bar lasts 2 s), each part a steady sine of its own from the start to 1.5 s: Kick and Snare in the
// bus Drums; Bass, which sends to the return Reverb (which has no devices: what reaches it comes out); Pad, muted; Lead in
// a plain folder; and Empty, which has no clips.
struct Band
{
    Band()
    {
        const auto tracks = phazite::ensureTracks (*song, 6);
        kick = tracks[0];
        snare = tracks[1];
        bass = tracks[2];
        pad = tracks[3];
        lead = tracks[4];
        empty = tracks[5];

        play (kick, "Kick", 110.0);
        play (snare, "Snare", 440.0);
        play (bass, "Bass", 165.0);
        play (pad, "Pad", 880.0);
        play (lead, "Lead", 330.0);
        phazite::setTrackName (*song, empty, "Empty");
        phazite::setTrackMuted (*song, pad, true);

        drums = phazite::groupIntoFolder (*song, { kick, snare }, nullptr);
        REQUIRE (drums.isValid());
        phazite::setTrackName (*song, drums, "Drums");
        const auto plain = phazite::groupIntoFolder (*song, { lead }, nullptr);
        REQUIRE (plain.isValid());
        phazite::setTrackName (*song, plain, "Plain");
        phazite::setMixedAsBus (*song, plain, false);

        reverb = phazite::addReturnTrack (*song, "Reverb", nullptr);
        REQUIRE (reverb.isValid());
        REQUIRE (phazite::addSend (*song, bass, reverb, false).isValid()); // at -6 dB
    }

    void play (phazite::TrackID track, const juce::String& name, double frequency)
    {
        phazite::setTrackName (*song, track, name);
        const auto sound = phazite::test::writeSound (temp.folder.getChildFile (name + ".wav"), 2.0, frequency);
        REQUIRE (phazite::test::addAudioClip (*song, track, name, sound, 0.0, 1.5).isValid());
    }

    phazite::test::TemporaryFolder temp;
    const juce::File renders = temp.folder.getChildFile ("Renders");
    const juce::File folder = renders.getChildFile ("Band Stems");
    phazite::Song::Ptr song = phazite::createSong();
    phazite::TrackID kick, snare, bass, pad, lead, empty, reverb, drums;
};

ExportSettings stems()
{
    ExportSettings settings;
    settings.stems = true;
    return settings;
}

juce::AudioBuffer<float> readAll (const juce::File& file)
{
    const auto reader = openFile (file);
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    return audio;
}
} // namespace

TEST_CASE ("Stems are the channels that reach the master and play something, in the mixer's order")
{
    Band band;

    // Kick and Snare are in their bus's stem; Lead is its own, as its folder is plain; Pad is muted and Empty has nothing.
    CHECK (getStemNames (*band.song) == juce::StringArray { "Drums", "Bass", "Lead", "Reverb" });
    CHECK (phazite::getStemFileName (0, "Drums", phazite::getFileSuffix (ExportSettings::Format::wav)) == "01 Drums.wav");
    CHECK (phazite::getStemFileName (11, "Lead", phazite::getFileSuffix (ExportSettings::Format::flac)) == "12 Lead.flac");
    CHECK (phazite::getStemsFolder (band.renders.getChildFile ("Band.wav")) == band.folder);

    // A solo leaves the others silent, and a return plays on (returns are solo-safe).
    phazite::setTrackSoloed (*band.song, band.bass, true);
    CHECK (getStemNames (*band.song) == juce::StringArray { "Bass", "Reverb" });

    // Settings: never a loudness target for stems.
    auto settings = stems();
    CHECK (phazite::isValid (settings));
    settings.loudnessTarget = -14.0;
    CHECK_FALSE (phazite::isValid (settings));
}

TEST_CASE ("Stems export into a folder, a file for each channel, each its own sound, lined up from the song's start")
{
    Band band;
    const auto report = exportAndWait (*band.song, band.folder, stems());
    REQUIRE (report.result.wasOk());
    CHECK (report.file == band.folder);
    CHECK (report.seconds == doctest::Approx (2.0).epsilon (0.001));
    REQUIRE (report.stems.size() == 4);
    CHECK (report.stems[1] == band.folder.getChildFile ("02 Bass.wav"));
    CHECK (getFileNames (band.folder) == juce::StringArray { "01 Drums.wav", "02 Bass.wav", "03 Lead.wav", "04 Reverb.wav" });
    CHECK (getFileNames (band.renders) == juce::StringArray { "Band Stems" }); // nothing left of the folder it was written in

    for (const auto& file : report.stems)
    {
        INFO (file.getFileName().toStdString());
        const auto reader = openFile (file);
        REQUIRE (reader != nullptr);
        CHECK (reader->sampleRate == doctest::Approx (44100.0));
        CHECK (reader->numChannels == 2);
        CHECK (reader->lengthInSamples >= juce::roundToInt (2.0 * 44100.0) - 1); // from the start, to the song's end at least
    }

    // Each stem has its own channel's sound alone: Bass its 165 Hz, Lead its 330 Hz; the drums are both of theirs.
    const auto drums = readAll (report.stems[0]), bass = readAll (report.stems[1]), lead = readAll (report.stems[2]),
               reverb = readAll (report.stems[3]);
    CHECK (getFrequency (bass, 44100.0, 0.1, 1.1) == doctest::Approx (165.0).epsilon (0.01));
    CHECK (getFrequency (lead, 44100.0, 0.1, 1.1) == doctest::Approx (330.0).epsilon (0.01));
    CHECK (getLevel (drums, 44100.0, 0.1, 1.4) > 0.1);
    CHECK (getLevel (bass, 44100.0, 1.55, 2.0) < 0.0001);

    // The return's stem is what Bass sends it, 6 dB down, though Bass isn't in it: Bass played silently while it rendered.
    CHECK (getFrequency (reverb, 44100.0, 0.1, 1.1) == doctest::Approx (165.0).epsilon (0.01));
    const auto sendRatio = getLevel (reverb, 44100.0, 0.1, 1.4) / getLevel (bass, 44100.0, 0.1, 1.4);
    CHECK (sendRatio == doctest::Approx (0.5).epsilon (0.05));

    // The mute switches are as they were.
    CHECK_FALSE (phazite::isTrackMuted (*band.song, band.bass, false));
    CHECK (phazite::isTrackMuted (*band.song, band.pad, false));
}

TEST_CASE ("An export of stems compiles each stem as its turn comes, not every one before the first starts")
{
    // Known bug 125: each stem's player and plan were compiled on the message thread before the first stem started, and all
    // held at once until the export ended. Each stem's plan asks the disk about the song's sounds once, as any plan of the
    // whole song does (SongPlayer::countFilesAsked()): as the export starts, the first stem's alone; by its end, each
    // stem's in its turn.
    Band band;
    auto asked = phazite::SongPlayer::countFilesAsked();
    phazite::SongPlayer (*band.song).compile (44100.0, 512, std::nullopt);
    const auto perPlan = phazite::SongPlayer::countFilesAsked() - asked;
    REQUIRE (perPlan > 0);

    asked = phazite::SongPlayer::countFilesAsked();
    std::optional<ExportReport> report;
    const auto running = phazite::startExport (*band.song, band.folder, stems(), [&report] (const ExportReport& done) { report = done; });
    CHECK (phazite::SongPlayer::countFilesAsked() - asked == perPlan);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }));
    REQUIRE (report->result.wasOk());
    CHECK (report->stems.size() == 4);
    CHECK (phazite::SongPlayer::countFilesAsked() - asked == 4 * perPlan);
}

TEST_CASE ("An export plays a reversed clip's source as it is now, written again in its place within the second (known bug 198)")
{
    // The second review of known bug 198: a source's identity on the disk is trusted for a second (model/engine/AudioFiles.h),
    // but the export's fresh player asks about every file it plays, and names the render for the source as it is now. The
    // renders made first ask about their sources too (renderMissingClipAudio()): trusting the old identity, they found the
    // old render there and made none, and the player named a render nobody made: silence. Phazite's clock held.
    const phazite::test::HeldClock clock;
    SineSong sine;
    phazite::setClipReversed (*sine.song, sine.clip, true);
    REQUIRE (phazite::renderMissingClipAudio (*sine.song)); // the source as first written, asked about
    const auto written = sine.sound.getLastModificationTime();
    phazite::test::writeSound (sine.sound, 2.0, 440.0);
    REQUIRE (sine.sound.setLastModificationTime (written + juce::RelativeTime::hours (1.0)));

    const auto report = exportAndWait (*sine.song, sine.renders.getChildFile ("Song.wav"), {});
    REQUIRE (report.result.wasOk());
    const auto audio = readAll (report.file);
    CHECK (getLevel (audio, 44100.0, 0.1, 1.4) > 0.1);
    CHECK (getFrequency (audio, 44100.0, 0.1, 1.1) == doctest::Approx (440.0).epsilon (0.01));
}

TEST_CASE ("An export of stems plays a reversed clip's source as it is when the clip's stem's turn comes (known bug 198)")
{
    // The second review of known bug 198: each stem's player is made as its turn comes, and names each render for its source
    // as it is then. A source written again after the export began, while the first stem played, named a render nobody made
    // (the renders were made once, as the export began): silence in that stem. Now the renders a stem's player plays are
    // made as it is made. The second stem's compile comes on the message thread, which this test holds until the source is
    // written again. Phazite's clock held.
    const phazite::test::HeldClock clock;
    phazite::test::TemporaryFolder temp;
    const auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto source = phazite::test::writeSound (temp.folder.getChildFile ("Source.wav"), 2.0, 220.0);
    REQUIRE (phazite::test::addAudioClip (*song, tracks[0], "Other",
                                          phazite::test::writeSound (temp.folder.getChildFile ("Other.wav"), 2.0, 165.0), 0.0, 1.5)
                 .isValid());
    const auto reversed = phazite::test::addAudioClip (*song, tracks[1], "Source", source, 0.0, 1.5);
    REQUIRE (reversed.isValid());
    phazite::setClipReversed (*song, reversed, true);
    REQUIRE (phazite::renderMissingClipAudio (*song));

    std::optional<ExportReport> report;
    const auto running =
        phazite::startExport (*song, temp.folder.getChildFile ("Stems"), stems(), [&report] (const ExportReport& done) { report = done; });
    const auto written = source.getLastModificationTime();
    phazite::test::writeSound (source, 2.0, 440.0);
    REQUIRE (source.setLastModificationTime (written + juce::RelativeTime::hours (1.0)));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }));
    REQUIRE (report->result.wasOk());
    REQUIRE (report->stems.size() == 2);

    const auto first = readAll (report->stems[0]), second = readAll (report->stems[1]);
    CHECK (getFrequency (first, 44100.0, 0.1, 1.1) == doctest::Approx (165.0).epsilon (0.01));
    CHECK (getLevel (second, 44100.0, 0.1, 1.4) > 0.05);
    CHECK (getFrequency (second, 44100.0, 0.1, 1.1) == doctest::Approx (440.0).epsilon (0.01));
}

TEST_CASE ("A cancelled export of stems leaves no stems, and the folder that was there")
{
    Band band;
    REQUIRE (band.folder.getChildFile ("Old.txt").create().wasOk());

    {
        const auto running = phazite::startExport (*band.song, band.folder, stems(),
                                                   [] (const ExportReport&) { FAIL ("a cancelled export reports nothing"); });
    }

    phazite::test::dispatchPendingMessages();
    CHECK (getFileNames (band.renders) == juce::StringArray { "Band Stems" });
    CHECK (getFileNames (band.folder) == juce::StringArray { "Old.txt" });
    CHECK_FALSE (phazite::isTrackMuted (*band.song, band.bass, false));

    // Nothing to export: no stems, and a reason.
    for (const auto track : { band.bass, band.lead, band.drums, band.reverb })
        phazite::setTrackMuted (*band.song, track, true);

    const auto report = exportAndWait (*band.song, band.renders.getChildFile ("None"), stems());
    CHECK (report.result.failed());
    CHECK (report.result.getErrorMessage() == "No channel plays anything, so there are no stems to export.");
    CHECK (getFileNames (band.renders) == juce::StringArray { "Band Stems" });
}

TEST_CASE ("An export makes the render of a clip that isn't there yet on its own thread, and plays it")
{
    // The audit of 2026-11-20: an export, a freeze, a bounce and a DAWproject's bounces made the clips' renders that weren't
    // there (with Rubber Band, seconds to minutes) on the message thread as each job's player was made, the window dead and
    // Cancel out of reach meanwhile. They are made on the render's thread now.
    SineSong sine;
    phazite::setClipPitch (*sine.song, sine.clip, 12.0f); // the A an octave up: 440 Hz
    const auto before = phazite::countClipRendersMade();

    const auto report = exportAndWait (*sine.song, sine.renders.getChildFile ("Song.wav"), {});
    REQUIRE (report.result.wasOk());
    const auto after = phazite::countClipRendersMade();
    CHECK (after.onMessageThread == before.onMessageThread);
    CHECK (after.elsewhere == before.elsewhere + 1);

    const auto audio = readAll (report.file);
    CHECK (getLevel (audio, 44100.0, 0.1, 1.4) > 0.08);
    CHECK (getFrequency (audio, 44100.0, 0.1, 1.1) == doctest::Approx (440.0).epsilon (0.01));
}

TEST_CASE ("An export cancelled while it makes a clip's render stops between the stretcher's blocks, and leaves no part of it")
{
    // A pitched clip whose render isn't made: the test's seam holds the making at the stretcher's first question whether to
    // stop, its part file written, until the export is cancelled. The message thread goes on taking messages meanwhile.
    phazite::test::TemporaryFolder temp;
    const auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1)[0];
    const auto clip = phazite::test::addAudioClip (*song, track, "Long",
                                                   phazite::test::writeSound (temp.folder.getChildFile ("Long.wav"), 10.0), 0.0, 10.0);
    REQUIRE (clip.isValid());
    phazite::setClipPitch (*song, clip, 12.0f);
    const auto cache = song->getCacheFolder();
    const auto before = phazite::countClipRendersMade();
    const auto file = temp.folder.getChildFile ("Renders").getChildFile ("Song.wav");

    std::atomic<int> asked { 0 };
    phazite::setClipRenderStopSeam (
        [&asked] (const std::function<bool()>& shouldStop)
        {
            if (++asked > 1 || shouldStop == nullptr)
                return;

            for (int waited = 0; ! shouldStop() && waited < phazite::test::hangTimeout; ++waited)
                juce::Thread::sleep (1);
        });

    auto running = phazite::startExport (*song, file, {}, [] (const ExportReport&) { FAIL ("a cancelled export reports nothing"); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&asked] { return asked.load() > 0; }));
    CHECK (cache.findChildFiles (juce::File::findFiles, false, "*.part").size() == 1); // being made
    running = nullptr;                                                                 // cancels it, and waits for its thread
    phazite::setClipRenderStopSeam ({});
    phazite::test::dispatchPendingMessages();

    CHECK (asked.load() == 1);                                             // it stopped at the question
    CHECK (cache.findChildFiles (juce::File::findFiles, false).isEmpty()); // neither the render nor a part of it
    CHECK (phazite::countClipRendersMade().elsewhere == before.elsewhere);
    CHECK (phazite::countClipRendersMade().onMessageThread == before.onMessageThread);
    CHECK_FALSE (file.exists());
}

TEST_CASE ("An export plays a long audio clip as it did while its render kept every part of its file it had read")
{
    // The audit of 2026-11-20: a render's files let go of what they read before its last part (AudioFile::letGoOfUnread()).
    // A clip of 20 s of a file at another rate than the song's (a resampler, made ready ahead of its start), and a second
    // playing the same file further in, at once (4.5 s on, a whole number of the sine's cycles, so that the two don't cancel
    // out): the export, in floats, is the same to the last bit as the song rendered in
    // one go by an engine of its own, whose files keep all they read.
    phazite::test::TemporaryFolder temp;
    const auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    // Longer than a short file (engine::AudioFile::shortSeconds), which a render keeps whole; at 44.1 kHz.
    const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Long.wav"), 40.0, 330.0);
    REQUIRE (phazite::test::addAudioClip (*song, tracks[0], "First", sound, 0.5, 20.0).isValid());
    REQUIRE (phazite::test::addAudioClip (*song, tracks[1], "Second", sound, 3.0, 20.0, 7.0).isValid());

    ExportSettings settings;
    settings.sampleRate = 48000.0;
    settings.bitDepth = 32;
    const auto report = exportAndWait (*song, temp.folder.getChildFile ("Song.wav"), settings);
    REQUIRE (report.result.wasOk());
    const auto exported = readAll (report.file);
    const auto expected = phazite::renderWithOwnEngine (*song, exported.getNumSamples(), 48000.0, 512);

    auto different = 0;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < exported.getNumSamples(); ++i)
            different += juce::exactlyEqual (exported.getSample (channel, i), expected.getSample (channel, i)) ? 0 : 1;

    CHECK (different == 0);
    CHECK (getLevel (exported, 48000.0, 4.0, 20.0) > 0.1);
}

//==============================================================================
namespace
{
struct ExportWindow
{
    ExportWindow()
    {
        panel.askToReplace = [this] (const juce::File& existing, std::function<void (bool)> answer)
        {
            asked.add (existing);
            answer (replace);
        };
    }

    SineSong sine;
    phazite::Settings settings; // in memory
    const juce::File file = sine.renders.getChildFile ("Song.wav");
    phazite::ui::ExportPanel panel { settings, file, 2.0, getStemNames (*sine.song),
                                     [this] (const ExportSettings& chosen, const juce::File& to,
                                             std::function<void (const ExportReport&)> onFinished)
                                     { return phazite::startExport (*sine.song, to, chosen, std::move (onFinished)); } };
    phazite::test::TestWindow window { panel, 560, 660 };
    phazite::test::Mouse mouse { panel };
    juce::Array<juce::File> asked; // the files the window asked to replace
    bool replace = false;          // the answer

    void click (juce::Component& c) { mouse.click (panel.getLocalPoint (&c, c.getLocalBounds().getCentre())); }

    bool waitUntilDone()
    {
        return phazite::test::dispatchMessagesUntil ([this] { return ! panel.isExporting(); });
    }
};
} // namespace

TEST_CASE ("The export window exports with the choices it shows, and keeps them")
{
    ExportWindow w;
    phazite::test::checkChildrenApart (w.panel);
    CHECK (w.panel.getFile() == w.file);
    CHECK_FALSE (w.panel.getShowButton().isVisible());

    // As it plays, until loudness is normalized.
    CHECK_FALSE (w.panel.getTargetField().isEnabled());
    w.panel.getLoudnessBox().setSelectedId (2, juce::sendNotificationSync);
    CHECK (w.panel.getTargetField().isEnabled());
    CHECK (w.panel.getCeilingField().isEnabled());
    CHECK (w.panel.getTargetField().getTextFromValue (w.panel.getTargetField().getValue()) == "-14.0 LUFS");

    // FLAC has no floating point, and its files end in .flac.
    w.panel.getBitDepthBox().setSelectedId (3, juce::sendNotificationSync); // 32-bit float
    w.panel.getFormatBox().setSelectedId (2, juce::sendNotificationSync);
    CHECK (w.panel.getBitDepthBox().getSelectedId() == 2);
    CHECK_FALSE (w.panel.getBitDepthBox().isItemEnabled (3));
    CHECK (w.panel.getFile() == w.file.withFileExtension ("flac"));
    w.panel.getFormatBox().setSelectedId (1, juce::sendNotificationSync);
    CHECK (w.panel.getBitDepthBox().isItemEnabled (3));
    CHECK (w.panel.getFile() == w.file);

    w.click (w.panel.getExportButton());
    CHECK (w.panel.isExporting());
    CHECK_FALSE (w.panel.getExportButton().isVisible());
    CHECK (w.panel.getCloseButton().getButtonText() == "Cancel");
    CHECK_FALSE (w.panel.getFormatBox().isEnabled());
    REQUIRE (w.waitUntilDone());

    CHECK (w.panel.getStatus().startsWith ("Exported Song.wav: 0:02, -14.0 LUFS, true peak "));
    CHECK (w.panel.getShowButton().isVisible());
    CHECK (w.panel.getCloseButton().getButtonText() == "Close");
    CHECK (w.panel.getExportButton().isVisible());
    CHECK (w.file.existsAsFile());
    CHECK (w.asked.isEmpty());

    // The next export starts with these choices.
    CHECK (w.settings.get (phazite::Settings::exportNormalize));
    CHECK (w.settings.get (phazite::Settings::exportFormat) == "wav");
    CHECK (w.settings.get (phazite::Settings::exportBitDepth) == doctest::Approx (24.0));
    CHECK (w.settings.get (phazite::Settings::exportLoudnessTarget) == doctest::Approx (-14.0));

    // Exporting it again replaces it without asking, and Cancel stops that export.
    w.click (w.panel.getExportButton());
    CHECK (w.asked.isEmpty());
    CHECK (w.panel.isExporting());
    w.click (w.panel.getCloseButton());
    CHECK_FALSE (w.panel.isExporting());
    CHECK (w.panel.getStatus() == "Export cancelled.");
    CHECK (openFile (w.file) != nullptr); // the first export, whole
}

TEST_CASE ("The export window asks before it replaces a file it didn't export")
{
    ExportWindow w;
    REQUIRE (w.file.create().wasOk());

    w.click (w.panel.getExportButton());
    CHECK (w.asked == juce::Array<juce::File> { w.file });
    CHECK_FALSE (w.panel.isExporting());
    CHECK (w.file.getSize() == 0);

    w.replace = true;
    w.click (w.panel.getExportButton());
    CHECK (w.asked.size() == 2);
    CHECK (w.panel.isExporting());
    REQUIRE (w.waitUntilDone());
    CHECK (w.panel.getStatus().startsWith ("Exported Song.wav"));
    CHECK (w.file.getSize() > 0);
}

TEST_CASE ("The export window exports stems into a folder next to the song's file, never normalized")
{
    ExportWindow w;
    w.panel.getLoudnessBox().setSelectedId (2, juce::sendNotificationSync); // normalizing, for the song
    w.panel.getWhatBox().setSelectedId (2, juce::sendNotificationSync);
    CHECK (w.panel.isExportingStems());
    CHECK_FALSE (w.panel.getLoudnessBox().isEnabled());
    CHECK_FALSE (w.panel.getTargetField().isEnabled());
    CHECK_FALSE (w.panel.getSettings().loudnessTarget.has_value());
    CHECK (w.panel.getAbout().contains (phazite::getTrackName (*w.sine.song, phazite::getTrackIDs (*w.sine.song)[0])));
    CHECK (w.panel.getDestination() == w.sine.renders.getChildFile ("Song Stems"));
    phazite::test::checkChildrenApart (w.panel);

    // A folder there already, which it didn't export: it asks first, and leaves it if not.
    REQUIRE (w.panel.getFolder().createDirectory().wasOk());
    w.click (w.panel.getExportButton());
    CHECK (w.asked == juce::Array<juce::File> { w.panel.getFolder() });
    CHECK_FALSE (w.panel.isExporting());
    REQUIRE (w.panel.getFolder().deleteRecursively());

    w.click (w.panel.getExportButton());
    CHECK (w.panel.isExporting());
    CHECK_FALSE (w.panel.getWhatBox().isEnabled());
    REQUIRE (w.waitUntilDone());
    CHECK (w.panel.getStatus().startsWith ("Exported 1 stem into Song Stems, from the song's start (0:02"));
    CHECK (getFileNames (w.panel.getFolder()).size() == 1);
    CHECK (w.panel.getShowButton().isVisible());

    // The next export starts with stems, and the song keeps its normalizing.
    CHECK (w.settings.get (phazite::Settings::exportStems));
    CHECK (w.settings.get (phazite::Settings::exportNormalize));
    w.panel.getWhatBox().setSelectedId (1, juce::sendNotificationSync);
    CHECK (w.panel.getLoudnessBox().isEnabled());
    CHECK (w.panel.getSettings().loudnessTarget.has_value());
    CHECK (w.panel.getDestination() == w.file);
}

TEST_CASE ("The export window says why an export failed")
{
    ExportWindow w;
    phazite::deleteClips (*w.sine.song, { w.sine.clip }, false); // nothing to export

    w.click (w.panel.getExportButton());
    REQUIRE (w.waitUntilDone());
    CHECK (w.panel.getStatus() == "Phazite couldn't export the song: The song has no clips yet, so there is nothing to export.");
    CHECK_FALSE (w.panel.getShowButton().isVisible());
    CHECK (w.panel.getCloseButton().getButtonText() == "Cancel");
}

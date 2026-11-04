// SPDX-License-Identifier: AGPL-3.0-or-later
// Export (roadmap step 1.8): the song rendered to WAV or FLAC, normalized to a loudness under a true-peak ceiling, and
// written whole or not at all (src/model/project/Export.h), and the window that does it (src/ui/shell/ExportPanel.h). And
// its stems (step 2.11): a file for each channel that reaches the master, lined up, into a folder written whole or not
// at all.
#include "TestClips.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/project/Export.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "services/Settings.h"
#include "ui/shell/ExportPanel.h"

#include <cmath>
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 60000));
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 60000));
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
        return phazite::test::dispatchMessagesUntil ([this] { return ! panel.isExporting(); }, 60000);
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

// SPDX-License-Identifier: AGPL-3.0-or-later
// Export (roadmap step 1.8): the song rendered to WAV or FLAC, normalized to a loudness under a true-peak ceiling, and
// written whole or not at all (src/model/project/Export.h), and the window that does it (src/ui/shell/ExportPanel.h).
#include "UiTestHelpers.h"
#include "model/project/Export.h"
#include "model/project/internal/Export.h"
#include "services/Settings.h"
#include "ui/shell/ExportPanel.h"

#include <cmath>
#include <optional>

using phazite::ExportReport;
using phazite::ExportSettings;

namespace
{
// An Edit at 120 BPM in 4/4 (a bar lasts 2 s) whose song is a steady A (220 Hz) at a quarter of full scale, in a clip
// from its start to 1.5 s: the song ends with its first bar, at 2 s.
struct SineSong
{
    SineSong()
    {
        clip = te::getAudioTracks (*edit)[0]->insertWaveClip (
            "Sine", sound, { te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.5)) }, false);
        REQUIRE (clip != nullptr);
    }

    phazite::test::TemporaryFolder temp;
    const juce::File sound = phazite::test::writeSound (temp.folder.getChildFile ("Sine.wav"), 2.0);
    const juce::File renders = temp.folder.getChildFile ("Renders");
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::WaveAudioClip::Ptr clip;
};

// Exports, and waits for the report.
ExportReport exportAndWait (te::Edit& edit, const juce::File& file, const ExportSettings& settings)
{
    std::optional<ExportReport> report;
    const auto running = phazite::startExport (edit, file, settings, [&report] (const ExportReport& r) { report = r; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 60000));
    return *report;
}

std::unique_ptr<juce::AudioFormatReader> openFile (const juce::File& file)
{
    return std::unique_ptr<juce::AudioFormatReader> (
        phazite::test::getEngine().getAudioFileFormatManager().readFormatManager.createReaderFor (file));
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
    SineSong song;
    CHECK (phazite::getExportRange (*song.edit) == te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (2.0)));
    CHECK (phazite::canExport (*song.edit));

    song.clip->removeFromParent();
    CHECK_FALSE (phazite::canExport (*song.edit));

    const auto file = song.renders.getChildFile ("Song.wav");
    const auto report = exportAndWait (*song.edit, file, {});
    CHECK (report.result.failed());
    CHECK (report.file == file);
    CHECK_FALSE (file.exists());
}

TEST_CASE ("A song exports to a WAV file that plays it")
{
    SineSong song;
    const auto file = song.renders.getChildFile ("Song.wav");
    const auto report = exportAndWait (*song.edit, file, {});
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
    CHECK (getFileNames (song.renders) == juce::StringArray { "Song.wav" });
}

TEST_CASE ("An export meets the loudness target it was given")
{
    SineSong song;

    ExportSettings streaming;
    streaming.loudnessTarget = -14.0;
    const auto loud = exportAndWait (*song.edit, song.renders.getChildFile ("Streaming.wav"), streaming);
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
    const auto quiet = exportAndWait (*song.edit, song.renders.getChildFile ("Broadcast.flac"), broadcast);
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
    SineSong song;
    ExportSettings settings;
    settings.loudnessTarget = -5.0;
    settings.truePeakCeiling = -6.0;

    const auto report = exportAndWait (*song.edit, song.renders.getChildFile ("Song.wav"), settings);
    REQUIRE (report.result.wasOk());
    CHECK (report.truePeakDb <= -6.0 + 0.01);
    CHECK (report.truePeakDb > -6.5);
    CHECK (report.integratedLufs < -5.5);
    CHECK (phazite::getLoudnessShortfall (settings, report) == doctest::Approx (-5.0 - report.integratedLufs));
}

TEST_CASE ("An export replaces a file only once it is complete")
{
    SineSong song;
    const auto file = song.renders.getChildFile ("Song.wav");
    REQUIRE (file.create().wasOk());
    REQUIRE (file.replaceWithText ("an earlier export"));

    // Cancelled: the file stays as it was, and nothing else is left in the folder.
    {
        const auto running =
            phazite::startExport (*song.edit, file, {}, [] (const ExportReport&) { FAIL ("a cancelled export reports nothing"); });
    }

    phazite::test::dispatchPendingMessages();
    CHECK (file.loadFileAsString() == "an earlier export");
    CHECK (getFileNames (song.renders) == juce::StringArray { "Song.wav" });

    // Complete: replaced.
    const auto report = exportAndWait (*song.edit, file, {});
    REQUIRE (report.result.wasOk());
    CHECK (openFile (file) != nullptr);
    CHECK (getFileNames (song.renders) == juce::StringArray { "Song.wav" });
}

TEST_CASE ("The song plays again after an export")
{
    SineSong song;
    auto& transport = song.edit->getTransport();
    transport.ensureContextAllocated();
    REQUIRE (transport.isPlayContextActive());

    std::optional<ExportReport> report;
    auto running = phazite::startExport (*song.edit, song.renders.getChildFile ("Song.wav"), ExportSettings {},
                                         [&report] (const ExportReport& r) { report = r; });
    CHECK_FALSE (transport.isPlayContextActive()); // the render has it
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 60000));
    CHECK (report->result.wasOk());
    CHECK (transport.isPlayContextActive());

    // And after one that is cancelled.
    running = phazite::startExport (*song.edit, song.renders.getChildFile ("Again.wav"), ExportSettings {}, [] (const ExportReport&) {});
    CHECK_FALSE (transport.isPlayContextActive());
    running = nullptr;
    CHECK (transport.isPlayContextActive());
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

    SineSong song;
    phazite::Settings settings; // in memory
    const juce::File file = song.renders.getChildFile ("Song.wav");
    phazite::ui::ExportPanel panel { settings, file, 2.0,
                                     [this] (const ExportSettings& chosen, const juce::File& to,
                                             std::function<void (const ExportReport&)> onFinished)
                                     { return phazite::startExport (*song.edit, to, chosen, std::move (onFinished)); } };
    phazite::test::TestWindow window { panel, 560, 600 };
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

TEST_CASE ("The export window says why an export failed")
{
    ExportWindow w;
    w.song.clip->removeFromParent(); // nothing to export

    w.click (w.panel.getExportButton());
    REQUIRE (w.waitUntilDone());
    CHECK (w.panel.getStatus() == "Phazite couldn't export the song: The song has no clips yet, so there is nothing to export.");
    CHECK_FALSE (w.panel.getShowButton().isVisible());
    CHECK (w.panel.getCloseButton().getButtonText() == "Cancel");
}

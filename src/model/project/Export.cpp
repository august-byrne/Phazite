// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Export.h"
#include "engine/nodes/LoudnessMeter.h"
#include "internal/Export.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/OfflineRender.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace phazite
{
namespace
{
    constexpr int flacQuality = 5;            // JUCE's "5 (Default)", the FLAC tools' own default
    constexpr double loudnessTolerance = 0.5; // LU: what still counts as meeting a loudness target
    constexpr int renderBlockSize = 512;      // samples; the tails end after one this long falls silent
    constexpr int renderPart = 8192;          // samples written at a time
    constexpr double renderedShare = 0.9;     // of an export's progress: playing it, before its file is written

    // A writer of stereo audio into a file, in an export's format, or of 32-bit floats into a WAV file; or nothing if the
    // file can't be written.
    std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::File& file, double sampleRate, const ExportSettings* settings)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
            return nullptr;

        auto options = juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (2);

        if (settings == nullptr || settings->bitDepth == 32)
            options = options.withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        else
            options = options.withBitsPerSample (settings->bitDepth);

        if (settings != nullptr && settings->format == ExportSettings::Format::flac)
            return juce::FlacAudioFormat().createWriterFor (stream, options.withQualityOptionIndex (flacQuality));

        return juce::WavAudioFormat().createWriterFor (stream, options);
    }

    // A file's integrated loudness and true peak, as Phazite's engine measures them (EBU R128), and its length.
    ExportReport measure (const juce::File& written, const juce::File& file)
    {
        ExportReport report;
        report.file = file;

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (written));

        if (reader == nullptr || reader->sampleRate <= 0.0)
        {
            report.result = juce::Result::fail ("Phazite couldn't read the exported file back.");
            return report;
        }

        constexpr int blockSize = 16384;
        const auto channels = std::min (2, (int) reader->numChannels);
        engine::LoudnessMeter meter;
        meter.prepare (reader->sampleRate, blockSize);
        juce::AudioBuffer<float> buffer (channels, blockSize);

        for (juce::int64 position = 0; position < reader->lengthInSamples; position += blockSize)
        {
            const auto samples = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - position);
            reader->read (&buffer, 0, samples, position, true, channels > 1);
            meter.process (buffer.getArrayOfReadPointers(), channels, samples);
        }

        const auto readings = meter.getReadings();
        report.seconds = (double) reader->lengthInSamples / reader->sampleRate;
        report.integratedLufs = readings.integrated.value_or (-100.0f);
        report.truePeakDb = readings.truePeak.value_or (-100.0f);
        return report;
    }

    // Why an export can't start, if it can't.
    juce::Result checkExport (const Song& song, const ExportSettings& settings)
    {
        if (! isValid (settings))
            return juce::Result::fail ("Phazite can't export with these settings.");

        if (! canExport (song))
            return juce::Result::fail (isRecording (song) ? "Stop recording first."
                                                          : "The song has no clips yet, so there is nothing to export.");

        if (settings.stems && getStemChannels (song).empty())
            return juce::Result::fail ("No channel plays anything, so there are no stems to export.");

        return juce::Result::ok();
    }

    // Whether a channel has something to play: clips, a track with clips in its bus, or a send to its return.
    bool hasSomethingToPlay (const Song& song, TrackID channel)
    {
        const auto state = findTrackState (song, channel);

        if (isReturnState (state))
        {
            for (const auto sender : getTrackIDs (song))
                for (const auto send : getSends (song, sender))
                    if (getSendTarget (song, send) == channel)
                        return true;

            return false;
        }

        if (isFolderState (state))
        {
            for (const auto inside : getTracksInFolder (song, channel))
                if (! getClipStates (findTrackState (song, inside)).empty())
                    return true;

            return false;
        }

        return ! getClipStates (state).empty();
    }

    // Reports on the message thread, unless the export that asked has gone by then: its owner, alive while this is
    // (each export's destructor waits for its render thread, the only other owner, before it lets go).
    template <typename Owner>
    void deliverTo (const std::weak_ptr<Owner*>& owner, const ExportReport& report)
    {
        juce::MessageManager::callAsync (
            [owner, report]
            {
                if (const auto locked = owner.lock())
                    (*locked)->finish (report);
            });
    }

    // How an export renders: on Phazite's engine, on as many threads as it plays on, with the plug-ins of the song's
    // session, which it lends (none play without one).
    OfflineRender::Setup getRenderSetup (Song& song, double sampleRate)
    {
        auto* session = findSession (song);
        return { .session = session,
                 .makePlayer = [&song, session]
                 { return std::make_unique<SongPlayer> (song, session != nullptr ? &session->getPlugins() : nullptr); },
                 .numThreads = session != nullptr ? session->getHost().getNumThreads() : 1,
                 .sampleRate = sampleRate,
                 .blockSize = renderBlockSize,
                 .playedShare = renderedShare };
    }

    //==============================================================================
    // What an export writes of a render (model/playback/internal/OfflineRender.h), on its thread: the floats played into
    // its file's format, at one gain, the loudness target's (unless that would push the true peak over the ceiling, which
    // never limits; none for silence, which nothing passes the gates of), dithered when 16 bits
    // (triangular, the same every time). The floats go once written.
    juce::Result writeExport (OfflineRender& render, int job, const juce::File& floats, const juce::File& file,
                              const ExportSettings& settings, const engine::LoudnessMeter::Readings& readings)
    {
        auto gain = 1.0f;

        if (settings.loudnessTarget.has_value() && readings.integrated.has_value())
        {
            auto gainDb = *settings.loudnessTarget - (double) *readings.integrated;

            if (readings.truePeak.has_value())
                gainDb = std::min (gainDb, settings.truePeakCeiling - (double) *readings.truePeak);

            gain = juce::jlimit (0.0f, 100.0f, juce::Decibels::decibelsToGain ((float) gainDb));
        }

        const auto failed = juce::Result::fail ("Phazite couldn't write " + file.getFileName() + ".");
        juce::WavAudioFormat wav;
        const std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (floats), true));
        auto writer = makeWriter (file, settings.sampleRate, &settings);

        if (reader == nullptr || writer == nullptr)
            return failed;

        const auto dither = settings.bitDepth == 16;
        const auto lsb = 1.0f / 32768.0f;
        juce::Random random (0x5048415a); // "PHAZ"
        juce::AudioBuffer<float> part (2, renderPart);

        for (juce::int64 position = 0; position < reader->lengthInSamples; position += renderPart)
        {
            if (render.isCancelled())
                return juce::Result::fail ("Cancelled.");

            const auto length = (int) std::min<juce::int64> (renderPart, reader->lengthInSamples - position);
            reader->read (&part, 0, length, position, true, true);
            part.applyGain (0, length, gain);

            if (dither)
                for (int channel = 0; channel < 2; ++channel)
                    for (auto *sample = part.getWritePointer (channel), *end = sample + length; sample != end; ++sample)
                        *sample += (random.nextFloat() - random.nextFloat()) * lsb;

            if (! writer->writeFromAudioSampleBuffer (part, 0, length))
                return failed;

            render.setProgress (job,
                                renderedShare + (1.0 - renderedShare) * (double) (position + length) / (double) reader->lengthInSamples);
        }

        writer = nullptr;
        floats.deleteFile();
        return juce::Result::ok();
    }
} // namespace

bool isValid (const ExportSettings& settings)
{
    const auto rateIsOffered = std::any_of (std::begin (exportSampleRates), std::end (exportSampleRates),
                                            [&settings] (double rate) { return juce::approximatelyEqual (rate, settings.sampleRate); });
    const auto bitsAreOffered =
        std::find (std::begin (exportBitDepths), std::end (exportBitDepths), settings.bitDepth) != std::end (exportBitDepths)
        && (settings.bitDepth != 32 || settings.format == ExportSettings::Format::wav);
    const auto& target = settings.loudnessTarget;
    const auto targetIsValid = ! target.has_value() || (! settings.stems && *target >= minLoudnessTarget && *target <= maxLoudnessTarget);
    const auto ceilingIsValid = settings.truePeakCeiling >= minTruePeakCeiling && settings.truePeakCeiling <= maxTruePeakCeiling;

    return rateIsOffered && bitsAreOffered && targetIsValid && ceilingIsValid;
}

juce::String getFileSuffix (ExportSettings::Format format)
{
    return format == ExportSettings::Format::flac ? ".flac" : ".wav";
}

juce::File getStemsFolder (const juce::File& songFile)
{
    return songFile.getSiblingFile (songFile.getFileNameWithoutExtension() + " Stems");
}

double getLoudnessShortfall (const ExportSettings& settings, const ExportReport& report)
{
    if (! settings.loudnessTarget.has_value() || report.result.failed() || report.integratedLufs <= -100.0)
        return 0.0;

    const auto shortfall = *settings.loudnessTarget - report.integratedLufs;
    return shortfall > loudnessTolerance ? shortfall : 0.0;
}

BeatRange getExportRange (const Song& song)
{
    return { BeatPosition(), getSongEnd (song) };
}

bool canExport (const Song& song)
{
    return getSongEnd (song) > BeatPosition() && ! isRecording (song);
}

std::vector<TrackID> getStemChannels (const Song& song)
{
    std::vector<TrackID> stems;

    for (const auto track : getChannelTracks (song))
    {
        const auto isPlainFolder = isFolder (song, track) && ! isBus (song, track);
        const auto isPlayed = ! isTrackMuted (song, track, true) && ! isSilencedBySolo (song, track);

        if (! getOutputBus (song, track).isValid() && ! isPlainFolder && isPlayed && hasSomethingToPlay (song, track))
            stems.push_back (track);
    }

    return stems;
}

juce::String getStemFileName (int index, const juce::String& channelName, const juce::String& suffix)
{
    const auto name = juce::File::createLegalFileName (channelName.trim());
    return juce::String (index + 1).paddedLeft ('0', 2) + " " + (name.isNotEmpty() ? name : juce::String ("Track")) + suffix;
}

//==============================================================================
// An export in progress: the song's, or its stems'.
struct SongExport::Impl
{
    virtual ~Impl() = default;
    virtual float getProgress() const = 0;
};

namespace
{
    // The song, through the master's devices, into one file, written next to it and taking its place once complete; then
    // measured again.
    struct SongRender final : SongExport::Impl
    {
        SongRender (Song& song, const juce::File& file, const ExportSettings& settings, std::function<void (const ExportReport&)> callback)
            : temporary (file),
              onFinished (std::move (callback)),
              alive (std::make_shared<SongRender*> (this))
        {
            JUCE_ASSERT_MESSAGE_THREAD

            if (const auto check = checkExport (song, settings); check.failed())
            {
                deliverTo (std::weak_ptr<SongRender*> (alive), { check, file });
                return;
            }

            if (isSongPlaying (song))
                stopAndReturn (song);

            file.getParentDirectory().createDirectory();
            renderMissingClipAudio (song); // the engine plays a clip whose render isn't there silent

            // Played into floats, written into the file, and measured on the render thread before the message thread hears.
            const OfflineRender::Job job { floats.getFile(), toSeconds (song, getExportRange (song).getEnd()), tailSeconds, std::nullopt };
            renderer = std::make_unique<OfflineRender> (
                getRenderSetup (song, settings.sampleRate), std::vector { job },
                [settings, floatsFile = floats.getFile(), written = temporary.getFile()] (OfflineRender& render, int index,
                                                                                          const auto& readings)
                { return writeExport (render, index, floatsFile, written, settings, readings); },
                [weakAlive = std::weak_ptr<SongRender*> (alive), rendered = temporary.getFile(),
                 target = temporary.getTargetFile()] (const juce::Result& result)
                { deliverTo (weakAlive, result.wasOk() ? measure (rendered, target) : ExportReport { result, target }); });
        }

        ~SongRender() override
        {
            alive = nullptr;    // no report from here on
            renderer = nullptr; // cancels the render, if it is still going
        }

        float getProgress() const override { return renderer != nullptr ? renderer->getProgress() : 0.0f; }

        void finish (const ExportReport& rendered)
        {
            renderer = nullptr; // the project plays its plug-ins again
            auto report = rendered;

            if (report.result.wasOk() && ! temporary.overwriteTargetFileWithTemporary())
                report.result =
                    juce::Result::fail ("Phazite couldn't replace " + report.file.getFileName() + ". Is it open in another app?");

            if (const auto callback = onFinished; callback != nullptr)
                callback (report); // last: it may delete this
        }

        juce::TemporaryFile temporary; // the render, until it replaces the chosen file; deleted if it never does
        juce::TemporaryFile floats { temporary.getFile().withFileExtension ("wav") }; // what is played, before it is written
        const std::function<void (const ExportReport&)> onFinished;
        std::shared_ptr<SongRender*> alive; // what the render thread's report holds on to
        std::unique_ptr<OfflineRender> renderer;

        JUCE_DECLARE_NON_COPYABLE (SongRender)
    };

    //==============================================================================
    // The stems, each into a file of its own in a folder, one after another: each its channel alone as it reaches the
    // master, what feeds it playing only through it (SongPlayer::compile()). The folder is written next to the chosen one
    // and takes its place only when every stem is complete.
    struct StemsRender final : SongExport::Impl
    {
        StemsRender (Song& song, const juce::File& f, const ExportSettings& settings, std::function<void (const ExportReport&)> callback)
            : folder (f),
              seconds (toSeconds (song, getExportRange (song).getEnd())),
              onFinished (std::move (callback)),
              alive (std::make_shared<StemsRender*> (this))
        {
            JUCE_ASSERT_MESSAGE_THREAD

            if (const auto check = checkExport (song, settings); check.failed())
            {
                deliverTo (std::weak_ptr<StemsRender*> (alive), { check, folder });
                return;
            }

            if (isSongPlaying (song))
                stopAndReturn (song);

            temporaryFolder = folder.getSiblingFile (folder.getFileName() + " (exporting)").getNonexistentSibling (false);

            if (! temporaryFolder.createDirectory())
            {
                deliverTo (
                    std::weak_ptr<StemsRender*> (alive),
                    { juce::Result::fail ("Phazite couldn't make a folder in " + folder.getParentDirectory().getFullPathName() + "."),
                      folder });
                return;
            }

            renderMissingClipAudio (song); // the engine plays a clip whose render isn't there silent
            const auto channels = getStemChannels (song);
            const auto end = toSeconds (song, getExportRange (song).getEnd());
            std::vector<OfflineRender::Job> jobs;
            std::vector<juce::File> files;

            for (int i = 0; i < (int) channels.size(); ++i)
            {
                const auto fileName = getStemFileName (i, getTrackName (song, channels[(size_t) i]), getFileSuffix (settings.format));
                const auto channel = (std::int64_t) channels[(size_t) i].getRaw();
                jobs.push_back (
                    { temporaryFolder.getChildFile (fileName + ".floats.wav"), end, tailSeconds, SongPlayer::RenderOf { channel } });
                files.push_back (temporaryFolder.getChildFile (fileName));
                stems.add (folder.getChildFile (fileName));
            }

            renderer = std::make_unique<OfflineRender> (
                getRenderSetup (song, settings.sampleRate), jobs,
                [settings, jobs, files] (OfflineRender& render, int index, const auto& readings)
                { return writeExport (render, index, jobs[(size_t) index].file, files[(size_t) index], settings, readings); },
                [weakAlive = std::weak_ptr<StemsRender*> (alive),
                 report = ExportReport { juce::Result::ok(), folder, seconds }] (const juce::Result& result) mutable
                {
                    report.result = result;
                    deliverTo (weakAlive, report);
                });
        }

        ~StemsRender() override
        {
            alive = nullptr;    // no report from here on
            renderer = nullptr; // cancels the stems still to render

            if (temporaryFolder.isDirectory())
                temporaryFolder.deleteRecursively();
        }

        float getProgress() const override { return renderer != nullptr ? renderer->getProgress() : 0.0f; }

        void finish (const ExportReport& rendered)
        {
            renderer = nullptr; // the project plays its plug-ins again
            auto report = rendered;

            if (report.result.wasOk())
            {
                // The folder replaced goes to the recycle bin, or, on a system without one, away.
                if (folder.exists() && ! folder.moveToTrash() && ! folder.deleteRecursively())
                    report.result = juce::Result::fail ("Phazite couldn't move the old " + folder.getFileName()
                                                        + " to the recycle bin. Is a file in it open in another app?");
                else if (! temporaryFolder.moveFileTo (folder))
                    report.result = juce::Result::fail ("Phazite couldn't name the stems' folder " + folder.getFileName() + ".");
                else
                    report.stems = stems;
            }

            if (temporaryFolder.isDirectory())
                temporaryFolder.deleteRecursively(); // only if something failed

            if (const auto callback = onFinished; callback != nullptr)
                callback (report); // last: it may delete this
        }

        const juce::File folder;
        const double seconds;
        juce::File temporaryFolder; // the stems, until it takes the chosen folder's place; deleted if it never does
        const std::function<void (const ExportReport&)> onFinished;
        std::shared_ptr<StemsRender*> alive; // what a report on its way holds on to
        juce::Array<juce::File> stems;
        std::unique_ptr<OfflineRender> renderer;

        JUCE_DECLARE_NON_COPYABLE (StemsRender)
    };
} // namespace

std::unique_ptr<SongExport> startExport (Song& song, const juce::File& file, const ExportSettings& settings,
                                         std::function<void (const ExportReport&)> onFinished)
{
    if (settings.stems)
        return std::make_unique<SongExport> (std::make_unique<StemsRender> (song, file, settings, std::move (onFinished)));

    return std::make_unique<SongExport> (std::make_unique<SongRender> (song, file, settings, std::move (onFinished)));
}

SongExport::SongExport (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

SongExport::~SongExport() = default;

float SongExport::getProgress() const
{
    return impl->getProgress();
}
} // namespace phazite

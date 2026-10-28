// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Export.h"
#include "internal/Export.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    constexpr int flacQuality = 5;            // JUCE's "5 (Default)", the FLAC tools' own default
    constexpr double loudnessTolerance = 0.5; // LU: what still counts as meeting a loudness target
    constexpr int renderBlockSize = 512;      // samples; the tails end after one this long falls silent

    te::Renderer::Parameters makeParameters (te::Edit& edit, const juce::File& destination, const ExportSettings& settings)
    {
        auto& formats = edit.engine.getAudioFileFormatManager();
        const bool flac = settings.format == ExportSettings::Format::flac;

        te::Renderer::Parameters params (edit);
        params.destFile = destination;
        params.audioFormat = flac ? formats.getFlacFormat() : formats.getWavFormat();
        params.quality = flac ? flacQuality : 0;
        params.bitDepth = settings.bitDepth;
        params.sampleRateForAudio = settings.sampleRate;
        params.blockSizeForAudio = renderBlockSize;
        params.time = getExportRange (edit);
        params.endAllowance = te::TimeDuration::fromSeconds (tailSeconds);
        params.tracksToDo = te::toBitSet (te::getAllTracks (edit)); // muted ones stay silent, as when playing
        params.useMasterPlugins = true;
        params.canRenderInMono = false;
        params.ditheringEnabled = settings.bitDepth == 16;

        if (settings.loudnessTarget.has_value())
        {
            params.shouldNormaliseByLUFS = true;
            params.normaliseToLevelDb = (float) *settings.loudnessTarget;
            params.limitTruePeak = true;
            params.truePeakCeilingDb = (float) settings.truePeakCeiling;
        }

        return params;
    }

    // A stem: its channel alone, through its devices, fader and pan, with the tracks that feed it through a sidechain or
    // a send playing silently, so that it sounds as it does in the mix; from the song's start, so that the stems line up,
    // with its tail; without the master's devices, and never normalized.
    te::RenderSpecification makeStemSpecification (te::Track& track, const juce::File& destination, const ExportSettings& settings)
    {
        const bool flac = settings.format == ExportSettings::Format::flac;

        te::RenderSpecification spec;
        spec.tracks = { track.itemID };
        spec.includeSourceTracks = true;
        spec.time = getExportRange (track.edit);
        spec.includeTails = true;
        spec.destination = destination;
        spec.format = flac ? te::RenderFormat::flac : te::RenderFormat::wav;
        spec.sampleRate = settings.sampleRate;
        spec.bitDepth = settings.bitDepth;
        spec.quality = flac ? flacQuality : 0;
        spec.channelLayout = "stereo";
        spec.dither = settings.bitDepth == 16;
        spec.useMasterPlugins = false;
        return spec;
    }

    // Measures a written file: how long it is, its integrated loudness and its true peak, as Tracktion measures it to
    // normalize. The report names the file it will be.
    ExportReport measure (te::Engine& engine, const juce::File& written, const juce::File& file)
    {
        ExportReport report;
        report.file = file;

        const std::unique_ptr<juce::AudioFormatReader> reader (
            engine.getAudioFileFormatManager().readFormatManager.createReaderFor (written));

        if (reader == nullptr || reader->sampleRate <= 0.0)
        {
            report.result = juce::Result::fail ("Phazite couldn't read the exported file back.");
            return report;
        }

        constexpr int blockSize = 16384;
        const auto channels = (int) reader->numChannels;
        te::LoudnessMeter meter;
        meter.prepare (reader->sampleRate, channels, blockSize);
        juce::AudioBuffer<float> buffer (channels, blockSize);

        for (juce::int64 position = 0; position < reader->lengthInSamples; position += blockSize)
        {
            const auto samples = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - position);
            reader->read (&buffer, 0, samples, position, true, channels > 1);
            meter.process (buffer.getArrayOfReadPointers(), channels, samples);
        }

        meter.flush();
        const auto readings = meter.getReadings();
        report.seconds = (double) reader->lengthInSamples / reader->sampleRate;
        report.integratedLufs = readings.integratedValid ? readings.integratedLufs : -100.0;
        report.truePeakDb = readings.truePeakDb;
        return report;
    }

    // Why an export can't start, if it can't.
    juce::Result checkExport (const te::Edit& edit, const ExportSettings& settings)
    {
        if (! isValid (settings))
            return juce::Result::fail ("Phazite can't export with these settings.");

        if (! canExport (edit))
            return juce::Result::fail (isRecording (edit) ? "Stop recording first."
                                                          : "The song has no clips yet, so there is nothing to export.");

        if (settings.stems && getStemTracks (edit).isEmpty())
            return juce::Result::fail ("No channel plays anything, so there are no stems to export.");

        return juce::Result::ok();
    }

    // Whether a channel has something to play: clips, a track with clips in its bus, or a send to its return.
    bool hasSomethingToPlay (const te::Track& track)
    {
        if (const auto bus = getReturnBus (track); bus >= 0)
        {
            for (auto* sender : te::getAudioTracks (track.edit))
                for (auto* send : getSends (*sender))
                    if (send->getBusNumber() == bus)
                        return true;

            return false;
        }

        if (auto* folder = dynamic_cast<const te::FolderTrack*> (&track))
        {
            for (auto* inside : folder->getAllAudioSubTracks (true))
                if (! inside->getClips().isEmpty())
                    return true;

            return false;
        }

        if (auto* clipTrack = dynamic_cast<const te::ClipTrack*> (&track))
            return ! clipTrack->getClips().isEmpty();

        return false;
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

te::TimeRange getExportRange (const te::Edit& edit)
{
    return { te::TimePosition(), getSongEnd (edit) };
}

bool canExport (const te::Edit& edit)
{
    return getSongEnd (edit) > te::TimePosition() && ! isRecording (edit);
}

juce::Array<te::Track*> getStemTracks (const te::Edit& edit)
{
    juce::Array<te::Track*> stems;

    for (auto* track : getChannelTracks (edit))
    {
        const auto isPlainFolder = track->isFolderTrack() && ! isBus (*track);

        if (getOutputBus (*track) == nullptr && ! isPlainFolder && track->shouldBePlayed() && hasSomethingToPlay (*track))
            stems.add (track);
    }

    return stems;
}

juce::String getStemFileName (int index, const te::Track& track, const juce::String& suffix)
{
    const auto name = juce::File::createLegalFileName (track.getName().trim());
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
    // The song, through the master's devices, into one file (Tracktion's EditRenderer).
    struct SongRender final : SongExport::Impl
    {
        SongRender (te::Edit&, const juce::File&, const ExportSettings&, std::function<void (const ExportReport&)> onFinished);
        ~SongRender() override;

        float getProgress() const override { return handle != nullptr ? handle->getProgress() : 0.0f; }

        te::Edit& edit;
        te::Engine& engine;
        juce::TemporaryFile temporary; // the render, until it replaces the chosen file; deleted if it never does
        const std::function<void (const ExportReport&)> onFinished;
        std::shared_ptr<SongRender*> alive; // what the render thread's report holds on to
        std::shared_ptr<te::EditRenderer::Handle> handle;
        bool restorePlayback = false;

        void finish (const ExportReport&);
        void endRender();

        JUCE_DECLARE_NON_COPYABLE (SongRender)
    };

    SongRender::SongRender (te::Edit& e, const juce::File& file, const ExportSettings& settings,
                            std::function<void (const ExportReport&)> callback)
        : edit (e),
          engine (e.engine),
          temporary (file),
          onFinished (std::move (callback)),
          alive (std::make_shared<SongRender*> (this))
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (const auto check = checkExport (edit, settings); check.failed())
        {
            deliverTo (std::weak_ptr<SongRender*> (alive), { check, file });
            return;
        }

        if (edit.getTransport().isPlaying())
            stopAndReturn (edit);

        restorePlayback = edit.getTransport().isPlayContextActive(); // the render frees it
        file.getParentDirectory().createDirectory();
        renderMissingClipAudio (edit); // Tracktion would render a clip whose render isn't ready silent

        // The renderer calls back on its own thread, which measures the file before the message thread hears of it.
        handle = te::EditRenderer::render (makeParameters (edit, temporary.getFile(), settings),
                                           [weakAlive = std::weak_ptr<SongRender*> (alive), &eng = engine,
                                            target = temporary.getTargetFile()] (tl::expected<juce::File, std::string> rendered)
                                           {
                                               if (rendered.has_value())
                                                   deliverTo (weakAlive, measure (eng, *rendered, target));
                                               else
                                                   deliverTo (weakAlive, { juce::Result::fail (juce::String (rendered.error())), target });
                                           });
    }

    SongRender::~SongRender()
    {
        alive = nullptr; // no report from here on
        endRender();     // cancels the render, if it is still going
    }

    void SongRender::endRender()
    {
        handle.reset(); // waits for the render thread

        if (std::exchange (restorePlayback, false))
            edit.getTransport().ensureContextAllocated(); // live MIDI and the meters play again
    }

    void SongRender::finish (const ExportReport& rendered)
    {
        endRender();
        auto report = rendered;

        if (report.result.wasOk() && ! temporary.overwriteTargetFileWithTemporary())
            report.result = juce::Result::fail ("Phazite couldn't replace " + report.file.getFileName() + ". Is it open in another app?");

        if (const auto callback = onFinished; callback != nullptr)
            callback (report); // last: it may delete this
    }

    //==============================================================================
    // The stems, each into a file of its own in a folder, one after another (Tracktion's RenderQueue, which mutes the
    // tracks that feed a stem silently while it renders, and puts their mute switches back). The folder is written next to
    // the chosen one and takes its place only when every stem is complete.
    struct StemsRender final : SongExport::Impl
    {
        StemsRender (te::Edit&, const juce::File& folder, const ExportSettings&, std::function<void (const ExportReport&)> onFinished);
        ~StemsRender() override;

        float getProgress() const override { return queue != nullptr ? queue->getTotalProgress() : 0.0f; }

        te::Edit& edit;
        const juce::File folder;
        juce::File temporaryFolder; // the stems, until it takes the chosen folder's place; deleted if it never does
        const std::function<void (const ExportReport&)> onFinished;
        std::shared_ptr<StemsRender*> alive; // what a report on its way holds on to
        std::unique_ptr<te::RenderQueue> queue;
        juce::Array<juce::File> stems;
        bool rendering = false, restorePlayback = false, wasChanged = false;

        void finish (const ExportReport&);
        void finishRendering(); // the queue's callback, once every stem has finished
        void endRender();

        JUCE_DECLARE_NON_COPYABLE (StemsRender)
    };

    StemsRender::StemsRender (te::Edit& e, const juce::File& f, const ExportSettings& settings,
                              std::function<void (const ExportReport&)> callback)
        : edit (e),
          folder (f),
          onFinished (std::move (callback)),
          alive (std::make_shared<StemsRender*> (this))
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (const auto check = checkExport (edit, settings); check.failed())
        {
            deliverTo (std::weak_ptr<StemsRender*> (alive), { check, folder });
            return;
        }

        if (edit.getTransport().isPlaying())
            stopAndReturn (edit);

        rendering = true;
        restorePlayback = edit.getTransport().isPlayContextActive(); // the renders free it
        wasChanged = edit.hasChangedSinceSaved();                    // the mute switches the queue sets aren't changes
        temporaryFolder = folder.getSiblingFile (folder.getFileName() + " (exporting)").getNonexistentSibling (false);

        if (! temporaryFolder.createDirectory())
        {
            deliverTo (std::weak_ptr<StemsRender*> (alive),
                       { juce::Result::fail ("Phazite couldn't make a folder in " + folder.getParentDirectory().getFullPathName() + "."),
                         folder });
            return;
        }

        renderMissingClipAudio (edit); // Tracktion would render a clip whose render isn't ready silent
        queue = std::make_unique<te::RenderQueue>();
        const auto tracks = getStemTracks (edit);

        for (int i = 0; i < tracks.size(); ++i)
        {
            const auto fileName = getStemFileName (i, *tracks[i], getFileSuffix (settings.format));
            const auto spec = makeStemSpecification (*tracks[i], temporaryFolder.getChildFile (fileName), settings);
            auto job = te::validateRenderSpecification (edit, spec).wasOk() ? te::createRenderJob (edit, spec) : std::nullopt;

            if (! job.has_value())
            {
                queue = nullptr;
                deliverTo (std::weak_ptr<StemsRender*> (alive),
                           { juce::Result::fail ("Phazite couldn't render " + tracks[i]->getName() + "."), folder });
                return;
            }

            job->name = tracks[i]->getName();
            queue->addJob (std::move (*job));
            stems.add (folder.getChildFile (fileName));
        }

        queue->onFinished = [this] { finishRendering(); };
        queue->start();
    }

    StemsRender::~StemsRender()
    {
        alive = nullptr; // no report from here on
        endRender();     // cancels the stems still to render

        if (temporaryFolder.isDirectory())
            temporaryFolder.deleteRecursively();
    }

    void StemsRender::endRender()
    {
        queue = nullptr; // cancels what is left, waiting for the render thread

        if (! std::exchange (rendering, false))
            return;

        if (restorePlayback)
            edit.getTransport().ensureContextAllocated(); // live MIDI and the meters play again

        if (! wasChanged)
            edit.resetChangedStatus();
    }

    void StemsRender::finishRendering()
    {
        ExportReport report;
        report.file = folder;
        report.seconds = getExportRange (edit).getLength().inSeconds();

        for (const auto& job : queue->getJobs())
        {
            if (job->getState() != te::RenderQueue::Job::State::completed)
            {
                report.result =
                    juce::Result::fail (job->getError().isNotEmpty() ? job->getError() : "Phazite couldn't render " + job->getName() + ".");
                break;
            }
        }

        // Not from inside the queue's own callback, which deleting the queue would cut short.
        deliverTo (std::weak_ptr<StemsRender*> (alive), report);
    }

    void StemsRender::finish (const ExportReport& rendered)
    {
        endRender();
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
} // namespace

std::unique_ptr<SongExport> startExport (te::Edit& edit, const juce::File& file, const ExportSettings& settings,
                                         std::function<void (const ExportReport&)> onFinished)
{
    if (settings.stems)
        return std::make_unique<SongExport> (std::make_unique<StemsRender> (edit, file, settings, std::move (onFinished)));

    return std::make_unique<SongExport> (std::make_unique<SongRender> (edit, file, settings, std::move (onFinished)));
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

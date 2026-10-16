// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Export.h"
#include "internal/Export.h"
#include "model/transport/Playback.h"
#include "model/transport/Recording.h"

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
    const auto targetIsValid = ! target.has_value() || (*target >= minLoudnessTarget && *target <= maxLoudnessTarget);
    const auto ceilingIsValid = settings.truePeakCeiling >= minTruePeakCeiling && settings.truePeakCeiling <= maxTruePeakCeiling;

    return rateIsOffered && bitsAreOffered && targetIsValid && ceilingIsValid;
}

juce::String getFileSuffix (ExportSettings::Format format)
{
    return format == ExportSettings::Format::flac ? ".flac" : ".wav";
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

//==============================================================================
struct SongExport::Impl
{
    Impl (te::Edit&, const juce::File&, const ExportSettings&, std::function<void (const ExportReport&)> onFinished);
    ~Impl();

    struct State; // what the render thread shares with this
    te::Edit& edit;
    std::shared_ptr<State> state;
    std::shared_ptr<te::EditRenderer::Handle> handle;
    bool restorePlayback = false;

    void finish (const ExportReport&);
    void endRender();

    JUCE_DECLARE_NON_COPYABLE (Impl)
};

struct SongExport::Impl::State
{
    State (Impl& e, te::Engine& en, const juce::File& file, std::function<void (const ExportReport&)> callback)
        : owner (e),
          engine (en),
          temporary (file),
          onFinished (std::move (callback))
    {
    }

    // Reports on the message thread, unless the export has gone by then. Any thread.
    static void deliver (const std::weak_ptr<State>& weakState, const ExportReport& report)
    {
        juce::MessageManager::callAsync (
            [weakState, report]
            {
                // Here, where the export is deleted, it is alive while this is: ~Impl waits for the render thread, the only
                // other owner, before it lets go.
                if (const auto locked = weakState.lock())
                    locked->owner.finish (report);
            });
    }

    Impl& owner;
    te::Engine& engine;
    juce::TemporaryFile temporary; // the render, until it replaces the chosen file; deleted if it never does
    const std::function<void (const ExportReport&)> onFinished;
};

SongExport::Impl::Impl (te::Edit& e, const juce::File& file, const ExportSettings& settings,
                        std::function<void (const ExportReport&)> onFinished)
    : edit (e),
      state (std::make_shared<State> (*this, e.engine, file, std::move (onFinished)))
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (const auto check = checkExport (edit, settings); check.failed())
    {
        State::deliver (state, { check, file });
        return;
    }

    if (edit.getTransport().isPlaying())
        stopAndReturn (edit);

    restorePlayback = edit.getTransport().isPlayContextActive(); // the render frees it
    file.getParentDirectory().createDirectory();

    // The renderer calls back on its own thread, which measures the file before the message thread hears of it.
    handle = te::EditRenderer::render (makeParameters (edit, state->temporary.getFile(), settings),
                                       [weakState = std::weak_ptr<State> (state)] (tl::expected<juce::File, std::string> rendered)
                                       {
                                           const auto locked = weakState.lock();

                                           if (locked == nullptr)
                                               return;

                                           const auto& target = locked->temporary.getTargetFile();

                                           if (rendered.has_value())
                                               State::deliver (weakState, measure (locked->engine, *rendered, target));
                                           else
                                               State::deliver (weakState, { juce::Result::fail (juce::String (rendered.error())), target });
                                       });
}

SongExport::Impl::~Impl()
{
    endRender(); // cancels it, if it is still rendering
}

void SongExport::Impl::endRender()
{
    handle.reset(); // waits for the render thread

    if (std::exchange (restorePlayback, false))
        edit.getTransport().ensureContextAllocated(); // live MIDI and the meters play again
}

void SongExport::Impl::finish (const ExportReport& rendered)
{
    endRender();
    auto report = rendered;

    if (report.result.wasOk() && ! state->temporary.overwriteTargetFileWithTemporary())
        report.result = juce::Result::fail ("Phazite couldn't replace " + report.file.getFileName() + ". Is it open in another app?");

    if (const auto callback = state->onFinished; callback != nullptr)
        callback (report); // last: it may delete this
}

std::unique_ptr<SongExport> startExport (te::Edit& edit, const juce::File& file, const ExportSettings& settings,
                                         std::function<void (const ExportReport&)> onFinished)
{
    return std::make_unique<SongExport> (std::make_unique<SongExport::Impl> (edit, file, settings, std::move (onFinished)));
}

SongExport::SongExport (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

SongExport::~SongExport() = default;

float SongExport::getProgress() const
{
    return impl->handle != nullptr ? impl->handle->getProgress() : 0.0f;
}
} // namespace phazite

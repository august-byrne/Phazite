// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/OfflineRender.h"
#include "engine/core/Engine.h"
#include "internal/PlaybackSession.h"
#include "model/plugins/internal/SandboxedProcessor.h"

#include <algorithm>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>

namespace phazite
{
namespace
{
    constexpr float silence = 0.000316f; // -70 dB, under which a tail's block is silent
    constexpr int partSamples = 8192;    // rendered and written at a time

    // The longest tail a plan's devices report, in seconds: its plug-ins' (the devices that take their blocks whole), which
    // say theirs as they open, asked on the message thread, where a plug-in is made anew; or Phazite's own devices', which
    // say theirs as they play, with their values as they are then, asked on the thread that plays them.
    double getLongestTail (const engine::Plan& plan, bool ofPlugins)
    {
        auto longest = 0.0;

        for (const auto& strip : plan.strips)
            for (const auto& device : strip.devices)
                if (const auto& runner = device.node->getRunner(); runner.getProcessor().takesWholeBlocks() == ofPlugins)
                    longest = std::max (longest, runner.getTailSeconds());

        return longest;
    }

    std::unique_ptr<juce::AudioFormatWriter> makeFloatWriter (const juce::File& file, double sampleRate)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
            return nullptr;

        return juce::WavAudioFormat().createWriterFor (stream,
                                                       juce::AudioFormatWriterOptions {}
                                                           .withSampleRate (sampleRate)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    }
} // namespace

OfflineRender::OfflineRender (Setup s, const std::vector<Job>& toRender, Rendered whenRendered, Finished whenFinished)
    : juce::Thread ("Phazite render"),
      setup (std::move (s)),
      rendered (std::move (whenRendered)),
      finished (std::move (whenFinished))
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The project's engine plays none of its plug-ins while this has them. One still loading, as the project opened, would
    // play nothing: nothing is rendered without it.
    if (setup.session != nullptr)
    {
        if (const auto loading = setup.session->getPlugins().getLoading(); ! loading.isEmpty())
            whyNot = loading.joinIntoString (", ") + (loading.size() == 1 ? " is" : " are")
                     + " still loading. Render again once the project's plug-ins have loaded.";

        setup.session->lendToRender (true);
    }

    for (const auto& job : toRender)
    {
        Compiled compiled { job, setup.makePlayer(), nullptr };
        compiled.plan = compiled.player->compile (setup.sampleRate, setup.blockSize, std::nullopt, job.of);
        compiled.pluginTailSeconds = getLongestTail (*compiled.plan, true);

        for (const auto& strip : compiled.plan->strips)
            for (const auto& device : strip.devices)
                if (auto* plugin = dynamic_cast<SandboxedProcessor*> (&device.node->getRunner().getProcessor()))
                {
                    compiled.plugins.push_back (plugin);

                    if (device.node->isOn())
                        compiled.heard.push_back (plugin);
                }

        jobs.push_back (std::move (compiled));
    }

    startThread (setup.background ? juce::Thread::Priority::low : juce::Thread::Priority::normal);
}

std::optional<OfflineRender::Wait> OfflineRender::findWait() const
{
    // Its plug-ins' blocks or channels it waits for (any thread: each plug-in's clock is an atomic; the jobs don't change
    // while it lives).
    std::optional<Wait> longest;
    const auto now = sandbox::getNanoseconds();

    for (const auto& job : jobs)
    {
        for (const auto* plugin : job.plugins)
        {
            const auto since = plugin->getRenderWaitSince();
            auto& helper = plugin->getInstance().getRemote().getHelper();
            const auto seconds = since != 0 ? (double) (now - since) * 1.0e-9 : 0.0;

            if (since != 0 && seconds * 1000.0 > helper.getAudioHangMilliseconds() && (! longest.has_value() || seconds > longest->seconds))
                longest = Wait { plugin->getInstance().getDescription().name, seconds };
        }
    }

    return longest;
}

OfflineRender::~OfflineRender()
{
    JUCE_ASSERT_MESSAGE_THREAD
    // It looks at threadShouldExit() between blocks, and waits no longer for a plug-in to play the one it is in: never
    // ended by force, which can leave a lock it held held for ever.
    signalThreadShouldExit();

    for (const auto& job : jobs)
        for (auto* plugin : job.plugins)
            plugin->cancelRender();

    stopThread (-1);

    // Its plug-ins told that they play live again now, not as the first live block comes (which their switch would make
    // late): a post, which waits for no helper.
    for (const auto& job : jobs)
        for (auto* plugin : job.plugins)
            plugin->getInstance().playLive();

    jobs.clear(); // what the renders played, before the project's engine plays the plug-ins again

    if (setup.session != nullptr)
        setup.session->lendToRender (false);
}
void OfflineRender::setProgress (int job, double ofJob) noexcept
{
    const auto share = juce::jlimit (0.0, 1.0, ofJob);
    progress.store ((float) ((job + share) / (double) std::max<size_t> (1, jobs.size())), std::memory_order_relaxed);
}

void OfflineRender::run()
{
    auto result = whyNot.isEmpty() ? juce::Result::ok() : juce::Result::fail (whyNot);

    for (size_t i = 0; i < jobs.size() && result.wasOk() && ! threadShouldExit(); ++i)
        result = play (jobs[i], (int) i);

    if (! threadShouldExit() && finished != nullptr)
        finished (result);
}

juce::Result OfflineRender::play (Compiled& compiled, int index)
{
    const auto& job = compiled.job;
    const auto failed = juce::Result::fail ("Phazite couldn't write " + job.file.getFileName() + ".");

    // A plug-in that had crashed before (and wasn't reloaded) would play nothing: nothing is rendered without it. One
    // switched off isn't heard, crashed or not.
    for (const auto* plugin : compiled.heard)
        if (plugin->getInstance().hasCrashed())
            return juce::Result::fail (plugin->getInstance().getDescription().name
                                       + " has crashed. Reload the plug-in, then render again.");

    auto writer = makeFloatWriter (job.file, setup.sampleRate);

    if (writer == nullptr)
        return failed;

    // From the song's time it starts at: its samples from there are what it counts.
    const auto firstSample = (std::int64_t) std::llround (std::max (0.0, job.startSeconds) * setup.sampleRate);
    const auto firstBeat = compiled.plan->tempo.toBeats ((double) firstSample);

    engine::Engine engine;
    engine.setNumThreads (setup.numThreads);
    engine.prepare (setup.sampleRate, setup.blockSize);
    engine.setPlan (std::move (compiled.plan));
    engine.locate (firstBeat);

    engine::LoudnessMeter meter;
    meter.prepare (setup.sampleRate, partSamples);
    juce::AudioBuffer<float> part (2, partSamples);
    const auto endSample = std::max<std::int64_t> (0, (std::int64_t) std::llround (job.endSeconds * setup.sampleRate) - firstSample);
    const auto lastSample = endSample + (std::int64_t) std::llround (job.tailSeconds * setup.sampleRate);
    std::int64_t done = 0;
    engine.beginRender();

    const auto write = [&writer, &meter, &done] (const juce::AudioBuffer<float>& audio, int length)
    {
        if (! writer->writeFromAudioSampleBuffer (audio, 0, length))
            return false;

        meter.process (audio.getArrayOfReadPointers(), 2, length);
        done += length;
        return true;
    };

    // A plug-in that went as it played, or was ended for hanging: nothing is written without it.
    const auto findFailedPlugin = [this, &compiled]() -> const SandboxedProcessor*
    {
        if (! threadShouldExit())
            for (const auto* plugin : compiled.plugins)
                if (plugin->hasFailedInRender())
                    return plugin;

        return nullptr;
    };

    const auto failedFor = [] (const SandboxedProcessor& plugin)
    {
        return juce::Result::fail (plugin.getInstance().getDescription().name
                                   + " crashed while Phazite rendered. Reload the plug-in, then render again.");
    };

    // The song.
    while (! threadShouldExit() && done < endSample)
    {
        const auto length = (int) std::min<std::int64_t> (partSamples, endSample - done);
        engine.renderPart (part.getArrayOfWritePointers(), length);

        if (const auto* plugin = findFailedPlugin())
            return failedFor (*plugin);

        if (! write (part, length))
            return failed;

        setProgress (index, setup.playedShare * (double) done / (double) std::max<std::int64_t> (1, endSample));
    }

    // Then its tails, a block at a time, until they have been silent for as long as the longest tail the devices played
    // report, with their values as they are then: a delay is silent between its echoes, and a block of silence isn't the
    // tail's end. The silence it ends with isn't written: what is silent waits here until sound follows it.
    const auto* plan = engine.getPlan();
    const auto blockLength = std::min (setup.blockSize, partSamples);
    juce::AudioBuffer<float> quiet;
    auto numQuiet = 0;

    for (auto played = done; ! threadShouldExit() && played < lastSample; played += blockLength)
    {
        engine.renderPart (part.getArrayOfWritePointers(), blockLength);

        if (const auto* plugin = findFailedPlugin())
            return failedFor (*plugin);

        if (part.getMagnitude (0, blockLength) >= silence)
        {
            if ((numQuiet > 0 && ! write (quiet, numQuiet)) || ! write (part, blockLength))
                return failed;

            numQuiet = 0;
            continue;
        }

        const auto tailSeconds = plan != nullptr ? std::max (compiled.pluginTailSeconds, getLongestTail (*plan, false)) : 0.0;
        const auto hold = (std::int64_t) std::llround (std::min (tailSeconds, job.tailSeconds) * setup.sampleRate);

        if (numQuiet + blockLength >= hold)
            break;

        if (quiet.getNumSamples() == 0)
            quiet.setSize (2, (int) (lastSample - endSample) + blockLength);

        for (int channel = 0; channel < 2; ++channel)
            quiet.copyFrom (channel, numQuiet, part, channel, 0, blockLength);

        numQuiet += blockLength;
    }

    engine.endRender();
    writer = nullptr; // written whole

    if (threadShouldExit())
        return juce::Result::fail ("Cancelled.");

    return rendered != nullptr ? rendered (*this, index, meter.getReadings()) : juce::Result::ok();
}
} // namespace phazite

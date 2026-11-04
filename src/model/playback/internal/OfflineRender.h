// SPDX-License-Identifier: AGPL-3.0-or-later
// The song, or a part of it, played offline by Phazite's engine on a thread of its own: an export's
// (model/project/Export.h), a freeze's or a bounce's (model/mixer/FreezeModel.h). Each job plays from the song's start
// to an end, and on until its tails fall silent (a block of 512 under -70 dB, so many seconds at most), into a WAV file
// of 32-bit floats, measured as it goes (EBU R128). What each plays is compiled on the message thread as this is made,
// with the project's plug-ins, which it has alone while it lives (PlaybackSession::lendToRender()); deleting it, on the
// message thread, cancels what is left, waits for its thread and gives them back.
#pragma once

#include "SongPlayer.h"
#include "engine/nodes/LoudnessMeter.h"

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class PlaybackSession;

class OfflineRender final : private juce::Thread
{
public:
    struct Job
    {
        juce::File file {};                        // where its floats go
        double endSeconds = 0.0;                   // the song's time it plays to, before its tails
        double tailSeconds = 0.0;                  // and on, for as long as its tails sound, at most
        std::optional<SongPlayer::RenderOf> of {}; // the song through the master, or a part of it
    };

    // On the render thread: what comes of a job's file once it is written, with what was measured of it (an export writes
    // its own format from it), or why nothing does; and, once every job has (or one has failed), the result. Never once
    // cancelled.
    using Rendered = std::function<juce::Result (OfflineRender&, int job, const engine::LoudnessMeter::Readings&)>;
    using Finished = std::function<void (const juce::Result&)>;

    // How it renders: the session of the project it renders (findSession(), or none), which lends it the plug-ins; what
    // makes a player of the song (a SongPlayer of the project's song), one for each job; and on so many threads, at a rate, in
    // blocks of so many samples, the engine's share of each job's progress (the rest is Rendered's).
    struct Setup
    {
        PlaybackSession* session = nullptr;
        std::function<std::unique_ptr<SongPlayer>()> makePlayer;
        int numThreads = 1;
        double sampleRate = 48000.0;
        int blockSize = 512;
        double playedShare = 1.0;
    };

    OfflineRender (Setup, const std::vector<Job>&, Rendered, Finished);
    ~OfflineRender() override;

    float getProgress() const noexcept { return progress.load (std::memory_order_relaxed); }

    // For what Rendered does with a job, on the render thread: whether it is cancelled, and how far it is through its own
    // part of the job, from 0 to 1.
    bool isCancelled() const noexcept { return threadShouldExit(); }
    void setProgress (int job, double ofJob) noexcept;

private:
    struct Compiled
    {
        Job job;
        std::unique_ptr<SongPlayer> player; // what its plan points at, kept to be let go of on the message thread
        std::unique_ptr<engine::Plan> plan;
    };

    const Setup setup;
    const Rendered rendered;
    const Finished finished;
    std::vector<Compiled> jobs;
    std::atomic<float> progress { 0.0f };

    void run() override;
    juce::Result play (Compiled&, int index);

    JUCE_DECLARE_NON_COPYABLE (OfflineRender)
};
} // namespace phazite

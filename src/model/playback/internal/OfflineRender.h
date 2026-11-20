// SPDX-License-Identifier: AGPL-3.0-or-later
// The song, or a part of it, played offline by Phazite's engine on a thread of its own: an export's
// (model/project/Export.h), a freeze's, a bounce's or a consolidation's (model/mixer/FreezeModel.h). Each job plays from the
// song's start (or a time in it) to an end, and on until its tails are over (under -70 dB for as long as the longest tail
// its devices report, a block at least: a delay is silent between its echoes; so many seconds at most, the silence at the
// end left out), into a WAV file of 32-bit floats, measured as it goes (EBU R128). What each plays is compiled on the
// message thread as its turn comes (the first as this is made), from the song as it is then (the windows renders run under
// are modal, so the song doesn't change meanwhile), once the clips' renders it plays are made (stretched, pitched or reversed
// clips', which the engine plays as silence until they are: ClipRenders.h): those not there yet are found on the message
// thread as its turn comes, made on this thread, under its cancel and with a share of its progress, and told on the
// message thread, where the job is compiled then. With the project's plug-ins, which it has alone while it lives
// (PlaybackSession::lendToRender()); deleting it, on the
// message thread, cancels what is left, waits for its thread and gives them back. A plug-in that had crashed before a job
// starts, or whose helper goes as it plays (a crash, or ended for a hang: SandboxedProcessor.h), fails the job, which says
// so, naming it: nothing is written without it (one switched off isn't heard, and fails nothing). One that keeps a block
// is waited for as long as it takes (it may only be slow: known bug 42), and named, once that is as long as a hang takes
// live, for the render's window (findWait()), whose Cancel the user then has. Its
// plug-ins are told that they play offline while it renders them (each block of a render says so, and the helper sets
// them so: host/core/Instance.h), and live again as it gives them back, finished or cancelled
// (SandboxedInstance::playLive()). Cancelling doesn't wait for a plug-in either. Between two parts of a job, the sound files
// its plan plays let go of what they read before the part just played and not in it, so that a render holds what it plays
// now, not all it has played: those longer than a short file whose format reads a place again the same, to the last bit
// (WAV, AIFF, FLAC); others are kept whole (engine/audio/AudioFile.h: letGoOfUnread()).
#pragma once

#include "SongPlayer.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/mixer/FreezeModel.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class PlaybackSession;
class SandboxedProcessor;

class OfflineRender final : private juce::Thread
{
public:
    struct Job
    {
        juce::File file {};                        // where its floats go
        double endSeconds = 0.0;                   // the song's time it plays to, before its tails
        double tailSeconds = 0.0;                  // and on, for as long as its tails sound, at most
        std::optional<SongPlayer::RenderOf> of {}; // the song through the master, or a part of it
        double startSeconds = 0.0;                 // the song's time it plays from
    };

    // On the render thread: what comes of a job's file once it is written, with what was measured of it (an export writes
    // its own format from it), or why nothing does; and, once every job has (or one has failed), the result. Never once
    // cancelled.
    using Rendered = std::function<juce::Result (OfflineRender&, int job, const engine::LoudnessMeter::Readings&)>;
    using Finished = std::function<void (const juce::Result&)>;

    // How it renders: the session of the project it renders (findSession(), or none), which lends it the plug-ins; what
    // makes a player of the song (a SongPlayer of the project's song), one for each job; the song whose clips' renders each
    // job has made first, where they aren't there yet (the song the players play; none for none); and on so many threads,
    // at a rate, in blocks of so many samples, the engine's share of each job's progress (the rest is Rendered's); in the
    // background, its thread at a low priority, beside the song that plays (an old version's bounce: model/versions/).
    struct Setup
    {
        PlaybackSession* session = nullptr;
        std::function<std::unique_ptr<SongPlayer>()> makePlayer;
        const Song* clipsOf = nullptr;
        int numThreads = 1;
        double sampleRate = 48000.0;
        int blockSize = 512;
        double playedShare = 1.0;
        bool background = false;
    };

    OfflineRender (Setup, const std::vector<Job>&, Rendered, Finished);
    ~OfflineRender() override;

    float getProgress() const noexcept { return progress.load (std::memory_order_relaxed); }

    // For what Rendered does with a job, on the render thread: whether it is cancelled, and how far it is through its own
    // part of the job, from 0 to 1.
    bool isCancelled() const noexcept { return threadShouldExit(); }
    void setProgress (int job, double ofJob) noexcept;

    // The plug-in it has waited for the longest, for a block or a channel for its rate, if that is longer than a hang takes
    // live (its helper's audio hang limit), and for how long (message thread): what its window says it waits for.
    using Wait = RenderWait;
    std::optional<Wait> findWait() const;

private:
    // A job, compiled as its turn comes, on the message thread (known bug 125: an export of stems compiled a player and a
    // plan for each stem before the first started, and held them all): the first as this is made, each other as the one
    // before it has played, which this thread waits for; and let go of as the next is compiled.
    struct Compiled
    {
        Job job;
        std::vector<ClipRenderToMake> toMake {}; // the clips' renders it needs made first, found as its turn came
        juce::Array<juce::File> made {};         // those of them this thread made, to be told on the message thread
        std::unique_ptr<SongPlayer> player {};   // what its plan points at, kept to be let go of on the message thread
        std::unique_ptr<engine::Plan> plan {};
        std::vector<SandboxedProcessor*> plugins {};     // the plug-ins its plan plays, which its player keeps
        std::vector<const SandboxedProcessor*> heard {}; // those of them switched on as it was compiled
        double pluginTailSeconds = 0.0;                  // the longest tail its plug-ins report, asked as it was compiled
    };

    const Setup setup;
    const Rendered rendered;
    const Finished finished;
    std::vector<Compiled> jobs; // made whole as this is, each compiled in its turn
    std::atomic<float> progress { 0.0f };
    double playedFrom = 0.0; // this thread's: the share of the job under way its clips' renders took, which its playing follows
    juce::String whyNot;     // a plug-in of the session's still loading as it began, which fails it at its start

    // What plays the plug-ins the jobs played, kept until this goes, which tells them that they play live again; what a
    // turn asked of the message thread finds (nothing once this has gone); and what wakes this thread once it is taken.
    std::vector<std::shared_ptr<engine::DeviceNode>> playedPlugins; // one for each plug-in's instance
    const std::shared_ptr<OfflineRender*> alive;
    juce::WaitableEvent turnTaken;

    // A job's turn, on the message thread: the clips' renders it needs found, and it compiled if it needs none; or, once this
    // thread has made them, they told and it compiled.
    void takeTurn (size_t index, bool rendersMade);
    void compile (size_t index);                        // message thread
    bool waitForTurn (size_t index, bool rendersMade);  // this thread: whether the render goes on
    void makeRendersOf (size_t index);                  // this thread
    void showProgress (int job, double share) noexcept; // this thread: so much of a job's whole share done
    void run() override;
    juce::Result play (Compiled&, int index);

    JUCE_DECLARE_NON_COPYABLE (OfflineRender)
};
} // namespace phazite

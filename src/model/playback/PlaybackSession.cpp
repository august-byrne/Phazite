// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackSession.h"
#include "internal/PlaybackHost.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Take.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Every session, for findSession(). Message thread only.
    std::vector<PlaybackSession*>& getSessions()
    {
        static std::vector<PlaybackSession*> sessions;
        return sessions;
    }
} // namespace

PlaybackSession::PlaybackSession (Song& s, PlaybackHost& h)
    : song (&s),
      host (h),
      songState (s.getState()),
      plugins (s, h.getPluginHost(), h.getEngine().getSampleRate()),
      player (s, &plugins),
      writer (s, *this)
{
    recorder = std::make_unique<AudioRecorder> (*this);
    plugins.setListener (this);
    player.readLive (host.getFileReader(), host.getEngine().getBlocksPlayed());
    song->addParameterListener (this);
    songState.addListener (this);
    getSessions().push_back (this);
    host.add (*this);

    // The engine plays this song from now on: stopped, from where its playback starts.
    host.getEngine().stop();
    host.getEngine().locate (getPlayStartBeat (s).inBeats());
    update();
    startTimerHz (30);
}

PlaybackSession::~PlaybackSession()
{
    stopTimer();
    take = nullptr; // a take not finished keeps nothing
    Gesture::forget (this);
    onProblem = nullptr; // what owns the reporter may be going too
    placeTakesNow();     // ended, and kept (a fall-back: ProjectSession places them before it closes the project)
    cancelPendingUpdate();
    songState.removeListener (this);
    song->removeParameterListener (this);
    plugins.setListener (nullptr);
    recorder = nullptr; // what it opened for the tracks closes

    // Live MIDI goes nowhere until the session the engine plays next (if any) sends it there again (host.remove()).
    if (host.getPlayingSession() == this)
        getAudioIO().stopLiveMidi (host.getEngine());

    player.letGoOfPlugins();
    host.remove (*this);
    std::erase (getSessions(), this);
}

AudioIO& PlaybackSession::getAudioIO() const noexcept
{
    return host.getAudioIO();
}

void PlaybackSession::changed (const juce::ValueTree&)
{
    triggerAsyncUpdate();
}

void PlaybackSession::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // Where playback starts is the transport's (movePlayhead() locates the engine itself): nothing a plan holds.
    if (tree == songState && property == schema::playStart)
        return;

    // A value set by hand plays at once; anything else, in a new plan.
    if (! player.takeValue (tree, property))
        triggerAsyncUpdate();
}

void PlaybackSession::valueSetByHand (const ParameterRef& parameter, float value, bool byPlugin)
{
    // A plug-in's value set in its own window plays at once (a value set by a control is in the song: takeValue()).
    if (byPlugin)
        player.takeValueSetByHand (parameter, value);

    writer.parameterChanged (parameter, value);
}

void PlaybackSession::pluginGestureChanged (const ParameterRef& parameter, bool starting)
{
    writer.pluginGestureChanged (parameter, starting);
}

bool PlaybackSession::isPlayingProject() const
{
    return host.getPlayingSession() == this;
}

engine::Engine& PlaybackSession::getEngine() noexcept
{
    return host.getEngine();
}

bool PlaybackSession::isPlaying() const
{
    return host.getEngine().isPlaying();
}

void PlaybackSession::play (bool countIn)
{
    update(); // what it plays as the song is now, should a change still be on its way
    host.getEngine().play (countIn);
    playStartClock = host.getEngine().getClock();
    lastPlayhead.reset();
    writer.playbackStarted();
}

bool PlaybackSession::startTake()
{
    if (take == nullptr)
    {
        update(); // the song as it is now, whose tempo and delays the take starts with
        take = Take::start (*this);
        update(); // its loop
    }

    return take != nullptr;
}

std::unique_ptr<Take> PlaybackSession::releaseTake()
{
    auto released = std::move (take);
    update(); // the song's loop again
    return released;
}

void PlaybackSession::placeTake (std::unique_ptr<Take> ended)
{
    if (ended == nullptr)
        return;

    takesToPlace.push_back (std::move (ended));
    Gesture::whenEnded (*song, this, [this] { placeTakesNow(); });
}

void PlaybackSession::placeTakesNow()
{
    for (auto& ended : std::exchange (takesToPlace, {}))
    {
        const UndoStep step (*song, "Record");
        ended->place();
    }
}

void PlaybackSession::locateWhileRecording (BeatPosition beat, const std::function<void (Take&)>& finish)
{
    if (take == nullptr)
    {
        locate (beat);
        return;
    }

    // The next take first, whole (its files, its buffers), given to the engine before the jump is asked for, which it waits
    // for: the audio thread may take the jump within a block of being asked, and the take starts where the playhead lands,
    // to the sample, however long the message thread takes meanwhile, this one handing it what the engine records from the
    // jump on (Take.h). It records the inputs this one did: the audio device's inputs stay as they are while a take records
    // (AudioRecorder::update()), so a track armed meanwhile has none open, and waits for Stop as it did before the jump.
    auto next = Take::start (*this, take.get());

    // The jump: the engine ends what this one records as it takes it.
    locate (beat);

    if (onJumpAsked != nullptr)
        onJumpAsked();

    // This one's clips, and the song with them, which the engine loops as the take looped it still (the song's own loop
    // would take a playhead past the song's end back to its start); then it goes, finished or else dropped.
    const auto restarted = take->getDeviceStarts() != host.getDeviceStarts();
    const auto was = take->getSampleRate();
    finish (*take);
    take = std::move (next);
    update();

    // None goes on, the audio device having started again meanwhile at another rate (Take::start() starts none until the
    // song is given for it): recording stopped, as deviceStarted() would have said.
    if (take == nullptr && restarted)
        tellDeviceStarted (was);

    // The next keeps the notes played, once this one has taken those it needed; or nothing records any more: the inputs as
    // the tracks need them now, which the take that ended left to the next.
    if (take != nullptr)
        take->keepPlayed();
    else
        recorder->update();
}

int PlaybackSession::getNumInputs() const noexcept
{
    return host.getNumInputs();
}

int PlaybackSession::getInputLatency() const noexcept
{
    return host.getInputLatency();
}

int PlaybackSession::getOutputLatency() const noexcept
{
    return host.getOutputLatency();
}

const std::deque<engine::Engine::PlayedEvent>& PlaybackSession::getPlayedEvents()
{
    return host.getPlayedEvents();
}

void PlaybackSession::keepPlayedFrom (std::optional<std::int64_t> clock)
{
    host.keepPlayedFrom (clock);
}

void PlaybackSession::stop()
{
    writer.playbackStopped(); // where the playhead is as it stops
    host.getEngine().stop();
}

void PlaybackSession::locate (BeatPosition beat)
{
    writer.playheadJumped (beat); // while the playhead heard is still where the song was
    host.getEngine().locate (beat.inBeats());
    lastPlayhead.reset();
}

BeatPosition PlaybackSession::getPlayhead() const
{
    return BeatPosition::fromBeats (host.getEngine().getPlayheadBeat());
}

double PlaybackSession::getHeardDelay() const
{
    const auto* plan = host.getEngine().getPlan();
    const auto samples = host.getOutputLatency() + (plan != nullptr ? engine::Engine::getOutputLatency (*plan) : 0);
    return samples * 1000.0 / host.getEngine().getSampleRate();
}

BeatPosition PlaybackSession::getHeardPlayhead (double milliseconds) const
{
    return BeatPosition::fromBeats (host.getEngine().getPlayheadBeatAt (milliseconds - getHeardDelay()));
}

void PlaybackSession::deviceStarted()
{
    // A take goes on only on the device as it started: at another rate its sound would play at the wrong speed, and what it
    // has recorded would be placed by the wrong clock; at the same, its buffer and delays may have changed, which would place
    // what comes after wrongly. Ended as Stop ends it, where the device stopped. A start the take began after (one asked for
    // just before Record) leaves it.
    if (take != nullptr && take->getDeviceStarts() != host.getDeviceStarts())
    {
        const auto was = take->getSampleRate();
        stopPlaying (*song);
        tellDeviceStarted (was);
    }

    update();
}

void PlaybackSession::tellDeviceStarted (double recordedAt)
{
    if (onProblem == nullptr)
        return;

    const auto kilohertz = [] (double rate)
    { return juce::String (rate / 1000.0, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz"; };
    const auto now = getEngine().getSampleRate();

    onProblem ("Recording stopped",
               juce::exactlyEqual (recordedAt, now)
                   ? juce::String ("The audio device started again (its buffer or its delays may have changed), so Phazite stopped "
                                   "recording and kept what it had recorded until then.")
                   : "The audio device started again at " + kilohertz (now) + ", not at the " + kilohertz (recordedAt)
                         + " the recording was made at, so Phazite stopped recording and kept what it had recorded until then.");
}

void PlaybackSession::timerCallback()
{
    // A take whose files can't be written any more (a full drive, one that went away) stops, keeping what was written, and
    // says so (Take.h); playback stops with it, where it is.
    if (take != nullptr && take->hasFailedWriting())
    {
        stopPlaying (*song);
        return;
    }

    // What the song's devices let go of on the audio thread (a sound swapped out), freed without waiting for the next given.
    player.releaseUnused();

    // Playing on with nothing looped, and nothing recorded: it stops as the playhead crosses the song's end, and goes back
    // to where playback starts. Playback started past the end plays on, there being nothing to cut short.
    if (! isPlayingProject() || ! isPlaying() || isLoopOn (*song) || take != nullptr)
    {
        lastPlayhead.reset();
        return;
    }

    const auto playhead = getPlayhead();

    if (lastPlayhead.has_value() && *lastPlayhead < songEnd && playhead >= songEnd)
    {
        stop();
        locate (getPlayStartBeat (*song));
        return;
    }

    lastPlayhead = playhead;
}

void PlaybackSession::setLiveTrack (TrackID track)
{
    // A track live MIDI starts playing records it, as Ableton Live arms the selected MIDI track by itself.
    if (track == liveTrack)
    {
        followLiveMidi();
        return;
    }

    liveTrackRecords = true;
    const auto before = liveTrack;
    liveTrack = track;
    followLiveMidi();

    // What was held on the track it played lets go, its pedal too: their ends would go to the new one (once live MIDI goes
    // there, so that nothing played meanwhile is left behind).
    if (before.isValid() && isPlayingProject())
        host.getEngine().endLive ((std::int64_t) before.getRaw());
}

void PlaybackSession::followLiveMidi()
{
    // While the engine plays this project, into its track.
    if (isPlayingProject())
        getAudioIO().setLiveMidi (&host.getEngine(), (std::int64_t) liveTrack.getRaw());
}

void PlaybackSession::playGuideNotes (std::int64_t track, const std::vector<int>& pitches, int velocity)
{
    for (const auto pitch : pitches)
    {
        // Not recordable: Capture and a take leave them out.
        host.getEngine().playLive (track, { devices::NoteEvent::Type::noteOn, 0, pitch, (float) juce::jlimit (1, 127, velocity) / 127.0f },
                                   false);
        guideNotes.emplace_back (track, pitch);
    }
}

void PlaybackSession::stopGuideNotes (std::int64_t track)
{
    for (const auto& [on, pitch] : guideNotes)
        if (on == track)
            host.getEngine().playLive (track, { devices::NoteEvent::Type::noteOff, 0, pitch, 0.0f }, false);

    std::erase_if (guideNotes, [track] (const auto& note) { return note.first == track; });
}

void PlaybackSession::update()
{
    cancelPendingUpdate();
    auto& engine = host.getEngine();
    songEnd = getSongLoopBeats (*song).getEnd();
    plugins.sync();

    if (isPlayingProject() && ! lentToRender)
    {
        SongPlayer::Live live;
        live.lowLatency = host.isLowLatency();

        if (recorder != nullptr)
            for (const auto track : recorder->getArmedTracks())
                live.armed.insert (track);

        plugins.setSampleRate (engine.getSampleRate());
        auto plan = player.compile (engine.getSampleRate(), engine.getMaximumBlockSize(), live);

        // A take loops the range looped on purpose, or nothing (Take.h).
        if (take != nullptr)
        {
            const auto loop = take->getLoop();
            plan->loop = loop.has_value() ? std::optional (engine::Plan::BeatRange { loop->getStart().inBeats(), loop->getEnd().inBeats() })
                                          : std::nullopt;
        }

        engine.setPlan (std::move (plan));
    }

    followLiveMidi();
}

void PlaybackSession::lendToRender (bool lent)
{
    if (std::exchange (lentToRender, lent) == lent)
        return;

    if (lent)
    {
        // Never beside a block still in its plug-ins: one that stalls is waited for as long as it takes.
        if (isPlayingProject())
            host.dropPlan (true);

        return;
    }

    player.makePluginsAnew();
    update();
}

//==============================================================================
PlaybackSession* findSession (const Song& song)
{
    for (auto* session : getSessions())
        if (&session->getSong() == &song)
            return session;

    return nullptr;
}

PlaybackSession* findPlayingSession (const Song& song)
{
    for (auto* session : getSessions())
        if (&session->getSong() == &song && session->isPlayingProject())
            return session;

    return nullptr;
}

juce::AudioBuffer<float> renderWithOwnEngine (Song& song, int numSamples, double sampleRate, int blockSize, int numThreads)
{
    // With the song's plug-ins, which its engine plays none of meanwhile.
    auto* session = findSession (song);

    if (session != nullptr)
        session->lendToRender (true);

    juce::AudioBuffer<float> audio;

    {
        engine::Engine engine;
        engine.setNumThreads (numThreads);
        engine.prepare (sampleRate, blockSize);
        SongPlayer player (song, session != nullptr ? &session->getPlugins() : nullptr);
        engine.setPlan (player.compile (sampleRate, blockSize, std::nullopt));
        engine.locate (0.0);
        audio = engine.render (numSamples);
    }

    if (session != nullptr)
        session->lendToRender (false);

    return audio;
}
} // namespace phazite

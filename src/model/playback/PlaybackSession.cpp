// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackSession.h"
#include "internal/PlaybackHost.h"
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

void PlaybackSession::timerCallback()
{
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
    if (track != liveTrack)
        liveTrackRecords = true;

    liveTrack = track;
    followLiveMidi();
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
        host.getEngine().playLive (track, { devices::NoteEvent::Type::noteOn, 0, pitch, (float) juce::jlimit (1, 127, velocity) / 127.0f });
        guideNotes.emplace_back (track, pitch);
    }
}

void PlaybackSession::stopGuideNotes (std::int64_t track)
{
    for (const auto& [on, pitch] : guideNotes)
        if (on == track)
            host.getEngine().playLive (track, { devices::NoteEvent::Type::noteOff, 0, pitch, 0.0f });

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
        if (isPlayingProject())
            host.dropPlan();

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

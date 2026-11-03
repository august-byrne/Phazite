// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackSession.h"
#include "internal/PlaybackHost.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/LiveInput.h"
#include "model/transport/internal/Take.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Every session, for findPlayingSession(). Message thread only.
    std::vector<PlaybackSession*>& getSessions()
    {
        static std::vector<PlaybackSession*> sessions;
        return sessions;
    }

    // Whether a change is the song's: in the Edit's PHAZITE element, or the element itself.
    bool isInSong (juce::ValueTree tree)
    {
        for (; tree.isValid(); tree = tree.getParent())
            if (tree.hasType (schema::PHAZITE))
                return true;

        return false;
    }
} // namespace

PlaybackSession::PlaybackSession (te::Edit& e, PlaybackHost& h)
    : edit (e),
      exists (&e),
      host (h),
      audio (AudioIO::getFor (e.engine)),
      player (e),
      writer (e, *this),
      editState (e.state)
{
    player.readLive (host.getFileReader(), host.getEngine().getBlocksPlayed());
    player.setPluginGestureListener ([this] (const ParameterRef& parameter, bool starting)
                                     { writer.pluginGestureChanged (parameter, starting); });
    edit.getParameterChangeHandler().addListener (*this);
    editState.addListener (this);
    getSessions().push_back (this);
    host.add (*this);
    update();
}

PlaybackSession::~PlaybackSession()
{
    take = nullptr; // a take not finished keeps nothing
    cancelPendingUpdate();
    editState.removeListener (this);

    // Live MIDI goes nowhere until the session the engine plays next (if any) sends it there again (host.remove()).
    if (host.getPlayingSession() == this)
        audio->stopLiveMidi (host.getEngine());

    // The Edit may have gone first (it is deleted while a context still holds it).
    if (exists.get() != nullptr)
    {
        holdTracktion (false);
        edit.getParameterChangeHandler().removeListener (*this);
        player.letGoOfPlugins();
    }

    host.remove (*this);
    std::erase (getSessions(), this);
}

void PlaybackSession::changed (const juce::ValueTree& tree, const juce::ValueTree& child)
{
    if (isInSong (tree) || child.hasType (schema::PHAZITE))
        triggerAsyncUpdate();
}

void PlaybackSession::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // A value set by hand plays at once; anything else, in a new plan.
    if (isInSong (tree) && ! player.takeValue (tree, property))
        triggerAsyncUpdate();
}

void PlaybackSession::pluginParameterChanged (te::AutomatableParameter& parameter, bool isFollowingAutomation)
{
    // A plug-in's value set by hand (as Tracktion keeps it) plays at once too.
    if (! isFollowingAutomation)
        player.takeValueSetByHand (getParameterRef (parameter), parameter.getCurrentExplicitValue());

    writer.parameterChanged (parameter, isFollowingAutomation);
}

bool PlaybackSession::isPlayingProject() const
{
    return exists.get() != nullptr && host.getPlayingSession() == this;
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
    writer.playbackStarted();
}

bool PlaybackSession::startTake()
{
    if (take == nullptr && exists.get() != nullptr)
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
}

BeatPosition PlaybackSession::getPlayhead() const
{
    return BeatPosition::fromBeats (host.getEngine().getPlayheadBeat());
}

void PlaybackSession::setLiveTrack (TrackID track)
{
    liveTrack = track;
    followLiveMidi();
}

void PlaybackSession::followLiveMidi()
{
    if (exists.get() == nullptr)
        return;

    // While the engine plays this project, into its track; Tracktion's graph held silent meanwhile.
    const auto playing = isPlayingProject();

    if (playing)
        audio->setLiveMidi (&host.getEngine(), (std::int64_t) liveTrack.getRaw());

    holdTracktion (playing);
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

    if (isPlayingProject() && ! lentToRender)
    {
        auto plan = player.compile (engine.getSampleRate(), engine.getMaximumBlockSize(), false);

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
    if (std::exchange (lentToRender, lent) == lent || exists.get() == nullptr)
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

void PlaybackSession::holdTracktion (bool hold)
{
    // A node that plays silence, holding Tracktion's graph without preparing it (so its plug-ins aren't prepared or
    // played by it), until Tracktion builds another.
    struct Held final : te::graph::Node
    {
        explicit Held (std::unique_ptr<te::graph::Node> graph)
            : held (std::move (graph))
        {
        }

        te::graph::NodeProperties getNodeProperties() override
        {
            te::graph::NodeProperties properties;
            properties.hasAudio = true;
            properties.hasMidi = true;
            properties.numberOfChannels = 2;
            properties.nodeID = (size_t) reinterpret_cast<std::uintptr_t> (this);
            return properties;
        }

        bool isReadyToProcess() override { return true; }

        void process (ProcessContext& context) override
        {
            context.buffers.audio.clear();
            context.buffers.midi.clear();
        }

        std::unique_ptr<te::graph::Node> held;
    };

    auto* context = exists.get() != nullptr ? edit.getTransport().getCurrentPlaybackContext() : nullptr;

    if (context == nullptr || (context == heldContext) == hold)
        return;

    if (hold)
        context->setInsertOptionalLastStageNodeForDeviceCallback (
            [] (te::OutputDevice&, const te::CreateNodeParams&, std::unique_ptr<te::graph::Node> graph) -> std::unique_ptr<te::graph::Node>
            { return std::make_unique<Held> (std::move (graph)); });
    else
        context->setInsertOptionalLastStageNodeForDeviceCallback ({});

    heldContext = hold ? context : nullptr;
    edit.restartPlayback();
}

//==============================================================================
PlaybackSession* findSession (const te::Edit& edit)
{
    for (auto* session : getSessions())
        if (session->hasEdit() && &session->getEdit() == &edit)
            return session;

    return nullptr;
}

PlaybackSession* findPlayingSession (const te::Edit& edit)
{
    for (auto* session : getSessions())
        if (session->hasEdit() && &session->getEdit() == &edit && session->isPlayingProject())
            return session;

    return nullptr;
}

std::optional<float> getPlayedValue (const te::AutomatableParameter& parameter, bool withModulation)
{
    const auto* session = findPlayingSession (parameter.getEdit());
    const auto ref = getParameterRef (parameter);

    if (session != nullptr && session->getPlayer().isMoved (ref))
        return session->getPlayer().getPlayedValue (ref, withModulation);

    return {};
}

juce::AudioBuffer<float> renderWithOwnEngine (te::Edit& edit, int numSamples, double sampleRate, int blockSize, int numThreads)
{
    // With the project's plug-ins, which its engine plays none of meanwhile.
    auto* session = findSession (edit);

    if (session != nullptr)
        session->lendToRender (true);

    juce::AudioBuffer<float> audio;

    {
        engine::Engine engine;
        engine.setNumThreads (numThreads);
        engine.prepare (sampleRate, blockSize);
        SongPlayer player (edit);
        engine.setPlan (player.compile (sampleRate, blockSize, true));
        engine.locate (0.0);
        audio = engine.render (numSamples);
    }

    if (session != nullptr)
        session->lendToRender (false);

    return audio;
}
} // namespace phazite

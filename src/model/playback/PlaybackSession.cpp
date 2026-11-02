// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackSession.h"
#include "internal/PlaybackHost.h"
#include "model/base/internal/Schema.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
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
      host (h),
      editState (e.state)
{
    editState.addListener (this);
    getSessions().push_back (this);
    host.add (*this);
    update();
}

PlaybackSession::~PlaybackSession()
{
    cancelPendingUpdate();
    editState.removeListener (this);
    host.remove (*this);
    std::erase (getSessions(), this);
}

void PlaybackSession::changed (const juce::ValueTree& tree, const juce::ValueTree& child)
{
    if (isInSong (tree) || child.hasType (schema::PHAZITE))
        triggerAsyncUpdate();
}

bool PlaybackSession::isPlayingProject() const
{
    return host.isOn() && host.getPlayingSession() == this && ! edit.getTransport().isRecording();
}

bool PlaybackSession::isPlaying() const
{
    return host.getEngine().isPlaying();
}

void PlaybackSession::play()
{
    update(); // what it plays as the song is now, should a change still be on its way
    host.getEngine().play();
}

void PlaybackSession::stop()
{
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

void PlaybackSession::update()
{
    cancelPendingUpdate();

    if (host.isOn() && host.getPlayingSession() == this)
        host.getEngine().setPlan (compilePlan (edit, { .sampleRate = host.getEngine().getSampleRate() }));
}

//==============================================================================
std::unique_ptr<engine::Plan> compilePlan (const te::Edit& edit, PlanOptions options)
{
    std::vector<engine::TempoMap::Tempo> tempos;
    std::vector<engine::TempoMap::TimeSignature> signatures;

    for (const auto& change : getTempoChanges (edit))
        tempos.push_back ({ change.beat.inBeats(), change.bpm });

    for (const auto& change : getTimeSignatureChanges (edit))
        signatures.push_back ({ change.beat.inBeats(), change.signature.numerator, change.signature.denominator });

    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap (std::move (tempos), std::move (signatures), options.sampleRate);

    // A render plays the song once, from its start, without the metronome.
    if (! options.isRender && isLoopOn (edit))
    {
        const auto loop = getLoopBeats (edit);
        plan->loop = engine::Plan::BeatRange { loop.getStart().inBeats(), loop.getEnd().inBeats() };
    }

    plan->metronome = ! options.isRender && isMetronomeOn (edit);
    return plan;
}

PlaybackSession* findPlayingSession (const te::Edit& edit)
{
    for (auto* session : getSessions())
        if (&session->getEdit() == &edit && session->isPlayingProject())
            return session;

    return nullptr;
}
} // namespace phazite

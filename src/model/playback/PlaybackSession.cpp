// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackSession.h"
#include "internal/PlaybackHost.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/LiveInput.h"

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
      exists (&e),
      host (h),
      player (e),
      editState (e.state)
{
    liveMidi.playing = &host.getEngine();
    editState.addListener (this);
    getSessions().push_back (this);
    host.add (*this);
    update();
}

PlaybackSession::~PlaybackSession()
{
    cancelPendingUpdate();
    editState.removeListener (this);

    // The Edit may have gone first (it is deleted while a context still holds it), and its inputs with it.
    if (exists.get() != nullptr)
        if (auto* instance = findLiveMidiInput (edit))
            instance->removeConsumer (&liveMidi);

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

bool PlaybackSession::isPlayingProject() const
{
    return exists.get() != nullptr && host.isOn() && host.getPlayingSession() == this && ! edit.getTransport().isRecording();
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

void PlaybackSession::LiveMidi::handleIncomingMidiMessage (const juce::MidiMessage& message, te::MPESourceID)
{
    // A MIDI input's thread, or the message thread for the computer keyboard: only what is lock-free.
    if (const auto event = engine::toNoteEvent (message, 0); event.has_value() && playing != nullptr)
        playing->playLive (track.load (std::memory_order_relaxed), *event);
}

void PlaybackSession::followLiveMidi()
{
    if (exists.get() == nullptr)
        return;

    const auto playing = isPlayingProject();
    auto* track = getLiveMidiTrack (edit);
    liveMidi.track.store (track != nullptr ? (juce::int64) track->itemID.getRawID() : 0, std::memory_order_relaxed);

    if (auto* instance = findLiveMidiInput (edit))
    {
        if (playing)
            instance->addConsumer (&liveMidi);
        else
            instance->removeConsumer (&liveMidi);
    }

    // Tracktion plays live MIDI only while Phazite's engine doesn't (the project it plays decides).
    if (const auto input = getAllMidiInputs (edit.engine); input != nullptr && host.getPlayingSession() == this)
    {
        const auto mode = playing ? te::InputDevice::MonitorMode::off : te::InputDevice::MonitorMode::automatic;

        if (input->getMonitorMode() != mode)
            input->setMonitorMode (mode);
    }
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

    if (exists.get() != nullptr && host.isOn() && host.getPlayingSession() == this)
        engine.setPlan (player.compile (engine.getSampleRate(), engine.getMaximumBlockSize(), false));

    followLiveMidi();
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

juce::AudioBuffer<float> renderWithOwnEngine (te::Edit& edit, int numSamples, double sampleRate, int blockSize)
{
    engine::Engine engine;
    engine.prepare (sampleRate, blockSize);
    SongPlayer player (edit);
    engine.setPlan (player.compile (sampleRate, blockSize, true));
    engine.locate (0.0);
    return engine.render (numSamples);
}
} // namespace phazite

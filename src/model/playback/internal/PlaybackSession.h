// SPDX-License-Identifier: AGPL-3.0-or-later
// A project as Phazite's own engine plays it (docs/tracktion-migration.md, T2): its song compiled into what the engine plays
// (SongPlayer.h), given again as the song changes, at most once a turn of the message loop, a value set by hand at once;
// the transport the model's functions use while Phazite's engine plays the project (model/transport/internal/Playback.h),
// in the song's beats; the notes played live into it; the automation written by hand while it plays (AutomationWriter.h),
// which gets the parameters' changes and a plug-in's window's gestures from it; and the take it records
// (model/transport/internal/Take.h).
// ProjectContext makes one for each Edit it holds, which joins the app's PlaybackHost.
#pragma once

#include "AutomationWriter.h"
#include "SongPlayer.h"
#include "engine/core/Engine.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/internal/Tracktion.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
class AudioIO;
class PlaybackHost;
class Take;

class PlaybackSession : private juce::ValueTree::Listener,
                        private te::ParameterChangeHandler::Listener,
                        private juce::AsyncUpdater
{
public:
    PlaybackSession (te::Edit&, PlaybackHost&);
    ~PlaybackSession() override;

    te::Edit& getEdit() const noexcept { return edit; }
    bool hasEdit() const noexcept { return exists.get() != nullptr; } // false once the Edit has gone
    SongPlayer& getPlayer() noexcept { return player; }
    const SongPlayer& getPlayer() const noexcept { return player; }
    AutomationWriter& getWriter() noexcept { return writer; }
    engine::Engine& getEngine() noexcept; // the host's, which plays it

    // Whether Phazite's engine plays this project now: its Edit is there, and it is the project the host plays.
    bool isPlayingProject() const;

    // The transport, through the host's engine. The playhead is in the song's beats.
    bool isPlaying() const;
    void play (bool countIn = false); // a bar of clicks first, from a stop (Engine.h)
    void stop();
    void locate (BeatPosition);
    BeatPosition getPlayhead() const;

    // The engine's clock as playback last started: what Capture keeps of what was played while the song plays.
    std::int64_t getPlayStartClock() const noexcept { return playStartClock; }

    // Recording through Phazite's engine: the take it keeps while it records (model/transport/internal/Take.h), whose loop
    // its engine plays; started, if anything would record; and let go of, to be finished or dropped.
    bool isRecording() const noexcept { return take != nullptr; }
    bool startTake();
    std::unique_ptr<Take> releaseTake();

    // The audio device's inputs and their delays, as the host has them (PlaybackHost.h); and the notes played live in the
    // last 30 seconds, and from a clock on while a take records.
    int getNumInputs() const noexcept;
    int getInputLatency() const noexcept;
    int getOutputLatency() const noexcept;
    const std::deque<engine::Engine::PlayedEvent>& getPlayedEvents();
    void keepPlayedFrom (std::optional<std::int64_t> clock);

    // Live MIDI (the computer keyboard and the MIDI inputs, model/engine/internal/AudioIO.h) plays the track it goes to
    // (model/engine/internal/LiveInput.h), none at first, through Phazite's engine while the engine plays the project, in
    // its next block. After that track or the project the engine plays changes, followLiveMidi() sends it there again.
    TrackID getLiveTrack() const noexcept { return liveTrack; }
    void setLiveTrack (TrackID);
    void followLiveMidi();

    // While Phazite's engine plays the project, Tracktion's playback graph plays nothing: it is held, never prepared, in a
    // silent node, so that a plug-in, which both reach, only ever plays through Phazite's engine. followLiveMidi() holds it
    // or lets it go as the project the engine plays changes.
    void holdTracktion (bool);

    // The piano roll's guide notes into a track, through Phazite's engine; and their ends.
    void playGuideNotes (std::int64_t track, const std::vector<int>& pitches, int velocity);
    void stopGuideNotes (std::int64_t track);

    // Gives the engine the song as it is now, if this is the project it plays.
    void update();

private:
    te::Edit& edit;
    te::SafeSelectable<te::Edit> exists; // until the Edit goes, which can be before this does
    PlaybackHost& host;
    std::shared_ptr<AudioIO> audio; // where live MIDI comes from
    SongPlayer player;
    AutomationWriter writer;
    juce::ValueTree editState; // the song is its PHAZITE element, which a bare Edit gets when something is first written

    TrackID liveTrack;
    std::vector<std::pair<std::int64_t, int>> guideNotes; // each track's sounding

    te::EditPlaybackContext* heldContext = nullptr; // the context whose graph is held, or none
    std::unique_ptr<Take> take;
    std::int64_t playStartClock = 0;

    void changed (const juce::ValueTree& tree, const juce::ValueTree& child = {});
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { changed (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { changed (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { changed (parent); }
    void handleAsyncUpdate() override { update(); }

    // A parameter changed in Tracktion's Edit (by hand, by a plug-in itself or by its automation): for what plays a
    // plug-in, and for the automation written by hand.
    void pluginParameterChanged (te::AutomatableParameter&, bool isFollowingAutomation) override;

    JUCE_DECLARE_NON_COPYABLE (PlaybackSession)
};

// The session of an Edit (one a ProjectContext holds), or nullptr; and the one through which Phazite's engine plays it now
// (isPlayingProject()), or nullptr while Tracktion's does.
PlaybackSession* findSession (const te::Edit&);
PlaybackSession* findPlayingSession (const te::Edit&);

// A parameter's value as Phazite's engine played it last, while the engine plays the parameter's project and its automation
// or modulation moves it (SongPlayer::getPlayedValue()), with its modulation or without; nothing otherwise, while its value
// is the one set by hand.
std::optional<float> getPlayedValue (const te::AutomatableParameter&, bool withModulation);

// The song rendered by Phazite's own engine, offline, from its start: so many samples at a rate, in blocks of a size, in
// stereo, through the master's devices, as an export renders it (T5) and the golden renders' harness compares it; on so
// many threads, the same to the last bit however many.
juce::AudioBuffer<float> renderWithOwnEngine (te::Edit&, int numSamples, double sampleRate, int blockSize, int numThreads = 1);
} // namespace phazite

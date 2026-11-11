// SPDX-License-Identifier: AGPL-3.0-or-later
// A project as Phazite's engine plays it: its song compiled into what the engine plays (SongPlayer.h), given again as the
// song changes, at most once a turn of the message loop, a value set by hand at once; the plug-ins it holds, each an
// instance in its helper (model/plugins/internal/PluginInstances.h); the transport, in the song's beats, which stops at the
// song's end while nothing is looped; the notes played live into it; the automation written by hand while it plays
// (AutomationWriter.h); and the tracks armed to record their inputs (model/transport/internal/AudioRecorder.h), with the
// take it records (model/transport/internal/Take.h). ProjectContext makes one for the song it holds, which joins the app's
// PlaybackHost.
#pragma once

#include "AutomationWriter.h"
#include "SongPlayer.h"
#include "engine/core/Engine.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/plugins/internal/PluginInstances.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
class AudioIO;
class AudioRecorder;
class PlaybackHost;
class Take;

class PlaybackSession : private juce::ValueTree::Listener,
                        private Song::ParameterListener,
                        private PluginInstances::Listener,
                        private juce::AsyncUpdater,
                        private juce::Timer // which stops playback at the song's end
{
public:
    PlaybackSession (Song&, PlaybackHost&);
    ~PlaybackSession() override;

    Song& getSong() const noexcept { return *song; }
    SongPlayer& getPlayer() noexcept { return player; }
    const SongPlayer& getPlayer() const noexcept { return player; }
    AutomationWriter& getWriter() noexcept { return writer; }
    PluginInstances& getPlugins() noexcept { return plugins; }
    AudioRecorder& getRecorder() noexcept { return *recorder; }
    const AudioRecorder& getRecorder() const noexcept { return *recorder; }
    PlaybackHost& getHost() const noexcept { return host; }
    AudioIO& getAudioIO() const noexcept;
    engine::Engine& getEngine() noexcept; // the host's, which plays it

    // Whether Phazite's engine plays this project now: it is the project the host plays.
    bool isPlayingProject() const;

    // The transport, through the host's engine. The playhead is in the song's beats. Playing on past the song's end with
    // nothing looped, it stops and goes back to where it started (as Stop does), unless it started past the end.
    bool isPlaying() const;
    void play (bool countIn = false); // a bar of clicks first, from a stop (Engine.h)
    void stop();
    void locate (BeatPosition);
    BeatPosition getPlayhead() const;

    // What is heard at a moment left the engine this many milliseconds before: the delay compensation that lines up its
    // devices, and the audio output's latency as the device reports it (a Bluetooth output's radio adds more, which Windows
    // doesn't report). The playhead heard at a moment (milliseconds by juce::Time::getMillisecondCounterHiRes()) is where
    // the engine was that much earlier: the window's frames show it while the song plays (docs/performance-audit.md,
    // section 16).
    double getHeardDelay() const;
    BeatPosition getHeardPlayhead (double milliseconds) const;

    // The engine's clock as playback last started: what Capture keeps of what was played while the song plays.
    std::int64_t getPlayStartClock() const noexcept { return playStartClock; }

    // Recording through Phazite's engine: the take it keeps while it records (model/transport/internal/Take.h), whose loop
    // its engine plays; started, if anything would record; and let go of, to be finished or dropped.
    bool isRecording() const noexcept { return take != nullptr; }
    bool startTake();
    std::unique_ptr<Take> releaseTake();
    Take* getTake() noexcept { return take.get(); } // while it records, or nullptr

    // What tells the user of a problem with what it recorded that the model can't show itself, a title and a message (a take
    // kept in time with silence where its sound was lost: Take::finish()). The project context that makes the session sets
    // it (model/project/internal/ProjectContext.h: setProblemReporter()); unset, nothing is said.
    std::function<void (const juce::String& title, const juce::String& message)> onProblem;

    // The audio device's inputs and their delays, as the host has them (PlaybackHost.h); and the notes played live in the
    // last 30 seconds, and from a clock on while a take records.
    int getNumInputs() const noexcept;
    int getInputLatency() const noexcept;
    int getOutputLatency() const noexcept;
    const std::deque<engine::Engine::PlayedEvent>& getPlayedEvents();
    void keepPlayedFrom (std::optional<std::int64_t> clock);

    // Live MIDI (the computer keyboard and the MIDI inputs, model/engine/internal/AudioIO.h) plays the track it goes to
    // (model/engine/internal/LiveInput.h), none at first, while the engine plays the project, in its next block. After that
    // track or the project the engine plays changes, followLiveMidi() sends it there again.
    TrackID getLiveTrack() const noexcept { return liveTrack; }
    void setLiveTrack (TrackID);
    void followLiveMidi();

    // Whether Record records what is played on MIDI onto that track (model/transport/RecordingModel.h: its arm switch): as
    // live MIDI starts playing a track, until its arm switch turns it off, as Ableton Live arms the selected MIDI track by
    // itself. Not saved.
    bool recordsLiveTrack() const noexcept { return liveTrackRecords; }
    void setRecordsLiveTrack (bool records) noexcept { liveTrackRecords = records; }

    // The piano roll's guide notes into a track; and their ends.
    void playGuideNotes (std::int64_t track, const std::vector<int>& pitches, int velocity);
    void stopGuideNotes (std::int64_t track);

    // Gives the engine the song as it is now, if this is the project it plays (and none while a render has its plug-ins).
    void update();

    // While a render plays the project's plug-ins (an export, model/project/Export.h), the engine plays nothing of the
    // project, so that the render has them alone; once it is done, what plays them is made anew, prepared again for the
    // device, and the engine plays the project again. Message thread.
    void lendToRender (bool);
    bool isLentToRender() const noexcept { return lentToRender; }

private:
    Song::Ptr song;
    PlaybackHost& host;
    juce::ValueTree songState;
    bool lentToRender = false;
    PluginInstances plugins;
    std::unique_ptr<AudioRecorder> recorder;
    SongPlayer player;
    AutomationWriter writer;

    TrackID liveTrack;
    bool liveTrackRecords = true;
    std::vector<std::pair<std::int64_t, int>> guideNotes; // each track's sounding

    std::unique_ptr<Take> take;
    std::int64_t playStartClock = 0;

    BeatPosition songEnd;                     // where the song's loop ends, which playing on past stops at
    std::optional<BeatPosition> lastPlayhead; // while playing without a loop: where the playhead was last time
    void timerCallback() override;

    void changed (const juce::ValueTree& tree);
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override { changed (parent); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override { changed (parent); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { changed (parent); }
    void handleAsyncUpdate() override { update(); }

    // A value set by hand (a plug-in's, by a control or in its window, plays at once); a plug-in's own window's gestures;
    // and its latency, which needs a new plan.
    void valueSetByHand (const ParameterRef&, float value, bool byPlugin) override;
    void pluginGestureChanged (const ParameterRef&, bool starting) override;
    void pluginLatencyChanged() override { triggerAsyncUpdate(); }

    JUCE_DECLARE_NON_COPYABLE (PlaybackSession)
};

// The session of a song (one a ProjectContext holds), or nullptr; and the one through which Phazite's engine plays it now
// (isPlayingProject()), or nullptr.
PlaybackSession* findSession (const Song&);
PlaybackSession* findPlayingSession (const Song&);

// The song rendered by Phazite's engine, offline, from its start: so many samples at a rate, in blocks of a size, in
// stereo, through the master's devices and its plug-ins, which the song's session lends it (lendToRender()), as an export
// renders it (OfflineRender.h) and the golden renders' harness compares it; on so many threads, the same to the last bit
// however many. Message thread.
juce::AudioBuffer<float> renderWithOwnEngine (Song&, int numSamples, double sampleRate, int blockSize, int numThreads = 1);
} // namespace phazite

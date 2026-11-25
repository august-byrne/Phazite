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
#include "model/base/internal/SongValues.h"
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

class PlaybackSession : private Song::ParameterListener,
                        private Song::HistoryListener,
                        private SongValues::Listener,
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
    const AutomationWriter& getWriter() const noexcept { return writer; }
    PluginInstances& getPlugins() noexcept { return plugins; }
    AudioRecorder& getRecorder() noexcept { return *recorder; }
    const AudioRecorder& getRecorder() const noexcept { return *recorder; }
    PlaybackHost& getHost() const noexcept { return host; }
    AudioIO& getAudioIO() const noexcept;
    engine::Engine& getEngine() noexcept; // the host's, which plays it

    // Whether Phazite's engine plays this project now: it is the project the host plays.
    bool isPlayingProject() const;

    // The transport, through the host's engine. The playhead is in the song's beats. Playing on past the song's end with
    // nothing looped, it stops and goes back to where it started (as Stop does), unless it started past the end. After a
    // stop, returnTo() sends the playhead back as locate() does, but what plays stays where the song stopped, so that what
    // rings on rings on as it was (Engine::returnTo()), until the song plays again or a locate() moves the playhead: Stop's
    // way back to where playback started (stopAndReturn(), model/transport/internal/Playback.h) and the song's end's.
    bool isPlaying() const;
    void play (bool countIn = false); // a bar of clicks first, from a stop (Engine.h)
    void stop();
    void locate (BeatPosition);
    void returnTo (BeatPosition);
    BeatPosition getPlayhead() const;

    // What is heard at a moment left the engine this many milliseconds before: the delay compensation that lines up its
    // devices, and the audio output's latency as the device reports it (a Bluetooth output's radio adds more, which Windows
    // doesn't report). The playhead heard at a moment (milliseconds by devices::Clock (JUCE's getMillisecondCounterHiRes() unless a test holds it)) is where
    // the engine was that much earlier: the window's frames show it while the song plays (docs/archive/performance-audit.md,
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

    // A take released and ended (Take::end()), whose clips it makes as a "Record" step of its own: at once, or once the drag
    // under way has ended (committed or taken back by Escape), so that the drag's step doesn't hold it. One still waiting as
    // the session goes is placed then, without a word (onProblem): ProjectSession places them before it closes the project
    // (placeTakesNow()), whose save then holds them.
    void placeTake (std::unique_ptr<Take>);
    void placeTakesNow();

    // The takes whose recordings the playlist shows (Take::getInProgress()): those that wait to be placed, then the one it
    // records, if any.
    std::vector<Take*> getTakesShown();

    // The playhead put elsewhere while it records, as locate() puts it: recording goes on from there, as Ableton Live's does
    // after a jump. Another take starts where the playhead lands, to the sample, if anything would still record: given to
    // the engine before the take ends, so that nothing after the jump is missed while the message thread finishes it
    // (Take::start()). The take ends where the song is (the engine ends what it records at a jump: Engine.h), handed to the
    // function given, which keeps it as a stop would, in an undo step of its own: ended at once, and placed now or once the
    // drag under way has ended (placeTake()). The engine loops what the take looped all the while.
    void locateWhileRecording (BeatPosition, const std::function<void (std::unique_ptr<Take>)>& finish);

    // For tests: called as locateWhileRecording() has asked the engine for the jump, before anything after it, as the audio
    // thread may take the jump then (a test plays blocks).
    std::function<void()> onJumpAsked;

    // What tells the user of a problem with what it recorded that the model can't show itself, a title and a message (a take
    // kept in time with silence where its sound was lost: Take::finish(); a drive with little room as recording starts; a
    // take stopped by a write that failed or by the audio device's new rate). The project context that makes the session
    // sets it (model/project/internal/ProjectContext.h: setProblemReporter()); unset, nothing is said.
    std::function<void (const juce::String& title, const juce::String& message)> onProblem;

    // The audio device started (again), the engine prepared for it (PlaybackHost.h): the song for its rate, in a new plan.
    // A take recorded while it started again (at another rate, or at the same with other delays) ends there, kept as Stop
    // keeps it, and onProblem says why. A take whose write failed ends too, as the session's timer sees it
    // (Take::hasFailedWriting()).
    void deviceStarted();

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

    // MIDI learn (model/midi/internal/MidiLinks.h): while the engine plays this project, what the MIDI inputs send on the
    // controllers of the song's links, and while one of its parameters learns, on any controller that can be linked, comes
    // to onMidiController rather than playing live (model/engine/internal/MidiTap.h), within a turn of the message loop: by
    // its channel (1 to 16), number (0 to 119) and value (0 to 127), of the values of one controller that came together the
    // last alone. The project context that makes the session sets it, and says while a parameter learns.
    void setMidiLearning (bool);
    std::function<void (int channel, int number, int value)> onMidiController;

    // What the MIDI tap diverts brought up to date with the song's links now, where they changed since it was last (and,
    // while the engine plays this project, the tap taken for it). The session follows a change of the links by itself at the
    // next turn of the message loop (midiLinksChanged(), which only marks: a listener of the song's values reads nothing as it
    // is told), and as it gives the engine the song (update()); what makes a link that must be diverted before it goes on asks
    // at once (MidiLinker::learn(), before learning ends, so that the knob that made the link never reaches the instrument).
    // Message thread, at a moment that is no listener's call: it reads the links' area.
    void followMidiLinks();

    // The piano roll's guide notes into a track, on a MIDI channel (1 to 16), and a preset's tried in the browser; and their
    // ends, each on its channel. Neither Capture nor a take records them (Engine::playLive()'s recordable).
    void playGuideNotes (std::int64_t track, const std::vector<int>& pitches, int velocity, int channel = 1);
    void stopGuideNotes (std::int64_t track);

    // Gives the engine the song as it is now, if this is the project it plays (and none while a render has its plug-ins).
    void update();

    // While a render plays the project's plug-ins (an export, model/project/Export.h), the engine plays nothing of the
    // project, so that the render has them alone; once it is done, what plays them is made anew, prepared again for the
    // device, and the engine plays the project again. Lent, it waits for a block the audio thread is still in, however long
    // it takes (a debugger, a driver that stalls): a render never plays the plug-ins beside it. Message thread.
    void lendToRender (bool);
    bool isLentToRender() const noexcept { return lentToRender; }

private:
    Song::Ptr song;
    PlaybackHost& host;
    bool lentToRender = false;
    bool latencyWaits = false; // a new plan for a device's latency waits for the gesture that drags it (valueTreePropertyChanged())
    // An undo or a redo set a value by hand that plays at once: what writes automation follows it once the move is whole
    // (historyMoved()).
    bool followAfterMove = false;
    PluginInstances plugins;
    std::unique_ptr<AudioRecorder> recorder;
    SongPlayer player;
    AutomationWriter writer;

    TrackID liveTrack;
    bool liveTrackRecords = true;
    bool midiLearning = false, linksToFollow = true; // linksToFollow: the links changed since the tap was told them

    // What follows the links' changes at the next turn of the message loop (followMidiLinks()), apart from the session's own
    // update, which makes a plan, and no plan holds links.
    struct LinksFollower final : juce::AsyncUpdater
    {
        explicit LinksFollower (PlaybackSession& s)
            : session (s)
        {
        }

        ~LinksFollower() override { cancelPendingUpdate(); }

        void handleAsyncUpdate() override { session.followMidiLinks(); }

        PlaybackSession& session;

        JUCE_DECLARE_NON_COPYABLE (LinksFollower)
    };

    LinksFollower linksFollower { *this };
    void takeMidiControllers();
    void letGoOfMidiTap();
    struct GuideNote
    {
        std::int64_t track = 0;
        int pitch = 60, channel = 0; // the engine's channel, from 0
    };

    std::vector<GuideNote> guideNotes; // each track's sounding

    std::unique_ptr<Take> take;
    std::vector<std::unique_ptr<Take>> takesToPlace; // ended during a drag (placeTake())
    void tellDeviceStarted (double recordedAt);      // that recording stopped as the device started again
    std::int64_t playStartClock = 0;

    BeatPosition songEnd;                     // where the song's loop ends, which playing on past stops at
    std::optional<BeatPosition> lastPlayhead; // while playing without a loop: where the playhead was last time
    void timerCallback() override;
    void jump (BeatPosition, bool keepsPlace); // locate() and returnTo()

    void handleAsyncUpdate() override { update(); }

    // A clip's notes changed, or clips, or the tempo map, or curves (SongValues.h): a new plan.
    // What plays it hears which itself.
    void clipNotesChanged (const std::vector<ClipID>&) override { triggerAsyncUpdate(); }
    void clipsChanged (const std::vector<SongValues::ClipChange>&, const std::vector<TrackID>&) override { triggerAsyncUpdate(); }
    void tempoMapChanged() override { triggerAsyncUpdate(); }

    // The chord lane (SongValues.h; a lane placed or taken away too): a
    // new plan, as a change in its element made one, though no plan holds chords (kept so until a piece lowers the counts of
    // plans on purpose, roadmap step S5.20).
    void chordLaneChanged() override { triggerAsyncUpdate(); }

    // The sections (SongValues.h): a new
    // plan, as a change in their element made one, though no plan holds sections, as the chord lane's (kept so until a piece
    // lowers the counts of plans on purpose, roadmap step S5.20).
    void sectionsChanged (const std::vector<SectionID>&) override { triggerAsyncUpdate(); }

    // What the song holds of its own (SongValues.h):
    // a new plan only where what plays changed, the loop's four or the
    // metronome, which a plan holds (SongPlayer.cpp: compile(), getLoop(), isMetronomeOn()). Nothing else of it makes one:
    // the notes, an outline applied, patterns' speed (which a tempo edit reads as it is made, no plan), the numbering, a merge
    // waiting, the key set last, the versions' names, the play start (movePlayhead() locates the engine itself), punch, the
    // input chosen last and the grids.
    void songOutsideChanged (bool plays) override
    {
        if (plays)
            triggerAsyncUpdate();
    }

    void curvesChanged (const std::vector<ParameterRef>&) override { triggerAsyncUpdate(); } // the curves: a new plan, as above

    // The modulators and macros (SongValues.h),
    // by the parts that changed, as the tree's changes of them were taken: one changed in
    // place in its values set by hand, its steps, its switch or a macro's value plays at once, from the value it is told
    // (SongPlayer::takeModulator()), and while the history undoes or redoes, what a pass writing it writes from then follows
    // (known bug 174) once the move is whole (historyMoved()), as for a macro's name, which plays nothing; one added, taken
    // out, come to another place or of another kind, its modulations, its first value set by hand or its last set back to its
    // default (as its VALUES came or went), and modulators in another order: a new plan.
    void modulationChanged (const std::vector<SongValues::ModulationChange>&, const std::vector<ModulatorPlace>& reordered) override;

    // The sends (SongValues.h), by the parts that changed, as the tree's changes of them were
    // taken while it held them: a level changed in place plays at once, from the value it is
    // told (SongPlayer::takeSendLevel()), and while the history undoes or redoes, what a pass writing it writes from then
    // follows once the move is whole (historyMoved()); one added, taken out, come to another track or routed elsewhere,
    // and sends in another order: a new plan.
    void sendsChanged (const std::vector<SongValues::SendChange>&, const std::vector<TrackID>& reordered, const SongSends& now) override;

    // The devices, containers and chains (SongValues.h;
    // the store's: deviceStoreChanged(), below), by the parts that changed, as the tree's changes of them were
    // taken: one changed in place in its values set by hand, its sounds, a container's mix or a chain's volume, pan, crossover
    // or notes plays at once, from the values before and after it is told (SongPlayer::takeDevice()), and while the history
    // undoes or redoes, what a pass writing it writes from then follows once the move is whole (historyMoved()), as for a name
    // alone, which plays nothing; values that make its sound come later or sooner, a new plan, after the gesture that drags
    // them if one does (latencyWaits); one added, taken out or come to another place, its switch (set at once too), sidechain,
    // type or plug-in, a plug-in's mix, a container's switch, split or kind, its first value set by hand or its last set back
    // to its default (as its VALUES came or went), its first sound or its last (as its SOUNDS did), and devices in another
    // order: a new plan.
    void devicesChanged (const std::vector<SongValues::DeviceChange>&, const std::vector<DevicePlace>& reordered,
                         const SongDevices& now) override;

    // The devices' store (SongValues.h), as the tree's changes of it were taken while it held
    // it: a plug-in's value set by hand and a chain's mute and solo play at once (SongPlayer::takeStoreEntry()), and while the
    // history undoes or redoes, what a pass writing them writes from then follows once the move is whole (historyMoved()); a
    // plug-in's first value set by hand or its last set back to its default (as its VALUE came or went): a new plan too. A
    // plug-in's state, program, format ID and version: nothing (its instance takes them: PluginInstances.h).
    void deviceStoreChanged (const std::vector<SongValues::DeviceStoreChange>&, const DeviceStore& now) override;

    // The tracks (SongValues.h), as the
    // tree's changes of them were taken: a volume or a pan changed in place plays at once, from the value it is told
    // (SongPlayer::takeTracks()), and while the history undoes or redoes, what a pass writing it writes from then follows once
    // the move is whole (historyMoved()); anything else of a track (added, taken out, renamed, coloured, a bus switch, a key,
    // an input...), and tracks in another order: a new plan. Their switches: a mute and a solo play at once
    // (SongPlayer::takeTrackSwitches()), followed after an undo or a redo as a volume is; a frozen switch and an automation
    // mode, a new plan. Their view (tracksViewChanged()): nothing, as no plan holds how a track shows.
    void tracksChanged (const std::vector<TrackChange>&, const std::vector<TrackID>& reordered, const SongTracks& now) override;
    void trackSwitchesChanged (const std::vector<TrackSwitchChange>&, const TrackSwitches& now) override;

    // The song's history moved by hand, and the move is whole (Song.h: HistoryListener): what a pass writing automation writes
    // follows the values the move set by hand that play at once (AutomationWriter::followValuesSetByHand()), where the
    // values' listener heard one meanwhile (followAfterMove), for every area alike. Read here, never
    // as they are told: during an undo the rest of its step may not be back yet.
    void historyMoved() override;

    // The MIDI links (SongValues.h):
    // no plan, as no plan holds them, but what the MIDI tap diverts, marked and
    // followed at the next turn of the message loop (followMidiLinks(), which reads them: never as a listener is told).
    void midiLinksChanged() override
    {
        linksToFollow = true;
        linksFollower.triggerAsyncUpdate();
    }

    // A value set by hand (a plug-in's, by a control or in its window, plays at once), and one a program the song set set (a
    // plug-in's, which plays at once too); a plug-in's own window's gestures; and its latency, which needs a new plan.
    void valueSetByHand (const ParameterRef&, float value, bool byPlugin) override;
    void valueSetByProgram (const ParameterRef&, float value) override;
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

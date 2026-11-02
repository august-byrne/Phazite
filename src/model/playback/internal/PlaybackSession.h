// SPDX-License-Identifier: AGPL-3.0-or-later
// A project as Phazite's own engine plays it (docs/tracktion-migration.md, T2): its song compiled into what the engine plays
// (SongPlayer.h), given again as the song changes, at most once a turn of the message loop, a value set by hand at once;
// the transport the model's functions use while Phazite's engine plays the project (model/transport/internal/Playback.h),
// in the song's beats; the notes played live into it; and the automation written by hand while it plays (AutomationWriter.h).
// ProjectContext makes one for each Edit it holds, which joins the app's PlaybackHost.
#pragma once

#include "AutomationWriter.h"
#include "SongPlayer.h"
#include "engine/core/Engine.h"
#include "model/base/Beats.h"
#include "model/base/internal/Tracktion.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
class PlaybackHost;

class PlaybackSession : private juce::ValueTree::Listener,
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

    // Whether Phazite's engine plays this project now: the host is on, this is the project it plays, and Tracktion isn't
    // recording (until T2 step 9 records through Phazite's engine).
    bool isPlayingProject() const;

    // The transport, through the host's engine. The playhead is in the song's beats.
    bool isPlaying() const;
    void play();
    void stop();
    void locate (BeatPosition);
    BeatPosition getPlayhead() const;

    // Live MIDI (the computer keyboard and the MIDI inputs, which Tracktion's "All MIDI Ins" brings together) plays the track
    // it goes to (model/engine/internal/LiveInput.h) through Phazite's engine while the engine plays the project, in its next
    // block, and Tracktion, which still records it, doesn't play it too: after the track it goes to changes, or Tracktion's
    // inputs are made again, this takes them in again.
    void followLiveMidi();

    // The piano roll's guide notes into a track, through Phazite's engine; and their ends.
    void playGuideNotes (std::int64_t track, const std::vector<int>& pitches, int velocity);
    void stopGuideNotes (std::int64_t track);

    // Gives the engine the song as it is now, if this is the project it plays.
    void update();

private:
    te::Edit& edit;
    te::SafeSelectable<te::Edit> exists; // until the Edit goes, which can be before this does
    PlaybackHost& host;
    SongPlayer player;
    AutomationWriter writer;
    juce::ValueTree editState; // the song is its PHAZITE element, which a bare Edit gets when something is first written

    // What takes live MIDI in from Tracktion's input, on its MIDI threads, for the track it goes to.
    struct LiveMidi : te::InputDeviceInstance::Consumer
    {
        engine::Engine* playing = nullptr;
        std::atomic<std::int64_t> track { 0 };
        void handleIncomingMidiMessage (const juce::MidiMessage&, te::MPESourceID) override;
    } liveMidi;

    std::vector<std::pair<std::int64_t, int>> guideNotes; // each track's sounding

    void changed (const juce::ValueTree& tree, const juce::ValueTree& child = {});
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { changed (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { changed (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { changed (parent); }
    void handleAsyncUpdate() override { update(); }

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
// stereo, through the master's devices, as an export renders it (T5) and the golden renders' harness compares it.
juce::AudioBuffer<float> renderWithOwnEngine (te::Edit&, int numSamples, double sampleRate, int blockSize);
} // namespace phazite

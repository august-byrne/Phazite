// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EnginePlayback.h"
#include "HostedDevice.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Selection.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"

namespace
{
// A song open in a project context of its own, whose session keeps where live MIDI goes and plays it through Phazite's
// engine (model/playback/internal/PlaybackSession.h).
struct OpenSong
{
    explicit OpenSong (int numTracks)
        : tracks (phazite::ensureTracks (*song, numTracks))
    {
        setSong (context, song.get());
    }

    ~OpenSong() { setSong (context, nullptr); }

    phazite::Song::Ptr song = phazite::createSong();
    std::vector<phazite::TrackID> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
};

// A note into live MIDI from the thread that calls, as a MIDI input's driver plays one (AudioIO.h): lock-free.
void playLive (const juce::MidiMessage& message)
{
    phazite::getAudioIO (phazite::test::getEngineContext()).playLive (message);
}

using LiveNote = phazite::test::HostedDevice::LiveNote;

// Whether a note's delay from where the engine put it to its sound is the one expected, by the engine's clock: to within
// the two samples a synth's voice may take more or less to cross -60 dBFS.
bool isNear (int samples, int expected)
{
    return samples >= 0 && std::abs (samples - expected) <= 2;
}

void addInstrument (phazite::Song& song, phazite::TrackID track)
{
    phazite::test::setOwnInstrument (song, track, phazite::devices::Synth::typeID);
}

// One of Phazite's own devices after a channel's devices (the master's with getMasterID()), with values set by hand.
phazite::DeviceID addDevice (phazite::Song& song, phazite::TrackID channel, const char* typeID,
                             const std::vector<std::pair<juce::String, float>>& values = {})
{
    const auto id = phazite::test::addOwnDevice (song, channel, typeID, values);
    REQUIRE (id.isValid());
    return id;
}

phazite::PlaybackHost& getPlaybackHost()
{
    return phazite::getPlaybackHost (phazite::test::getEngineContext());
}

// A note played between two blocks, as a MIDI input plays one, after 50 blocks, and let go of before 200 more: where the
// engine put it and when it sounded (HostedDevice::playLiveNote()).
LiveNote playNote (phazite::test::HostedDevice& device)
{
    device.process (50);
    const auto note = device.playLiveNote ([] { playLive (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)); }, 60);
    playLive (juce::MidiMessage::noteOff (1, 60));
    device.process (200);
    return note;
}

// Plays a note until it sounds as wanted, a few times at most, and says how it sounded the last time: the engine is given
// the song again after a change in the message loop's own time, which a sanitizer's build makes slow.
LiveNote playUntil (phazite::test::HostedDevice& device, const std::function<bool (const LiveNote&)>& wanted)
{
    auto note = playNote (device);
    const auto start = juce::Time::getMillisecondCounter();

    while (! wanted (note) && juce::Time::getMillisecondCounter() - start < (juce::uint32) phazite::test::hangTimeout)
    {
        phazite::test::dispatchPendingMessages();
        note = playNote (device);
    }

    return note;
}
} // namespace

TEST_CASE ("Live MIDI goes to the first track with an instrument")
{
    OpenSong open (3);
    auto& song = *open.song;
    const auto& tracks = open.tracks;
    CHECK_FALSE (phazite::getLiveMidiTrack (song).isValid()); // nowhere at first
    addInstrument (song, tracks[1]);

    phazite::routeLiveMidiInput (song);
    CHECK (phazite::getLiveMidiTrack (song) == tracks[1]);

    // Another track can take it over, and it then plays only there.
    REQUIRE (phazite::setLiveMidiTrack (song, tracks[2]));
    CHECK (phazite::getLiveMidiTrack (song) == tracks[2]);

    // A project that already sends live MIDI somewhere keeps that.
    phazite::routeLiveMidiInput (song);
    CHECK (phazite::getLiveMidiTrack (song) == tracks[2]);
}

TEST_CASE ("Without an instrument, live MIDI goes nowhere")
{
    OpenSong open (1);
    phazite::routeLiveMidiInput (*open.song);
    CHECK_FALSE (phazite::getLiveMidiTrack (*open.song).isValid());

    // Nor anywhere in a song that no project context holds, which has no session to keep it.
    auto alone = phazite::createSong();
    addInstrument (*alone, phazite::getTrackIDs (*alone).front());
    phazite::routeLiveMidiInput (*alone);
    CHECK_FALSE (phazite::getLiveMidiTrack (*alone).isValid());
}

TEST_CASE ("Playing and routing live MIDI don't count as unsaved changes")
{
    const auto folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteLiveInputTests", {}, false);
    folder.createDirectory();

    {
        juce::String error;
        auto project = phazite::Project::create (folder, "Live", error);
        REQUIRE (project != nullptr);

        auto& song = project->getSong();
        const auto track = phazite::ensureTracks (song, 1).front();
        addInstrument (song, track);
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());

        // What opening a project and playing it do: where live MIDI goes, and the playhead moved.
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setSong (context, &song);
        phazite::routeLiveMidiInput (song);
        CHECK (phazite::getLiveMidiTrack (song) == track);
        phazite::movePlayhead (song, phazite::BeatPosition::fromBeats (4.0));

        CHECK_FALSE (project->hasUnsavedChanges());
        setSong (context, nullptr);
    }

    folder.deleteRecursively();
}

TEST_CASE ("Live MIDI follows the selected track, if it has an instrument")
{
    OpenSong open (3);
    auto& song = *open.song;
    const auto& tracks = open.tracks;
    addInstrument (song, tracks[0]);
    addInstrument (song, tracks[2]);
    phazite::routeLiveMidiInput (song);
    REQUIRE (phazite::getLiveMidiTrack (song) == tracks[0]);

    phazite::Selection selection;
    selection.setSong (&song);

    selection.selectOnly (tracks[2]);
    phazite::followSelectedTrack (selection);
    CHECK (phazite::getLiveMidiTrack (song) == tracks[2]);

    // A track without an instrument can't play it, so live MIDI stays where it was.
    CHECK_FALSE (phazite::hasInstrument (song, tracks[1]));
    selection.selectOnly (tracks[1]);
    phazite::followSelectedTrack (selection);
    CHECK (phazite::getLiveMidiTrack (song) == tracks[2]);

    selection.deselectAll();
}

TEST_CASE ("The computer keyboard's notes play live into the track live MIDI plays, through Phazite's engine")
{
    OpenSong open (2);
    addInstrument (*open.song, open.tracks[1]);
    phazite::routeLiveMidiInput (*open.song);
    const auto track = phazite::getLiveMidiTrack (open.context);
    REQUIRE (track.isValid());

    phazite::test::EnginePlayback playback;
    auto& host = getPlaybackHost();
    playback.play (0.01);   // what earlier tests left in the engine's queue (the ends of their live notes)
    host.getPlayedEvents(); // none from before
    auto playedSince = [&host, before = host.getPlayedEvents().size()]() mutable
    {
        std::vector<phazite::engine::Engine::PlayedEvent> since;
        const auto& played = host.getPlayedEvents();
        since.assign (played.begin() + (std::ptrdiff_t) std::min (before, played.size()), played.end());
        before = played.size();
        return since;
    };

    playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    playLive (juce::MidiMessage::noteOff (1, 60));
    playback.play (0.05);
    auto played = playedSince();
    REQUIRE (played.size() == 2);
    CHECK (played[0].track == (std::int64_t) track.getRaw());
    CHECK (played[0].event.type == phazite::devices::NoteEvent::Type::noteOn);
    CHECK (played[0].event.number == 60);
    CHECK (played[1].event.type == phazite::devices::NoteEvent::Type::noteOff);

    // Only notes: a controller, even All Notes Off, plays nothing.
    playLive (juce::MidiMessage::allNotesOff (1));
    playLive (juce::MidiMessage::controllerEvent (1, 1, 64));
    playback.play (0.05);
    CHECK (playedSince().empty());
}

TEST_CASE ("A live note sounds at once, and waits only for devices that are switched on and look ahead")
{
    phazite::test::HostedDevice device;

    OpenSong open (2);
    auto& song = *open.song;
    const auto& tracks = open.tracks;
    addInstrument (song, tracks[0]);
    phazite::routeLiveMidiInput (song);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block

    // A note played between two blocks, as a MIDI input plays one, put as far into the next block as it came, and sounding
    // from there at once. Played until it sounds: the engine is given the song in the message loop's own time, which a
    // busy machine or a sanitizer's build makes slow: a note played before it had it went unheard, and the notes after
    // were compared with that.
    const auto first = playUntil (device, [] (const LiveNote& note) { return note.toSound() >= 0; });
    const auto atOnce = first.toSound();
    CHECK (first.isPlacedAsPlayed());
    CHECK (atOnce >= 0);
    CHECK (atOnce < 16);

    // A limiter on another track looks ahead, and delay compensation holds what is played back to match it...
    const auto limiter = addDevice (song, tracks[1], phazite::devices::Limiter::typeID);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block
    phazite::devices::Limiter itsDSP;
    itsDSP.prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);
    phazite::devices::Synth theInstrument;
    theInstrument.prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);

    // The note's own track is already as late as its instrument (the Synth's Quality, a few samples): it is held back by
    // the rest of the limiter's latency.
    const auto latency = itsDSP.getLatencySamples() - theInstrument.getLatencySamples();
    REQUIRE (latency > 0);
    const auto compensated = playUntil (device, [&] (const LiveNote& note) { return isNear (note.toSound(), atOnce + latency); });
    CHECK (compensated.isPlacedAsPlayed());
    CHECK (isNear (compensated.toSound(), atOnce + latency));

    // ...but not once it is switched off.
    phazite::setDeviceOn (song, limiter, false);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block
    const auto off = playUntil (device, [atOnce] (const LiveNote& note) { return isNear (note.toSound(), atOnce); });
    CHECK (off.isPlacedAsPlayed());
    CHECK (isNear (off.toSound(), atOnce));
}

TEST_CASE ("Low latency plays live notes without delay compensation, and back with it when it is off")
{
    phazite::test::HostedDevice device;

    // A limiter, which looks ahead, on another track.
    OpenSong open (2);
    auto& song = *open.song;
    const auto& tracks = open.tracks;
    addInstrument (song, tracks[0]);
    addDevice (song, tracks[1], phazite::devices::Limiter::typeID);
    phazite::routeLiveMidiInput (song);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block

    const auto compensated = playUntil (device, [] (const LiveNote& note) { return note.toSound() > 90; }).toSound();
    CHECK (compensated > 90); // the limiter's look-ahead, about 99 samples

    auto& host = getPlaybackHost();
    host.setLowLatency (true);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block
    CHECK (host.isLowLatency());
    const auto low = playUntil (device, [] (const LiveNote& note) { return note.toSound() >= 0 && note.toSound() < 16; }).toSound();
    CHECK (low >= 0);
    CHECK (low < 16);

    host.setLowLatency (false);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block
    CHECK (isNear (playUntil (device, [compensated] (const LiveNote& note) { return isNear (note.toSound(), compensated); }).toSound(),
                   compensated));
}

TEST_CASE ("Every live note sounds, however long the message thread was busy")
{
    // The blocks are processed while the test's message thread waits, as the app's does through a long repaint: every
    // note played meanwhile still sounds, as soon as the first did.
    phazite::test::HostedDevice device;

    OpenSong open (2);
    auto& song = *open.song;
    addInstrument (song, open.tracks[0]);
    auto lookAhead = 0; // samples: what the master's devices look ahead by delays what is played live as much

    SUBCASE ("Alone in the song")
    {
    }

    // With devices that look ahead on the master, as when notes were once seen to be lost.
    SUBCASE ("With a limiter and a multiband on the master")
    {
        for (const auto* type : { phazite::devices::Limiter::typeID, phazite::devices::Multiband::typeID })
            addDevice (song, phazite::getMasterID (song), type);

        phazite::devices::Limiter limiter;
        phazite::devices::Multiband multiband;

        for (auto* processor : std::initializer_list<phazite::devices::Processor*> { &limiter, &multiband })
        {
            processor->prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);
            lookAhead += processor->getLatencySamples();
        }
    }

    phazite::routeLiveMidiInput (song);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block

    // Once the engine plays the note, in the block after it came, as far into it as it came, it sounds after the master's
    // look-ahead, and every note after it as soon. Each is measured by the engine's clock from where the engine put it,
    // which no load on the machine moves (known bug 178: measured by the wall clock, a note failed beside other builds).
    const auto first = playUntil (device, [] (const LiveNote& note) { return note.toSound() >= 0; });
    REQUIRE (first.toSound() >= 0);
    CHECK (first.isPlacedAsPlayed());
    CHECK (first.toSound() >= lookAhead);
    CHECK (first.toSound() < lookAhead + 16);

    for (int i = 0; i < 12; ++i)
    {
        const auto note = playNote (device);
        INFO ("note " << i << ": taken to have come " << note.cameAt - note.blockStart << " samples into its block (" << note.earliest
                      << " to " << note.latest << " by the wall clock), sounding " << note.toSound()
                      << " samples after the engine played it, the first " << first.toSound());
        CHECK (note.isPlacedAsPlayed());
        CHECK (isNear (note.toSound(), first.toSound()));
    }
}

TEST_CASE ("Low latency leaves what renders play as the song has it")
{
    // A synth's chord into a limiter on the master, which low latency would bypass while playing.
    auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song).front();
    addInstrument (*song, track);
    const auto clip =
        phazite::test::addMidiClip (*song, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) }, "Chord");

    for (int note : { 48, 55, 60, 64 })
        phazite::test::addMidiNote (*song, clip, note, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (3.0), 120);

    addDevice (*song, phazite::getMasterID (*song), phazite::devices::Limiter::typeID, { { "gain", 12.0f } });

    // A second of it, as an export renders it (Phazite's engine, offline).
    const auto render = [&song] { return phazite::renderWithOwnEngine (*song, 48000, 48000.0, 512); };
    auto& host = getPlaybackHost();
    const auto asItIs = render();
    host.setLowLatency (true);
    const auto whileLow = render();
    host.setLowLatency (false);

    REQUIRE (asItIs.getNumSamples() == whileLow.getNumSamples());
    CHECK (asItIs.getMagnitude (0, asItIs.getNumSamples()) > 0.5f);
    CHECK (asItIs.getMagnitude (0, asItIs.getNumSamples()) < 0.95f); // the limiter's ceiling, -1 dB

    auto largest = 0.0f;

    for (int i = 0; i < asItIs.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (asItIs.getSample (0, i) - whileLow.getSample (0, i)));

    CHECK (largest < 1.0e-6f);
}

TEST_CASE ("A note held as live MIDI moves to another track ends on the track it played")
{
    // Hold a key, select another instrument's track, let go: the note's end went to the new track, and the note played on
    // the first for ever (known bug 84). Now the track that was live lets go of every note, and its pedal, as live MIDI
    // moves away.
    OpenSong open (2);
    auto& song = *open.song;

    for (const auto track : open.tracks)
        phazite::test::addTestDevice (song, track, phazite::test::TestTone::typeID);

    REQUIRE (phazite::setLiveMidiTrack (song, open.tracks[0]));
    phazite::test::EnginePlayback playback;
    playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    const auto held = playback.play (0.05);
    REQUIRE (held.getMagnitude (0, held.getNumSamples() / 2, held.getNumSamples() / 2) > 0.1f);

    REQUIRE (phazite::setLiveMidiTrack (song, open.tracks[1]));
    playLive (juce::MidiMessage::noteOff (1, 60));
    const auto after = playback.play (0.05);
    CHECK (after.getMagnitude (0, after.getNumSamples() / 2, after.getNumSamples() / 2) < 1.0e-6f);
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EnginePlayback.h"
#include "HostedDevice.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Selection.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/EngineSetup.h"
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
        : edit (phazite::createSongEdit (phazite::test::getEngine()))
    {
        phazite::ensureTracks (*edit, numTracks);
        setEdit (context, edit.get());
    }

    ~OpenSong() { setEdit (context, nullptr); }

    std::unique_ptr<te::Edit> edit;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
};

// A note into live MIDI from the thread that calls, as a MIDI input's driver plays one (AudioIO.h): lock-free.
void playLive (const juce::MidiMessage& message)
{
    phazite::getAudioIO (phazite::test::getEngineContext()).playLive (message);
}

// Whether a note's delay is the one expected, to within the time the test reads of when it was played.
bool isNear (int samples, int expected)
{
    return std::abs (samples - expected) <= 2;
}

void addInstrument (te::AudioTrack& track)
{
    phazite::insertDevice (track, track.edit.getPluginCache().createNewPlugin (phazite::devices::Synth::typeID, {}));
}

// Plays a note until it sounds as wanted, a few times at most, and says when it sounded the last time: Tracktion makes its
// graph again after a change in its own time, which a sanitizer's build makes slow.
int playUntil (const std::function<int()>& playNote, const std::function<bool (int samples)>& wanted)
{
    auto samples = playNote();

    for (int tries = 0; tries < 20 && ! wanted (samples); ++tries)
    {
        phazite::test::settle (250);
        samples = playNote();
    }

    return samples;
}
} // namespace

TEST_CASE ("Live MIDI goes to the first track with an instrument")
{
    OpenSong song (3);
    auto& edit = song.edit;
    const auto tracks = te::getAudioTracks (*edit);
    CHECK (phazite::getLiveMidiTrack (*edit) == nullptr); // nowhere at first
    addInstrument (*tracks[1]);

    phazite::routeLiveMidiInput (*edit);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[1]);

    // Another track can take it over, and it then plays only there.
    REQUIRE (phazite::setLiveMidiTrack (*tracks[2]));
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);

    // A project that already sends live MIDI somewhere keeps that.
    phazite::routeLiveMidiInput (*edit);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);
}

TEST_CASE ("Without an instrument, live MIDI goes nowhere")
{
    OpenSong song (1);
    phazite::routeLiveMidiInput (*song.edit);
    CHECK (phazite::getLiveMidiTrack (*song.edit) == nullptr);

    // Nor anywhere in an Edit that no project context holds, which has no session to keep it.
    auto alone = phazite::createSongEdit (phazite::test::getEngine());
    addInstrument (*te::getAudioTracks (*alone)[0]);
    phazite::routeLiveMidiInput (*alone);
    CHECK (phazite::getLiveMidiTrack (*alone) == nullptr);
}

TEST_CASE ("Playing and routing live MIDI don't count as unsaved changes")
{
    const auto folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteLiveInputTests", {}, false);
    folder.createDirectory();

    {
        juce::String error;
        auto project = phazite::Project::create (phazite::test::getEngine(), folder, "Live", error);
        REQUIRE (project != nullptr);

        auto& edit = project->getEdit();
        phazite::ensureTracks (edit, 1);
        addInstrument (*te::getAudioTracks (edit)[0]);
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());

        // What opening a project and playing it do: where live MIDI goes, and the playhead moved.
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setEdit (context, &edit);
        phazite::routeLiveMidiInput (edit);
        CHECK (phazite::getLiveMidiTrack (edit) == te::getAudioTracks (edit)[0]);
        phazite::movePlayhead (edit, te::TimePosition::fromSeconds (2.0));

        CHECK_FALSE (project->hasUnsavedChanges());
        setEdit (context, nullptr);
    }

    folder.deleteRecursively();
}

TEST_CASE ("Live MIDI follows the selected track, if it has an instrument")
{
    OpenSong song (3);
    auto& edit = song.edit;
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[0]);
    addInstrument (*tracks[2]);
    phazite::routeLiveMidiInput (*edit);
    REQUIRE (phazite::getLiveMidiTrack (*edit) == tracks[0]);

    phazite::Selection selection (phazite::test::getEngine());
    selection.setEdit (edit.get());

    selection.selectOnly (phazite::getTrackID (*tracks[2]));
    phazite::followSelectedTrack (selection);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);

    // A track without an instrument can't play it, so live MIDI stays where it was.
    CHECK_FALSE (phazite::hasInstrument (*tracks[1]));
    selection.selectOnly (phazite::getTrackID (*tracks[1]));
    phazite::followSelectedTrack (selection);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);

    selection.deselectAll();
}

TEST_CASE ("The computer keyboard's notes play live into the track live MIDI plays, through Phazite's engine")
{
    OpenSong song (2);
    auto& edit = song.edit;
    addInstrument (*te::getAudioTracks (*edit)[1]);
    phazite::routeLiveMidiInput (*edit);
    const auto track = phazite::getLiveMidiTrack (song.context);
    REQUIRE (track.isValid());

    phazite::test::EnginePlayback playback;
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    host.getPlayedEvents(); // none from before
    auto playedSince = [&host, before = host.getPlayedEvents().size()]() mutable
    {
        std::vector<phazite::engine::Engine::PlayedEvent> since;
        const auto& played = host.getPlayedEvents();
        since.assign (played.begin() + (std::ptrdiff_t) std::min (before, played.size()), played.end());
        before = played.size();
        return since;
    };

    auto& engine = phazite::test::getEngine();
    phazite::playLiveMidi (engine, juce::MidiMessage::noteOn (1, 60, 0.8f));
    phazite::playLiveMidi (engine, juce::MidiMessage::noteOff (1, 60));
    playback.play (0.05);
    auto played = playedSince();
    REQUIRE (played.size() == 2);
    CHECK (played[0].track == (std::int64_t) track.getRaw());
    CHECK (played[0].event.type == phazite::devices::NoteEvent::Type::noteOn);
    CHECK (played[0].event.number == 60);
    CHECK (played[1].event.type == phazite::devices::NoteEvent::Type::noteOff);

    // Only notes: a controller, even All Notes Off, plays nothing.
    phazite::playLiveMidi (engine, juce::MidiMessage::allNotesOff (1));
    phazite::playLiveMidi (engine, juce::MidiMessage::controllerEvent (1, 1, 64));
    playback.play (0.05);
    CHECK (playedSince().empty());
}

TEST_CASE ("A live note sounds at once, and waits only for devices that are switched on and look ahead")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::HostedDevice device;

    OpenSong song (2);
    auto& edit = song.edit;
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[0]);
    phazite::routeLiveMidiInput (*edit);
    phazite::test::settle (500);

    // A note played between two blocks, as a MIDI input plays one: how long after it it sounds.
    const auto playNote = [&]
    {
        device.process (50);
        const auto samples = device.samplesFromNoteToSound ([] { playLive (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)); });
        phazite::playLiveMidi (engine, juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        return samples;
    };

    const auto atOnce = playNote();
    CHECK (atOnce >= 0);
    CHECK (atOnce < 16);

    // A limiter on another track looks ahead, and delay compensation holds what is played back to match it...
    auto limiter = edit->getPluginCache().createNewPlugin (phazite::devices::Limiter::typeID, {});
    phazite::insertDevice (*tracks[1], limiter);
    phazite::test::settle (500);
    phazite::devices::Limiter itsDSP;
    itsDSP.prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);
    const auto latency = itsDSP.getLatencySamples();
    REQUIRE (latency > 0);
    CHECK (isNear (playUntil (playNote, [&] (int samples) { return isNear (samples, atOnce + latency); }), atOnce + latency));

    // ...but not once it is switched off.
    phazite::setDeviceOn (*limiter, false);
    phazite::test::settle (500);
    CHECK (isNear (playUntil (playNote, [atOnce] (int samples) { return isNear (samples, atOnce); }), atOnce));
}

TEST_CASE ("Low latency plays live notes without delay compensation, and back with it when it is off")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::HostedDevice device;

    // A limiter, which looks ahead, on another track.
    OpenSong song (2);
    auto& edit = song.edit;
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[0]);
    phazite::insertDevice (*tracks[1], edit->getPluginCache().createNewPlugin (phazite::devices::Limiter::typeID, {}));
    phazite::routeLiveMidiInput (*edit);
    phazite::test::settle (500);

    const auto playNote = [&]
    {
        device.process (50);
        const auto samples = device.samplesFromNoteToSound ([] { playLive (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)); });
        phazite::playLiveMidi (engine, juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        return samples;
    };

    const auto compensated = playUntil (playNote, [] (int samples) { return samples > 90; });
    CHECK (compensated > 90); // the limiter's look-ahead, about 99 samples

    phazite::setLowLatency (engine, true);
    phazite::test::settle (500);
    CHECK (phazite::isLowLatency (engine));
    const auto low = playUntil (playNote, [] (int samples) { return samples >= 0 && samples < 16; });
    CHECK (low >= 0);
    CHECK (low < 16);

    phazite::setLowLatency (engine, false);
    phazite::test::settle (500);
    CHECK (isNear (playUntil (playNote, [compensated] (int samples) { return isNear (samples, compensated); }), compensated));
}

TEST_CASE ("Every live note sounds, however long the message thread was busy")
{
    // The blocks are processed while the test's message thread waits, as the app's does through a long repaint: Tracktion
    // dropped the notes waiting when JUCE's approximate clock caught up between two blocks, about every other one here,
    // until Phazite kept that clock up to date; Phazite's engine reads no such clock.
    auto& engine = phazite::test::getEngine();
    phazite::test::HostedDevice device;

    OpenSong song (2);
    auto& edit = song.edit;
    addInstrument (*te::getAudioTracks (*edit)[0]);
    auto lookAhead = 0; // samples: what the master's devices look ahead by delays what is played live as much

    SUBCASE ("Alone in the song")
    {
    }

    // As the notes were lost when they were first seen to be.
    SUBCASE ("With a limiter and a multiband on the master")
    {
        for (const auto* type : { phazite::devices::Limiter::typeID, phazite::devices::Multiband::typeID })
            phazite::insertMasterDevice (*edit, edit->getPluginCache().createNewPlugin (type, {}));

        phazite::devices::Limiter limiter;
        phazite::devices::Multiband multiband;

        for (auto* processor : std::initializer_list<phazite::devices::Processor*> { &limiter, &multiband })
        {
            processor->prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);
            lookAhead += processor->getLatencySamples();
        }
    }

    phazite::routeLiveMidiInput (*edit);
    phazite::test::settle (500);

    const auto playNote = [&]
    {
        device.process (50);
        const auto samples = device.samplesFromNoteToSound ([] { playLive (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)); });
        phazite::playLiveMidi (engine, juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        return samples;
    };

    // Once the graph plays the note, in the block it came in (after the master's look-ahead), every note after it sounds as
    // soon.
    const auto first = playUntil (playNote, [] (int samples) { return samples >= 0; });
    REQUIRE (first >= 0);
    CHECK (first < phazite::test::HostedDevice::blockSize + lookAhead);

    for (int i = 0; i < 12; ++i)
        CHECK (isNear (playNote(), first));
}

TEST_CASE ("Low latency leaves what renders play as the song has it")
{
    // A synth's chord into a limiter on the master, which low latency would bypass while playing.
    auto& engine = phazite::test::getEngine();
    auto edit = phazite::createSongEdit (engine);
    auto& track = *te::getAudioTracks (*edit)[0];
    addInstrument (track);
    auto& song = phazite::getSong (*edit);
    const auto clip = phazite::test::addMidiClip (song, phazite::getTrackID (track),
                                                  { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) }, "Chord");

    for (int note : { 48, 55, 60, 64 })
        phazite::test::addMidiNote (song, clip, note, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (3.0), 120);

    auto limiter = edit->getPluginCache().createNewPlugin (phazite::devices::Limiter::typeID, {});
    phazite::insertMasterDevice (*edit, limiter);
    phazite::setDeviceValue (*limiter->getAutomatableParameterByID ("gain"), 12.0f);

    // A second of it, as an export renders it (Phazite's engine, offline).
    const auto render = [&edit] { return phazite::renderWithOwnEngine (*edit, 48000, 48000.0, 512); };
    const auto asItIs = render();
    phazite::setLowLatency (engine, true);
    const auto whileLow = render();
    phazite::setLowLatency (engine, false);

    REQUIRE (asItIs.getNumSamples() == whileLow.getNumSamples());
    CHECK (asItIs.getMagnitude (0, asItIs.getNumSamples()) > 0.5f);
    CHECK (asItIs.getMagnitude (0, asItIs.getNumSamples()) < 0.95f); // the limiter's ceiling, -1 dB

    auto largest = 0.0f;

    for (int i = 0; i < asItIs.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (asItIs.getSample (0, i) - whileLow.getSample (0, i)));

    CHECK (largest < 1.0e-6f);
}

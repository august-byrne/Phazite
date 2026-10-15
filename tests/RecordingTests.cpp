// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording MIDI and Capture (src/model/transport/Recording.h, Capture.h): the settings and their commands, takes
// recorded in real time through Tracktion's hosted audio device, as the app's audio device would play it, and what
// Capture makes of notes played a moment ago.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/Undo.h"
#include "model/engine/LiveInput.h"
#include "model/notes/Notes.h"
#include "model/transport/Capture.h"
#include "model/transport/Metronome.h"
#include "model/transport/Playback.h"
#include "model/transport/Recording.h"
#include "model/transport/Tempo.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
// Tracktion's merged MIDI input, which it creates on a timer once asked to scan the MIDI devices (as LiveInputTests.cpp
// waits for it).
std::shared_ptr<te::MidiInputDevice> waitForAllMidiInputs()
{
    auto& engine = phazite::test::getEngine();

    if (phazite::getAllMidiInputs (engine) == nullptr)
    {
        engine.getDeviceManager().rescanMidiDeviceList();
        phazite::test::runTimersUntil ([&engine] { return phazite::getAllMidiInputs (engine) != nullptr; });
    }

    return phazite::getAllMidiInputs (engine);
}

void addInstrument (te::AudioTrack& track)
{
    track.pluginList.insertPlugin (track.edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}), 0, nullptr);
}

// Plays an Edit through Tracktion's hosted audio device for as long as it lives, on a thread that acts as the audio
// thread and processes a block every block's length, so that MIDI played meanwhile is recorded as the app records it.
class RealTimePlayback
{
public:
    explicit RealTimePlayback (te::Engine& e)
        : engine (e)
    {
        auto& deviceManager = engine.getDeviceManager();
        auto& audioIO = deviceManager.getHostedAudioDeviceInterface();

        te::HostedAudioDeviceInterface::Parameters params;
        params.sampleRate = sampleRate;
        params.blockSize = blockSize;
        audioIO.initialise (params);
        audioIO.prepareToPlay (sampleRate, blockSize);
        deviceManager.dispatchPendingUpdates();

        audioThread = std::thread (
            [this, &audioIO]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                const auto blockDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                    std::chrono::duration<double> (blockSize / sampleRate));
                auto nextBlock = std::chrono::steady_clock::now();

                while (! stopping.load())
                {
                    nextBlock += blockDuration;
                    buffer.clear();
                    midi.clear();
                    audioIO.processBlock (buffer, midi);
                    std::this_thread::sleep_until (nextBlock);
                }
            });
    }

    ~RealTimePlayback()
    {
        stopping = true;
        audioThread.join();

        // As Tracktion's own tests do, so later tests start without the hosted device.
        auto& deviceManager = engine.getDeviceManager();
        deviceManager.closeDevices();
        deviceManager.removeHostedAudioDeviceInterface();
        deviceManager.deviceManager.closeAudioDevice();
    }

    // Lets some time pass on the message thread, which delivers its messages and runs its timers meanwhile.
    static void wait (int milliseconds)
    {
        phazite::test::dispatchMessagesUntil ([] { return false; }, milliseconds);
    }

private:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;

    te::Engine& engine;
    std::atomic<bool> stopping { false };
    std::thread audioThread;
};
} // namespace

TEST_CASE ("Overdub and quantizing recordings are the app's settings, which their commands switch")
{
    auto& engine = phazite::test::getEngine();
    REQUIRE (waitForAllMidiInputs() != nullptr);

    auto edit = te::Edit::createSingleTrackEdit (engine);
    te::SelectionManager selection (engine);
    selection.edit = edit.get();
    phazite::CommandRegistry registry;
    phazite::addRecordingCommands (registry, selection);

    const auto* overdub = registry.find ("transport.overdub");
    const auto* quantize = registry.find ("transport.quantizeRecordings");
    REQUIRE (overdub != nullptr);
    REQUIRE (quantize != nullptr);
    CHECK (overdub->isTicked()); // at first
    CHECK_FALSE (quantize->isTicked());

    REQUIRE (registry.invoke ("transport.overdub"));
    CHECK_FALSE (phazite::isOverdubOn (engine));
    CHECK (phazite::getAllMidiInputs (engine)->replaceExistingClips);

    REQUIRE (registry.invoke ("transport.quantizeRecordings"));
    CHECK (phazite::isRecordQuantizeOn (engine));
    CHECK (quantize->isTicked());

    // Back as they were, for the other tests.
    phazite::setOverdubOn (engine, true);
    phazite::setRecordQuantizeOn (engine, false);
    CHECK (overdub->isTicked());
    CHECK_FALSE (quantize->isTicked());
    selection.edit = nullptr;
}

TEST_CASE ("Recording needs a track for live MIDI to play")
{
    REQUIRE (waitForAllMidiInputs() != nullptr);
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::routeLiveMidiInput (*edit);
    CHECK_FALSE (phazite::canRecord (*edit)); // no instrument anywhere
    CHECK_FALSE (phazite::canToggleRecording (*edit));

    phazite::startRecording (*edit);
    CHECK_FALSE (phazite::isRecording (*edit));

    addInstrument (*te::getAudioTracks (*edit)[0]);
    phazite::routeLiveMidiInput (*edit);
    CHECK (phazite::canRecord (*edit));
}

TEST_CASE ("What is played while recording becomes a clip on live MIDI's track, as one undo step")
{
    REQUIRE (waitForAllMidiInputs() != nullptr);
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine); // 120 BPM: a beat is half a second
    auto* track = te::getAudioTracks (*edit)[0];
    addInstrument (*track);
    edit->getTransport().looping = false;
    phazite::setCountInOn (*edit, false);

    const RealTimePlayback playback (engine);
    phazite::routeLiveMidiInput (*edit);
    RealTimePlayback::wait (200); // for Tracktion to rebuild its playback graph with the input armed
    REQUIRE (phazite::getLiveMidiTrack (*edit) == track);
    REQUIRE (phazite::canRecord (*edit));
    edit->getUndoManager().clearUndoHistory();

    phazite::startRecording (*edit);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&edit] { return phazite::isRecording (*edit); }));

    // Middle C, a quarter of a second in, for a quarter of a second.
    RealTimePlayback::wait (250);
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 60, 0.8f));
    RealTimePlayback::wait (250);
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 60));
    RealTimePlayback::wait (250);

    phazite::stopRecording (*edit);
    CHECK_FALSE (phazite::isRecording (*edit));
    CHECK (edit->getTransport().getPosition() == te::TimePosition()); // back where recording started

    const auto clips = track->getClips();
    REQUIRE (clips.size() == 1);
    auto* clip = dynamic_cast<te::MidiClip*> (clips.getFirst());
    REQUIRE (clip != nullptr);
    REQUIRE (clip->getSequence().getNumNotes() == 1);

    auto& note = *clip->getSequence().getNotes().getFirst();
    CHECK (note.getNoteNumber() == 60);
    CHECK (phazite::getNoteStart (*clip, note).inBeats() == doctest::Approx (0.5).epsilon (0.4)); // timing as played, roughly
    CHECK (note.getLengthBeats().inBeats() == doctest::Approx (0.5).epsilon (0.4));
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Record"));

    phazite::undo (*edit);
    CHECK (track->getClips().isEmpty());
}

TEST_CASE ("The song's own loop doesn't loop a recording, and is back around the song after it")
{
    REQUIRE (waitForAllMidiInputs() != nullptr);
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto* track = te::getAudioTracks (*edit)[0];
    addInstrument (*track);
    edit->tempoSequence.getTempo (0)->setBpm (240.0); // a bar a second, the loop of a song without clips
    phazite::loopSong (*edit);
    REQUIRE (edit->getTransport().getLoopRange().getEnd().inSeconds() == doctest::Approx (1.0));
    phazite::setCountInOn (*edit, false);

    const RealTimePlayback playback (engine);
    phazite::routeLiveMidiInput (*edit);
    RealTimePlayback::wait (200);

    phazite::startRecording (*edit);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&edit] { return phazite::isRecording (*edit); }));
    CHECK (phazite::isLoopOn (*edit));

    // Past the song loop's end: a quarter of a second into the second bar.
    RealTimePlayback::wait (1250);
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 64, 0.8f));
    RealTimePlayback::wait (250);
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 64));
    RealTimePlayback::wait (100);
    phazite::stopRecording (*edit);

    const auto clips = track->getClips();
    REQUIRE (clips.size() == 1);
    auto* clip = dynamic_cast<te::MidiClip*> (clips.getFirst());
    REQUIRE (clip != nullptr);
    REQUIRE (clip->getSequence().getNumNotes() == 1);
    CHECK (phazite::getNoteStart (*clip, *clip->getSequence().getNotes().getFirst()).inBeats()
           == doctest::Approx (5.0).epsilon (0.15)); // where it was played, not folded into the first bar

    // The loop is the song's again: to the end of the bar the new clip ends in.
    CHECK (phazite::isLoopOn (*edit));
    CHECK (phazite::isLoopingSong (*edit));
    CHECK (edit->getTransport().getLoopRange().getEnd() == phazite::getSongLoopRange (*edit).getEnd());
    CHECK (edit->getTransport().getLoopRange().getEnd().inSeconds() == doctest::Approx (2.0));
}

//==============================================================================
namespace
{
// Note starts, in seconds, on a grid of beats at a tempo: each beat in a list, where 0.5 is an eighth after the first.
std::vector<double> atTempo (double bpm, std::initializer_list<double> beats, double jitterSeconds = 0.0, int seed = 1)
{
    juce::Random random (seed);
    std::vector<double> starts;

    for (const auto beat : beats)
        starts.push_back (beat * 60.0 / bpm + (random.nextDouble() * 2.0 - 1.0) * jitterSeconds);

    return starts;
}

// Plays notes into "All MIDI Ins" as the computer keyboard does, each a beat apart at a tempo and half a beat long.
void playQuarterNotes (te::Engine& engine, double bpm, std::initializer_list<int> pitches)
{
    // On time: each note at its own moment from the first, so that waiting a little long doesn't add up.
    const auto beatMs = 60000.0 / bpm;
    const auto first = juce::Time::getMillisecondCounterHiRes();
    const auto waitUntil = [] (double moment)
    { phazite::test::dispatchMessagesUntil ([moment] { return juce::Time::getMillisecondCounterHiRes() >= moment; }); };
    double beat = 0.0;

    for (const auto pitch : pitches)
    {
        waitUntil (first + beat * beatMs);
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, pitch, 0.8f));
        waitUntil (first + (beat + 0.5) * beatMs);
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, pitch));
        beat += 1.0;
    }
}

// Notes played before this test are still in Tracktion's last 30 seconds: Capture takes them, and they are undone.
void forgetWhatWasPlayed (te::Edit& edit)
{
    if (phazite::capture (edit, nullptr) != nullptr)
        phazite::undo (edit);

    edit.getUndoManager().clearUndoHistory();
}

std::vector<double> getNoteBeats (te::MidiClip& clip)
{
    std::vector<double> beats;

    for (auto* note : clip.getSequence().getNotes())
        beats.push_back (phazite::getNoteStart (clip, *note).inBeats());

    std::sort (beats.begin(), beats.end());
    return beats;
}
} // namespace

TEST_CASE ("Capture's tempo is the one at which the notes fall best on a grid, more of them on beats")
{
    using phazite::detectTempo;

    // Eighths and quarters at 100 BPM.
    CHECK (detectTempo (atTempo (100.0, { 0, 1, 1.5, 2, 3, 3.5, 4, 5, 6, 6.5, 7 })).value_or (0.0) == doctest::Approx (100.0));

    // A sixteenth beat at 128, played up to 15 ms early or late.
    CHECK (detectTempo (atTempo (128.0, { 0, 0.75, 1, 1.5, 2, 2.25, 2.75, 3, 3.5, 4, 4.75, 5, 5.5, 6, 6.25, 6.75, 7, 7.5 }, 0.015))
               .value_or (0.0)
           == doctest::Approx (128.0).epsilon (0.01));

    // Quarter notes at 90: not dotted eighths at 135.
    CHECK (detectTempo (atTempo (90.0, { 0, 1, 2, 3, 4, 5, 6, 7 })).value_or (0.0) == doctest::Approx (90.0));

    // Quarter notes at 60 are eighths at 120: one octave only.
    CHECK (detectTempo (atTempo (60.0, { 0, 1, 2, 3, 4, 5 })).value_or (0.0) == doctest::Approx (120.0));

    // Too few notes, too short a time, or notes at random: no tempo.
    CHECK_FALSE (detectTempo (atTempo (120.0, { 0, 1, 2 })).has_value());
    CHECK_FALSE (detectTempo (atTempo (120.0, { 0, 0.5, 1, 1.5 })).has_value());

    juce::Random random (7);
    std::vector<double> scattered;

    for (int i = 0; i < 24; ++i)
        scattered.push_back (i * 0.37 + random.nextDouble() * 0.3);

    CHECK_FALSE (detectTempo (scattered).has_value());
}

TEST_CASE ("Capture while stopped, in a song without clips, sets the tempo from the notes and puts them at bar 1")
{
    REQUIRE (waitForAllMidiInputs() != nullptr);
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine); // at 120 BPM
    auto* track = te::getAudioTracks (*edit)[0];
    addInstrument (*track);
    phazite::routeLiveMidiInput (*edit);
    REQUIRE (phazite::canCapture (*edit));
    forgetWhatWasPlayed (*edit);

    playQuarterNotes (engine, 100.0, { 60, 62, 64, 65, 67, 69 });
    te::SelectionManager selection (engine);
    selection.edit = edit.get();
    auto* clip = phazite::capture (*edit, &selection);

    REQUIRE (clip != nullptr);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (100.0));
    CHECK (clip->getTrack() == track);
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (0.0));
    CHECK (clip->getLengthInBeats().inBeats() == doctest::Approx (8.0)); // two whole bars
    CHECK (selection.isSelected (clip));

    const auto beats = getNoteBeats (*clip);
    REQUIRE (beats.size() == 6);

    for (size_t i = 0; i < beats.size(); ++i)
        CHECK (beats[i] == doctest::Approx ((double) i).epsilon (0.03));

    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Capture"));
    phazite::undo (*edit);
    CHECK (track->getClips().isEmpty());
    CHECK (phazite::getTempo (*edit) == doctest::Approx (120.0)); // the tempo with it
    selection.edit = nullptr;
}

TEST_CASE ("Capture while stopped, in a song with clips, keeps its tempo and goes after the track's clips")
{
    REQUIRE (waitForAllMidiInputs() != nullptr);
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto* track = te::getAudioTracks (*edit)[0];
    addInstrument (*track);
    phazite::insertMidiClip (*track, { te::BeatPosition(), te::BeatPosition::fromBeats (4.0) }, nullptr);
    phazite::routeLiveMidiInput (*edit);
    forgetWhatWasPlayed (*edit);

    playQuarterNotes (engine, 120.0, { 60, 64, 67, 72 });
    auto* clip = phazite::capture (*edit, nullptr);

    REQUIRE (clip != nullptr);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (120.0));
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (4.0)); // bar 2, after the clip in bar 1

    const auto beats = getNoteBeats (*clip);
    REQUIRE (beats.size() == 4);
    CHECK (beats.front() == doctest::Approx (4.0));
    CHECK (beats.back() == doctest::Approx (7.0).epsilon (0.03));
}

TEST_CASE ("Capture while the song plays keeps the notes where they were played")
{
    REQUIRE (waitForAllMidiInputs() != nullptr);
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine); // 120 BPM: a beat is half a second
    auto* track = te::getAudioTracks (*edit)[0];
    addInstrument (*track);
    edit->getTransport().looping = false;

    const RealTimePlayback playback (engine);
    phazite::routeLiveMidiInput (*edit);
    RealTimePlayback::wait (200);
    forgetWhatWasPlayed (*edit);

    edit->getTransport().play (false);
    RealTimePlayback::wait (1000); // two beats in
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 60, 0.8f));
    RealTimePlayback::wait (250);
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 60));
    RealTimePlayback::wait (250);

    auto* clip = phazite::capture (*edit, nullptr);
    phazite::stopAndReturn (*edit);

    REQUIRE (clip != nullptr);
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (0.0)); // the bar the note is in

    const auto beats = getNoteBeats (*clip);
    REQUIRE (beats.size() == 1);
    CHECK (beats.front() == doctest::Approx (2.0).epsilon (0.15));
}

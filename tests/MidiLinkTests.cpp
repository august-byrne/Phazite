// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI learn (model/midi/MidiLinkModel.h): a hardware controller linked to a parameter, saved in the song, turning it as its
// control does; what the MIDI inputs send reaching it through AudioIO::takeMidi(), as an input's thread calls it.
#include "EnginePlayback.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/MidiTap.h"
#include "model/midi/MidiLinkModel.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/AutomationWriter.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/SongDiff.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using phazite::MidiController;
using phazite::test::TestSong;

phazite::AudioIO& getAudioIO()
{
    return phazite::getAudioIO (phazite::test::getEngineContext());
}

// What a MIDI input sends, as its thread would hand it over, then the message loop's turn that takes it.
void send (const juce::MidiMessage& message)
{
    getAudioIO().takeMidi (message);
    phazite::test::dispatchPendingMessages();
}

void sendController (int channel, int number, int value)
{
    send (juce::MidiMessage::controllerEvent (channel, number, value));
}

phazite::Song& getSong (TestSong& song)
{
    return *phazite::findSong (song.context);
}

struct SynthSong
{
    SynthSong() { synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID); }

    phazite::ParameterHandle parameter (std::string_view id) { return phazite::getDeviceHandle (song.context, synth).getParameter (id); }

    // Links a controller to a parameter as a user does: MIDI Learn, then the knob moved.
    void link (const phazite::ParameterHandle& handle, MidiController controller)
    {
        phazite::startMidiLearn (handle);
        REQUIRE (phazite::isLearningMidi (handle));
        sendController (controller.channel, controller.number, 0);
        REQUIRE (phazite::getMidiLink (handle) == controller);
    }

    phazite::MidiLinker& getLinker() { return *phazite::findMidiLinker (getSong (song)); }

    TestSong song;
    phazite::DeviceID synth;
};
} // namespace

TEST_CASE ("MIDI learn links the next controller moved, and only one that can be linked")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    phazite::test::UndoCheck undoCheck (s.song.context);
    REQUIRE (phazite::canLearnMidi (cutoff));

    phazite::startMidiLearn (cutoff);
    CHECK (phazite::isLearningMidi (cutoff));
    CHECK (phazite::getMidiLearnName (s.song.context) == juce::String ("Filter Cutoff"));

    // Notes, the bend, pressure and channel mode messages play on, and leave it learning.
    send (juce::MidiMessage::noteOn (1, 60, 0.8f));
    send (juce::MidiMessage::noteOff (1, 60));
    send (juce::MidiMessage::pitchWheel (1, 9000));
    send (juce::MidiMessage::channelPressureChange (1, 50));
    send (juce::MidiMessage::controllerEvent (1, 123, 0)); // All Notes Off
    CHECK (phazite::isLearningMidi (cutoff));
    CHECK (phazite::getMidiLinks (s.song.context).empty());

    const auto before = cutoff.getValue();
    undoCheck.check ("Link MIDI Controller", [] { sendController (3, 21, 90); });
    CHECK_FALSE (phazite::isLearningMidi (cutoff));
    CHECK (phazite::getMidiLink (cutoff) == MidiController { 3, 21 });
    CHECK (cutoff.getValue() == doctest::Approx (before)); // learning doesn't turn it
    CHECK (phazite::describeUndo (&getSong (s.song)) == "Undo Link MIDI Controller");
    CHECK (phazite::takeMidiLinkMade (s.song.context) == juce::String ("CC 21 on channel 3 now turns Filter Cutoff."));
    CHECK_FALSE (phazite::takeMidiLinkMade (s.song.context).has_value());

    const auto links = phazite::getMidiLinks (s.song.context);
    REQUIRE (links.size() == 1);
    CHECK (phazite::describeMidiController (links[0].controller) == "CC 21, Channel 3");
    CHECK (links[0].turns == "Track 1: Synth: Filter Cutoff");
    CHECK (links[0].device == s.synth);
    CHECK (links[0].track == s.song.firstTrack);

    // Learning cancelled links nothing.
    auto resonance = s.parameter ("resonance");
    phazite::startMidiLearn (resonance);
    phazite::cancelMidiLearn (s.song.context);
    sendController (1, 22, 10);
    CHECK_FALSE (phazite::getMidiLink (resonance).has_value());
}

TEST_CASE ("A controller's value lands along its parameter's travel")
{
    SynthSong s;
    auto& song = getSong (s.song);
    const auto ref = [&s] (const char* id) { return phazite::ParameterRef { s.synth, id }; };
    const auto at = [&song] (const phazite::ParameterRef& parameter, int position)
    { return *phazite::getValueAtControllerPosition (song, parameter, position); };

    // A knob with a logarithmic scale: its ends, and its middle where the knob's middle is.
    const auto range = s.parameter ("cutoff").getRange();
    CHECK (at (ref ("cutoff"), 0) == doctest::Approx (range.start));
    CHECK (at (ref ("cutoff"), 127) == doctest::Approx (range.end));
    CHECK (at (ref ("cutoff"), 64) == doctest::Approx (phazite::fromControlPosition (range, 64.0f / 127.0f)));
    CHECK (at (ref ("cutoff"), 64) < (range.start + range.end) / 2.0f);

    // A switch is on from 64; a choice's four steps share the travel evenly.
    CHECK (at (ref ("osc2On"), 63) == 0.0f);
    CHECK (at (ref ("osc2On"), 64) == 1.0f);
    CHECK (at (ref ("subShape"), 31) == 0.0f);
    CHECK (at (ref ("subShape"), 32) == 1.0f);
    CHECK (at (ref ("subShape"), 63) == 1.0f);
    CHECK (at (ref ("subShape"), 64) == 2.0f);
    CHECK (at (ref ("subShape"), 96) == 3.0f);
    CHECK (at (ref ("subShape"), 127) == 3.0f);

    // Through a link, as the controller sends it.
    auto shape = s.parameter ("subShape");
    s.link (shape, { 1, 30 });
    sendController (1, 30, 100);
    CHECK (shape.getValue() == 3.0f);
}

TEST_CASE ("A burst of a knob's moves is one undo step, kept once the knob rests")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    s.link (cutoff, { 1, 21 });
    const auto before = cutoff.getValue();
    auto& song = getSong (s.song);
    const auto steps = phazite::getHistorySteps (song).done.size();

    phazite::test::HeldClock clock;

    for (auto value : { 10, 40, 80, 120 })
    {
        sendController (1, 21, value);
        clock.advance (100.0);
    }

    CHECK (s.getLinker().isInBurst());
    CHECK (cutoff.getValue() == doctest::Approx (phazite::fromControlPosition (cutoff.getRange(), 120.0f / 127.0f)));

    clock.advance (phazite::MidiLinker::restMilliseconds);
    CHECK (phazite::test::runTimersUntil ([&s] { return ! s.getLinker().isInBurst(); }));
    CHECK (phazite::getHistorySteps (song).done.size() == steps + 1);
    CHECK (phazite::describeUndo (&song) == "Undo Change Filter Cutoff");

    phazite::undo (song);
    CHECK (cutoff.getValue() == doctest::Approx (before));
    phazite::redo (song);
    CHECK (cutoff.getValue() == doctest::Approx (phazite::fromControlPosition (cutoff.getRange(), 120.0f / 127.0f)));
}

TEST_CASE ("A step, an undo or a redo begun during a burst keeps the burst first")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    s.link (cutoff, { 1, 21 });
    auto& song = getSong (s.song);
    const auto before = cutoff.getValue();
    phazite::test::HeldClock clock;

    SUBCASE ("Another step")
    {
        phazite::test::UndoCheck undoCheck (s.song.context);
        sendController (1, 21, 100);
        undoCheck.check ("Rename Track", [&s] { s.song.rename (s.song.firstTrack, "Lead"); });
        const auto done = phazite::getHistorySteps (song).done;
        REQUIRE (done.size() >= 2);
        CHECK (done[done.size() - 2] == "Change Filter Cutoff");

        // The knob turned again is a burst of its own.
        sendController (1, 21, 20);
        s.getLinker().endBurst();
        CHECK (phazite::getHistorySteps (song).done.strings.getLast() == "Change Filter Cutoff");
        undoCheck.walkBackAndForth();
    }

    // The undo keeps the burst first, then takes it back whole; the redo does it again (UndoCheck follows only steps it saw
    // begin, and this one ends inside the undo).
    SUBCASE ("An undo, then a redo")
    {
        sendController (1, 21, 100);
        phazite::undo (song);
        CHECK (cutoff.getValue() == doctest::Approx (before));
        CHECK (phazite::canRedo (song));
        CHECK_FALSE (s.getLinker().isInBurst());
        phazite::redo (song);
        CHECK (cutoff.getValue() == doctest::Approx (phazite::fromControlPosition (cutoff.getRange(), 100.0f / 127.0f)));
        phazite::undo (song);
        CHECK (phazite::describeUndo (&song) == "Undo Link MIDI Controller");
    }
}

TEST_CASE ("Two knobs turned at once are one burst, named after the first")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    auto resonance = s.parameter ("resonance");
    s.link (cutoff, { 1, 21 });
    s.link (resonance, { 1, 22 });
    const auto cutoffBefore = cutoff.getValue(), resonanceBefore = resonance.getValue();
    auto& song = getSong (s.song);

    phazite::test::HeldClock clock;
    sendController (1, 21, 100);
    sendController (1, 22, 100);
    sendController (1, 21, 110);
    s.getLinker().endBurst();
    CHECK (phazite::describeUndo (&song) == "Undo Change Filter Cutoff");

    phazite::undo (song);
    CHECK (cutoff.getValue() == doctest::Approx (cutoffBefore));
    CHECK (resonance.getValue() == doctest::Approx (resonanceBefore));
}

TEST_CASE ("Learning a linked controller moves its link, and a parameter's link is replaced, in one step")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    auto resonance = s.parameter ("resonance");
    s.link (cutoff, { 1, 21 });
    auto& song = getSong (s.song);
    phazite::test::UndoCheck undoCheck (s.song.context);

    phazite::startMidiLearn (resonance);
    undoCheck.check ("Link MIDI Controller", [] { sendController (1, 21, 0); });
    CHECK (phazite::getMidiLink (resonance) == MidiController { 1, 21 });
    CHECK_FALSE (phazite::getMidiLink (cutoff).has_value());
    CHECK (phazite::takeMidiLinkMade (s.song.context)
           == "CC 21 on channel 1 now turns " + resonance.getName() + ", instead of Filter Cutoff.");
    CHECK (phazite::getMidiLinks (s.song.context).size() == 1);

    // Learning another controller for it replaces its link.
    phazite::startMidiLearn (resonance);
    undoCheck.check ("Link MIDI Controller", [] { sendController (2, 7, 0); });
    CHECK (phazite::getMidiLink (resonance) == MidiController { 2, 7 });
    CHECK (phazite::getMidiLinks (s.song.context).size() == 1);

    // Undo takes each step back whole.
    phazite::undo (song);
    CHECK (phazite::getMidiLink (resonance) == MidiController { 1, 21 });
    phazite::undo (song);
    CHECK (phazite::getMidiLink (cutoff) == MidiController { 1, 21 });
    CHECK_FALSE (phazite::getMidiLink (resonance).has_value());
    undoCheck.walkBackAndForth();
}

TEST_CASE ("Remove MIDI Link, by a control or by its controller")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    auto resonance = s.parameter ("resonance");
    s.link (cutoff, { 1, 21 });
    s.link (resonance, { 1, 22 });
    phazite::test::UndoCheck undoCheck (s.song.context);

    undoCheck.check ("Remove MIDI Link", [&cutoff] { phazite::removeMidiLink (cutoff); });
    CHECK (phazite::describeUndo (&getSong (s.song)) == "Undo Remove MIDI Link");
    CHECK_FALSE (phazite::getMidiLink (cutoff).has_value());

    undoCheck.check ("Remove MIDI Link", [&s] { phazite::removeMidiLink (s.song.context, { 1, 22 }); });
    CHECK (phazite::getMidiLinks (s.song.context).empty());

    // An unlinked controller turns nothing.
    const auto before = cutoff.getValue();
    sendController (1, 21, 127);
    CHECK (cutoff.getValue() == doctest::Approx (before));
    undoCheck.walkBackAndForth();
}

TEST_CASE ("A device removed takes its links along in its step, and Undo brings both back")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    s.link (cutoff, { 1, 21 });
    auto volume = phazite::getVolume (s.song.context, s.song.firstTrack);
    s.link (volume, { 1, 7 });
    phazite::test::UndoCheck undoCheck (s.song.context);

    undoCheck.check ("Remove Device", [&s] { phazite::removeDevice (s.song.context, s.synth); });
    CHECK (phazite::getMidiLinks (s.song.context).size() == 1); // the fader's stays
    CHECK (getSong (s.song).getState().getChildWithName (phazite::schema::MIDILINKS).getNumChildren() == 1);

    phazite::undo (getSong (s.song));
    auto again = s.parameter ("cutoff");
    CHECK (phazite::getMidiLink (again) == MidiController { 1, 21 });
    CHECK (phazite::getMidiLinks (s.song.context).size() == 2);
    undoCheck.walkBackAndForth();
}

TEST_CASE ("A macro, a fader and a send the first change makes can be learned")
{
    SynthSong s;
    const auto macro = s.song.addModulator (s.song.firstTrack, phazite::ModulatorType::macro);
    auto macroKnob =
        phazite::getModulatorParameter (s.song.context, macro, phazite::getModulatorParameters (s.song.context, macro).front().id);
    REQUIRE (phazite::canLearnMidi (macroKnob));
    s.link (macroKnob, { 1, 1 });
    sendController (1, 1, 127);
    CHECK (macroKnob.getValue() == doctest::Approx (1.0f));
    CHECK (phazite::getMidiLinks (s.song.context).front().turns.endsWith (macroKnob.getName()));

    // A send to a return: none until its first change makes it.
    const auto other = s.song.addTrack ("Other");
    const auto returnTrack = s.song.addReturn (other, phazite::devices::Reverb::typeID);
    auto send = phazite::getSendLevelTo (s.song.context, s.song.firstTrack, returnTrack);
    REQUIRE (send.isValid());
    CHECK_FALSE (phazite::canLearnMidi (send));
    send.setValue (0.5f);
    REQUIRE (send.exists());
    CHECK (phazite::canLearnMidi (send));
    s.link (send, { 1, 2 });
    sendController (1, 2, 0);
    CHECK (send.getValue() == doctest::Approx (0.0f));
}

TEST_CASE ("A duplicated track has no links: its original keeps them")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    s.link (cutoff, { 1, 21 });
    auto& song = getSong (s.song);

    {
        const phazite::UndoStep step (song, "Duplicate Track");
        REQUIRE (phazite::duplicateTracks (song, { s.song.firstTrack }, nullptr).size() == 1);
    }

    const auto links = phazite::getMidiLinks (s.song.context);
    REQUIRE (links.size() == 1);
    CHECK (links[0].device == s.synth);
}

TEST_CASE ("A link is saved: reopened, the controller turns its parameter again, sent from an input's thread")
{
    SynthSong s;
    s.link (s.parameter ("cutoff"), { 4, 74 });
    s.song.reopen();

    auto cutoff = s.parameter ("cutoff");
    REQUIRE (phazite::getMidiLink (cutoff) == MidiController { 4, 74 });
    std::thread input ([] { getAudioIO().takeMidi (juce::MidiMessage::controllerEvent (4, 74, 127)); });
    input.join();
    CHECK (phazite::test::dispatchMessagesUntil ([&cutoff] { return cutoff.getValue() == doctest::Approx (cutoff.getRange().end); }));
}

TEST_CASE ("A linked controller goes to its parameter alone: not to the instrument, nor into what is played")
{
    SynthSong s;
    s.link (s.parameter ("cutoff"), { 1, 21 });
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    auto* session = phazite::getSession (s.song.context);
    REQUIRE (session != nullptr);
    session->setLiveTrack (s.song.firstTrack);

    phazite::test::EnginePlayback playback;
    playback.play (0.01);
    host.getPlayedEvents();
    const auto before = host.getPlayedEvents().size();

    getAudioIO().takeMidi (juce::MidiMessage::controllerEvent (1, 21, 64));
    playback.play (0.05);
    CHECK (host.getPlayedEvents().size() == before);

    // An unlinked one plays live, as before.
    getAudioIO().takeMidi (juce::MidiMessage::controllerEvent (1, 22, 64));
    playback.play (0.05);
    const auto& played = host.getPlayedEvents();
    REQUIRE (played.size() == before + 1);
    CHECK (played.back().event.type == phazite::devices::NoteEvent::Type::controller);
    CHECK (played.back().event.number == 22);
    phazite::test::dispatchPendingMessages();
}

TEST_CASE ("Another project opened ends learning, and its own links are diverted, not the first's")
{
    SynthSong s;
    s.link (s.parameter ("cutoff"), { 1, 21 });
    auto& tap = getAudioIO().getMidiTap();
    CHECK (tap.isDiverted (1, 21));
    phazite::startMidiLearn (s.parameter ("resonance"));
    CHECK (tap.isLearning());

    s.song.openSong (phazite::createSong()->getState());
    CHECK_FALSE (phazite::isLearningMidi (s.song.context));
    CHECK_FALSE (tap.isLearning());
    CHECK_FALSE (tap.isDiverted (1, 21));

    // A link of the song open now is diverted.
    const auto synth = s.song.setInstrument (getTrackIDs (getSong (s.song)).front(), phazite::devices::Synth::typeID);
    auto resonance = phazite::getDeviceHandle (s.song.context, synth).getParameter ("resonance");
    phazite::startMidiLearn (resonance);
    sendController (2, 5, 0);
    CHECK (tap.isDiverted (2, 5));
    CHECK_FALSE (tap.isDiverted (1, 21));
}

TEST_CASE ("While a track writes automation and the song plays, a knob's burst writes as a control's drag does")
{
    SynthSong s;
    auto& song = s.song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getVolumeParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    auto volume = phazite::getVolume (song.context, track);
    s.link (volume, { 1, 7 });

    phazite::test::EnginePlayback playback;
    phazite::test::HeldClock clock;
    song.run ("transport.playStop");
    playback.play (0.5); // to beat 1
    sendController (1, 7, 38);
    playback.play (0.5);
    sendController (1, 7, 76);
    playback.play (0.5);
    s.getLinker().endBurst(); // the knob rested
    playback.play (0.5);
    CHECK (volume.getValue() == doctest::Approx (0.5f)); // the lane plays again
    song.run ("transport.playStop");

    const auto points = phazite::getAutomationPoints (song.context, lane);
    REQUIRE (points.size() >= 5);
    CHECK (points[1].beat.inBeats() == doctest::Approx (1.0).epsilon (0.05));
    const auto has = [&points] (float position)
    {
        return std::any_of (points.begin(), points.end(), [position] (const auto& p) { return std::abs (p.position - position) < 0.001f; });
    };
    CHECK (has (38.0f / 127.0f));
    CHECK (has (76.0f / 127.0f));

    // One step takes it back, the knob's moves with it.
    CHECK (phazite::describeUndo (&getSong (song)) == "Undo Change Volume");
    phazite::undo (getSong (song));
    CHECK (phazite::getAutomationPoints (song.context, lane).size() == 2);
}

TEST_CASE ("MIDI Learn Last Touched Parameter learns the parameter changed last, and cancels while one learns")
{
    SynthSong s;
    CHECK_FALSE (phazite::canLearnLastTouched (s.song.context));
    auto cutoff = s.parameter ("cutoff");
    cutoff.setValue (1000.0f);
    CHECK (phazite::test::dispatchMessagesUntil ([&s] { return phazite::canLearnLastTouched (s.song.context); }));
    CHECK (phazite::describeLearnLastTouched (s.song.context) == "MIDI Learn Filter Cutoff");

    phazite::learnLastTouched (s.song.context);
    CHECK (phazite::isLearningMidi (cutoff));
    CHECK (phazite::describeLearnLastTouched (s.song.context) == "Cancel MIDI Learn");
    phazite::learnLastTouched (s.song.context);
    CHECK_FALSE (phazite::isLearningMidi (s.song.context));

    // Learning ends as its parameter goes.
    phazite::startMidiLearn (cutoff);
    phazite::removeDevice (s.song.context, s.synth);
    CHECK_FALSE (phazite::isLearningMidi (s.song.context));
}

TEST_CASE ("A link made is a change of the song's own in its versions: \"Update MIDI links\"")
{
    SynthSong s;
    const auto before = getSong (s.song).getState().createCopy();
    s.link (s.parameter ("cutoff"), { 1, 21 });
    const auto after = getSong (s.song).getState().createCopy();

    CHECK (phazite::findSongChange (before, after).what.contains ("MIDI links"));
    CHECK (phazite::summarise (phazite::findChanges (before, after)) == "Update MIDI links");
    CHECK (phazite::findChanges (after, after).empty());
}

TEST_CASE ("A burst that changed nothing begins no step: a drag begun before the knob rests is one step, which Undo takes back")
{
    // A knob linked to a switch that is on already, moved within its top half, while the song plays; then a control dragged
    // before the knob rests. The knob's rest, during the drag, must not begin a step that takes the rest of the drag.
    SynthSong s;
    auto osc2 = s.parameter ("osc2On");
    osc2.setValue (1.0f);
    s.link (osc2, { 1, 30 });
    auto cutoff = s.parameter ("cutoff");
    const auto before = cutoff.getValue();
    auto& song = getSong (s.song);

    phazite::test::EnginePlayback playback;
    phazite::test::HeldClock clock;
    s.song.run ("transport.playStop");
    playback.play (0.1);
    const auto steps = phazite::getHistorySteps (song).done.size();

    sendController (1, 30, 100);
    CHECK (osc2.getValue() == doctest::Approx (1.0f));
    CHECK (s.getLinker().isBurstOpen());
    CHECK_FALSE (s.getLinker().isInBurst());

    cutoff.beginGesture();
    cutoff.setValue (500.0f);
    clock.advance (phazite::MidiLinker::restMilliseconds);
    CHECK (phazite::test::runTimersUntil ([&s] { return ! s.getLinker().isBurstOpen(); }));
    cutoff.setValue (600.0f);
    cutoff.endGesture();
    s.song.run ("transport.playStop");

    CHECK (phazite::getHistorySteps (song).done.size() == steps + 1);
    CHECK (phazite::describeUndo (&song) == "Undo Change Filter Cutoff");
    phazite::undo (song);
    CHECK (cutoff.getValue() == doctest::Approx (before));
}

TEST_CASE ("While the window is held for work a knob changes nothing and learning waits; after the hold both work again")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    auto resonance = s.parameter ("resonance");
    s.link (cutoff, { 1, 21 });
    auto& song = getSong (s.song);
    const auto before = cutoff.getValue();
    const auto steps = phazite::getHistorySteps (song).done.size();

    auto hold = s.song.context.holdForWork ("Reading", [] (const juce::String&) {});
    sendController (1, 21, 127);
    CHECK (cutoff.getValue() == doctest::Approx (before));
    CHECK_FALSE (s.getLinker().isBurstOpen());

    phazite::startMidiLearn (resonance);
    sendController (1, 22, 0);
    CHECK (phazite::isLearningMidi (resonance));
    CHECK_FALSE (phazite::getMidiLink (resonance).has_value());
    CHECK (phazite::getHistorySteps (song).done.size() == steps);

    hold = nullptr;
    sendController (1, 22, 0);
    CHECK (phazite::getMidiLink (resonance) == MidiController { 1, 22 });
    sendController (1, 21, 127);
    CHECK (cutoff.getValue() == doctest::Approx (cutoff.getRange().end));
    s.getLinker().endBurst();
}

TEST_CASE ("The link is made before learning ends: what the knob sends as the window hears of it never reaches the instrument")
{
    SynthSong s;
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    auto* session = phazite::getSession (s.song.context);
    REQUIRE (session != nullptr);
    session->setLiveTrack (s.song.firstTrack);

    phazite::test::EnginePlayback playback;
    playback.play (0.01);
    host.getPlayedEvents();
    const auto before = host.getPlayedEvents().size();

    // The window lays out its notice as learning ends, while the knob is still being swept.
    struct Sweeping final : phazite::ProjectContext::Listener
    {
        explicit Sweeping (const phazite::ProjectContext& c)
            : context (c)
        {
        }

        void projectChanged() override {}

        void midiLearnChanged() override
        {
            if (! sent && ! phazite::isLearningMidi (context))
            {
                sent = true;
                getAudioIO().takeMidi (juce::MidiMessage::controllerEvent (1, 21, 100));
            }
        }

        const phazite::ProjectContext& context;
        bool sent = false;
    };

    auto cutoff = s.parameter ("cutoff");
    Sweeping sweeping (s.song.context);
    s.song.context.addListener (&sweeping);
    phazite::startMidiLearn (cutoff);
    sendController (1, 21, 0);
    phazite::test::dispatchPendingMessages();
    s.song.context.removeListener (&sweeping);

    CHECK (sweeping.sent);
    CHECK (phazite::getMidiLink (cutoff) == MidiController { 1, 21 });
    CHECK (cutoff.getValue() == doctest::Approx (phazite::fromControlPosition (cutoff.getRange(), 100.0f / 127.0f)));
    playback.play (0.05);
    CHECK (host.getPlayedEvents().size() == before);
    s.getLinker().endBurst();
}

TEST_CASE ("A knob turned during a control's drag joins the drag's step: its Escape takes both back, leaving nothing open")
{
    SynthSong s;
    auto cutoff = s.parameter ("cutoff");
    auto resonance = s.parameter ("resonance");
    auto shape = s.parameter ("subShape");
    s.link (resonance, { 1, 22 });
    const auto cutoffBefore = cutoff.getValue(), resonanceBefore = resonance.getValue();
    auto& song = getSong (s.song);
    const auto steps = phazite::getHistorySteps (song).done.size();
    phazite::test::HeldClock clock;

    cutoff.beginGesture();
    cutoff.setValue (500.0f);
    sendController (1, 22, 120);
    CHECK_FALSE (s.getLinker().isBurstOpen());
    CHECK (resonance.getValue() != doctest::Approx (resonanceBefore));

    // A link learned meanwhile is in the drag's step too.
    phazite::startMidiLearn (shape);
    sendController (1, 40, 0);
    CHECK (phazite::getMidiLink (shape) == MidiController { 1, 40 });

    cutoff.cancelGesture();
    CHECK (cutoff.getValue() == doctest::Approx (cutoffBefore));
    CHECK (resonance.getValue() == doctest::Approx (resonanceBefore));
    CHECK_FALSE (phazite::getMidiLink (shape).has_value());
    CHECK (phazite::getHistorySteps (song).done.size() == steps);
    CHECK_FALSE (phazite::canRedo (song));
    CHECK_FALSE (s.getLinker().isBurstOpen());

    // The knob turned later is a burst, and a step, of its own.
    sendController (1, 22, 60);
    CHECK (s.getLinker().isInBurst());
    clock.advance (phazite::MidiLinker::restMilliseconds);
    CHECK (phazite::test::runTimersUntil ([&s] { return ! s.getLinker().isBurstOpen(); }));
    CHECK (phazite::getHistorySteps (song).done.size() == steps + 1);
    CHECK (phazite::describeUndo (&song) == "Undo Change " + resonance.getName());
}

TEST_CASE ("A knob's burst is named as a control's change of the same parameter is")
{
    SynthSong s;
    const auto other = s.song.addTrack ("Other");
    const auto returnTrack = s.song.addReturn (other, phazite::devices::Reverb::typeID);
    auto send = phazite::getSendLevelTo (s.song.context, s.song.firstTrack, returnTrack);
    send.setValue (0.5f);
    REQUIRE (send.exists());
    auto& song = getSong (s.song);
    auto number = 0;

    const auto check = [&s, &song, &number] (const juce::String& name, phazite::ParameterHandle handle)
    {
        INFO (name.toStdString());
        ++number;
        s.link (handle, { 1, number });
        sendController (1, number, 100);
        s.getLinker().endBurst();
        const auto burst = phazite::describeUndo (&song);
        handle.setValue (handle.getRange().start);
        CHECK (burst == phazite::describeUndo (&song));
        CHECK (burst == "Undo " + name);
    };

    check ("Change Master Volume", phazite::getMasterVolume (s.song.context));
    check ("Change Volume", phazite::getVolume (s.song.context, s.song.firstTrack));
    check ("Change Pan", phazite::getPan (s.song.context, s.song.firstTrack));
    check ("Change Send Level", std::move (send));
    check ("Change Filter Cutoff", s.parameter ("cutoff"));
}

TEST_CASE ("A device removed during a burst: the burst's step, then the removal's, and Undo twice brings the device and its value back")
{
    SynthSong s;
    s.link (s.parameter ("cutoff"), { 1, 21 });
    const auto before = s.parameter ("cutoff").getValue();
    auto& song = getSong (s.song);

    sendController (1, 21, 120);
    const auto turned = s.parameter ("cutoff").getValue();
    CHECK (turned != doctest::Approx (before));
    phazite::removeDevice (s.song.context, s.synth);
    CHECK_FALSE (s.getLinker().isBurstOpen());
    const auto done = phazite::getHistorySteps (song).done;
    REQUIRE (done.size() >= 2);
    CHECK (done[done.size() - 2] == "Change Filter Cutoff");
    CHECK (done[done.size() - 1] == "Remove Synth");
    sendController (1, 21, 10); // its link went with it: nothing to turn

    phazite::undo (song);
    CHECK (s.parameter ("cutoff").getValue() == doctest::Approx (turned));
    CHECK (phazite::getMidiLink (s.parameter ("cutoff")) == MidiController { 1, 21 });
    phazite::undo (song);
    CHECK (s.parameter ("cutoff").getValue() == doctest::Approx (before));
}

TEST_CASE ("The project closed during a burst: the burst is kept with its song, and the next project's linker has none open")
{
    SynthSong s;
    s.link (s.parameter ("cutoff"), { 1, 21 });
    sendController (1, 21, 120);
    REQUIRE (s.getLinker().isInBurst());

    s.song.openSong (phazite::createSong()->getState());
    auto* linker = phazite::findMidiLinker (getSong (s.song));
    REQUIRE (linker != nullptr);
    CHECK_FALSE (linker->isBurstOpen());
    sendController (1, 21, 10); // no link in this song
    CHECK_FALSE (linker->isBurstOpen());
}

TEST_CASE ("A knob turned while a wheel's burst or a Swap's trial is open: each is a step of its own, in order")
{
    SynthSong s;
    s.link (s.parameter ("cutoff"), { 1, 21 });
    const auto clip = s.song.addMidiClip (s.song.firstTrack, 0.0, 4.0);
    const auto note = phazite::addNote (s.song.context, clip, 60, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (1.0), 60);
    REQUIRE (note.isValid());
    auto& song = getSong (s.song);
    song.getUndoManager().clearUndoHistory();
    // The steps to undo, the newest first.
    const auto history = [&song] { return song.getUndoManager().getUndoDescriptions().joinIntoString (", ").toStdString(); };

    SUBCASE ("The wheel's burst")
    {
        phazite::VelocityWheel wheel (s.song.context);
        wheel.change ({ note }, 4);
        sendController (1, 21, 100); // keeps the wheel's burst first
        wheel.change ({ note }, 4);  // keeps the knob's first
        wheel.end();
        s.getLinker().endBurst();
        CHECK (history() == "Change Velocity, Change Filter Cutoff, Change Velocity");
    }

    SUBCASE ("A Swap's trial")
    {
        phazite::Trial swap (song, "Swap Preset", nullptr);
        swap.apply ([&] { phazite::setTrackName (song, s.song.firstTrack, "Tried"); }, true);
        sendController (1, 21, 100); // keeps the Swap's first
        s.getLinker().endBurst();
        swap.putBack(); // kept already: nothing to put back
        CHECK (history() == "Change Filter Cutoff, Swap Preset");
        CHECK (phazite::getTrackName (song, s.song.firstTrack) == "Tried");
    }
}

TEST_CASE ("An undo during a burst that writes automation ends its hold: the track writes no more without a new touch")
{
    SynthSong s;
    auto& song = s.song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getVolumeParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    s.link (phazite::getVolume (song.context, track), { 1, 7 });
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);

    phazite::test::EnginePlayback playback;
    phazite::test::HeldClock clock;
    song.run ("transport.playStop");
    playback.play (0.5);
    sendController (1, 7, 38);
    playback.play (0.5);
    CHECK (session->getWriter().isWriting (lane));

    phazite::undo (getSong (song)); // keeps the burst first, then takes it back
    CHECK_FALSE (s.getLinker().isBurstOpen());
    CHECK_FALSE (session->getWriter().isWriting (lane));
    playback.play (0.5);
    CHECK_FALSE (session->getWriter().isWriting (lane));
    song.run ("transport.playStop");
    CHECK (phazite::getAutomationPoints (song.context, lane).size() == 2);
}

TEST_CASE ("A link made makes the project unsaved, and Undo of it makes it saved again")
{
    SynthSong s;
    s.song.saveWithPluginStates();
    auto& project = s.song.getProject();
    REQUIRE_FALSE (project.hasUnsavedChanges());

    s.link (s.parameter ("cutoff"), { 1, 21 });
    CHECK (project.hasUnsavedChanges());

    // The song as saved again, as closing the project finds it, comparing the whole song with the saved copy.
    phazite::undo (getSong (s.song));
    CHECK_FALSE (project.checkUnsavedChanges());
}

TEST_CASE ("A take recorded while a linked controller moves holds none of its values, and holds an unlinked controller's")
{
    SynthSong s;
    s.song.setTempo (120.0);
    s.song.setSwitch ("transport.countIn", false);
    phazite::routeLiveMidiInput (getSong (s.song)); // live MIDI plays the track with the synth
    s.link (s.parameter ("cutoff"), { 1, 21 });

    phazite::test::EnginePlayback playback;
    playback.play (0.01);
    REQUIRE (phazite::getLiveMidiTrack (getSong (s.song)) == s.song.firstTrack);
    s.song.run ("transport.record");
    playback.play (0.25);
    send (juce::MidiMessage::controllerEvent (1, 21, 100)); // as a MIDI input's thread hands it over
    send (juce::MidiMessage::controllerEvent (1, 22, 50));
    playback.play (0.5);
    s.song.run ("transport.record");

    const auto clips = phazite::getClips (s.song.context, s.song.firstTrack);
    REQUIRE (clips.size() == 1);
    std::vector<int> numbers;

    for (const auto& child : phazite::findClipState (getSong (s.song), clips[0].id))
        if (child.hasType (phazite::schema::CONTROL))
            numbers.push_back ((int) child[phazite::schema::controlNumber]);

    CHECK (std::find (numbers.begin(), numbers.end(), 22) != numbers.end());
    CHECK (std::find (numbers.begin(), numbers.end(), 21) == numbers.end());
}

TEST_CASE ("The MIDI tap's queue full: what doesn't fit is counted, and what was taken comes out in order")
{
    // No message loop turns meanwhile: nothing reads the queue until it is full, and well past it.
    auto tap = std::make_unique<phazite::MidiTap>();
    tap->setDiverted ({ { 1, 21 } });
    const auto sent = (int) phazite::MidiTap::capacity + 100;
    auto taken = 0;

    for (int i = 0; i < sent; ++i)
        taken += tap->take (juce::MidiMessage::controllerEvent (1, 21, i % 128)) ? 1 : 0;

    CHECK (taken == sent); // each taken from what plays, room or none

    auto popped = 0;
    auto inOrder = true;

    for (phazite::MidiTap::Value value; tap->pop (value); ++popped)
        inOrder = inOrder && value.value == popped % 128;

    CHECK (popped > 0);
    CHECK (inOrder);
    CHECK ((int) tap->getDropped() == sent - popped);
    CHECK (tap->getDropped() >= 100);
}

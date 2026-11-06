// SPDX-License-Identifier: AGPL-3.0-or-later
// The transport and the tempo as the top bar reaches them, by plain values (src/model/transport/TransportModel.h), and
// the engine as the UI reaches it (src/model/engine/EngineContext.h).
#include "TestHelpers.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

TEST_CASE ("The top bar's tempo is a drag of one undo step, or a step for each change outside a drag")
{
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    auto& undo = song->getUndoManager();

    CHECK_FALSE (phazite::getTempoHandle (context).isValid()); // no project open
    CHECK (phazite::getTempoHandle (context).getValue() == doctest::Approx (120.0));

    setSong (context, song.get());
    auto tempo = phazite::getTempoHandle (context);
    REQUIRE (tempo.isValid());
    undo.clearUndoHistory();

    tempo.beginGesture();
    CHECK (tempo.isInGesture());
    tempo.setValue (100.0);
    tempo.setValue (90.0);
    tempo.endGesture();
    CHECK_FALSE (tempo.isInGesture());
    CHECK (tempo.getValue() == doctest::Approx (90.0));
    CHECK (undo.getUndoDescription() == juce::String ("Change Tempo"));
    phazite::undo (*song);
    CHECK (phazite::getTempo (*song) == doctest::Approx (120.0)); // the whole drag at once
    CHECK_FALSE (undo.canUndo());

    tempo.setValue (140.0); // typed: a step of its own
    CHECK (phazite::getTempo (*song) == doctest::Approx (140.0));
    CHECK (undo.canUndo());

    // A drag in progress when another project opens is kept, and the handle then has nothing.
    tempo.beginGesture();
    tempo.setValue (150.0);
    tempo = phazite::TempoHandle();
    CHECK (phazite::getTempo (*song) == doctest::Approx (150.0));
    CHECK_FALSE (tempo.isValid());
    tempo.setValue (60.0);
    CHECK (phazite::getTempo (*song) == doctest::Approx (150.0));
    setSong (context, nullptr);
}

TEST_CASE ("The transport's switches and the time signature by the project's context")
{
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };

    CHECK_FALSE (phazite::isPlaying (context));
    CHECK_FALSE (phazite::isLoopOn (context));
    CHECK_FALSE (phazite::getTimeSignature (context).has_value());
    CHECK_FALSE (phazite::getLiveMidiTrack (context).isValid());
    phazite::setTimeSignature (context, { 3, 4 }); // no project: nothing to change

    setSong (context, song.get());
    CHECK (phazite::isLoopOn (context) == phazite::isLoopOn (*song));
    phazite::toggleLoop (*song);
    CHECK (phazite::isLoopOn (context) == phazite::isLoopOn (*song));

    CHECK (phazite::getTimeSignature (context) == phazite::TimeSignature { 4, 4 });
    phazite::setTimeSignature (context, { 6, 8 });
    CHECK (phazite::getTimeSignature (context) == phazite::TimeSignature { 6, 8 });
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Change Time Signature"));
    setSong (context, nullptr);
}

TEST_CASE ("A drag of the playhead pauses playback where it is and plays on from where the playhead is let go")
{
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    CHECK_FALSE (phazite::pausePlayback (context)); // no project
    setSong (context, song.get());
    song->getUndoManager().clearUndoHistory();

    // Stopped: nothing to pause, and nothing to play on afterwards.
    CHECK_FALSE (phazite::pausePlayback (context));

    // Playing from beat 2: paused, not back where playback started; the playhead moved meanwhile; then playing on from there.
    phazite::movePlayhead (*song, phazite::BeatPosition::fromBeats (2.0));
    phazite::playOrStop (*song);
    REQUIRE (phazite::isPlaying (context));
    CHECK (phazite::pausePlayback (context));
    CHECK_FALSE (phazite::isPlaying (context));
    phazite::movePlayhead (*song, phazite::BeatPosition::fromBeats (6.0));
    phazite::resumePlayback (context);
    CHECK (phazite::isPlaying (context));
    CHECK (phazite::getPlayheadBeat (*song).inBeats() >= 6.0 - 1.0e-6);
    CHECK (phazite::getPlayheadBeat (*song).inBeats() < 7.0);
    phazite::playOrStop (*song);
    CHECK_FALSE (song->getUndoManager().canUndo()); // none of it an undo step
    setSong (context, nullptr);
}

TEST_CASE ("The engine as the UI reaches it: its audio devices, how busy it is, and the MIDI keyboards connected")
{
    auto& engineContext = phazite::test::getEngineContext();

    // The device manager the audio settings show is the one Phazite opens its device on.
    CHECK (&engineContext.getAudioDeviceManager() == &phazite::getAudioIO (engineContext).getDevices());
    CHECK (engineContext.getCpuUsage() >= 0.0);

    // The MIDI devices connected, as JUCE lists them, each by its ID.
    const auto connected = juce::MidiInput::getAvailableDevices();
    CHECK (engineContext.getMidiInputs().size() == (size_t) connected.size());

    for (const auto& input : engineContext.getMidiInputs())
    {
        CAPTURE (input.name.toStdString());
        CHECK (input.id.isNotEmpty());
        CHECK (input.isOn); // on, unless switched off
    }

    engineContext.setMidiInputOn ("no such device", false); // changes nothing
    const auto after = engineContext.getMidiInputs();
    CHECK (std::all_of (after.begin(), after.end(), [] (const auto& input) { return input.isOn; }));
}

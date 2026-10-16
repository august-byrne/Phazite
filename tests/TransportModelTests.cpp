// SPDX-License-Identifier: AGPL-3.0-or-later
// The transport and the tempo as the top bar reaches them, by plain values (src/model/transport/TransportModel.h), and
// the engine as the UI reaches it (src/model/engine/EngineContext.h).
#include "TestHelpers.h"
#include "model/commands/Undo.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/ProjectContext.h"
#include "model/transport/Playback.h"
#include "model/transport/Tempo.h"
#include "model/transport/TransportModel.h"

TEST_CASE ("The top bar's tempo is a drag of one undo step, or a step for each change outside a drag")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngine() };
    auto& undo = edit->getUndoManager();

    CHECK_FALSE (phazite::getTempoHandle (context).isValid()); // no project open
    CHECK (phazite::getTempoHandle (context).getValue() == doctest::Approx (120.0));

    context.setEdit (edit.get());
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
    phazite::undo (*edit);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (120.0)); // the whole drag at once
    CHECK_FALSE (undo.canUndo());

    tempo.setValue (140.0); // typed: a step of its own
    CHECK (phazite::getTempo (*edit) == doctest::Approx (140.0));
    CHECK (undo.canUndo());

    // A drag in progress when another project opens is kept, and the handle then has nothing.
    tempo.beginGesture();
    tempo.setValue (150.0);
    tempo = phazite::TempoHandle();
    CHECK (phazite::getTempo (*edit) == doctest::Approx (150.0));
    CHECK_FALSE (tempo.isValid());
    tempo.setValue (60.0);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (150.0));
    context.setEdit (nullptr);
}

TEST_CASE ("The transport's switches and the time signature by the project's context")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngine() };

    CHECK_FALSE (phazite::isPlaying (context));
    CHECK_FALSE (phazite::isLoopOn (context));
    CHECK_FALSE (phazite::getTimeSignature (context).has_value());
    CHECK_FALSE (phazite::getLiveMidiTrack (context).isValid());
    phazite::setTimeSignature (context, { 3, 4 }); // no project: nothing to change

    context.setEdit (edit.get());
    CHECK (phazite::isLoopOn (context) == phazite::isLoopOn (*edit));
    phazite::toggleLoop (*edit);
    CHECK (phazite::isLoopOn (context) == phazite::isLoopOn (*edit));

    CHECK (phazite::getTimeSignature (context) == phazite::TimeSignature { 4, 4 });
    phazite::setTimeSignature (context, { 6, 8 });
    CHECK (phazite::getTimeSignature (context) == phazite::TimeSignature { 6, 8 });
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Time Signature"));
    context.setEdit (nullptr);
}

TEST_CASE ("The engine as the UI reaches it: its audio devices, how busy it is, and the MIDI keyboards connected")
{
    auto& engine = phazite::test::getEngine();
    const auto engineContext = phazite::createEngineContext (engine);

    CHECK (&engineContext->getAudioDeviceManager() == &engine.getDeviceManager().deviceManager);
    CHECK (engineContext->getCpuUsage() >= 0.0);

    // Only the devices connected: Phazite's own inputs (the computer keyboard's, and the tests') aren't listed.
    for (const auto& input : engineContext->getMidiInputs())
    {
        CAPTURE (input.name.toStdString());
        CHECK (input.id.isNotEmpty());

        for (const auto& device : engine.getDeviceManager().getMidiInDevices())
            if (device->getDeviceID() == input.id)
                CHECK (device->getDeviceType() == te::InputDevice::physicalMidiDevice);
    }

    engineContext->setMidiInputOn ("no such device", true); // changes nothing
}

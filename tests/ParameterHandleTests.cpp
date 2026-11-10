// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter as controls use it (src/model/devices/ParameterHandle.h): its value, range and default, and its changes
// as named undo steps that the model, not the control, begins.
#include "GoldenFiles.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/reverb/Reverb.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
// A reverb at the end of the song's first track; its ID.
phazite::DeviceID addReverb (phazite::Song& song)
{
    const auto reverb = phazite::insertDeviceState (song, phazite::getTrackIDs (song)[0],
                                                    phazite::makeOwnDeviceState (song, phazite::devices::Reverb::typeID));
    REQUIRE (reverb.isValid());
    return reverb;
}
} // namespace

TEST_CASE ("A parameter handle shows its parameter, and nothing once the plug-in is gone")
{
    const phazite::ParameterHandle empty;
    CHECK_FALSE (empty.isValid());
    CHECK (empty.getValue() == doctest::Approx (0.0f));

    // The plug-in plays in the session of the context that holds its song.
    phazite::test::TestPlugin testPlugin;
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::setSong (context, song.get());
    const auto plugin = phazite::test::TestPlugin::add (*song, phazite::getTrackIDs (*song)[0]);
    auto handle = phazite::makeParameterHandle (*song, { plugin, phazite::test::TestPlugin::toneID });

    REQUIRE (handle.isValid());
    CHECK (
        handle.refersToSameParameter (phazite::makeParameterHandle (*song, { plugin, phazite::test::TestPlugin::toneID }, "Another Name")));
    CHECK_FALSE (handle.refersToSameParameter (phazite::makeParameterHandle (*song, { plugin, phazite::test::TestPlugin::gainID })));
    CHECK_FALSE (handle.refersToSameParameter (empty));
    CHECK (empty.refersToSameParameter (phazite::ParameterHandle()));
    CHECK (handle.getName() == juce::String ("Tone"));
    CHECK (handle.getRange().start == doctest::Approx (0.0f));
    CHECK (handle.getRange().end == doctest::Approx (1.0f));
    CHECK (handle.getDefaultValue().value_or (-1.0f) == doctest::Approx (0.5f));
    CHECK (handle.getValue() == doctest::Approx (0.5f));
    CHECK (handle.getText (0.25f) == juce::String ("25 %")); // as the plug-in writes it, in its helper
    CHECK_FALSE (handle.getInfo().has_value());              // not one of Phazite's own devices

    // And text typed as the plug-in reads it, in its helper: a percentage, where a number alone would be the whole range's.
    CHECK (handle.readText ("75 %").value_or (-1.0f) == doctest::Approx (0.75f));
    CHECK (handle.readText ("40").value_or (-1.0f) == doctest::Approx (0.4f));

    // When the project closes, the handle finds nothing and changes nothing.
    phazite::setSong (context, nullptr);
    song = nullptr;
    CHECK_FALSE (handle.isValid());
    CHECK (handle.getName().isEmpty());
    handle.beginGesture();
    handle.setValue (0.5f);
    handle.endGesture();
    CHECK_FALSE (handle.isInGesture());
    CHECK_FALSE (handle.readText ("75 %").has_value());
}

TEST_CASE ("A parameter handle reads a value typed as its text writes it, in its units")
{
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::setSong (context, song.get());
    const auto track = phazite::getTrackIDs (*song)[0];
    const auto read = [] (const phazite::ParameterHandle& h, const juce::String& text) { return h.readText (text).value_or (-99.0f); };

    // A fader's decibels, as its position, kept to its range.
    const auto volume = phazite::getVolume (context, track);
    CHECK (read (volume, "-6 dB") == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)));
    CHECK (read (volume, volume.getText (phazite::decibelsToFaderPosition (-12.0f)))
           == doctest::Approx (phazite::decibelsToFaderPosition (-12.0f)).epsilon (0.001));
    CHECK (read (volume, "-inf") == doctest::Approx (0.0f));
    CHECK (read (volume, "+60") == doctest::Approx (volume.getRange().end));
    CHECK_FALSE (volume.readText ("loud").has_value());

    // A pan as it writes it, a fraction, or as a percentage.
    const auto pan = phazite::getPan (context, track);
    CHECK (read (pan, pan.getText (-0.3f)) == doctest::Approx (-0.3f));
    CHECK (read (pan, "30% left") == doctest::Approx (-0.3f));
    CHECK (read (pan, "25R") == doctest::Approx (0.25f));
    CHECK (read (pan, "-50") == doctest::Approx (-0.5f));
    CHECK (read (pan, "Centre") == doctest::Approx (0.0f));

    // One of Phazite's devices': a choice by its name, any other as its text's number.
    const auto reverb = addReverb (*song);

    for (const auto& parameter : phazite::devices::Reverb::info.parameters)
    {
        const auto id = juce::String (parameter.id.data(), parameter.id.size());
        CAPTURE (id.toStdString());
        const auto handle = phazite::makeParameterHandle (*song, { reverb, id });
        const auto value = parameter.unit == phazite::devices::Unit::choice ? parameter.maximum : parameter.defaultValue;
        CHECK (read (handle, handle.getText (value)) == doctest::Approx (value).epsilon (0.001));
    }

    phazite::setSong (context, nullptr);
}

TEST_CASE ("Changes through a parameter handle are named undo steps: one per gesture, and one per single change")
{
    auto song = phazite::createSong();
    const auto reverb = addReverb (*song);
    song->getUndoManager().clearUndoHistory();

    auto handle = phazite::makeParameterHandle (*song, { reverb, "size" }); // its steps are called "Change Size"

    // A drag: every value it passes through is one step, however long it takes.
    handle.beginGesture();
    CHECK (handle.isInGesture());
    handle.setValue (0.4f);
    phazite::test::settle (500);
    handle.setValue (0.6f);
    handle.setValue (0.8f);
    handle.endGesture();
    CHECK_FALSE (handle.isInGesture());
    CHECK (handle.getValue() == doctest::Approx (0.8f));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Change Size"));

    phazite::undo (*song);
    CHECK (handle.getValue() == doctest::Approx (0.5f)); // where the drag began
    phazite::redo (*song);
    CHECK (handle.getValue() == doctest::Approx (0.8f));

    // A key press: a step of its own, so undo goes back that far only.
    handle.setValue (0.5f);
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Change Size"));
    phazite::undo (*song);
    CHECK (handle.getValue() == doctest::Approx (0.8f));

    // A name given for the steps.
    auto damping = phazite::makeParameterHandle (*song, { reverb, "damping" }, "Change Reverb Damping");
    damping.setValue (9000.0f);
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Change Reverb Damping"));

    // A handle that goes away during a drag keeps the drag's change.
    {
        auto width = phazite::makeParameterHandle (*song, { reverb, "width" });
        width.beginGesture();
        width.setValue (0.25f);
    }

    CHECK (phazite::makeParameterHandle (*song, { reverb, "width" }).getValue() == doctest::Approx (0.25f));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Change Width"));
}

TEST_CASE ("A parameter handle describes the parameters of Phazite's own devices")
{
    auto song = phazite::createSong();
    const auto device = phazite::test::addTestDevice (*song, phazite::getTrackIDs (*song)[0], phazite::test::TestGain::typeID);

    const auto mode = phazite::makeParameterHandle (*song, { device, "mode" });
    const auto info = mode.getInfo();
    REQUIRE (info.has_value());
    CHECK (info->unit == phazite::devices::Unit::choice);
    REQUIRE (info->choices.size() == 2);
    CHECK (info->choices[1] == "Inverted");
    CHECK (mode.getRange().interval == doctest::Approx (1.0f));

    const auto gain = phazite::makeParameterHandle (*song, { device, "gain" });
    CHECK (gain.getRange().start == doctest::Approx (-60.0f));
    CHECK (gain.getInfo()->unit == phazite::devices::Unit::decibels);
}

TEST_CASE ("A gesture begun during another for the same project joins it: a drag that moves two parameters is one step")
{
    auto song = phazite::createSong();
    const auto reverb = addReverb (*song);
    song->getUndoManager().clearUndoHistory();

    auto size = phazite::makeParameterHandle (*song, { reverb, "size" }, "Move Both");
    auto damping = phazite::makeParameterHandle (*song, { reverb, "damping" });
    size.beginGesture();
    damping.beginGesture();
    CHECK (size.isInGesture());
    CHECK (damping.isInGesture());
    size.setValue (0.9f);
    damping.setValue (2000.0f);
    damping.endGesture();
    size.endGesture();
    CHECK_FALSE (damping.isInGesture());

    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Move Both"));
    phazite::undo (*song);
    CHECK (size.getValue() == doctest::Approx (0.5f));
    CHECK (damping.getValue() == doctest::Approx (6000.0f));
}

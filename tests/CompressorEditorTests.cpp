// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (src/ui/devices/CompressorEditor.h): its characters' buttons, its knobs, its curve (the multiband's,
// src/ui/devices/DynamicsCurve.h) lit up to the level it hears, its meter both ways, and its key's cuts with Listen.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DeviceEditors.h"

#include <vector>

namespace
{
using phazite::devices::Compressor;
namespace curves = phazite::devices::curves;

// Three tracks, Kick, Bass and Keys, with a compressor on the bass.
struct Song
{
    Song()
    {
        phazite::ensureTracks (*edit, 3);
        const auto tracks = te::getAudioTracks (*edit);
        phazite::setTrackName (*tracks[0], "Kick");
        phazite::setTrackName (*tracks[1], "Bass");
        phazite::setTrackName (*tracks[2], "Keys");
        compressor = edit->getPluginCache().createNewPlugin (Compressor::typeID, {});
        REQUIRE (phazite::insertDevice (*tracks[1], compressor) != nullptr);
        phazite::test::settle();
        phazite::getSongUndoManager (*edit).clearUndoHistory();
    }

    float valueOf (const char* id) const { return compressor->getAutomatableParameterByID (id)->getCurrentValue(); }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
    te::Plugin::Ptr compressor;
};

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// How far its curve turns a level down, at its default settings, once settled.
float getDefaultReduction (float decibels)
{
    const auto values = getDefaults();
    return decibels
           - 20.0f * std::log10 (curves::fromAxis (curves::getAt (Compressor::getCurve (values), curves::axisForDecibels (decibels))));
}

// Plays a steady level through the compressor's DSP, at its default settings or at the ones given, as the audio thread would.
void play (Compressor& compressor, float decibels, int numSamples, std::vector<float> values = getDefaults())
{
    juce::AudioBuffer<float> buffer (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample (channel, i, (i % 2 == 0 ? 1.0f : -1.0f) * juce::Decibels::decibelsToGain (decibels));

    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = numSamples;
    block.parameters = values;
    compressor.process (block);
}
} // namespace

TEST_CASE ("The compressor's editor shows its characters, its knobs and its curve, whose corner drags as one undo step")
{
    Song song;
    const auto device = phazite::makeDeviceHandle (*song.compressor);
    CHECK (dynamic_cast<phazite::ui::CompressorEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    // A knob for each parameter but its character, a row of buttons, its curve's, and its mix, its header's.
    CHECK (phazite::test::findAll<phazite::ui::Knob> (editor).size() == Compressor::info.parameters.size() - 2 - curves::parameterCount);
    CHECK (editor.findControl ("character") != nullptr);
    CHECK (editor.findControl ("mix") == nullptr);
    CHECK (editor.findControl ("points") == nullptr);
    phazite::test::checkChildrenApart (editor);

    // The curve whole, square, beside its meter, over its tools.
    auto& curve = editor.getCurve();
    CHECK (curve.isShowing());
    CHECK (curve.getWidth() == curve.getHeight());
    CHECK (curve.getWidth() >= 150);

    // Its corner at -18 dB dragged left, level (Shift): it turns down from lower.
    phazite::test::Mouse mouse (curve);
    const auto corner = curve.getPointPosition (1).roundToInt();
    mouse.press (corner, juce::ModifierKeys::shiftModifier);
    mouse.dragTo (corner - juce::Point<int> (20, 0));
    mouse.release();
    CHECK (song.valueOf ("point2X") < curves::axisForDecibels (-18.0f) - 0.02f);
    CHECK (song.valueOf ("point2Y") == doctest::Approx (curves::axisForDecibels (-18.0f)).epsilon (1.0e-4));
    CHECK (phazite::describeUndo (song.edit.get()) == juce::String ("Undo Move Point"));

    phazite::undo (*song.edit);
    editor.refresh();
    CHECK (song.valueOf ("point2X") == doctest::Approx (curves::axisForDecibels (-18.0f)).epsilon (1.0e-4));
}

TEST_CASE ("The compressor's editor has its key's cuts, and Listen plays the key while it shows")
{
    Song song;
    phazite::ui::CompressorEditor editor ((phazite::makeDeviceHandle (*song.compressor)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (editor.findControl ("keyLowCut") != nullptr);
    CHECK (editor.findControl ("keyHighCut") != nullptr);

    // Off at first; on, the compressor plays its key, as long as the editor asks (each refresh).
    auto* compressor = phazite::makeDeviceHandle (*song.compressor).getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    editor.refresh();
    CHECK_FALSE (compressor->isPlayingKey());
    phazite::test::Mouse mouse (editor.getListenButton());
    mouse.click (editor.getListenButton().getLocalBounds().getCentre());
    CHECK (editor.getListenButton().getToggleState());
    editor.refresh();
    CHECK (compressor->isPlayingKey());
}

TEST_CASE ("The compressor's editor lights its curve up to the level it hears, and shows how far it turns the sound down or up")
{
    Song song;
    const auto device = phazite::makeDeviceHandle (*song.compressor);
    auto* compressor = device.getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);

    // What played before the editor opened doesn't show.
    play (*compressor, -6.0f, 4800);
    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    CHECK (editor.getShownReduction() == doctest::Approx (0.0f));
    CHECK (curve.getHeardX() == doctest::Approx (curve.getPosition (0.0f, 0.0f).x));

    // 12 dB over its corner, lit up to -6 dB and turned down as its curve says.
    play (*compressor, -6.0f, 24000);
    editor.refresh();
    CHECK (editor.getShownLevel() == doctest::Approx (-6.0f).epsilon (0.01));
    CHECK (curve.getHeardX() == doctest::Approx (curve.getPosition (curves::axisForDecibels (-6.0f), 0.0f).x).epsilon (0.01));
    CHECK (getDefaultReduction (-6.0f) > 8.0f);
    CHECK (editor.getShownReduction() == doctest::Approx (getDefaultReduction (-6.0f)).epsilon (0.01));
    CHECK (editor.getShownBoost() == doctest::Approx (0.0f));

    // Then, with nothing playing, the meters fall back smoothly.
    editor.refresh();
    CHECK (editor.getShownReduction() == doctest::Approx (getDefaultReduction (-6.0f) - 0.5f).epsilon (0.01));

    // Its curve lifting what is quiet: a quiet level comes out louder, and the meter says how far.
    REQUIRE (curve.applyShape ("Lift under -30 dB"));
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (song.valueOf (parameter.id.data()));

    play (*compressor, -45.0f, 24000, values);
    editor.refresh();
    CHECK (editor.getShownBoost() > 3.0f);
}

TEST_CASE ("The compressor's characters are a row of buttons: a click picks one as one undo step, and the one in use is lit")
{
    Song song;
    phazite::ui::CompressorEditor editor ((phazite::makeDeviceHandle (*song.compressor)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.refresh();
    CHECK (editor.getCharacterButton (0).getToggleState()); // Clean at first
    CHECK (editor.getCharacterButton (0).getButtonText() == juce::String ("Clean"));
    CHECK (editor.getCharacterButton (4).getButtonText() == juce::String ("Tube"));

    // Opto: a click, one undo step, and its button lit alone.
    phazite::test::Mouse mouse (editor.getCharacterButton (3));
    mouse.click (editor.getCharacterButton (3).getLocalBounds().getCentre());
    CHECK (song.valueOf ("character") == doctest::Approx ((float) Compressor::Character::opto));
    CHECK (phazite::describeUndo (song.edit.get()) == juce::String ("Undo Change Character"));
    CHECK (editor.getCharacterButton (3).getToggleState());
    CHECK_FALSE (editor.getCharacterButton (0).getToggleState());

    // Undone, the editor shows Clean again at its next refresh.
    phazite::undo (*song.edit);
    editor.refresh();
    CHECK (editor.getCharacterButton (0).getToggleState());
    CHECK_FALSE (editor.getCharacterButton (3).getToggleState());
}

TEST_CASE ("How far the compressor turns the sound down reads the same for any number of displays, then falls back")
{
    Song song;
    auto* compressor = phazite::makeDeviceHandle (*song.compressor).getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);
    const auto* reduction = compressor->getGainReduction();
    REQUIRE (reduction != nullptr);
    CHECK (reduction->get() == doctest::Approx (0.0f));

    // 12 dB over its corner, turned down as its curve says; reading it takes nothing away from the next reader.
    play (*compressor, -6.0f, 24000);
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto first = reduction->get();
    CHECK (first == doctest::Approx (getDefaultReduction (-6.0f)).epsilon (0.02));
    CHECK (reduction->get() == doctest::Approx (first).epsilon (0.01));

    // With nothing played since (a device asleep on silence plays nothing), it falls 15 dB a second.
    juce::Thread::sleep (200);
    const auto seconds = (float) ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0);
    CHECK (reduction->get() == doctest::Approx (first - 15.0f * seconds).epsilon (0.06));
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (src/ui/devices/CompressorEditor.h): its knobs, its curve, its meters, and its key's cuts with
// Listen.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DeviceEditors.h"

#include <vector>

namespace
{
using phazite::devices::Compressor;

// Three tracks, Kick, Bass and Keys, with a compressor on the bass.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (3);
        const auto tracks = te::getAudioTracks (*edit);
        tracks[0]->setName ("Kick");
        tracks[1]->setName ("Bass");
        tracks[2]->setName ("Keys");
        compressor = edit->getPluginCache().createNewPlugin (Compressor::typeID, {});
        REQUIRE (phazite::insertDevice (*tracks[1], compressor) != nullptr);
        phazite::test::settle();
        edit->getUndoManager().clearUndoHistory();
    }

    float valueOf (const char* id) const { return compressor->getAutomatableParameterByID (id)->getCurrentValue(); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr compressor;
};

// Plays a steady level through the compressor's DSP at its default settings, as the audio thread would.
void play (Compressor& compressor, float decibels, int numSamples)
{
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

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

TEST_CASE ("The compressor's editor shows its knobs, and dragging across its curve moves the threshold as one undo step")
{
    Song song;
    const auto device = phazite::makeDeviceHandle (*song.compressor);
    CHECK (dynamic_cast<phazite::ui::CompressorEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (phazite::test::findAll<phazite::ui::Knob> (editor).size() == Compressor::info.parameters.size() - 1); // its mix is its header's
    CHECK (editor.findControl ("mix") == nullptr);
    phazite::test::checkChildrenApart (editor);

    // Twelve decibels to the right, from the default -18 dB.
    phazite::test::Mouse mouse (editor.getGraph());
    const auto start = editor.getGraphPosition (-30.0f, -30.0f).roundToInt();
    const auto twelveDecibels = editor.getGraphPosition (-18.0f, -30.0f).roundToInt().x - start.x;
    mouse.press (start);
    mouse.dragTo (start + juce::Point<int> (twelveDecibels, 0));
    mouse.release();
    CHECK (song.valueOf ("threshold") == doctest::Approx (-6.0f).epsilon (0.05));
    CHECK (phazite::describeUndo (song.edit.get()) == juce::String ("Undo Change Threshold"));

    phazite::undo (*song.edit);
    editor.refresh();
    CHECK (song.valueOf ("threshold") == doctest::Approx (-18.0f));
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

TEST_CASE ("The compressor's editor shows the level it hears and how far it turns the sound down")
{
    Song song;
    const auto device = phazite::makeDeviceHandle (*song.compressor);
    auto* compressor = device.getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);

    // What played before the editor opened doesn't show.
    play (*compressor, -6.0f, 4800);
    phazite::ui::CompressorEditor editor (device);
    CHECK (editor.getShownReduction() == doctest::Approx (0.0f));

    // 12 dB over the threshold at 4:1, past the knee, settles 9 dB down.
    play (*compressor, -6.0f, 24000);
    editor.refresh();
    CHECK (editor.getShownLevel() == doctest::Approx (-6.0f).epsilon (0.01));
    CHECK (editor.getShownReduction() == doctest::Approx (9.0f).epsilon (0.01));

    // Then, with nothing playing, the meters fall back smoothly.
    editor.refresh();
    CHECK (editor.getShownReduction() == doctest::Approx (8.5f).epsilon (0.01));
}

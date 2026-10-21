// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's editor (src/ui/devices/DrumSamplerEditor.h): its pads, which play and select, the selected pad's
// controls, and sounds dropped on pads.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DrumSamplerEditor.h"

#include <vector>

namespace
{
using phazite::devices::DrumSampler;

// What the sampler plays in its next tenth of a second, at its defaults: its loudest sample.
float playNext (DrumSampler& sampler)
{
    std::vector<float> values;

    for (const auto& parameter : DrumSampler::info.parameters)
        values.push_back (parameter.defaultValue);

    juce::AudioBuffer<float> buffer (2, 4800);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = buffer.getNumSamples();
    block.parameters = values;
    sampler.process (block);
    return buffer.getMagnitude (0, 0, buffer.getNumSamples());
}
} // namespace

TEST_CASE ("The drum sampler's editor plays and selects its pads, and drops sounds on them")
{
    phazite::test::TemporaryFolder project, elsewhere;
    auto edit = te::createEmptyEdit (phazite::test::getEngine(), project.folder.getChildFile ("Song.tracktionedit"));
    const auto plugin = edit->getPluginCache().createNewPlugin (DrumSampler::typeID, {});
    REQUIRE (phazite::setInstrument (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();

    const auto device = phazite::makeDeviceHandle (*plugin);
    CHECK (dynamic_cast<phazite::ui::DrumSamplerEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);
    auto* sampler = device.getProcessor<DrumSampler>();
    REQUIRE (sampler != nullptr);
    sampler->prepare (48000.0, 4800);

    phazite::ui::DrumSamplerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::test::checkChildrenApart (editor);
    CHECK (editor.getSelectedPad() == 0);
    CHECK (editor.getPad (0).getDescription() == juce::String ("Kick"));

    // The first pad at the bottom left, the last at the top right.
    CHECK (editor.getPad (0).getY() > editor.getPad (15).getY());
    CHECK (editor.getPad (0).getX() < editor.getPad (3).getX());

    // Clicking the snare's pad plays it and shows its controls.
    phazite::test::Mouse mouse (editor.getPad (2));
    mouse.click (editor.getPad (2).getLocalBounds().getCentre());
    CHECK (editor.getSelectedPad() == 2);
    CHECK (playNext (*sampler) > 0.1f);

    const auto knobs = phazite::test::findAll<phazite::ui::Knob> (editor);
    const auto level =
        std::find_if (knobs.begin(), knobs.end(), [] (auto* knob) { return knob->getTitle() == juce::String ("Pad 3 Level"); });
    REQUIRE (level != knobs.end());
    (*level)->setValue (-12.0, juce::sendNotificationSync);
    CHECK (plugin->getAutomatableParameterByID ("pad3Level")->getCurrentValue() == doctest::Approx (-12.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Pad 3 Level"));

    // A sound dropped on a pad plays there, named after its file.
    const auto file = elsewhere.folder.getChildFile ("Shaker.wav");
    {
        juce::AudioBuffer<float> noise (1, 4410);

        for (int i = 0; i < noise.getNumSamples(); ++i)
            noise.setSample (0, i, (float) ((i * 7919) % 200 - 100) / 200.0f);

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioSampleBuffer (noise, 0, noise.getNumSamples()));
    }

    CHECK (editor.dropFile (14, file));
    CHECK (editor.getSelectedPad() == 14);
    CHECK (editor.getPad (14).getDescription() == juce::String ("Shaker"));
    CHECK (device.hasPadSound (14));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Put Shaker on Pad 15"));

    phazite::undo (*edit);
    editor.refresh();
    CHECK (editor.getPad (14).getDescription() == juce::String ("High Tom 2"));
}

TEST_CASE ("The drum sampler's editor draws the pad's sound, with its attack's ramp and its cut to drag, and where it plays")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (DrumSampler::typeID, {});
    REQUIRE (phazite::setInstrument (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();
    edit->getUndoManager().clearUndoHistory();

    const auto device = phazite::makeDeviceHandle (*plugin);
    auto* sampler = device.getProcessor<DrumSampler>();
    REQUIRE (sampler != nullptr);
    sampler->prepare (48000.0, 4800);

    phazite::ui::DrumSamplerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& waveform = editor.getWaveform();
    const auto valueOf = [&plugin] (const char* id) { return plugin->getAutomatableParameterByID (id)->getCurrentValue(); };

    // As tall as the dock is short, the pads still fit, at least 44 pixels each.
    CHECK (editor.getMinimumHeight() <= 235);
    window.setSize (editor.getWidthForHeight (editor.getMinimumHeight()), editor.getMinimumHeight());
    phazite::test::checkChildrenApart (editor);
    CHECK (editor.getPad (0).getWidth() >= 44);

    // The cut, from the whole sound, dragged to the middle: the kick plays half of itself, one undo step.
    phazite::test::Mouse mouse (waveform);
    const auto y = waveform.getHeight() / 2;
    mouse.press ({ juce::roundToInt (editor.getLengthX()) - 1, y });
    mouse.dragTo ({ waveform.getWidth() / 2, y });
    mouse.release();
    CHECK (valueOf ("pad1Length") < 10000.0f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Pad 1 Length"));

    // The attack's ramp, dragged right: a longer attack.
    mouse.pause();
    const auto attackX = juce::roundToInt (editor.getAttackX());
    mouse.press ({ attackX, 4 });
    mouse.dragTo ({ attackX + 20, 4 });
    mouse.release();
    CHECK (valueOf ("pad1Attack") > 0.5f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Pad 1 Attack"));

    // As it plays, its playhead moves along it.
    sampler->audition (0, 1.0f);
    playNext (*sampler);
    CHECK (sampler->getPlayhead (0).get() > 0.0f);
    CHECK (sampler->getPlayhead (1).get() < 0.0f);
}

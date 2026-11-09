// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's editor (src/ui/devices/DrumSamplerEditor.h): its pads, sixteen of 128 at a time, which play and select,
// the map of them all, which shows others, the selected pad's controls, its sound drawn with its envelope's handles, and
// sounds dropped on pads.
#include "GoldenFiles.h"
#include "HostedDevice.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DrumSamplerEditor.h"

#include <vector>

namespace
{
using phazite::devices::DrumSampler;
using Field = DrumSampler::Field;

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

// A drum sampler as the song's first track's instrument, which the song's session plays.
phazite::DeviceID addSampler (phazite::test::TestSong& song)
{
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::setInstrument (s, song.firstTrack, phazite::makeOwnDeviceState (s, DrumSampler::typeID));
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    return id;
}

juce::Button& getShownPad (phazite::ui::DrumSamplerEditor& editor, int pad)
{
    auto* button = editor.getPad (pad);
    REQUIRE (button != nullptr);
    return *button;
}

void writeNoise (const juce::File& file)
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
} // namespace

TEST_CASE ("The drum sampler's editor plays and selects its pads, and drops sounds on them")
{
    phazite::test::TemporaryFolder elsewhere;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = addSampler (song);

    const auto device = phazite::makeDeviceHandle (s, id);
    CHECK (dynamic_cast<phazite::ui::DrumSamplerEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);
    auto* sampler = device.getProcessor<DrumSampler>();
    REQUIRE (sampler != nullptr);
    sampler->prepare (48000.0, 4800);

    phazite::ui::DrumSamplerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::test::checkChildrenApart (editor);
    CHECK (editor.getSelectedPad() == 36);
    CHECK (editor.getFirstShownPad() == 36);
    CHECK (getShownPad (editor, 36).getDescription() == juce::String ("Kick"));
    CHECK (getShownPad (editor, 36).getTitle() == juce::String ("Pad 36 (C2)"));
    CHECK (editor.getPad (35) == nullptr);
    CHECK (editor.getPad (52) == nullptr);

    // The lowest at the bottom left, the highest at the top right.
    CHECK (getShownPad (editor, 36).getY() > getShownPad (editor, 48).getY());
    CHECK (getShownPad (editor, 36).getX() < getShownPad (editor, 39).getX());

    // Clicking the snare's pad plays it and shows its controls, which change its parameters.
    phazite::test::Mouse mouse (getShownPad (editor, 38));
    mouse.click (getShownPad (editor, 38).getLocalBounds().getCentre());
    CHECK (editor.getSelectedPad() == 38);
    CHECK (playNext (*sampler) > 0.1f);
    CHECK (editor.findControl ("pad36Level") == nullptr);
    REQUIRE (editor.findControl ("pad38Level") != nullptr);
    CHECK (editor.findControl ("pad38Cutoff") != nullptr);
    CHECK (editor.findControl ("pad38Solo") != nullptr);
    CHECK (editor.findControl ("output") != nullptr);

    auto* level = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("pad38Level"));
    REQUIRE (level != nullptr);
    CHECK (level->getTitle() == juce::String ("Pad 38 Level"));
    level->setValue (-12.0, juce::sendNotificationSync);
    CHECK (phazite::getValueSetByHand (s, phazite::ParameterRef { id, "pad38Level" }) == doctest::Approx (-12.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Pad 38 Level"));

    // Muted, it shows M, faint, and plays nothing.
    auto* mute = dynamic_cast<juce::Button*> (editor.findControl ("pad38Mute"));
    REQUIRE (mute != nullptr);
    phazite::test::Mouse muteMouse (*mute);
    muteMouse.click (mute->getLocalBounds().getCentre());
    CHECK (phazite::getValueSetByHand (s, phazite::ParameterRef { id, "pad38Mute" }) == doctest::Approx (1.0f));

    // A sound dropped on a pad plays there, named after its file.
    const auto file = elsewhere.folder.getChildFile ("Shaker.wav");
    writeNoise (file);

    CHECK (editor.dropFile (50, file));
    CHECK (editor.getSelectedPad() == 50);
    CHECK (getShownPad (editor, 50).getDescription() == juce::String ("Shaker"));
    CHECK (device.hasPadSound (50));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Put Shaker on Pad 50"));

    phazite::undo (s);
    editor.refresh();
    CHECK (getShownPad (editor, 50).getDescription() == juce::String ("High Tom 2"));

    // On the snare's second layer: its place names it, the pad counts it, and Clear takes it off.
    editor.selectPad (38);
    CHECK (editor.dropFile (38, file, 1));
    CHECK (editor.getLayerSlot (1).getDescription() == juce::String ("Shaker"));
    CHECK (editor.getLayerSlot (0).getDescription() == juce::String ("Snare"));
    CHECK (getShownPad (editor, 38).getDescription() == juce::String ("Snare +1"));
    CHECK (editor.findControl ("pad38LayerMode") != nullptr);
    CHECK (editor.findControl ("pad38Layer2Level") != nullptr);
    const auto buttons = phazite::test::findAll<juce::Button> (editor.getLayerSlot (1));
    REQUIRE (buttons.size() == 1);
    auto* clear = buttons.getFirst();
    REQUIRE (clear->isVisible());
    phazite::test::Mouse clearMouse (*clear);
    clearMouse.click (clear->getLocalBounds().getCentre());
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Clear Pad 38's Layer 2"));
    CHECK (editor.getLayerSlot (1).getDescription().isEmpty());
    CHECK (getShownPad (editor, 38).getDescription() == juce::String ("Snare"));

    // Others shown from the map: its top shows the highest sixteen, its bottom the lowest; the wheel over the pads, a row.
    auto& map = editor.getPadMap();
    phazite::test::Mouse mapMouse (map);
    mapMouse.click ({ map.getWidth() / 2, 1 });
    CHECK (editor.getFirstShownPad() == 112);
    mapMouse.pause();
    mapMouse.click ({ map.getWidth() / 2, map.getHeight() - 2 });
    CHECK (editor.getFirstShownPad() == 0);
    CHECK (editor.getPad (0) != nullptr);
    CHECK (editor.getPad (36) == nullptr);

    editor.showPadsFrom (60);
    CHECK (editor.getFirstShownPad() == 60);
    phazite::test::Mouse wheelMouse (getShownPad (editor, 60));
    wheelMouse.wheel (getShownPad (editor, 60).getLocalBounds().getCentre(), (float) phazite::ui::wheelNotch);
    CHECK (editor.getFirstShownPad() == 64);
    wheelMouse.wheel (getShownPad (editor, 64).getLocalBounds().getCentre(), -2.0f * (float) phazite::ui::wheelNotch);
    CHECK (editor.getFirstShownPad() == 56);

    // An empty pad shows nothing of its own, and plays a sound dropped on it.
    CHECK (getShownPad (editor, 60).getDescription().isEmpty());
    CHECK (device.isPadEmpty (60));
    CHECK (editor.dropFile (60, file));
    CHECK (getShownPad (editor, 60).getDescription() == juce::String ("Shaker"));
    CHECK_FALSE (device.isPadEmpty (60));

    // Own Track gives the selected pad a track of its own, and takes it away, each one undo step.
    editor.selectPad (38);
    auto& ownTrack = editor.getOwnTrackButton();
    CHECK_FALSE (ownTrack.getToggleState());
    phazite::test::Mouse ownTrackMouse (ownTrack);
    ownTrackMouse.click (ownTrack.getLocalBounds().getCentre());
    CHECK (device.getPadTrack (38).isValid());
    CHECK (ownTrack.getToggleState());
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Give Pad 38 Its Own Track"));
    ownTrackMouse.pause();
    ownTrackMouse.click (ownTrack.getLocalBounds().getCentre());
    CHECK_FALSE (device.getPadTrack (38).isValid());
    CHECK_FALSE (ownTrack.getToggleState());
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Bring Pad 38 Back"));

    // Selecting a pad out of view shows it.
    editor.selectPad (100);
    CHECK (editor.getPad (100) != nullptr);
    CHECK (editor.findControl ("pad100Level") != nullptr);
}

TEST_CASE ("The drum sampler's editor draws the pad's sound, with its start and envelope to drag, and where it plays")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = addSampler (song);
    s.getUndoManager().clearUndoHistory();

    const auto device = phazite::makeDeviceHandle (s, id);
    auto* sampler = device.getProcessor<DrumSampler>();
    REQUIRE (sampler != nullptr);
    sampler->prepare (48000.0, 4800);

    phazite::ui::DrumSamplerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& waveform = editor.getWaveform();
    const auto valueOf = [&s, id] (const char* parameter)
    { return phazite::getValueSetByHand (s, phazite::ParameterRef { id, parameter }); };

    // As tall as the dock is short, the pads still fit.
    CHECK (editor.getMinimumHeight() <= 235);
    window.setSize (editor.getWidthForHeight (editor.getMinimumHeight()), editor.getMinimumHeight());
    phazite::test::checkChildrenApart (editor);
    CHECK (getShownPad (editor, 36).getWidth() >= 44);
    CHECK (getShownPad (editor, 36).getHeight() >= 36);
    window.setSize (editor.getWidthForHeight (300), 300);

    // The hold, from the whole sound, dragged to the middle: the kick holds half of itself, then decays, one undo step.
    phazite::test::Mouse mouse (waveform);
    const auto y = waveform.getHeight() / 2;
    CHECK (editor.getHandleX (Field::hold) > (float) waveform.getWidth() - 4.0f);
    mouse.press ({ juce::roundToInt (editor.getHandleX (Field::hold)) - 1, 4 });
    mouse.dragTo ({ waveform.getWidth() / 2, 4 });
    mouse.release();
    CHECK (valueOf ("pad36Hold") < DrumSampler::wholeSound);
    CHECK (valueOf ("pad36Hold") > 10.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Pad 36 Hold"));

    // The decay's handle, after it, dragged right: a longer decay.
    mouse.pause();
    const auto decayX = juce::roundToInt (editor.getHandleX (Field::decay));
    const auto decayBefore = valueOf ("pad36Decay");
    mouse.press ({ decayX, waveform.getHeight() - 4 });
    mouse.dragTo ({ decayX + 20, waveform.getHeight() - 4 });
    mouse.release();
    CHECK (valueOf ("pad36Decay") > decayBefore + 1.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Pad 36 Decay"));

    // The attack's ramp, dragged right: a longer attack.
    mouse.pause();
    const auto attackX = juce::roundToInt (editor.getHandleX (Field::attack));
    mouse.press ({ attackX, 4 });
    mouse.dragTo ({ attackX + 20, 4 });
    mouse.release();
    CHECK (valueOf ("pad36Attack") > 0.5f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Pad 36 Attack"));

    // The start, a line down the middle, dragged right: it starts later in its sound.
    mouse.pause();
    const auto startX = juce::roundToInt (editor.getHandleX (Field::start));
    mouse.press ({ startX, y });
    mouse.dragTo ({ startX + 10, y });
    mouse.release();
    CHECK (valueOf ("pad36Start") > 0.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Pad 36 Start"));

    // As it plays, its playhead moves along it.
    sampler->audition (36, 1.0f);
    playNext (*sampler);
    CHECK (sampler->getPlayhead (36).get() > 0.0f);
    CHECK (sampler->getPlayhead (37).get() < 0.0f);
}

TEST_CASE ("A pad clicked in the drum sampler's editor sounds at once, however long the sampler was silent")
{
    // The sampler sleeps once it has been silent a while (engine/nodes/DeviceRunner.h): a click then waited in it unheard,
    // and every click since sounded at once when the song's first note woke it. (An instrument has no switch of its own
    // since 2026-11-01, so it is never off: DevicesViewTests.cpp.)
    phazite::test::HostedDevice output;
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, DrumSampler::typeID);
    phazite::test::settle (500);

    phazite::ui::DrumSamplerEditor editor (phazite::getDeviceHandle (song.context, drums));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& snare = getShownPad (editor, 38);
    phazite::test::Mouse mouse (snare);
    const auto clickSnare = [&] { mouse.click (snare.getLocalBounds().getCentre()); };

    // While the song is stopped, after a second of silence each time: in the next block.
    for (int i = 0; i < 3; ++i)
    {
        output.process (100);
        clickSnare();
        const auto samples = output.samplesToFirstSound();
        CHECK (samples >= 0);
        CHECK (samples < phazite::test::HostedDevice::blockSize);
    }
}

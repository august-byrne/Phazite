// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiTestHelpers.h"
#include "model/commands/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/LevelSource.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/components/ValueText.h"

TEST_CASE ("A slider linked to a track's volume sets it, one undo step per change")
{
    juce::Slider slider;
    phazite::ui::ParameterAttachment attachment (slider);
    CHECK_FALSE (slider.isEnabled()); // nothing to control yet

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setEdit (context, edit.get());
    auto* track = te::getAudioTracks (*edit)[0];
    auto* volume = track->getVolumePlugin();
    REQUIRE (volume != nullptr);

    attachment.setParameter (phazite::getVolume (context, phazite::getTrackID (*track)));
    CHECK (slider.isEnabled());
    CHECK (slider.getValue() == doctest::Approx (volume->volParam->getCurrentValue()));

    // Moving the slider sets the volume, as a named undo step.
    const auto before = volume->getVolumeDb();
    slider.setValue (phazite::decibelsToFaderPosition (-12.0f), juce::sendNotificationSync);
    CHECK (volume->getVolumeDb() == doctest::Approx (-12.0f).epsilon (0.01));
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Volume"));

    // Undo (as the app's commands do it) plays the old volume again, not just stores it.
    phazite::undo (*edit);
    CHECK (volume->getVolumeDb() == doctest::Approx (before).epsilon (0.01));

    phazite::redo (*edit);
    CHECK (volume->getVolumeDb() == doctest::Approx (-12.0f).epsilon (0.01));
    phazite::undo (*edit);

    // A change made elsewhere shows on the slider.
    volume->setVolumeDb (-3.0f);
    attachment.update();
    CHECK (phazite::faderPositionToDecibels ((float) slider.getValue()) == doctest::Approx (-3.0f).epsilon (0.01));

    // A drag is one step, even when the strip hands the slider a handle of the same parameter meanwhile.
    edit->getUndoManager().clearUndoHistory();
    slider.onDragStart();
    slider.setValue (phazite::decibelsToFaderPosition (-6.0f), juce::sendNotificationSync);
    attachment.setParameter (phazite::getVolume (context, phazite::getTrackID (*track)));
    slider.setValue (phazite::decibelsToFaderPosition (-9.0f), juce::sendNotificationSync);
    slider.onDragEnd();
    CHECK (volume->getVolumeDb() == doctest::Approx (-9.0f).epsilon (0.01));
    phazite::undo (*edit);
    CHECK (volume->getVolumeDb() == doctest::Approx (-3.0f).epsilon (0.01));
    CHECK_FALSE (edit->getUndoManager().canUndo());

    // The master volume's parameter has its own ID.
    attachment.setParameter (phazite::getMasterVolume (context));
    CHECK (slider.isEnabled());

    // When the project closes, the slider turns off instead of reaching for a deleted plug-in.
    setEdit (context, nullptr);
    edit = nullptr;
    attachment.update();
    CHECK_FALSE (slider.isEnabled());
}

TEST_CASE ("A level source follows what it measures, and outlives it")
{
    auto measurer = std::make_unique<te::LevelMeasurer>();
    te::LevelMeasurer* found = measurer.get();
    auto source = phazite::makeLevelSource ([&found] { return found; });
    CHECK (source.isMeasuring());

    // Deleted while the source still reads it. Mustn't be touched: AddressSanitizer (the linux-asan preset) would report it.
    measurer = nullptr;
    found = nullptr;
    CHECK_FALSE (source.isMeasuring());
    CHECK (source.getAndClearLevel (0) == doctest::Approx (phazite::LevelSource::silenceDb));

    // What it measures can come and go: a folder that becomes a bus, playback that is set up again.
    te::LevelMeasurer another;
    found = &another;
    source.refresh();
    CHECK (source.isMeasuring());
    found = nullptr;
    source.refresh();
    CHECK_FALSE (source.isMeasuring());

    // A meter lets go of what it shows when it gets another source.
    found = &another;
    source.refresh();
    phazite::ui::LevelMeter meter ("Level");
    meter.setSource (std::move (source));
    meter.setSource ({});
}

TEST_CASE ("The mute and solo buttons switch their track's own mute and solo")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setEdit (context, edit.get());

    phazite::ui::MuteSoloButtons buttons (context);
    phazite::test::TestWindow window (buttons, 60, 24);
    buttons.setTrack (phazite::getTrackID (*tracks[0]));
    phazite::test::Mouse mouse (buttons);
    const juce::Point<int> muteButton (10, 12), soloButton (50, 12);

    mouse.click (muteButton);
    CHECK (tracks[0]->isMuted (false));
    mouse.click (muteButton);
    CHECK_FALSE (tracks[0]->isMuted (false));

    // Solo plays the track along with the others soloed; with Ctrl, alone. Neither touches the mute switch.
    tracks[1]->setSolo (true);
    mouse.click (soloButton);
    CHECK (tracks[0]->isSolo (false));
    CHECK (tracks[1]->isSolo (false));

    mouse.click (soloButton, juce::ModifierKeys::commandModifier);
    CHECK (tracks[0]->isSolo (false));
    CHECK_FALSE (tracks[1]->isSolo (false));
    CHECK_FALSE (tracks[0]->isMuted (false));
}

TEST_CASE ("A number field moves in steps when dragged or stepped, takes any value typed, and says it with its units")
{
    phazite::ui::NumberField field ("Tempo", 20.0, 300.0, 1.0, 120.0);
    field.textFromValueFunction = [] (double bpm) { return phazite::ui::formatTempo (bpm); };
    phazite::test::TestWindow window (field, 96, 24);

    int changes = 0, drags = 0;
    field.onValueChange = [&changes] { ++changes; };
    field.onDragStart = [&drags] { ++drags; };

    // Two pixels a step, up for more.
    phazite::test::Mouse mouse (field);
    mouse.press ({ 48, 12 });
    mouse.dragTo ({ 48, -8 });
    mouse.release();
    CHECK (field.getValue() == doctest::Approx (130.0));
    CHECK (drags == 1);
    CHECK (changes > 0);

    // The drag gave it the keyboard: the arrow keys step it.
    window.pressKey (juce::KeyPress (juce::KeyPress::upKey));
    CHECK (field.getValue() == doctest::Approx (131.0));
    window.pressKey (juce::KeyPress (juce::KeyPress::leftKey));
    window.pressKey (juce::KeyPress (juce::KeyPress::leftKey));
    CHECK (field.getValue() == doctest::Approx (129.0));

    // A typed value can fall between the steps, to a hundredth of one.
    field.setValue (128.374, juce::sendNotificationSync);
    CHECK (field.getValue() == doctest::Approx (128.37));
    CHECK (field.getTextFromValue (field.getValue()) == juce::String ("128.37 BPM"));

    // Screen readers hear the units, and step it as the keys do.
    const auto handler = field.createAccessibilityHandler();
    REQUIRE (handler != nullptr);
    auto* value = handler->getValueInterface();
    REQUIRE (value != nullptr);
    CHECK (value->getCurrentValueAsString() == juce::String ("128.37 BPM"));
    CHECK (value->getRange().getInterval() == doctest::Approx (1.0));
    value->setValue (140.0);
    CHECK (field.getValue() == doctest::Approx (140.0));
}

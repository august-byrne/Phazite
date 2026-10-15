// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiTestHelpers.h"
#include "model/commands/Undo.h"
#include "ui/components/Controls.h"

TEST_CASE ("A slider linked to a track's volume sets it, one undo step per change")
{
    juce::Slider slider;
    phazite::ui::ParameterAttachment attachment (slider, "Change Volume");
    CHECK_FALSE (slider.isEnabled()); // nothing to control yet

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* volume = te::getAudioTracks (*edit)[0]->getVolumePlugin();
    REQUIRE (volume != nullptr);

    attachment.setParameter (volume, volume->volParam->paramID);
    CHECK (slider.isEnabled());
    CHECK (slider.getValue() == doctest::Approx (volume->volParam->getCurrentValue()));

    // Moving the slider sets the volume, as a named undo step.
    const auto before = volume->getVolumeDb();
    slider.setValue (te::decibelsToVolumeFaderPosition (-12.0f), juce::sendNotificationSync);
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
    CHECK (te::volumeFaderPositionToDB ((float) slider.getValue()) == doctest::Approx (-3.0f).epsilon (0.01));

    // The master volume's parameter has its own ID.
    auto master = edit->getMasterVolumePlugin();
    REQUIRE (master != nullptr);
    attachment.setParameter (master.get(), master->volParam->paramID);
    CHECK (slider.isEnabled());
    master = nullptr;

    // When the project closes, the slider turns off instead of reaching for a deleted plug-in.
    edit = nullptr;
    attachment.update();
    CHECK_FALSE (slider.isEnabled());
}

TEST_CASE ("A level meter outlives the measurer it shows")
{
    phazite::ui::LevelMeter meter ("Level");

    {
        te::LevelMeasurer measurer;
        meter.setSource (&measurer);
    } // deleted while the meter still shows it

    // Mustn't touch the deleted measurer: AddressSanitizer (the linux-asan preset) would report it.
    meter.setSource (nullptr);

    te::LevelMeasurer another;
    meter.setSource (&another);
    meter.setSource (nullptr);
}

TEST_CASE ("The mute and solo buttons switch their track's own mute and solo")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);

    phazite::ui::MuteSoloButtons buttons;
    phazite::test::TestWindow window (buttons, 60, 24);
    buttons.setTrack (tracks[0]);
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

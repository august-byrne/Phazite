// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiTestHelpers.h"
#include "engine/nodes/LevelMeter.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/LevelSource.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"
#include "ui/components/TrackSwitch.h"
#include "ui/components/ValueText.h"

TEST_CASE ("A slider linked to a track's volume sets it, one undo step per change")
{
    juce::Slider slider;
    phazite::ui::ParameterAttachment attachment (slider);
    CHECK_FALSE (slider.isEnabled()); // nothing to control yet

    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::setSong (context, song.get());
    const auto track = phazite::getTrackIDs (*song)[0];
    const auto volume = [&song, track] { return phazite::getChannelVolume (*song, track); };

    attachment.setParameter (phazite::getVolume (context, track));
    CHECK (slider.isEnabled());
    CHECK (slider.getValue() == doctest::Approx (phazite::decibelsToFaderPosition (volume())));

    // Moving the slider sets the volume, as a named undo step.
    const auto before = volume();
    slider.setValue (phazite::decibelsToFaderPosition (-12.0f), juce::sendNotificationSync);
    CHECK (volume() == doctest::Approx (-12.0f).epsilon (0.01));
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Change Volume"));

    // Undo (as the app's commands do it) puts the old volume back.
    phazite::undo (*song);
    CHECK (volume() == doctest::Approx (before).epsilon (0.01));

    phazite::redo (*song);
    CHECK (volume() == doctest::Approx (-12.0f).epsilon (0.01));
    phazite::undo (*song);

    // A change made elsewhere shows on the slider.
    phazite::setChannelVolume (*song, track, -3.0f);
    attachment.update();
    CHECK (phazite::faderPositionToDecibels ((float) slider.getValue()) == doctest::Approx (-3.0f).epsilon (0.01));

    // A drag is one step, even when the strip hands the slider a handle of the same parameter meanwhile.
    song->getUndoManager().clearUndoHistory();
    slider.onDragStart();
    slider.setValue (phazite::decibelsToFaderPosition (-6.0f), juce::sendNotificationSync);
    attachment.setParameter (phazite::getVolume (context, track));
    slider.setValue (phazite::decibelsToFaderPosition (-9.0f), juce::sendNotificationSync);
    slider.onDragEnd();
    CHECK (volume() == doctest::Approx (-9.0f).epsilon (0.01));
    phazite::undo (*song);
    CHECK (volume() == doctest::Approx (-3.0f).epsilon (0.01));
    CHECK_FALSE (song->getUndoManager().canUndo());

    // The master volume's parameter has its own ID.
    attachment.setParameter (phazite::getMasterVolume (context));
    CHECK (slider.isEnabled());

    // When the project closes, the slider turns off instead of reaching for a deleted song.
    phazite::setSong (context, nullptr);
    song = nullptr;
    attachment.update();
    CHECK_FALSE (slider.isEnabled());
}

TEST_CASE ("A level source follows what it measures, and outlives it")
{
    auto measured = std::make_unique<phazite::engine::LevelMeter>();
    phazite::engine::LevelMeter* found = measured.get();
    auto source = phazite::makeLevelSource ([&found] { return found; });
    CHECK (source.isMeasuring());

    // Deleted while the source still reads it. Mustn't be touched: AddressSanitizer (the linux-asan preset) would report it.
    measured = nullptr;
    found = nullptr;
    CHECK_FALSE (source.isMeasuring());
    CHECK (source.getAndClearLevel (0) == doctest::Approx (phazite::LevelSource::silenceDb));

    // What it measures can come and go: a folder that becomes a bus, playback that is set up again.
    phazite::engine::LevelMeter another;
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

TEST_CASE ("A level meter repaints only while what it shows moves: a silent one never does")
{
    phazite::engine::LevelMeter measured;
    phazite::ui::LevelMeter meter ("Level");
    meter.setSource (phazite::makeLevelSource ([&measured] { return &measured; }));
    phazite::test::TestWindow window (meter, 12, 120);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    const auto tick = [&ticker] (int times)
    {
        for (int i = 0; i < times; ++i)
            ticker->tickNow();
    };

    // Silence, however long it shows (it once repainted 30 times a second).
    window.takeRepaintedArea();
    tick (90);
    CHECK (window.takeRepaintedArea().isEmpty());

    // A sound: the bars jump up, then fall back 30 dB a second, repainting as they go.
    juce::AudioBuffer<float> sound (2, 480);

    for (int channel = 0; channel < 2; ++channel)
        juce::FloatVectorOperations::fill (sound.getWritePointer (channel), 0.5f, 480);

    measured.measure (sound.getArrayOfReadPointers(), 2, 480);
    tick (1);
    CHECK (window.takeRepaintedArea().containsRectangle (meter.getBounds()));
    tick (5);
    CHECK_FALSE (window.takeRepaintedArea().isEmpty());

    // Below the meter's range, the bars rest, and only the peak's line waits to be let go, a second after it came.
    tick (60);
    window.takeRepaintedArea();
    tick (10);
    CHECK (window.takeRepaintedArea().isEmpty());
    juce::Thread::sleep (1100);
    tick (1);
    CHECK_FALSE (window.takeRepaintedArea().isEmpty());
    tick (30);
    CHECK (window.takeRepaintedArea().isEmpty());
}

TEST_CASE ("A track's one switch mutes it with a click and solos it with a right-click, each leaving the other as it is")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::setSong (context, song.get());
    const auto isMuted = [&song] (phazite::TrackID track) { return phazite::isTrackMuted (*song, track, false); };
    const auto isSoloed = [&song] (phazite::TrackID track) { return phazite::isTrackSoloed (*song, track); };

    phazite::ui::TrackSwitch trackSwitch (context);
    phazite::test::TestWindow window (trackSwitch, 20, 20);
    trackSwitch.setTrack (tracks[0]);
    phazite::test::Mouse mouse (trackSwitch);
    const juce::Point<int> centre (10, 10);
    const auto rightButton = juce::ModifierKeys::rightButtonModifier;
    CHECK (trackSwitch.getStateText() == "Playing");

    mouse.click (centre);
    CHECK (isMuted (tracks[0]));
    CHECK (trackSwitch.getStateText() == "Muted");
    mouse.click (centre);
    CHECK_FALSE (isMuted (tracks[0]));

    // A right-click solos the track alone, as in FL Studio, and again unsolos it; with Ctrl, along with the others soloed.
    phazite::setTrackSoloed (*song, tracks[1], true);
    trackSwitch.update();
    CHECK (trackSwitch.getStateText() == "Silent while other tracks are soloed");

    mouse.click (centre, rightButton);
    CHECK (isSoloed (tracks[0]));
    CHECK_FALSE (isSoloed (tracks[1]));
    CHECK (trackSwitch.getStateText() == "Soloed");
    mouse.click (centre, rightButton);
    CHECK_FALSE (isSoloed (tracks[0]));

    phazite::setTrackSoloed (*song, tracks[1], true);
    mouse.click (centre, rightButton | juce::ModifierKeys::commandModifier);
    CHECK (isSoloed (tracks[0]));
    CHECK (isSoloed (tracks[1]));

    // Soloing never touched the mute switch, nor muting the solo.
    CHECK_FALSE (isMuted (tracks[0]));
    mouse.click (centre);
    CHECK (isSoloed (tracks[0]));
    CHECK (trackSwitch.getStateText() == "Muted and soloed");
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

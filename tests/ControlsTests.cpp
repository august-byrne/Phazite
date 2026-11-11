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
#include "ui/components/PeakBar.h"
#include "ui/components/ScrollBarFade.h"
#include "ui/components/TrackSwitch.h"
#include "ui/components/ValueText.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/LookAndFeel.h"

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

TEST_CASE ("A track's peak bar shows its louder channel at once, falls 20 dB in half a second, and lights a clip for two seconds")
{
    phazite::engine::LevelMeter measured;
    phazite::ui::PeakBar bar;
    bar.setSource (phazite::makeLevelSource ([&measured] { return &measured; }));
    phazite::test::TestWindow window (bar, phazite::ui::PeakBarTint::width, 60);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    const auto tick = [&ticker] (int times)
    {
        for (int i = 0; i < times; ++i)
            ticker->tickNow();
    };

    const auto play = [&measured] (float left, float right)
    {
        juce::AudioBuffer<float> sound (2, 480);
        juce::FloatVectorOperations::fill (sound.getWritePointer (0), left, 480);
        juce::FloatVectorOperations::fill (sound.getWritePointer (1), right, 480);
        measured.measure (sound.getArrayOfReadPointers(), 2, 480);
    };

    // Silence, however long it shows: nothing repaints. It never takes a click: the header has them.
    tick (1);
    window.takeRepaintedArea();
    tick (30);
    CHECK (window.takeRepaintedArea().isEmpty());
    CHECK_FALSE (bar.hitTest (2, 30));

    // A sound: the louder channel shows at once, then falls 20 dB in half a second, repainting as it goes.
    play (0.5f, 0.1f);
    tick (1);
    CHECK (bar.getLevel() == doctest::Approx (juce::Decibels::gainToDecibels (0.5f)).epsilon (0.01));
    CHECK_FALSE (window.takeRepaintedArea().isEmpty());
    tick (phazite::ui::DisplayTicker::ticksPerSecond / 2);
    CHECK (bar.getLevel() == doctest::Approx (juce::Decibels::gainToDecibels (0.5f) - 20.0f).epsilon (0.02));

    // Over 0 dB: its top lights, for two seconds from the last clip, and goes out by itself.
    play (1.5f, 0.0f);
    tick (1);
    CHECK (bar.isClipLit());
    tick (2 * phazite::ui::DisplayTicker::ticksPerSecond - 2);
    CHECK (bar.isClipLit());
    tick (2);
    CHECK_FALSE (bar.isClipLit());
    CHECK_FALSE (bar.hitTest (2, 1));
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

TEST_CASE ("A knob takes a value typed: a number typed while it has the keys opens a field with it, and Enter sets it as one undo step")
{
    // The owner, 2026-11-09: "i need a way to type in exact values ... rather than move my mouse to an exact infentesimal
    // position".
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::setSong (context, song.get());
    const auto track = phazite::getTrackIDs (*song)[0];
    const auto pan = [&song, track] { return phazite::getChannelPan (*song, track); };

    // A knob, and another control that takes the keys, side by side in a window.
    juce::Component holder;
    phazite::ui::Knob knob ("Pan", 0.0);
    phazite::ui::showAsPan (knob);
    phazite::ui::ParameterAttachment attachment (knob);
    attachment.setParameter (phazite::getPan (context, track));
    juce::Component other;
    other.setWantsKeyboardFocus (true);
    holder.addAndMakeVisible (knob);
    holder.addAndMakeVisible (other);
    knob.setBounds (80, 40, 40, 40);
    other.setBounds (150, 40, 40, 40);
    phazite::test::TestWindow window (holder, 200, 120);
    song->getUndoManager().clearUndoHistory();

    // Clicked, it has the keys, and nothing changed. A number typed opens its field, over it, with what was typed and the keys.
    phazite::test::Mouse mouse (holder);
    mouse.click (knob.getBounds().getCentre());
    REQUIRE (knob.hasKeyboardFocus (false));
    CHECK_FALSE (song->getUndoManager().canUndo());
    window.pressKey (juce::KeyPress ('3', {}, '3'));
    auto* field = knob.getValueField();
    REQUIRE (field != nullptr);
    CHECK (field->isShowing());
    CHECK (field->hasKeyboardFocus (false));
    CHECK (field->getText() == juce::String ("3"));
    CHECK (field->getBounds().getCentre() == knob.getBounds().getCentre());
    CHECK (field->getWidth() > knob.getWidth()); // room for its text
    CHECK (window.getLocalBounds().contains (field->getBounds()));

    // What is typed next goes into it; Enter sets 30% left as one undo step, and the knob has the keys again.
    window.pressKey (juce::KeyPress ('0', {}, '0'));
    window.pressKey (juce::KeyPress ('L', {}, 'L'));
    CHECK (field->getText() == juce::String ("30L"));
    window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (pan() == doctest::Approx (-0.3f));
    CHECK (knob.getValue() == doctest::Approx (-0.3));
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Change Pan"));
    CHECK (knob.getValueField() == nullptr);
    CHECK (knob.hasKeyboardFocus (false));
    phazite::test::dispatchPendingMessages(); // the field goes, a message after it closed
    CHECK (phazite::test::findAll<juce::TextEditor> (window).isEmpty());

    // One undo puts it back.
    phazite::undo (*song);
    CHECK (pan() == doctest::Approx (0.0f));
    attachment.update();
    song->getUndoManager().clearUndoHistory();

    // Enter alone opens nothing: it keeps its meaning where the knob is (a window's default button, the piano roll's Apply).
    window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (knob.getValueField() == nullptr);

    // Its menu's Type a Value... opens the field with the value, all of it selected, so that what is typed replaces it; Esc
    // leaves the value as it was.
    auto menu = attachment.getMenu();
    REQUIRE (phazite::test::runMenuItem (menu, { "Type a Value..." }));
    field = knob.getValueField();
    REQUIRE (field != nullptr);
    CHECK (field->getText() == juce::String ("Center"));
    CHECK (field->getHighlightedRegion().getLength() == 6);
    window.pressKey (juce::KeyPress ('5', {}, '5'));
    CHECK (field->getText() == juce::String ("5"));
    window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    CHECK (pan() == doctest::Approx (0.0f));
    CHECK (knob.getValueField() == nullptr);
    CHECK (knob.hasKeyboardFocus (false));
    phazite::test::dispatchPendingMessages();

    // Its value set again unchanged, or text it can't read, changes nothing either.
    for (const auto* text : { "", "loud" })
    {
        REQUIRE (phazite::test::runMenuItem (menu, { "Type a Value..." }));
        REQUIRE (knob.getValueField() != nullptr);

        if (juce::String (text).isNotEmpty())
            knob.getValueField()->setText (text, false);

        window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
        phazite::test::dispatchPendingMessages();
        CHECK (pan() == doctest::Approx (0.0f));
        CHECK_FALSE (song->getUndoManager().canUndo());
    }

    // The keys going elsewhere set what it says, as a click elsewhere does: "-50" is half left.
    for (const auto typed : { '-', '5', '0' })
        window.pressKey (juce::KeyPress (typed, {}, (juce::juce_wchar) typed));

    other.grabKeyboardFocus();
    CHECK (pan() == doctest::Approx (-0.5f));
    CHECK (knob.getValueField() == nullptr);
    phazite::test::dispatchPendingMessages();

    // A number typed reaches only the control that has the keys, and not with Ctrl, which makes a key a shortcut's.
    window.pressKey (juce::KeyPress ('7', {}, '7'));
    CHECK (knob.getValueField() == nullptr);
    knob.grabKeyboardFocus();
    window.pressKey (juce::KeyPress ('1', juce::ModifierKeys::ctrlModifier, '1'));
    CHECK (knob.getValueField() == nullptr);

    // Off, as it is without a parameter, it types nothing.
    phazite::setSong (context, nullptr);
    song = nullptr;
    attachment.update();
    CHECK_FALSE (knob.isEnabled());
    knob.showValueField ("4");
    CHECK (knob.getValueField() == nullptr);
}

TEST_CASE ("A knob's readout opens the field over it, a pitch drags in whole semitones, and a number field types in its own box")
{
    // A knob with its value under it, as a device's editor shows one, for a pitch of up to two octaves either way.
    using phazite::devices::Unit;
    const phazite::devices::ParameterInfo pitch { .id = "p",
                                                  .name = "Pitch",
                                                  .minimum = -24.0f,
                                                  .maximum = 24.0f,
                                                  .unit = Unit::semitones };
    juce::Component holder;
    phazite::ui::Knob knob ("Pitch", 0.0);
    knob.setRange (-24.0, 24.0);
    phazite::ui::showAsParameter (knob, pitch);
    juce::Label readout;
    knob.setValueReadout (&readout);
    CHECK (knob.getValueReadout() == &readout);
    holder.addAndMakeVisible (knob);
    holder.addAndMakeVisible (readout);
    knob.setBounds (60, 10, 40, 40);
    readout.setBounds (52, 52, 56, 16);
    phazite::test::TestWindow window (holder, 240, 120);
    int drags = 0;
    knob.onDragStart = [&drags] { ++drags; };

    // A click on the readout opens the field over it, the value selected; 7 and Enter set it exactly, as one drag's start and
    // end, which an attachment makes one undo step. The pointer over the readout is a text cursor.
    CHECK (readout.getMouseCursor() == juce::MouseCursor::IBeamCursor);
    phazite::test::Mouse mouse (holder);
    mouse.click (readout.getBounds().getCentre());
    auto* field = knob.getValueField();
    REQUIRE (field != nullptr);
    CHECK (field->getBounds().getCentre() == readout.getBounds().getCentre());
    CHECK (field->getText() == juce::String ("0 st"));
    window.pressKey (juce::KeyPress ('7', {}, '7'));
    window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (juce::exactlyEqual (knob.getValue(), 7.0));
    CHECK (drags == 1);
    CHECK (knob.getTextFromValue (knob.getValue()) == juce::String ("+7 st"));
    phazite::test::dispatchPendingMessages();

    // Dragged, it moves in whole semitones: 13 pixels of 200 for 48 semitones are 3.
    const auto isWhole = [&knob] { return std::abs (knob.getValue() - std::round (knob.getValue())) < 1.0e-9; };
    const auto centre = knob.getBounds().getCentre();
    mouse.pause();
    mouse.press (centre);
    mouse.dragTo (centre + juce::Point<int> (0, -13));
    mouse.release();
    CHECK (knob.getValue() == doctest::Approx (10.0));
    CHECK (isWhole());

    // With Shift, between them, a fifth as far: 0.62 of one.
    mouse.pause();
    mouse.press (centre, juce::ModifierKeys::shiftModifier);
    mouse.dragTo (centre + juce::Point<int> (0, -13));
    mouse.release();
    CHECK (knob.getValue() == doctest::Approx (10.624).epsilon (0.001));
    CHECK_FALSE (isWhole());
    CHECK (knob.getTextFromValue (knob.getValue()) == juce::String ("+10.62 st")); // not "+11 st"
    phazite::test::dispatchPendingMessages();

    // A range of two values (a chain's keys) types nothing.
    phazite::ui::ValueSlider keys (juce::Slider::TwoValueHorizontal, "Keys", 0.0);
    holder.addAndMakeVisible (keys);
    keys.setBounds (10, 80, 100, 20);
    keys.grabKeyboardFocus();
    window.pressKey (juce::KeyPress ('5', {}, '5'));
    CHECK (keys.getValueField() == nullptr);

    // A number field has a text box of its own, which a number typed opens with it, as a knob's field.
    phazite::ui::NumberField tempo ("Tempo", 20.0, 300.0, 1.0, 120.0);
    holder.addAndMakeVisible (tempo);
    tempo.setBounds (130, 80, 96, 24);
    tempo.grabKeyboardFocus();
    window.pressKey (juce::KeyPress ('9', {}, '9'));
    auto editors = phazite::test::findAll<juce::TextEditor> (tempo);
    REQUIRE (editors.size() == 1);
    CHECK (editors[0]->getText() == juce::String ("9"));
    window.pressKey (juce::KeyPress ('5', {}, '5'));
    window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    phazite::test::dispatchPendingMessages();
    CHECK (tempo.getValue() == doctest::Approx (95.0));

    // In a call-out that holds the keys while it is up (a modulator's editor), the field is in the call-out, where the keys
    // reach it: outside, they would go to the call-out instead.
    juce::Component callOut;
    phazite::ui::Knob rate ("Rate", 0.0);
    rate.setRange (0.0, 100.0);
    callOut.addAndMakeVisible (rate);
    rate.setBounds (10, 10, 40, 40);
    holder.addAndMakeVisible (callOut);
    callOut.setBounds (120, 0, 110, 70);
    callOut.enterModalState (false);
    rate.grabKeyboardFocus();
    window.pressKey (juce::KeyPress ('2', {}, '2'));
    REQUIRE (rate.getValueField() != nullptr);
    CHECK (rate.getValueField()->getParentComponent() == &callOut);
    window.pressKey (juce::KeyPress ('5', {}, '5'));
    window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (rate.getValue() == doctest::Approx (25.0));
    callOut.exitModalState (0);
    phazite::test::dispatchPendingMessages();
}

TEST_CASE ("A volume fader's scale marks the decibels where the fader puts them, 0 dB in the text colour")
{
    // The owner, 2026-11-07: the mixer's faders had no scale, which consoles print and Pro Tools draws beside its faders.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };

    // A strip's fader with its scale left of it, as the mixer lays them out.
    struct FaderWithScale : public juce::Component
    {
        FaderWithScale()
        {
            phazite::ui::showAsDecibels (fader);
            addAndMakeVisible (fader);
            addAndMakeVisible (scale);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            scale.setBounds (area.removeFromLeft (phazite::ui::FaderScaleTint::width));
            fader.setBounds (area);
        }

        phazite::ui::Fader fader { phazite::ui::Fader::Direction::vertical, "Volume", phazite::decibelsToFaderPosition (0.0f) };
        phazite::ui::FaderScale scale { fader };
    } shown;

    phazite::test::TestWindow window (shown, phazite::ui::FaderScaleTint::width + 46, 160);

    // A slider shown in decibels holds a fader's position, from 0 to 1, whatever range it had before: the browser's preview
    // volume kept JUCE's, 0 to 10, which put 0 dB a tenth of the way along.
    CHECK (shown.fader.getMinimum() == 0.0);
    CHECK (shown.fader.getMaximum() == 1.0);

    // Each tick at the scale's right edge, on the row of the fader's thumb at its level.
    const auto image = window.paintEverything();
    const auto& palette = phazite::ui::getPalette (shown);
    const auto tickAt = [&] (float decibels)
    {
        const auto y = (int) std::floor (shown.fader.getPositionOfValue (phazite::decibelsToFaderPosition (decibels)));
        return image.getPixelAt (shown.scale.getRight() - 1, y);
    };

    CHECK (tickAt (0.0f) == palette.text);
    CHECK (tickAt (-12.0f) == palette.textMuted);
    CHECK (tickAt (6.0f) == palette.textMuted);
    CHECK (shown.scale.getTitle().isEmpty()); // only for the eye: screen readers read the fader
}

TEST_CASE ("An overlay scroll bar shows while the pointer is over its editor, and fades a moment after it leaves, still taking clicks")
{
    // The owner, 2026-11-07: the playlist's should "fade away when the cursor is not hovering on the playlist for a while".
    struct Editor : public juce::Component
    {
        Editor()
        {
            bar.setRangeLimits (0.0, 100.0);
            bar.setCurrentRange (0.0, 10.0);
            addAndMakeVisible (bar);
        }

        void resized() override { bar.setBounds (getLocalBounds().removeFromRight (phazite::ui::ScrollTint::overlayWidth)); }

        juce::ScrollBar bar { true };
        phazite::ui::ScrollBarFade fade { bar, *this };
    } editor;

    phazite::test::TestWindow window (editor, 200, 200);

    // The pointer away from the editor to start with. Without a display (Linux's CI) JUCE knows only where the tests' pointer
    // last was, which an earlier test may have left over this window; JUCE then moves it over the editor of its own accord
    // a moment later, which showed the bar while the test waited for nothing to repaint.
    phazite::test::Mouse (editor).moveTo ({ -100, -100 });

    // Faded out until it is wanted, though its range was set (the editor scrolling itself doesn't show it), and then nothing
    // repaints.
    const auto rests = [&window]
    {
        phazite::test::dispatchPendingMessages();
        window.takeRepaintedArea();
        phazite::test::dispatchMessagesUntil ([] { return false; }, 100);
        return window.takeRepaintedArea().isEmpty();
    };

    CHECK (juce::exactlyEqual (editor.bar.getAlpha(), 0.0f));
    CHECK_FALSE (editor.fade.isShown());
    CHECK (rests());

    // The pointer over its editor shows it at once, while it keeps moving too, more often than a frame: each move doesn't put
    // the fade off (it did until the fade's timer ran on rather than start again at each).
    phazite::test::Mouse mouse (editor);
    mouse.moveTo ({ 50, 50 });
    CHECK (editor.fade.isShown());
    const auto until = juce::Time::getMillisecondCounterHiRes() + 1000.0;

    for (int i = 0; ! juce::exactlyEqual (editor.bar.getAlpha(), 1.0f) && juce::Time::getMillisecondCounterHiRes() < until; ++i)
    {
        mouse.moveTo ({ 50 + i % 2, 50 });
        phazite::test::dispatchMessagesUntil ([] { return false; }, 4);
    }

    CHECK (juce::exactlyEqual (editor.bar.getAlpha(), 1.0f));

    // Gone from the editor, it fades out a moment later; faded, it still takes clicks where it is, and rests.
    mouse.moveTo ({ -20, -20 });
    CHECK (phazite::test::dispatchMessagesUntil ([&editor] { return juce::exactlyEqual (editor.bar.getAlpha(), 0.0f); }, 4000));
    CHECK_FALSE (editor.fade.isShown());
    CHECK (editor.getComponentAt (editor.bar.getBounds().getCentre()) == &editor.bar);
    CHECK (rests());
}

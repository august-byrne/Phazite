// SPDX-License-Identifier: AGPL-3.0-or-later
// The synth's editor (src/ui/devices/SynthEditor.h), driven as the user would: its tables drawn and named,
// the table menu, a sound dropped on a table, a drag through a table, its controls in their groups (the unison's stack and
// drift among them), how it fits a shorter Devices view, its filter's dot and an envelope as a note plays, and its pitch
// knob taking a value typed.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/SynthEditor.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace
{
using phazite::devices::Synth;
using phazite::ui::SynthEditor;

// The synth on a project's first track, and its editor in a window.
struct Screen
{
    Screen()
    {
        auto& s = getSong();
        synth = phazite::setInstrument (s, song.firstTrack, phazite::makeOwnDeviceState (s, Synth::typeID));
        REQUIRE (synth.isValid());
        phazite::getSession (song.context)->update();
        s.getUndoManager().clearUndoHistory();

        editor = std::make_unique<SynthEditor> (phazite::makeDeviceHandle (s, synth));
        window = std::make_unique<phazite::test::TestWindow> (*editor, editor->getWidth(), editor->getHeight());
    }

    ~Screen()
    {
        window = nullptr;
        editor = nullptr;
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }
    float getValue (const char* id) const { return phazite::getValueSetByHand (getSong(), phazite::ParameterRef { synth, id }); }
    juce::String describeUndo() const { return phazite::describeUndo (&getSong()); }

    phazite::test::TemporaryFolder temp; // for sound files outside the project
    phazite::test::TestSong song;
    phazite::DeviceID synth;
    std::unique_ptr<SynthEditor> editor;
    std::unique_ptr<phazite::test::TestWindow> window;
};
} // namespace

TEST_CASE ("The synth's editor draws each oscillator's table and names it")
{
    Screen screen;
    auto& editor = *screen.editor;
    CHECK (dynamic_cast<SynthEditor*> (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (screen.getSong(), screen.synth)).get())
           != nullptr);
    phazite::test::checkChildrenApart (editor);

    for (int osc = 0; osc < SynthEditor::numOscillators; ++osc)
    {
        CHECK (editor.getTableButton (osc).getButtonText() == juce::String ("Basic Shapes"));
        CHECK (editor.getTableButton (osc).getTitle() == "Osc " + juce::String (osc + 1) + " Wavetable Choice");
        CHECK (phazite::test::findComponentWithTitle (editor, "Osc " + juce::String (osc + 1) + " Wavetable") != nullptr);
    }

    // Every parameter has its control, titled with its whole name and named within its part: under "Envelope 2", its
    // attack is "Attack".
    for (const auto& parameter : Synth::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        const auto* control = editor.findControl (parameter.id);
        REQUIRE (control != nullptr);
        CHECK (control->getTitle() == juce::String (parameter.name.data(), parameter.name.size()));
    }

    juce::StringArray names;

    for (auto* label : phazite::test::findAll<juce::Label> (editor))
        names.add (label->getText());

    CHECK (phazite::test::findComponentWithTitle (editor, "Envelope 2") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (editor, "Mod 8 Source") != nullptr);

    // As wide in a short dock as in a tall one: a shorter dock shows a part at a time under tabs.
    CHECK (editor.getWidthForHeight (editor.getMinimumHeight()) == editor.getWidthForHeight (editor.getHeight()));
    CHECK (editor.getMinimumHeight() <= 235); // the default dock's height in a 1280 x 800 window
    CHECK (editor.getWidth() < 1100);
    CHECK_FALSE (names.contains ("2 Attack"));
    CHECK_FALSE (names.contains ("Envelope 2 Attack"));
}

TEST_CASE ("The synth's table menu lists the factory tables and puts the one chosen on the oscillator")
{
    Screen screen;
    auto& editor = *screen.editor;
    auto menu = editor.getTableMenu (0);

    const auto* basic = phazite::test::findMenuItem (menu, { "Basic Shapes" });
    REQUIRE (basic != nullptr);
    CHECK (basic->isTicked);
    CHECK (phazite::test::findMenuItem (menu, { "Load Wavetable File..." }) != nullptr);

    REQUIRE (phazite::test::runMenuItem (menu, { "Vowels" }));
    CHECK (editor.getTableButton (0).getButtonText() == juce::String ("Vowels"));
    CHECK (screen.describeUndo() == juce::String ("Undo Put Vowels on Osc 1"));
    CHECK (phazite::test::findMenuItem (editor.getTableMenu (0), { "Vowels" })->isTicked);

    // Undo shows on the editor.
    phazite::undo (screen.getSong());
    editor.refresh();
    CHECK (editor.getTableButton (0).getButtonText() == juce::String ("Basic Shapes"));
}

TEST_CASE ("A sound dropped on a wavetable oscillator's drawing becomes its table")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto sound = phazite::test::writeSound (screen.temp.folder.getChildFile ("Drops/Choir.wav"), 0.2);

    REQUIRE (editor.dropFile (1, sound));
    phazite::test::waitForAudioWork(); // its sound copied into the project in the background (known bug 123)
    editor.refresh();                  // as the devices view refreshes it once the song changed
    CHECK (editor.getTableButton (1).getButtonText() == juce::String ("Choir"));
    CHECK (screen.describeUndo() == juce::String ("Undo Put Choir on Osc 2"));

    // Its menu shows the file, ticked, above the factory tables, none of them ticked.
    const auto menu = editor.getTableMenu (1);
    const auto* choir = phazite::test::findMenuItem (menu, { "Choir" });
    REQUIRE (choir != nullptr);
    CHECK (choir->isTicked);
    CHECK_FALSE (choir->isEnabled);
    CHECK_FALSE (phazite::test::findMenuItem (menu, { "Basic Shapes" })->isTicked);

    // Not a sound: nothing changes.
    const auto text = screen.temp.folder.getChildFile ("words.txt");
    REQUIRE (text.replaceWithText ("words"));
    CHECK_FALSE (editor.dropFile (0, text));
    CHECK (editor.getTableButton (0).getButtonText() == juce::String ("Basic Shapes"));
}

TEST_CASE ("Dragging across a wavetable oscillator's drawing moves its position, as one undo step")
{
    Screen screen;
    auto& editor = *screen.editor;
    auto& view = editor.getTableView (0);
    const auto before = screen.getValue ("osc1Position");
    phazite::test::Mouse mouse (view);
    const auto centre = view.getLocalBounds().getCentre();

    // To the left: down the table.
    mouse.press (centre);
    mouse.dragTo (centre.translated (-view.getWidth() / 4, 0));
    mouse.release();
    CHECK (screen.getValue ("osc1Position") == doctest::Approx (before - 0.25f).epsilon (0.02));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Osc 1 Position"));

    phazite::undo (screen.getSong());
    CHECK (screen.getValue ("osc1Position") == doctest::Approx (before));
}

TEST_CASE ("The synth's editor fits the dock: its parts in pages, the modulation a list down the side")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto fullHeight = editor.getHeight();
    const auto shortest = editor.getMinimumHeight();
    REQUIRE (shortest <= fullHeight);
    CHECK (editor.getWidthForHeight (shortest) == editor.getWidthForHeight (fullHeight));

    // As wide as its parts side by side, at its least height: every part showing; the modulation a list down the side, a part
    // at a time.
    editor.setSize (editor.getWidthForHeight (shortest), shortest);
    phazite::test::checkChildrenApart (editor);
    CHECK_FALSE (editor.getLayout().usesPages());
    CHECK (editor.getTableView (0).isShowing());
    CHECK (editor.getTableView (1).isShowing());
    CHECK (editor.getLayout().usesTabs (6));

    // As wide and as tall as the dock at the default window: four pages, the first with both oscillators, whole, the last the
    // unison beside the modulation.
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    REQUIRE (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    editor.setSize (dockWidth, dockHeight);
    REQUIRE (editor.getLayout().getNumPages() == 4);
    CHECK (editor.getLayout().getPageName (3).contains ("Unison"));
    CHECK (editor.getLayout().getPageName (3).contains ("Modulation"));
    CHECK (editor.getTableView (0).isShowing());
    CHECK (editor.getTableView (1).isShowing());
    CHECK (editor.getTableView (0).getHeight() > 0);
    phazite::test::checkChildrenApart (editor);

    editor.getLayout().showPage (1);
    CHECK_FALSE (editor.getTableView (0).isShowing());
    CHECK (editor.findControl ("cutoff")->isShowing());
}

TEST_CASE ("The synth's editor shows the unison's stack and drift with its other controls, on one page of the dock")
{
    Screen screen;
    auto& editor = *screen.editor;

    // In the dock at the default window: the sub, the noise and how it plays on the third page, and the unison's six controls
    // on the fourth, beside the modulation, all whole, none under a tab.
    constexpr int dockWidth = 552, dockHeight = 268;
    editor.setSize (dockWidth, dockHeight);
    REQUIRE (editor.getLayout().getNumPages() == 4);
    CHECK (editor.getLayout().getPageName (2).contains ("Sub and Noise"));
    CHECK (editor.getLayout().getPageName (3).contains ("Unison"));

    for (int column = 0; column < 6; ++column)
        CHECK_FALSE (editor.getLayout().usesTabs (column));

    const auto checkShowing = [&editor] (std::initializer_list<const char*> ids)
    {
        for (const auto* id : ids)
        {
            CAPTURE (id);
            auto* control = editor.findControl (id);
            REQUIRE (control != nullptr);
            CHECK (control->isShowing());
            CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
        }
    };

    editor.getLayout().showPage (2);
    phazite::test::checkChildrenApart (editor);
    checkShowing ({ "subLevel", "noiseLevel", "voiceMode", "glide", "glideMode", "phase", "bendRange", "velocity", "level", "quality" });

    editor.getLayout().showPage (3);
    phazite::test::checkChildrenApart (editor);
    checkShowing ({ "unisonVoices", "unisonDetune", "unisonSpread", "unisonStack", "unisonDrift", "unisonPosition" });

    // Named within their part, as their neighbours are.
    juce::StringArray names;

    for (auto* label : phazite::test::findAll<juce::Label> (editor))
        if (label->isShowing())
            names.add (label->getText());

    CHECK (names.contains ("Stack"));
    CHECK (names.contains ("Drift"));
    CHECK_FALSE (names.contains ("Unison Stack"));

    // The stack is a list of its five ways, off at first, and the drift a knob in cents, at none: each changes its parameter
    // as an undo step the model names.
    auto* stack = dynamic_cast<juce::ComboBox*> (editor.findControl ("unisonStack"));
    REQUIRE (stack != nullptr);
    CHECK (stack->getTitle() == juce::String ("Unison Stack"));
    CHECK (stack->getText() == juce::String ("Off"));
    REQUIRE (stack->getNumItems() == 5);
    CHECK (stack->getItemText (2) == juce::String ("Two Octaves"));
    CHECK (stack->getItemText (4) == juce::String ("Power Chord"));
    stack->setSelectedItemIndex ((int) Synth::Stack::twoOctaves, juce::sendNotificationSync);
    CHECK (screen.getValue ("unisonStack") == doctest::Approx ((float) Synth::Stack::twoOctaves));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Unison Stack"));

    auto* drift = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("unisonDrift"));
    REQUIRE (drift != nullptr);
    CHECK (drift->getTitle() == juce::String ("Unison Drift"));
    CHECK (drift->getValue() == doctest::Approx (0.0));
    CHECK (drift->getMaximum() == doctest::Approx (50.0));
    drift->setValue (12.0, juce::sendNotificationSync);
    CHECK (screen.getValue ("unisonDrift") == doctest::Approx (12.0f));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Unison Drift"));

    // Undo shows on the editor.
    phazite::undo (screen.getSong());
    phazite::undo (screen.getSong());
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (stack->getText() == juce::String ("Off"));
}

TEST_CASE (
    "The synth's editor shows each LFO's fade after its retrigger, Glide Mode after Glide, Phase after it, and Drift and the slots' amounts in the matrix")
{
    Screen screen;
    auto& editor = *screen.editor;

    // Each LFO's fade a knob in milliseconds, named within its part; Glide Mode a list of its two ways.
    for (const auto* id : { "lfo1FadeIn", "lfo2FadeIn" })
    {
        CAPTURE (id);
        auto* fade = dynamic_cast<phazite::ui::Knob*> (editor.findControl (id));
        REQUIRE (fade != nullptr);
        CHECK (fade->getValue() == doctest::Approx (0.0));
        CHECK (fade->getMaximum() == doctest::Approx (10000.0));
    }

    auto* fade = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("lfo1FadeIn"));
    fade->setValue (250.0, juce::sendNotificationSync);
    CHECK (screen.getValue ("lfo1FadeIn") == doctest::Approx (250.0f));
    CHECK (screen.describeUndo() == juce::String ("Undo Change LFO 1 Fade In"));

    auto* glideMode = dynamic_cast<juce::ComboBox*> (editor.findControl ("glideMode"));
    REQUIRE (glideMode != nullptr);
    REQUIRE (glideMode->getNumItems() == 2);
    CHECK (glideMode->getText() == juce::String ("Always"));
    CHECK (glideMode->getItemText (1) == juce::String ("Legato"));
    glideMode->setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK (screen.getValue ("glideMode") == doctest::Approx ((float) Synth::GlideMode::legato));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Glide Mode"));

    // Phase a list of Reset and Free, as the FM synth's, at Reset, its tooltip saying what each does.
    auto* phase = dynamic_cast<juce::ComboBox*> (editor.findControl ("phase"));
    REQUIRE (phase != nullptr);
    CHECK (phase->getTitle() == juce::String ("Phase"));
    REQUIRE (phase->getNumItems() == 2);
    CHECK (phase->getText() == juce::String ("Reset"));
    CHECK (phase->getItemText (1) == juce::String ("Free"));
    CHECK (phase->getTooltip().contains ("run on from where they were"));
    phase->setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK (screen.getValue ("phase") == doctest::Approx ((float) Synth::Phase::free));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Phase"));

    // The matrix's sources end with Drift, its targets with the eight slots' amounts, of which a slot's own can't be chosen.
    auto* source = dynamic_cast<juce::ComboBox*> (editor.findControl ("mod2Source"));
    REQUIRE (source != nullptr);
    CHECK (source->getItemText (source->getNumItems() - 1) == juce::String ("Drift"));

    for (int slot = 0; slot < Synth::numModSlots; ++slot)
    {
        CAPTURE (slot);
        auto* target = dynamic_cast<juce::ComboBox*> (editor.findControl ("mod" + std::to_string (slot + 1) + "Target"));
        REQUIRE (target != nullptr);
        REQUIRE (target->getNumItems() == (int) Synth::ModTarget::slot8Amount + 1);

        for (int other = 0; other < Synth::numModSlots; ++other)
        {
            const auto item = (int) Synth::getAmountTarget (other);
            CHECK (target->getItemText (item) == "Mod " + juce::String (other + 1) + " Amount");
            CHECK (target->isItemEnabled (item + 1) == (other != slot));
        }
    }

    auto* target = dynamic_cast<juce::ComboBox*> (editor.findControl ("mod1Target"));
    target->setSelectedItemIndex ((int) Synth::ModTarget::slot2Amount, juce::sendNotificationSync);
    CHECK (screen.getValue ("mod1Target") == doctest::Approx ((float) Synth::ModTarget::slot2Amount));
}

TEST_CASE ("The synth's editor drags its filter's dot for the cutoff and resonance, and shows a note's envelope as it plays")
{
    Screen screen;
    auto& editor = *screen.editor;

    // The dot dragged to the middle: a lower cutoff, the resonance half way, one undo step.
    auto& filter = editor.getFilter();
    const auto cutoffBefore = screen.getValue ("cutoff");
    phazite::test::Mouse mouse (filter);
    mouse.press (filter.getDotPosition().roundToInt());
    mouse.dragTo (filter.getLocalBounds().getCentre());
    mouse.release();
    CHECK (screen.getValue ("cutoff") < cutoffBefore);
    CHECK (screen.getValue ("resonance") == doctest::Approx (0.5f).epsilon (0.1));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Filter Cutoff"));

    // A note held: the amp's envelope shows how far it has got.
    auto* synth = phazite::makeDeviceHandle (screen.getSong(), screen.synth).getProcessor<Synth>();
    REQUIRE (synth != nullptr);
    synth->prepare (48000.0, 4800);
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (parameter.defaultValue);

    juce::AudioBuffer<float> buffer (2, 4800);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    const phazite::devices::NoteEvent note { .type = phazite::devices::NoteEvent::Type::noteOn, .sample = 0, .number = 60, .value = 1.0f };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = buffer.getNumSamples();
    block.parameters = values;
    block.events = { &note, 1 };
    synth->process (block);

    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getEnvelope (0).getShownLevel() > 0.1f);
}

TEST_CASE ("The synth's pitch knob takes a value typed at its value, exactly, and drags in whole semitones")
{
    // The owner, 2026-11-09: "this is very important for things like pitch where a tiny bit off is totally wrong".
    Screen screen;
    auto& editor = *screen.editor;
    auto& s = screen.getSong();
    auto* knob = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("osc2Pitch"));
    REQUIRE (knob != nullptr);
    auto* readout = dynamic_cast<juce::Label*> (knob->getValueReadout());
    REQUIRE (readout != nullptr);
    REQUIRE (readout->isShowing());
    const auto pitch = [&screen] { return screen.getValue ("osc2Pitch"); };

    // A click on its value, under it, opens the field there; "+7" and Enter set it to 7 semitones exactly, one undo step.
    phazite::test::Mouse mouse (editor);
    mouse.click (editor.getLocalArea (readout, readout->getLocalBounds()).getCentre());
    REQUIRE (knob->getValueField() != nullptr);

    for (const auto typed : { '+', '7' })
        screen.window->pressKey (juce::KeyPress (typed, {}, (juce::juce_wchar) typed));

    screen.window->pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (juce::exactlyEqual (pitch(), 7.0f));
    CHECK (s.getUndoManager().getUndoDescription() == juce::String ("Change Osc 2 Pitch"));
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (readout->getText() == juce::String ("+7 st"));
    phazite::test::dispatchPendingMessages();

    // Dragged, it stays on whole semitones.
    const auto centre = editor.getLocalArea (knob, knob->getLocalBounds()).getCentre();
    mouse.pause();
    mouse.press (centre);
    mouse.dragTo (centre + juce::Point<int> (0, -7));
    mouse.release();
    CHECK (pitch() > 7.0f);
    CHECK (juce::exactlyEqual (pitch(), std::round (pitch())));
}

TEST_CASE ("The synth's filter has its Character as two buttons after its Drive, and its picture says Driven")
{
    Screen screen;
    auto& editor = *screen.editor;
    auto* buttons = dynamic_cast<phazite::ui::ChoiceButtons*> (editor.findControl ("filterCharacter"));
    REQUIRE (buttons != nullptr);
    REQUIRE (buttons->getNumButtons() == 2);
    CHECK (buttons->getButton (0).getButtonText() == juce::String ("Clean"));
    CHECK (buttons->getButton (1).getButtonText() == juce::String ("Driven"));
    CHECK (buttons->getButton (0).getToggleState());

    // After the Drive: right of it, in its row, each button wider than its word.
    const auto* drive = editor.findControl ("drive");
    REQUIRE (drive != nullptr);
    REQUIRE (buttons->isShowing());
    CHECK (buttons->getScreenBounds().getX() >= drive->getScreenBounds().getRight());
    CHECK (buttons->getScreenBounds().getCentreY() > drive->getScreenBounds().getY());
    CHECK (buttons->getScreenBounds().getCentreY() < drive->getScreenBounds().getBottom());

    for (int i = 0; i < buttons->getNumButtons(); ++i)
    {
        auto* button = dynamic_cast<juce::TextButton*> (&buttons->getButton (i));
        REQUIRE (button != nullptr);
        const auto font = button->getLookAndFeel().getTextButtonFont (*button, button->getHeight());
        CHECK ((float) button->getWidth() > juce::GlyphArrangement::getStringWidth (font, button->getButtonText()) + 4.0f);
    }
    CHECK_FALSE (editor.getFilter().getReadout().contains ("Driven"));

    // Driven: a click, one undo step, and the picture's readout says so, its curve Driven's small-signal response.
    phazite::test::Mouse mouse (buttons->getButton (1));
    mouse.click (buttons->getButton (1).getLocalBounds().getCentre());
    CHECK (screen.getValue ("filterCharacter") == doctest::Approx (1.0f));
    CHECK (screen.describeUndo() == juce::String ("Undo Change Filter Character"));
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (buttons->getButton (1).getToggleState());
    CHECK (editor.getFilter().getReadout().startsWith ("Driven  "));

    // A type the Driven character doesn't play (a comb) plays Clean, and the readout says nothing of it.
    phazite::setValueSetByHand (screen.getSong(), phazite::ParameterRef { screen.synth, "filterType" },
                                (float) phazite::devices::dsp::FilterMode::combPlus);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK_FALSE (editor.getFilter().getReadout().contains ("Driven"));
}

TEST_CASE ("The synth's wavetable picture draws the frame it plays through its warp, at the amount it plays it at")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto set = [&screen] (const char* id, float value)
    { phazite::setValueSetByHand (screen.getSong(), phazite::ParameterRef { screen.synth, id }, value); };

    // The frame drawn as the device reads its place in the cycle (the synth's own mapping, written out here as it plays
    // it): the saw of Basic Shapes, where the first oscillator plays at first.
    const auto table = phazite::makeDeviceHandle (screen.getSong(), screen.synth).getWavetable (0);
    REQUIRE (table != nullptr);
    const auto place = screen.getValue ("osc1Position") * (float) (table->getNumFrames() - 1);
    const auto first = std::min ((int) place, table->getNumFrames() - 1);
    const auto from = table->getFrameForDrawing (first), to = table->getFrameForDrawing (std::min (first + 1, table->getNumFrames() - 1));
    const auto morph = place - (float) first;
    const auto readDrawn = [] (std::span<const float> samples, float along)
    { return samples[std::min (samples.size() - 1, (size_t) (along * (float) samples.size()))]; };
    const auto read = [&] (float at) { return readDrawn (from, at) + morph * (readDrawn (to, at) - readDrawn (from, at)); };
    const auto wrap = [] (float x) { return x - std::floor (x) < 1.0f ? x - std::floor (x) : 0.0f; };

    const auto expected = [&] (Synth::Warp warp, float amount, float t)
    {
        switch (warp)
        {
            case Synth::Warp::sync:
                return read (wrap (t * std::exp2 (3.0f * amount)));
            case Synth::Warp::bendUp:
            case Synth::Warp::bendDown:
            {
                const auto bent = warp == Synth::Warp::bendUp ? 1.0f + 7.0f * amount : 1.0f / (1.0f + 7.0f * amount);
                return read (wrap (t * bent / (1.0f + (bent - 1.0f) * t)));
            }
            case Synth::Warp::pulseWidth:
            {
                const auto width = 1.0f - 0.95f * amount;
                return read (t < width ? wrap (t / width) : 0.0f);
            }
            case Synth::Warp::mirror:
                return read (t) + amount * (read (wrap (t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t)) - read (t));
            case Synth::Warp::none:
            case Synth::Warp::fm:
            case Synth::Warp::ringMod:
                break;
        }

        return read (t);
    };

    const auto compare = [&] (Synth::Warp warp, float amount)
    {
        const auto wave = editor.getPlayedWave (0);
        REQUIRE (wave.size() > 100);
        auto mostOff = 0.0f;

        for (size_t i = 0; i < wave.size(); ++i)
            mostOff = std::max (mostOff, std::abs (wave[i] - expected (warp, amount, (float) i / (float) (wave.size() - 1))));

        return mostOff;
    };

    for (const auto warp : { Synth::Warp::sync, Synth::Warp::bendUp, Synth::Warp::bendDown, Synth::Warp::pulseWidth, Synth::Warp::mirror,
                             Synth::Warp::fm, Synth::Warp::ringMod })
    {
        for (const auto amount : { 0.3f, 0.8f })
        {
            CAPTURE ((int) warp);
            CAPTURE (amount);
            set ("osc1Warp", (float) warp);
            set ("osc1WarpAmount", amount);
            editor.refresh();
            CHECK (compare (warp, amount) < 1.0e-5f);

            // FM and ring modulation, which the other oscillator's sound makes, name themselves.
            const auto readout = editor.getTableReadout (0);
            CHECK (readout
                   == (warp == Synth::Warp::fm ? juce::String ("FM")
                                               : (warp == Synth::Warp::ringMod ? juce::String ("Ring Mod") : juce::String())));
        }
    }

    // While a note plays, at the amount its modulation moves it to: sync at 20 %, a Velocity slot moving it by 50 %, a note at
    // full velocity: drawn at 70 %.
    set ("osc1Warp", (float) Synth::Warp::sync);
    set ("osc1WarpAmount", 0.2f);
    set ("mod2Source", (float) Synth::ModSource::velocity);
    set ("mod2Target", (float) Synth::ModTarget::osc1Warp);
    set ("mod2Amount", 0.5f);
    auto* synth = phazite::makeDeviceHandle (screen.getSong(), screen.synth).getProcessor<Synth>();
    REQUIRE (synth != nullptr);
    synth->prepare (48000.0, 4800);
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (screen.getValue (parameter.id.data()));

    juce::AudioBuffer<float> buffer (2, 4800);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    const phazite::devices::NoteEvent note { .type = phazite::devices::NoteEvent::Type::noteOn, .sample = 0, .number = 60, .value = 1.0f };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = buffer.getNumSamples();
    block.parameters = values;
    block.events = { &note, 1 };
    synth->process (block);
    editor.refresh();
    CHECK (compare (Synth::Warp::sync, 0.7f) < 1.0e-5f);
}

TEST_CASE ("A knob the synth's matrix moves shows a dashed ring over the range its slots move it, beside Phazite's modulators' ring")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto set = [&screen] (const char* id, float value)
    { phazite::setValueSetByHand (screen.getSong(), phazite::ParameterRef { screen.synth, id }, value); };
    auto* cutoff = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("cutoff"));
    auto* resonance = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("resonance"));
    REQUIRE (cutoff != nullptr);
    REQUIRE (resonance != nullptr);
    const auto at = [cutoff] (double hertz) { return (float) cutoff->valueToProportionOfLength (hertz); };

    // A new synth's first slot: Envelope 2 (0 to 1) on the cutoff by 25 %, two of its eight octaves: from 2.5 to 10 kHz.
    editor.refresh();
    REQUIRE (cutoff->getOwnModulation().has_value());
    CHECK (cutoff->getOwnModulation()->low == doctest::Approx (at (2500.0)).epsilon (0.001));
    CHECK (cutoff->getOwnModulation()->high == doctest::Approx (at (10000.0)).epsilon (0.001));
    CHECK_FALSE (cutoff->getOwnModulation()->isOn);    // no note: no dot
    CHECK_FALSE (cutoff->getModulation().has_value()); // none of Phazite's modulators
    CHECK_FALSE (resonance->getOwnModulation().has_value());

    // A second slot on it, LFO 1 (-1 to 1) by 12.5 %, an octave each way: the two added, from an octave under to three
    // over, up to the knob's end.
    set ("mod2Source", (float) Synth::ModSource::lfo1);
    set ("mod2Target", (float) Synth::ModTarget::cutoff);
    set ("mod2Amount", 0.125f);
    editor.refresh();
    REQUIRE (cutoff->getOwnModulation().has_value());
    CHECK (cutoff->getOwnModulation()->low == doctest::Approx (at (1250.0)).epsilon (0.001));
    CHECK (cutoff->getOwnModulation()->high == doctest::Approx (at (20000.0)).epsilon (0.001));

    // Pitch on both oscillators' Pitch knobs, by its 24 semitones for 100 %; the first slot off: the cutoff's ring goes
    // with it but for the second slot's.
    set ("mod3Source", (float) Synth::ModSource::modWheel);
    set ("mod3Target", (float) Synth::ModTarget::pitch);
    set ("mod3Amount", 0.5f);
    set ("mod1Source", (float) Synth::ModSource::none);
    editor.refresh();
    auto* pitch = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("osc2Pitch"));
    REQUIRE (pitch != nullptr);
    REQUIRE (pitch->getOwnModulation().has_value());
    CHECK (pitch->getOwnModulation()->low == doctest::Approx (pitch->valueToProportionOfLength (0.0)));
    CHECK (pitch->getOwnModulation()->high == doctest::Approx (pitch->valueToProportionOfLength (12.0)));
    CHECK (cutoff->getOwnModulation()->low == doctest::Approx (at (1250.0)).epsilon (0.001));
    CHECK (cutoff->getOwnModulation()->high == doctest::Approx (at (5000.0)).epsilon (0.001));

    // Every slot off: no ring.
    set ("mod2Source", (float) Synth::ModSource::none);
    set ("mod3Source", (float) Synth::ModSource::none);
    editor.refresh();
    CHECK_FALSE (cutoff->getOwnModulation().has_value());
    CHECK_FALSE (pitch->getOwnModulation().has_value());
}

TEST_CASE ("The synth's matrix ring adds what a slot on another's amount moves, and while a note plays is about its cutoff")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto set = [&screen] (const char* id, float value)
    { phazite::setValueSetByHand (screen.getSong(), phazite::ParameterRef { screen.synth, id }, value); };
    auto* cutoff = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("cutoff"));
    REQUIRE (cutoff != nullptr);
    const auto at = [cutoff] (double hertz) { return (float) cutoff->valueToProportionOfLength (hertz); };

    // The first slot, Envelope 2 on the cutoff by 25 % (2.5 to 10 kHz), its amount moved by LFO 1 by 50 %: from -25 % to
    // 75 %, so the cutoff from two octaves under the knob (625 Hz) to the knob's end.
    set ("mod2Source", (float) Synth::ModSource::lfo1);
    set ("mod2Target", (float) Synth::ModTarget::slot1Amount);
    set ("mod2Amount", 0.5f);
    editor.refresh();
    REQUIRE (cutoff->getOwnModulation().has_value());
    CHECK (cutoff->getOwnModulation()->low == doctest::Approx (at (625.0)).epsilon (0.001));
    CHECK (cutoff->getOwnModulation()->high == doctest::Approx (at (20000.0)).epsilon (0.001));

    // Without the mover, key tracking at 100 % and a note an octave over C4 playing: the ring is about 5 kHz, the cutoff the
    // note's key makes of the knob's, from it to the knob's end, and the dot where the note plays the cutoff, inside it.
    set ("mod2Source", (float) Synth::ModSource::none);
    set ("keyTracking", 1.0f);
    auto* synth = phazite::makeDeviceHandle (screen.getSong(), screen.synth).getProcessor<Synth>();
    REQUIRE (synth != nullptr);
    synth->prepare (48000.0, 4800);
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (screen.getValue (parameter.id.data()));

    juce::AudioBuffer<float> buffer (2, 4800);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    const phazite::devices::NoteEvent note { .type = phazite::devices::NoteEvent::Type::noteOn, .sample = 0, .number = 72, .value = 1.0f };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = buffer.getNumSamples();
    block.parameters = values;
    block.events = { &note, 1 };
    synth->process (block);
    editor.refresh();
    REQUIRE (cutoff->getOwnModulation().has_value());
    CHECK (cutoff->getOwnModulation()->low == doctest::Approx (at (5000.0)).epsilon (0.001));
    CHECK (cutoff->getOwnModulation()->high == doctest::Approx (at (20000.0)).epsilon (0.001));
    CHECK (cutoff->getOwnModulation()->isOn);
    CHECK (cutoff->getOwnModulation()->now >= cutoff->getOwnModulation()->low);
    CHECK (cutoff->getOwnModulation()->now <= cutoff->getOwnModulation()->high);
}

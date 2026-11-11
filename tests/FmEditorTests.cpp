// SPDX-License-Identifier: AGPL-3.0-or-later
// The FM synth's editor (src/ui/devices/FmEditor.h), driven as the user would: a control for every parameter; the algorithm's
// picture, a click on which shows all eight, a click on one of which plays it as one undo step and closes them, with the
// arrows beside its name and the arrow keys stepping through them; every operator's column saying what it does in the
// algorithm; the tabs showing one group of every operator's controls at a time; a fixed operator's frequency in place of its
// ratio; an operator switched off shown faint; a strip dragged as one undo step; each operator's Phase in its Tone group, in
// degrees, faint while the phases run free; the operator under the pointer lit; the filter's envelope and velocity, as the
// synth's editor shows its own; and how it fits the default dock.
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/fm/FmSynth.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/FmEditor.h"
#include "ui/devices/SynthParts.h"

namespace
{
using phazite::devices::FmSynth;
using phazite::ui::FmEditor;
using Group = FmEditor::Group;

// The FM synth on a project's first track, and its editor in a window.
struct Screen
{
    Screen()
    {
        synth = song.setInstrument (song.firstTrack, FmSynth::typeID);
        REQUIRE (synth.isValid());
        phazite::getSession (song.context)->update();
        getSong().getUndoManager().clearUndoHistory();
        editor = std::make_unique<FmEditor> (phazite::makeDeviceHandle (getSong(), synth));
        window = std::make_unique<phazite::test::TestWindow> (*editor, editor->getWidth(), editor->getHeight());
    }

    ~Screen()
    {
        window = nullptr;
        editor = nullptr;
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }
    float getValue (const char* id) const { return phazite::getValueSetByHand (getSong(), phazite::ParameterRef { synth, id }); }
    void setValue (const char* id, float value) { song.setParameter (synth, id, value); }
    juce::String describeUndo() const { return phazite::describeUndo (&getSong()); }

    // The editor at the size of the default dock, beside the Devices view's column.
    void dock()
    {
        constexpr int dockWidth = 552, dockHeight = 268;
        REQUIRE (editor->getMinimumHeightAtWidth (dockWidth) <= dockHeight);
        window->setSize (dockWidth, dockHeight);
        editor->setSize (dockWidth, dockHeight);
    }

    phazite::test::TestSong song;
    phazite::DeviceID synth;
    std::unique_ptr<FmEditor> editor;
    std::unique_ptr<phazite::test::TestWindow> window;
};
} // namespace

TEST_CASE ("The FM synth's editor has a titled control for every parameter")
{
    Screen screen;
    auto& editor = *screen.editor;
    CHECK (dynamic_cast<FmEditor*> (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (screen.getSong(), screen.synth)).get())
           != nullptr);

    for (const auto& parameter : FmSynth::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        const auto* control = editor.findControl (parameter.id);
        REQUIRE (control != nullptr);
        CHECK (control->getTitle() == juce::String (parameter.name.data(), parameter.name.size()));
    }
}

TEST_CASE ("A click on the FM synth's algorithm shows all eight; a click on one plays it, as one undo step, and closes them")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    auto& view = editor.getAlgorithmView();
    CHECK (screen.getValue ("algorithm") == 4.0f); // two pairs, the piano's
    CHECK (editor.getAlgorithmGrid() == nullptr);

    phazite::test::Mouse (view).click (view.getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    auto* grid = editor.getAlgorithmGrid();
    REQUIRE (grid != nullptr);
    CHECK (grid->isShowing());

    // The first, at the top left.
    phazite::test::Mouse (*grid).click ({ 30, 30 });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getValue ("algorithm") == 0.0f);
    CHECK (screen.describeUndo() == juce::String ("Undo Change Algorithm"));
    CHECK (phazite::test::dispatchMessagesUntil ([&editor] { return editor.getAlgorithmGrid() == nullptr; }));

    phazite::undo (screen.getSong());
    CHECK (screen.getValue ("algorithm") == 4.0f);
}

TEST_CASE ("The arrows beside the FM synth's algorithm and the arrow keys step through the eight, round and round")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    screen.setValue ("algorithm", 0.0f);

    editor.getAlgorithmStep (true).triggerClick();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getValue ("algorithm") == 1.0f);
    CHECK (screen.describeUndo() == juce::String ("Undo Change Algorithm"));

    editor.getAlgorithmStep (false).triggerClick();
    phazite::test::dispatchPendingMessages();
    editor.getAlgorithmStep (false).triggerClick();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getValue ("algorithm") == 7.0f); // from the first back to the last

    editor.getAlgorithmView().grabKeyboardFocus();
    screen.window->pressKey (juce::KeyPress (juce::KeyPress::rightKey));
    CHECK (screen.getValue ("algorithm") == 0.0f);
    screen.window->pressKey (juce::KeyPress (juce::KeyPress::downKey));
    CHECK (screen.getValue ("algorithm") == 1.0f);
    screen.window->pressKey (juce::KeyPress (juce::KeyPress::leftKey));
    CHECK (screen.getValue ("algorithm") == 0.0f);
}

TEST_CASE ("Each FM operator's column says what the operator does in the algorithm played")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto arrow = juce::String::fromUTF8 ("\xe2\x86\x92");

    // Two pairs: 4 into 3 and 2 into 1, 3 and 1 heard.
    CHECK (editor.getRoleText (0) == "Heard");
    CHECK (editor.getRoleText (1) == arrow + " 1");
    CHECK (editor.getRoleText (2) == "Heard");
    CHECK (editor.getRoleText (3) == arrow + " 3");

    // One into three; then four side by side, all heard.
    screen.setValue ("algorithm", 6.0f);
    editor.refresh();
    CHECK (editor.getRoleText (3) == arrow + " 3 2 1");
    CHECK (editor.getRoleText (2) == "Heard");

    screen.setValue ("algorithm", 7.0f);
    editor.refresh();

    for (int op = 0; op < FmSynth::numOperators; ++op)
        CHECK (editor.getRoleText (op) == "Heard");
}

TEST_CASE ("The FM synth's tabs show one group of every operator's controls at a time, the envelopes' at first")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    const auto showing = [&editor] (const std::string& id) { return editor.findControl (id)->isShowing(); };

    // Every operator's head: its level and ratio, whatever the group.
    for (const auto* id : { "op1Level", "op2Level", "op3Ratio", "op4Fine" })
        CHECK (showing (id));

    CHECK (editor.getShownGroup() == Group::envelope);

    for (const auto* id : { "op1Attack", "op2Decay", "op3Sustain", "op4Release", "op1AttackCurve", "op4ReleaseCurve" })
        CHECK (showing (id));

    CHECK_FALSE (showing ("op1Wave"));

    phazite::test::Mouse tone (editor.getGroupTab (Group::tone));
    tone.click (editor.getGroupTab (Group::tone).getLocalBounds().getCentre());
    CHECK (editor.getShownGroup() == Group::tone);

    for (const auto* id : { "op1Wave", "op4Wave", "op1Feedback", "op4Feedback", "op1Phase", "op4Phase" })
        CHECK (showing (id));

    CHECK_FALSE (showing ("op1Attack"));

    editor.showGroup (Group::pitch);
    CHECK (showing ("op2Fixed"));
    CHECK (showing ("op3Detune"));
    CHECK_FALSE (showing ("op1Wave"));

    editor.showGroup (Group::scaling);
    CHECK (showing ("op1Velocity"));
    CHECK (showing ("op4KeyScale"));

    for (int op = 0; op < FmSynth::numOperators; ++op)
        phazite::test::checkChildrenApart (editor.getOperatorColumn (op));

    phazite::test::checkChildrenApart (*editor.getOperatorColumn (0).getParentComponent());
}

TEST_CASE ("An FM operator's wave and Fixed switch change it as one undo step; while fixed, its frequency takes its ratio's place")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    editor.showGroup (Group::tone);

    // Its wave: the saw, the third.
    auto& wave = *editor.findControl ("op2Wave");
    REQUIRE (wave.getNumChildComponents() == 4);
    phazite::test::Mouse (*wave.getChildComponent (2)).click (wave.getChildComponent (2)->getLocalBounds().getCentre());
    CHECK (screen.getValue ("op2Wave") == (float) FmSynth::Wave::saw);
    CHECK (screen.describeUndo() == juce::String ("Undo Change Op 2 Wave"));

    // Fixed.
    editor.showGroup (Group::pitch);
    CHECK (editor.findControl ("op1Ratio")->isShowing());
    CHECK_FALSE (editor.findControl ("op1Frequency")->isShowing());
    auto* fixed = dynamic_cast<juce::Button*> (editor.findControl ("op1Fixed"));
    REQUIRE (fixed != nullptr);
    fixed->triggerClick();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getValue ("op1Fixed") == 1.0f);
    editor.refresh();
    CHECK (editor.findControl ("op1Frequency")->isShowing());
    CHECK_FALSE (editor.findControl ("op1Ratio")->isShowing());
    CHECK_FALSE (editor.findControl ("op1Fine")->isShowing());
}

TEST_CASE ("Dragging an FM operator's strip changes it as one undo step; switched off, its column shows faint")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    auto& level = *editor.findControl ("op2Level");
    const auto before = screen.getValue ("op2Level");

    phazite::test::Mouse mouse (level);
    const auto middle = level.getLocalBounds().getCentre();
    mouse.press (middle);
    mouse.dragTo (middle.translated (0, -20));
    mouse.dragTo (middle.translated (0, -40));
    mouse.release();
    CHECK (screen.getValue ("op2Level") > before);
    CHECK (screen.describeUndo() == juce::String ("Undo Change Op 2 Level"));

    // Switched off: its column faint.
    auto* on = dynamic_cast<juce::Button*> (editor.findControl ("op2On"));
    REQUIRE (on != nullptr);
    on->triggerClick();
    phazite::test::dispatchPendingMessages();
    editor.refresh();
    CHECK (screen.getValue ("op2On") == 0.0f);
    CHECK (level.getParentComponent()->getAlpha() < 1.0f);
    CHECK (editor.findControl ("op2Attack")->getParentComponent()->getParentComponent()->getAlpha() < 1.0f);
    CHECK (editor.findControl ("op1Level")->getParentComponent()->getAlpha() == 1.0f);
}

TEST_CASE ("Each FM operator's Phase is in its Tone group, in degrees, dragged as one undo step, and faint while the phases run free")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    editor.showGroup (Group::tone);

    // Under its wave and its feedback, inside its group, for each of the four.
    for (const auto* id : { "op1Phase", "op2Phase", "op3Phase", "op4Phase" })
    {
        CAPTURE (id);
        auto* strip = editor.findControl (id);
        REQUIRE (strip != nullptr);
        CHECK (strip->isShowing());
        phazite::test::checkChildrenApart (*strip->getParentComponent());
        auto* tip = dynamic_cast<juce::SettableTooltipClient*> (strip);
        REQUIRE (tip != nullptr);
        CHECK (tip->getTooltip().contains ("Reset"));
    }

    // Its angle as it reads, and a drag up for more of a turn, one undo step.
    auto* phase = dynamic_cast<juce::Slider*> (editor.findControl ("op3Phase"));
    REQUIRE (phase != nullptr);
    CHECK (screen.getValue ("op3Phase") == 0.0f);
    screen.setValue ("op3Phase", 180.0f);
    editor.refresh();
    CHECK (phase->getTextFromValue (phase->getValue()) == juce::String::fromUTF8 ("180\xc2\xb0"));

    phazite::test::Mouse mouse (*phase);
    const auto middle = phase->getLocalBounds().getCentre();
    mouse.press (middle);
    mouse.dragTo (middle.translated (0, -20));
    mouse.dragTo (middle.translated (0, -40));
    mouse.release();
    CHECK (screen.getValue ("op3Phase") > 180.0f);
    CHECK (screen.getValue ("op3Phase") <= 360.0f);
    CHECK (screen.describeUndo() == juce::String ("Undo Change Op 3 Phase"));

    // While the phases run free it means nothing, and shows faint; the wave and the feedback beside it as they were.
    CHECK (phase->getAlpha() == 1.0f);
    screen.setValue ("phase", (float) FmSynth::Phase::free);
    editor.refresh();
    CHECK (phase->getAlpha() < 1.0f);
    CHECK (editor.findControl ("op3Feedback")->getAlpha() == 1.0f);
    screen.setValue ("phase", (float) FmSynth::Phase::reset);
    editor.refresh();
    CHECK (phase->getAlpha() == 1.0f);
}

TEST_CASE ("The FM synth's filter has its envelope and velocity as the synth's has: the envelope a part of its own, faint while the "
           "filter is off, its picture showing a note's as it plays")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    auto& layout = editor.getLayout();
    REQUIRE (layout.usesPages());

    // The page that shows a control, every page whole.
    const auto pageOf = [&editor, &layout] (const char* id)
    {
        for (int page = 0; page < layout.getNumPages(); ++page)
        {
            layout.showPage (page);
            phazite::test::checkChildrenApart (editor);

            if (editor.findControl (id)->isShowing())
                return page;
        }

        return -1;
    };

    const auto inside = [&editor] (const char* id)
    {
        auto* control = editor.findControl (id);
        return control->isShowing() && editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds()));
    };

    // The filter's own, with how far its envelope and velocity move the cutoff.
    REQUIRE (pageOf ("cutoff") > 0);

    for (const auto* id : { "filterType", "cutoff", "resonance", "keyTracking", "filterSlope", "filterEnvelope", "filterVelocity" })
    {
        CAPTURE (id);
        CHECK (inside (id));
    }

    // Its envelope's times, sustain and bends, under its picture.
    REQUIRE (pageOf ("filterAttack") > 0);

    for (const auto* id :
         { "filterAttack", "filterDecay", "filterSustain", "filterRelease", "filterAttackCurve", "filterDecayCurve", "filterReleaseCurve" })
    {
        CAPTURE (id);
        CHECK (inside (id));
    }

    phazite::ui::EnvelopeView* picture = nullptr;

    for (auto* view : phazite::test::findAll<phazite::ui::EnvelopeView> (editor))
        if (view->getTitle() == "Filter envelope")
            picture = view;

    REQUIRE (picture != nullptr);
    CHECK (picture->isShowing());

    // Faint while the filter is off, as a new FM synth's is, with nothing for it to move.
    CHECK (screen.getValue ("filterOn") == 0.0f);
    CHECK (picture->getParentComponent()->getAlpha() < 1.0f);
    screen.setValue ("filterOn", 1.0f);
    editor.refresh();
    CHECK (picture->getParentComponent()->getAlpha() == 1.0f);

    // A note held: the picture shows how far its envelope has got.
    auto* synth = phazite::makeDeviceHandle (screen.getSong(), screen.synth).getProcessor<FmSynth>();
    REQUIRE (synth != nullptr);
    synth->prepare (48000.0, 4800);
    std::vector<float> values;

    for (const auto& parameter : FmSynth::info.parameters)
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
    CHECK (picture->getShownLevel() > 0.1f);
}

TEST_CASE ("The FM operator under the pointer is lit in the algorithm's picture")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    CHECK (editor.getHoveredOperator() == -1);

    auto& column = editor.getOperatorColumn (2);
    phazite::test::Mouse (column).moveTo (column.getLocalBounds().getCentre());
    editor.refresh();
    CHECK (editor.getHoveredOperator() == 2);

    phazite::test::Mouse (column).moveTo ({ -400, -400 });
    editor.refresh();
    CHECK (editor.getHoveredOperator() == -1);
}

TEST_CASE ("The FM synth's editor fits the default dock: its operators alone on the first page, then the rest")
{
    Screen screen;
    screen.dock();
    auto& editor = *screen.editor;
    CHECK (editor.getLayout().usesPages());
    CHECK (editor.getAlgorithmView().isShowing());

    for (int op = 0; op < FmSynth::numOperators; ++op)
        CHECK (editor.getOperatorColumn (op).isShowing());

    phazite::test::checkChildrenApart (editor);

    editor.getLayout().showPage (1);
    CHECK_FALSE (editor.getAlgorithmView().isShowing());
    CHECK (editor.findControl ("pitchAmount")->isShowing());
}

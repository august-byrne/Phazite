// SPDX-License-Identifier: AGPL-3.0-or-later
// The top bar (src/ui/shell/TopBar.h), driven with the mouse: its tempo field and time signature show the open
// project's, and change them.
#include "UiTestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Tempo.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/shell/TopBar.h"

namespace
{
// A top bar in a window, over a project with one track.
struct TopBarScreen
{
    TopBarScreen()
    {
        setEdit (context, edit.get());
        phazite::addMetronomeCommands (registry, getSelection (context));
        edit->getUndoManager().clearUndoHistory();

        // The bar shows the project at its next update.
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return getTempoField().isEnabled(); }));
    }

    ~TopBarScreen() { setEdit (context, nullptr); }

    phazite::ui::NumberField& getTempoField()
    {
        auto* field = dynamic_cast<phazite::ui::NumberField*> (phazite::test::findComponentWithTitle (topBar, "Tempo"));
        REQUIRE (field != nullptr);
        return *field;
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge bridge { registry };
    phazite::ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {} };
    phazite::ui::TopBar topBar { bridge, keyboard, context };
    phazite::test::TestWindow window { topBar, 1200, 40 };
};
} // namespace

TEST_CASE ("The top bar's tempo field shows the song's tempo, and a drag on it is one undo step")
{
    TopBarScreen screen;
    auto& field = screen.getTempoField();
    CHECK (field.getValue() == doctest::Approx (120.0));
    CHECK (field.getTextFromValue (field.getValue()) == juce::String ("120 BPM"));

    // Dragged up 20 pixels, a step every 2, over several moves.
    const auto centre = field.getLocalBounds().getCentre();
    phazite::test::Mouse mouse (field);
    mouse.press (centre);
    mouse.dragTo (centre.translated (0, -10));
    mouse.dragTo (centre.translated (0, -20));
    mouse.release();
    CHECK (phazite::getTempo (*screen.edit) == doctest::Approx (130.0));
    CHECK (screen.edit->getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));

    phazite::undo (*screen.edit);
    CHECK (phazite::getTempo (*screen.edit) == doctest::Approx (120.0)); // the whole drag
    CHECK_FALSE (screen.edit->getUndoManager().canUndo());

    // A change made elsewhere shows there.
    phazite::setTempo (*screen.edit, 95.5);
    CHECK (phazite::test::dispatchMessagesUntil ([&field] { return juce::approximatelyEqual (field.getValue(), 95.5); }));
}

TEST_CASE ("The top bar shows the song's time signature")
{
    TopBarScreen screen;
    auto* button = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.topBar, "Time signature"));
    REQUIRE (button != nullptr);
    CHECK (button->getButtonText() == juce::String ("4/4"));

    phazite::setTimeSignature (*screen.edit, { 6, 8 });
    CHECK (phazite::test::dispatchMessagesUntil ([button] { return button->getButtonText() == "6/8"; }));
}

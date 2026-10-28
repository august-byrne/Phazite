// SPDX-License-Identifier: AGPL-3.0-or-later
// The top bar (src/ui/shell/TopBar.h), driven with the mouse: its tempo field and time signature show the open
// project's, and change them.
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
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
// A top bar in a window, over a project with one track. A window taller than the bar has room for what shows under it.
struct TopBarScreen
{
    explicit TopBarScreen (int height = 40)
        : windowHeight (height)
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
    const int windowHeight;
    phazite::test::TestWindow window { topBar, 1200, windowHeight };
};
} // namespace

TEST_CASE ("While the tempo changes, the top bar shows what it stretches, with a switch for samples and one for patterns")
{
    TopBarScreen screen (300);
    phazite::addTempoCommands (screen.registry, getSelection (screen.context));

    // A loop that follows the tempo, a sound that doesn't, and a pattern on a track of its own.
    phazite::test::TemporaryFolder temp;
    auto& track = *te::getAudioTracks (*screen.edit)[0];
    REQUIRE (phazite::importFile (track, phazite::test::writeSound (temp.folder.getChildFile ("Loop 120 BPM.wav"), 4.0), te::BeatPosition())
             != nullptr);
    REQUIRE (phazite::importFile (track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0),
                                  te::BeatPosition::fromBeats (10.0))
             != nullptr);
    auto* other = phazite::insertTrack (*screen.edit, &track, nullptr);
    REQUIRE (other != nullptr);
    REQUIRE (phazite::insertMidiClip (*other, { te::BeatPosition(), te::BeatPosition::fromBeats (4.0) }, nullptr) != nullptr);
    screen.edit->getUndoManager().clearUndoHistory();

    // Nothing shows until the tempo is dragged.
    auto* window = screen.topBar.getTopLevelComponent();
    REQUIRE (window != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*window, "Changing the tempo stretches") == nullptr);

    auto& field = screen.getTempoField();
    phazite::test::Mouse mouse (field);
    const auto centre = field.getLocalBounds().getCentre();
    mouse.press (centre);
    mouse.dragTo (centre.translated (0, -4));
    mouse.release();

    auto* panel = phazite::test::findComponentWithTitle (*window, "Changing the tempo stretches");
    REQUIRE (panel != nullptr);
    CHECK (panel->isShowing());
    CHECK (panel->getY() >= screen.topBar.getLocalArea (&field, field.getLocalBounds()).getBottom()); // under the field

    auto* samples = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "Samples follow the tempo"));
    auto* patterns = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "Patterns follow the tempo"));
    REQUIRE (samples != nullptr);
    REQUIRE (patterns != nullptr);
    CHECK_FALSE (samples->getToggleState());
    CHECK (samples->getDescription() == juce::String ("1 of 2 follow the tempo"));
    CHECK (patterns->getToggleState());
    CHECK (patterns->getDescription() == juce::String ("It follows the tempo"));

    // Samples: now all of them follow it, as one undo step.
    phazite::test::Mouse samplesMouse (*samples);
    samplesMouse.click (samples->getLocalBounds().getCentre());
    CHECK (samples->getToggleState());
    CHECK (samples->getDescription() == juce::String ("All 2 follow the tempo"));
    CHECK (screen.edit->getUndoManager().getUndoDescription() == juce::String ("Samples Follow Tempo"));

    // Patterns: now they keep their speed.
    phazite::test::Mouse patternsMouse (*patterns);
    patternsMouse.click (patterns->getLocalBounds().getCentre());
    CHECK_FALSE (patterns->getToggleState());
    CHECK (patterns->getDescription() == juce::String ("It keeps its speed"));
    CHECK_FALSE (phazite::doPatternsFollowTempo (*screen.edit));
}

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

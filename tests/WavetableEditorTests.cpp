// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth's editor (src/ui/devices/WavetableEditor.h), driven as the user would: its tables drawn and named,
// the table menu, a sound dropped on a table, a drag through a table, its controls in their groups, and how it fits a
// shorter Devices view.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/WavetableEditor.h"

namespace
{
using phazite::devices::WavetableSynth;
using phazite::ui::WavetableEditor;

// The wavetable synth on a project's first track, and its editor in a window.
struct Screen
{
    Screen()
    {
        auto& s = getSong();
        synth = phazite::setInstrument (s, song.firstTrack, phazite::makeOwnDeviceState (s, WavetableSynth::typeID));
        REQUIRE (synth.isValid());
        phazite::getSession (song.context)->update();
        s.getUndoManager().clearUndoHistory();

        editor = std::make_unique<WavetableEditor> (phazite::makeDeviceHandle (s, synth));
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
    std::unique_ptr<WavetableEditor> editor;
    std::unique_ptr<phazite::test::TestWindow> window;
};
} // namespace

TEST_CASE ("The wavetable synth's editor draws each oscillator's table and names it")
{
    Screen screen;
    auto& editor = *screen.editor;
    CHECK (
        dynamic_cast<WavetableEditor*> (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (screen.getSong(), screen.synth)).get())
        != nullptr);
    phazite::test::checkChildrenApart (editor);

    for (int osc = 0; osc < WavetableEditor::numOscillators; ++osc)
    {
        CHECK (editor.getTableButton (osc).getButtonText() == juce::String ("Basic Shapes"));
        CHECK (editor.getTableButton (osc).getTitle() == "Osc " + juce::String (osc + 1) + " Wavetable Choice");
        CHECK (phazite::test::findComponentWithTitle (editor, "Osc " + juce::String (osc + 1) + " Wavetable") != nullptr);
    }

    // Every parameter has its control, titled with its whole name and named within its part: under "Envelope 2", its
    // attack is "Attack".
    for (const auto& parameter : WavetableSynth::info.parameters)
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

TEST_CASE ("The wavetable synth's table menu lists the factory tables and puts the one chosen on the oscillator")
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

TEST_CASE ("The wavetable synth's editor fits the dock: its parts in pages, two to a page, the modulation a list down the side")
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

    // As wide and as tall as the dock at the default window: four pages, the first with both oscillators, whole.
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    REQUIRE (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    editor.setSize (dockWidth, dockHeight);
    REQUIRE (editor.getLayout().getNumPages() == 4);
    CHECK (editor.getLayout().getPageName (3) == "Modulation");
    CHECK (editor.getTableView (0).isShowing());
    CHECK (editor.getTableView (1).isShowing());
    CHECK (editor.getTableView (0).getHeight() > 0);
    phazite::test::checkChildrenApart (editor);

    editor.getLayout().showPage (1);
    CHECK_FALSE (editor.getTableView (0).isShowing());
    CHECK (editor.findControl ("cutoff")->isShowing());
}

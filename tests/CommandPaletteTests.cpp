// SPDX-License-Identifier: AGPL-3.0-or-later
// The command palette (src/ui/shell/CommandPalette.h), as someone uses it: typed into, the arrow keys, Enter, a click on a
// row. Its search's ranking is CommandTests.cpp's.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "ui/input/CommandBridge.h"
#include "ui/shell/CommandPalette.h"

#include <vector>

namespace
{
// Four commands that each say when they ran.
struct Commands
{
    Commands()
    {
        for (const auto& [id, name] : { std::pair ("edit.undo", "Undo"), std::pair ("edit.redo", "Redo"),
                                        std::pair ("clip.split", "Split Clip"), std::pair ("transport.playStop", "Play / Stop") })
        {
            phazite::Command command;
            command.id = id;
            command.name = name;
            command.category = "Test";
            command.perform = [this, commandID = juce::String (id)] { ran.push_back (commandID); };
            REQUIRE (registry.add (std::move (command)));
        }
    }

    phazite::CommandRegistry registry;
    std::vector<juce::String> ran;
};

// A palette over them in a window, the search field with the keyboard's focus.
struct PaletteScreen : Commands
{
    PaletteScreen()
    {
        field = dynamic_cast<juce::TextEditor*> (phazite::test::findComponentWithTitle (palette, "Search commands"));
        list = phazite::test::findAll<juce::ListBox> (palette).getFirst();
        REQUIRE (field != nullptr);
        REQUIRE (list != nullptr);
        field->grabKeyboardFocus();
        phazite::test::dispatchPendingMessages();
    }

    void type (const juce::String& text)
    {
        field->setText (text);
        phazite::test::dispatchPendingMessages(); // the field tells of its change a message later
    }

    void press (const juce::KeyPress& key)
    {
        window.pressKey (key);
        phazite::test::dispatchPendingMessages(); // the command runs once the palette has closed, a message later
    }

    juce::String shownAt (int row) const { return list->getListBoxModel()->getNameForRow (row); }

    phazite::ui::CommandBridge bridge { registry };
    phazite::ui::CommandPalette palette { bridge };
    phazite::test::TestWindow window { palette, palette.getWidth(), palette.getHeight() };
    juce::TextEditor* field = nullptr;
    juce::ListBox* list = nullptr;
};
} // namespace

TEST_CASE ("The command palette lists the commands as it is typed into, and Enter runs the first, once")
{
    PaletteScreen screen;
    CHECK (screen.list->getListBoxModel()->getNumRows() == 4); // nothing typed: every command
    CHECK (screen.list->getSelectedRow() == 0);

    screen.type ("redo");
    REQUIRE (screen.list->getListBoxModel()->getNumRows() >= 1);
    CHECK (screen.shownAt (0).startsWith ("Redo"));
    CHECK (screen.list->getSelectedRow() == 0);
    CHECK (screen.ran.empty());

    screen.press (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (screen.ran == std::vector<juce::String> { "edit.redo" });

    // Nothing that matches: Enter runs nothing.
    screen.type ("xyz");
    CHECK (screen.list->getListBoxModel()->getNumRows() == 0);
    screen.press (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (screen.ran.size() == 1);
}

TEST_CASE ("The command palette's arrow keys move through what it lists, the search field keeping the keys, and stop at its ends")
{
    PaletteScreen screen;
    const auto all = screen.registry.search ({}, 50);
    REQUIRE (all.size() == 4);

    screen.press (juce::KeyPress (juce::KeyPress::downKey));
    screen.press (juce::KeyPress (juce::KeyPress::downKey));
    CHECK (screen.list->getSelectedRow() == 2);
    CHECK (screen.field->hasKeyboardFocus (true));

    for (int i = 0; i < 5; ++i)
        screen.press (juce::KeyPress (juce::KeyPress::downKey));

    CHECK (screen.list->getSelectedRow() == 3); // the last
    screen.press (juce::KeyPress (juce::KeyPress::upKey));
    CHECK (screen.list->getSelectedRow() == 2);

    screen.press (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (screen.ran == std::vector<juce::String> { all[2]->id });

    for (int i = 0; i < 5; ++i)
        screen.press (juce::KeyPress (juce::KeyPress::upKey));

    CHECK (screen.list->getSelectedRow() == 0); // the first
}

TEST_CASE ("A click on a row of the command palette runs that row's command")
{
    PaletteScreen screen;
    screen.type ("split");
    REQUIRE (screen.list->getListBoxModel()->getNumRows() >= 1);
    REQUIRE (screen.shownAt (0).startsWith ("Split Clip"));

    phazite::test::Mouse mouse (*screen.list);
    mouse.click (screen.list->getRowPosition (0, true).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.ran == std::vector<juce::String> { "clip.split" });
}

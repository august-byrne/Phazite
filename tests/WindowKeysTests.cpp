// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's keys (ui/input/WindowKeys.h): which listener gets a key first, the Shift rule, and letting held keys
// go when the window loses the keyboard.
#include "UiTestHelpers.h"
#include "ui/input/WindowKeys.h"

#include <set>

namespace
{
// A window whose keyPressed() is the main window's: JUCE calls it when no key listener took the key.
struct KeysWindow : phazite::test::TestWindow
{
    explicit KeysWindow (juce::Component& content)
        : TestWindow (content, 400, 300)
    {
    }

    bool keyPressed (const juce::KeyPress& key) override { return keys != nullptr && keys->keyPressedAfterListeners (key); }

    phazite::ui::WindowKeys* keys = nullptr;
};

// Three commands with Phazite's keys (S for Slip, B for Paint, Ctrl+Z for Undo), which record when they run and when
// their key is let go, and the computer keyboard, in a window wired as the main window is.
struct Keys
{
    Keys()
    {
        for (const auto* id : { "tool.slip", "tool.paint", "edit.undo" })
            registry.add (
                { .id = id, .name = id, .perform = [this, id] { ran.add (id); }, .keyReleased = [this, id] { released.add (id); } });

        windowKeys = std::make_unique<phazite::ui::WindowKeys> (window, shortcutKeys, keyboard);
        window.keys = windowKeys.get();
    }

    ~Keys()
    {
        window.keys = nullptr;
        windowKeys = nullptr;
    }

    void press (const char* description) { window.pressKey (juce::KeyPress::createFromDescription (description)); }

    juce::StringArray ran, released;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry }; // with Phazite's keymap
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {} };
    juce::Component content;
    KeysWindow window { content };
    std::unique_ptr<phazite::ui::WindowKeys> windowKeys;
};
} // namespace

TEST_CASE ("The main window's keys: the computer keyboard gets its letters first, and Shift and a letter run the letter's shortcut")
{
    Keys keys;

    // A note key: the computer keyboard takes it. (The note itself starts from the real key state, which tests can't set.)
    keys.press ("S");
    CHECK (keys.ran.isEmpty());

    keys.press ("shift + S");
    CHECK (keys.ran == juce::StringArray { "tool.slip" });

    keys.press ("command + Z");
    CHECK (keys.ran == juce::StringArray { "tool.slip", "edit.undo" });

    // Without the computer keyboard, the letter is the shortcut.
    keys.keyboard.setEnabled (false);
    keys.press ("S");
    CHECK (keys.ran == juce::StringArray { "tool.slip", "edit.undo", "tool.slip" });
}

TEST_CASE ("When the main window loses the keyboard, held keys are let go and the tools spring back")
{
    Keys keys;

    keys.press ("B"); // not a note key
    CHECK (keys.ran == juce::StringArray { "tool.paint" });
    keys.windowKeys->releaseKeys();
    CHECK (keys.released == juce::StringArray { "tool.paint" });

    keys.press ("B");
    keys.window.releaseKeys(); // a key going up (none is down in tests)
    CHECK (keys.released == juce::StringArray { "tool.paint", "tool.paint" });

    // Once the window's keys are gone, its shortcuts don't run.
    keys.window.keys = nullptr;
    keys.windowKeys = nullptr;
    keys.press ("command + Z");
    CHECK (keys.ran == juce::StringArray { "tool.paint", "tool.paint" });
}

TEST_CASE ("Letters typed into a text field play no notes, even typed quickly, each key down before the last is let go")
{
    // The owner heard notes now and then while typing (2026-11-12): the field takes keys going down, and lets those
    // coming up pass on to the window, whose computer keyboard then found the next letter down.
    std::set<int> down; // the keys held
    std::vector<juce::MidiMessage> sent;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::ComputerKeyboard keyboard { [&sent] (const juce::MidiMessage& m) { sent.push_back (m); },
                                             [&down] (int key) { return down.count (key) > 0; } };
    juce::Component content;
    juce::TextEditor field;
    content.addAndMakeVisible (field);
    field.setBounds (10, 10, 200, 24);
    KeysWindow window { content };
    phazite::ui::WindowKeys windowKeys { window, shortcutKeys, keyboard };
    window.keys = &windowKeys;

    const auto type = [&] (char key)
    {
        down.insert (key);
        window.keyGoesDown();
        window.pressKey (juce::KeyPress (key, {}, (juce::juce_wchar) key));
    };
    const auto letGo = [&] (char key)
    {
        down.erase (key);
        window.releaseKeys();
    };

    field.grabKeyboardFocus();
    REQUIRE (field.hasKeyboardFocus (false));

    type ('s');
    type ('a');
    letGo ('s');
    type ('d');
    letGo ('a');
    type ('f');
    letGo ('d');
    letGo ('f');
    CHECK (field.getText() == "sadf");
    CHECK (sent.empty());

    // A key going down that the field passes on to the window (Escape, in a field that doesn't take it), while a letter
    // typed into it is held: still no note.
    type ('g');
    keyboard.keyStateChanged (true, &window);
    letGo ('g');
    CHECK (sent.empty());

    // Away from the field, the same keys play, and those let go end their notes.
    field.giveAwayKeyboardFocus();
    REQUIRE (! field.hasKeyboardFocus (false));
    type ('s');
    type ('a');
    letGo ('s');
    letGo ('a');
    REQUIRE (sent.size() == 4);
    CHECK ((sent[0].isNoteOn() && sent[0].getNoteNumber() == 62)); // S: D4
    CHECK ((sent[1].isNoteOn() && sent[1].getNoteNumber() == 60)); // A: C4
    CHECK ((sent[2].isNoteOff() && sent[2].getNoteNumber() == 62));
    CHECK ((sent[3].isNoteOff() && sent[3].getNoteNumber() == 60));
    CHECK (field.getText() == "sadfg");

    window.keys = nullptr;
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's keys (ui/input/WindowKeys.h): which listener gets a key first, the Shift rule, and letting held keys
// go when the window loses the keyboard.
#include "UiTestHelpers.h"
#include "ui/input/WindowKeys.h"

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

        windowKeys = std::make_unique<phazite::ui::WindowKeys> (window, registry, keyboard);
        window.keys = windowKeys.get();
    }

    ~Keys()
    {
        window.keys = nullptr;
        windowKeys = nullptr;
    }

    void press (const char* description) { window.pressKey (juce::KeyPress::createFromDescription (description)); }

    juce::StringArray ran, released;
    phazite::CommandRegistry registry; // with Phazite's keymap
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

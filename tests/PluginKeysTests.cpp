// SPDX-License-Identifier: AGPL-3.0-or-later
// Keys over plug-in windows: play and stop, and the computer keyboard, while a plug-in's editor has the keyboard focus;
// and the other keys too for a plug-in that gets them all.
#include "UiTestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/PluginKeys.h"

namespace
{
// A plug-in's editor as JUCE hosts a VST3 plug-in's: it takes every key that reaches it.
struct GreedyEditor : public juce::Component
{
    GreedyEditor() { setWantsKeyboardFocus (true); }

    bool keyPressed (const juce::KeyPress&) override
    {
        ++keysTaken;
        return true;
    }

    bool keyStateChanged (bool) override { return true; }

    int keysTaken = 0;
};

// A plug-in's window, watched, with its editor focused; Space plays and B paints.
struct PluginScreen
{
    PluginScreen()
    {
        registry.add ({ .id = "transport.playStop", .name = "Play / Stop", .perform = [this] { ++plays; }, .overPlugins = true });
        registry.add ({ .id = "tool.paint", .name = "Paint", .perform = [this] { ++paints; } });

        phazite::ui::Keymap keymap;
        keymap.name = "Test";
        keymap.keys["transport.playStop"] = { "spacebar" };
        keymap.keys["tool.paint"] = { "B" };
        commands.setKeymap (keymap);

        keys.watch (window);
        editor.grabKeyboardFocus();
        REQUIRE (editor.hasKeyboardFocus (false));
    }

    void press (const juce::KeyPress& key)
    {
        window.pressKey (key);
        window.releaseKeys();
    }

    int plays = 0, paints = 0;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::ComputerKeyboard computerKeyboard { [] (const juce::MidiMessage&) {} };
    phazite::ui::PluginKeys keys { shortcutKeys, computerKeyboard };
    GreedyEditor editor;
    phazite::test::TestWindow window { editor, 300, 200 };
};

const juce::KeyPress spaceBar (juce::KeyPress::spaceKey);
} // namespace

TEST_CASE ("Play and stop, and the computer keyboard, work over a plug-in's editor, which gets the other keys")
{
    PluginScreen screen;
    auto& editor = screen.editor;
    auto& keys = screen.keys;

    // Space plays, though the editor takes every key that reaches it.
    screen.press (spaceBar);
    CHECK (screen.plays == 1);
    CHECK (editor.keysTaken == 0);

    // A shortcut that doesn't work over plug-ins goes to the plug-in.
    screen.press (juce::KeyPress ('b', 0, 'b'));
    CHECK (screen.paints == 0);
    CHECK (editor.keysTaken == 1);

    // The computer keyboard's keys are Phazite's while it plays notes; with a modifier, they are the plug-in's.
    CHECK (keys.wants (juce::KeyPress ('a', 0, 'a')));
    CHECK_FALSE (keys.wants (juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0)));
    screen.window.pressKey (juce::KeyPress ('a', 0, 'a'));
    CHECK (editor.keysTaken == 1);

    screen.computerKeyboard.setEnabled (false);
    CHECK_FALSE (keys.wants (juce::KeyPress ('a', 0, 'a')));
    screen.window.pressKey (juce::KeyPress ('a', 0, 'a'));
    CHECK (editor.keysTaken == 2);

    // Once the window isn't watched, the editor gets everything.
    keys.unwatch (screen.window);
    screen.window.pressKey (spaceBar);
    CHECK (screen.plays == 1);
    CHECK (editor.keysTaken == 3);
    screen.window.releaseKeys();
}

TEST_CASE ("A plug-in that gets every key: play, stop and the computer keyboard don't work over its window")
{
    PluginScreen screen;
    auto& editor = screen.editor;
    auto& keys = screen.keys;
    CHECK_FALSE (keys.sendsAllKeys (screen.window));

    // Its "Send All Keys to the Plug-in" switch on: Space and the computer keyboard's letters go to it, so that a name can
    // be typed into a field it draws itself.
    keys.setSendsAllKeys (screen.window, true);
    CHECK (keys.sendsAllKeys (screen.window));
    screen.press (spaceBar);
    CHECK (screen.plays == 0);
    CHECK (editor.keysTaken == 1);
    screen.press (juce::KeyPress ('a', 0, 'a'));
    CHECK (editor.keysTaken == 2);

    // Off again, Space plays.
    keys.setSendsAllKeys (screen.window, false);
    screen.press (spaceBar);
    CHECK (screen.plays == 1);
    CHECK (editor.keysTaken == 2);

    // A window that isn't watched has no switch.
    keys.unwatch (screen.window);
    keys.setSendsAllKeys (screen.window, true);
    CHECK_FALSE (keys.sendsAllKeys (screen.window));
}

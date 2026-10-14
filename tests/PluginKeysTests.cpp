// SPDX-License-Identifier: AGPL-3.0-or-later
// Keys over plug-in windows: play and stop, and the computer keyboard, while a plug-in's editor has the keyboard focus.
#include "UiTestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "ui/input/ComputerKeyboard.h"
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
} // namespace

TEST_CASE ("Play and stop, and the computer keyboard, work over a plug-in's editor, which gets the other keys")
{
    phazite::CommandRegistry registry;
    auto plays = 0, paints = 0;
    registry.add ({ .id = "transport.playStop", .name = "Play / Stop", .perform = [&plays] { ++plays; }, .overPlugins = true });
    registry.add ({ .id = "tool.paint", .name = "Paint", .perform = [&paints] { ++paints; } });

    phazite::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["transport.playStop"] = { "spacebar" };
    keymap.keys["tool.paint"] = { "B" };
    registry.setKeymap (keymap);

    phazite::ui::ComputerKeyboard computerKeyboard ([] (const juce::MidiMessage&) {});
    phazite::ui::PluginKeys keys (registry, computerKeyboard);

    GreedyEditor editor;
    phazite::test::TestWindow window { editor, 300, 200 };
    keys.watch (window);
    editor.grabKeyboardFocus();
    REQUIRE (editor.hasKeyboardFocus (false));

    // Space plays, though the editor takes every key that reaches it.
    window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    window.releaseKeys();
    CHECK (plays == 1);
    CHECK (editor.keysTaken == 0);

    // A shortcut that doesn't work over plug-ins goes to the plug-in.
    window.pressKey (juce::KeyPress ('b', 0, 'b'));
    window.releaseKeys();
    CHECK (paints == 0);
    CHECK (editor.keysTaken == 1);

    // The computer keyboard's keys are Phazite's while it plays notes; with a modifier, they are the plug-in's.
    CHECK (keys.wants (juce::KeyPress ('a', 0, 'a')));
    CHECK_FALSE (keys.wants (juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0)));
    window.pressKey (juce::KeyPress ('a', 0, 'a'));
    CHECK (editor.keysTaken == 1);

    computerKeyboard.setEnabled (false);
    CHECK_FALSE (keys.wants (juce::KeyPress ('a', 0, 'a')));
    window.pressKey (juce::KeyPress ('a', 0, 'a'));
    CHECK (editor.keysTaken == 2);

    // Once the window isn't watched, the editor gets everything.
    keys.unwatch (window);
    window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    CHECK (plays == 1);
    CHECK (editor.keysTaken == 3);
    window.releaseKeys();
}

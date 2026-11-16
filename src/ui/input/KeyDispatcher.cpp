// SPDX-License-Identifier: AGPL-3.0-or-later
#include "KeyDispatcher.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    // The commands a key runs while a mouse button is down: those that change nothing in the song, its undo history or its
    // files, but how it plays and how it shows (playing and stopping, the tools, which take over as the mouse goes up, and the
    // editors' zoom). A drag is one undo step from its press to its release: Ctrl+Z in the middle of a clip's drag took the
    // drag's step back and left the rest of it in a step without a name (known bug 45 until 2026-11-14), and a command that
    // begins a step of its own (Capture, Consolidate) joined the drag's, which Escape then took back with it. Named one by one,
    // so that a command added later waits for the mouse to go up until it is shown to change nothing
    // (tests/EditorDragTests.cpp runs every command by its key in the middle of a drag).
    bool runsWhileMouseIsDown (const Command& command)
    {
        static const juce::StringArray ids { "transport.playStop",
                                             "transport.playPause",
                                             "tool.smart",
                                             "tool.draw",
                                             "tool.paint",
                                             "tool.slice",
                                             "tool.slip",
                                             "tool.mute",
                                             "tool.delete",
                                             "tool.select",
                                             "tool.zoom",
                                             "view.zoomIn",
                                             "view.zoomOut",
                                             "view.zoomToFit",
                                             "view.zoomToSelection",
                                             "view.zoomBack",
                                             "view.centreOnPlayhead",
                                             "view.followPlayback" };
        return ids.contains (command.id);
    }

    bool isMouseButtonDown()
    {
        return juce::Desktop::getInstance().getNumDraggingMouseSources() > 0;
    }
} // namespace

KeyDispatcher::KeyDispatcher (CommandBridge& bridge, const KeyboardLayout& keyboardLayout)
    : commands (bridge),
      layout (keyboardLayout)
{
}

std::vector<KeyDispatcher::Match> KeyDispatcher::findMatches (const juce::KeyPress& pressed) const
{
    // JUCE gives a key's code as the layout types it, without Shift on Windows and with it on Linux, and AltGr as Alt (JUCE
    // workarounds W47 and W48, docs/juce-workarounds.md). Each command's keys are looked at first: whether it is enabled
    // is asked only of the commands found (findCommand()).
    if (isTypedWithAltGr (pressed, layout))
        return {};

    const auto& registry = commands.getRegistry();
    const auto key = getShortcutKey (pressed, layout);
    const auto digit = getDigitRowKey (pressed, layout);
    const auto modifiers = pressed.getModifiers();
    const auto typed = pressed.getTextCharacter();
    const auto canMatchTyped = typed > ' ' && ! modifiers.isCtrlDown() && ! modifiers.isCommandDown() && ! modifiers.isAltDown();
    std::vector<Match> found;

    for (const auto way : { Way::asItIs, Way::asDigit, Way::byCharacter })
    {
        if ((way == Way::asDigit && digit == key) || (way == Way::byCharacter && ! canMatchTyped))
            continue;

        for (int i = 0; i < registry.getNumCommands(); ++i)
        {
            const auto* command = registry.getCommand (i);

            for (const auto& shortcut : commands.getKeyPresses (command->id))
            {
                if (way == Way::asItIs ? shortcut == key : way == Way::asDigit ? shortcut == digit : getTypedCharacter (shortcut) == typed)
                {
                    found.push_back ({ command, shortcut, way });
                    break;
                }
            }
        }

        if (! found.empty())
            break;
    }

    return found;
}

const Command* KeyDispatcher::findCommand (const juce::KeyPress& key, bool enabledOnly) const
{
    // The way that finds shortcuts decides among every command, so that a key whose command can't run now does nothing,
    // rather than run what it means another way.
    const Command* best = nullptr;

    for (const auto& match : findMatches (key))
        if ((best == nullptr || match.command->keyPriority > best->keyPriority)
            && (! enabledOnly || CommandRegistry::isEnabled (*match.command)))
            best = match.command;

    return best;
}

const Command* KeyDispatcher::findCommandForKey (const juce::KeyPress& key) const
{
    return findCommand (key, true);
}

bool KeyDispatcher::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (const auto* command = findCommandForKey (key))
        return runForKey (*command, key);

    return false;
}

bool KeyDispatcher::runsOverPlugins (const juce::KeyPress& key) const
{
    const auto* command = findCommandForKey (key);
    return command != nullptr && command->overPlugins;
}

std::vector<juce::KeyPress> KeyDispatcher::getKeysOverPlugins() const
{
    std::vector<juce::KeyPress> keys;
    const auto& registry = commands.getRegistry();

    for (int i = 0; i < registry.getNumCommands(); ++i)
        if (const auto& command = *registry.getCommand (i); command.overPlugins)
            for (const auto& key : commands.getKeyPresses (command.id))
                keys.push_back (key);

    return keys;
}

bool KeyDispatcher::keyPressedWhileHeld (const juce::KeyPress& key)
{
    if (const auto* command = findCommandForKey (key); command != nullptr && runsWhileMouseIsDown (*command))
        return runForKey (*command, key);

    return false;
}

bool KeyDispatcher::keyPressedOverPlugin (const juce::KeyPress& key)
{
    if (const auto* command = findCommandForKey (key); command != nullptr && command->overPlugins)
        return runForKey (*command, key);

    return false;
}

bool KeyDispatcher::runForKey (const Command& command, const juce::KeyPress& key)
{
    // With a mouse button down, the key is used up: what the mouse does goes on as it began.
    if (isMouseButtonDown() && ! runsWhileMouseIsDown (command))
        return true;

    // A held key repeats. Only commands that take a step run again; for the others the repeat is used up.
    const auto held = std::find_if (heldKeys.begin(), heldKeys.end(),
                                    [&key, &command] (const auto& entry) { return entry.first == key && entry.second == command.id; });

    if (held != heldKeys.end())
        return command.repeatsWhileHeld ? commands.invoke (command.id) : true;

    heldKeys.emplace_back (key, command.id);
    return commands.invoke (command.id);
}

bool KeyDispatcher::keyStateChanged (bool, juce::Component*)
{
    releaseHeldKeys ([] (const juce::KeyPress& key) { return ! juce::KeyPress::isKeyCurrentlyDown (key.getKeyCode()); });
    return false; // the other key listeners hear about it too
}

void KeyDispatcher::releaseHeldKeys()
{
    releaseHeldKeys ([] (const juce::KeyPress&) { return true; });
}

void KeyDispatcher::releaseHeldKeys (const std::function<bool (const juce::KeyPress&)>& isReleased)
{
    // The commands hear about it after the list is up to date, in case they press keys of their own.
    juce::StringArray released;

    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (isReleased (it->first))
        {
            released.add (it->second);
            it = heldKeys.erase (it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& id : released)
        if (const auto* command = commands.getRegistry().find (id); command != nullptr && command->keyReleased != nullptr)
            command->keyReleased();
}

bool KeyDispatcher::invokeWithoutShift (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();

    if (! modifiers.isShiftDown() || modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown())
        return false;

    if (findShortcutOf (key) != nullptr)
        return false; // Shift and the key have a command of their own

    if (const auto* command = findCommandForKey (juce::KeyPress (key.getKeyCode())))
        return runForKey (*command, key);

    return false;
}
} // namespace phazite::ui

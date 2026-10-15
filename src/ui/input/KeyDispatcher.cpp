// SPDX-License-Identifier: AGPL-3.0-or-later
#include "KeyDispatcher.h"

#include <algorithm>

namespace phazite::ui
{
KeyDispatcher::KeyDispatcher (CommandBridge& bridge)
    : commands (bridge)
{
}

const Command* KeyDispatcher::findCommandForKey (const juce::KeyPress& key) const
{
    const auto& registry = commands.getRegistry();
    const Command* found = nullptr;

    for (int i = 0; i < registry.getNumCommands(); ++i)
    {
        const auto& command = *registry.getCommand (i);

        if (found != nullptr && command.keyPriority <= found->keyPriority)
            continue;

        if (commands.getKeyPresses (command.id).contains (key) && CommandRegistry::isEnabled (command))
            found = &command;
    }

    return found;
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

bool KeyDispatcher::keyPressedOverPlugin (const juce::KeyPress& key)
{
    if (const auto* command = findCommandForKey (key); command != nullptr && command->overPlugins)
        return runForKey (*command, key);

    return false;
}

bool KeyDispatcher::runForKey (const Command& command, const juce::KeyPress& key)
{
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

    if (commands.getKeyMappings().findCommandForKeyPress (key) != 0)
        return false; // Shift and the key have a command of their own

    if (const auto* command = findCommandForKey (juce::KeyPress (key.getKeyCode())))
        return runForKey (*command, key);

    return false;
}
} // namespace phazite::ui

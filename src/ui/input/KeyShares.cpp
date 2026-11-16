// SPDX-License-Identifier: AGPL-3.0-or-later
#include "KeyShares.h"
#include "CommandUI.h"

namespace phazite::ui
{
juce::Array<const Command*> findOthersWithKey (const CommandBridge& commands, const juce::String& commandID, const juce::KeyPress& key)
{
    juce::Array<const Command*> others;
    const auto& registry = commands.getRegistry();

    for (int i = 0; i < registry.getNumCommands(); ++i)
        if (const auto* command = registry.getCommand (i); command->id != commandID && commands.getKeyPresses (command->id).contains (key))
            others.add (command);

    return others;
}

juce::StringArray describeKeyShare (const CommandBridge& commands, const KeyDispatcher& keys, const ComputerKeyboard& keyboard,
                                    const juce::String& commandID, const juce::KeyPress& pressed)
{
    juce::StringArray lines;
    const auto& registry = commands.getRegistry();
    const auto* command = registry.find (commandID);

    if (command == nullptr || ! pressed.isValid())
        return lines;

    // The key as the command would keep it, and the commands that would share it then: those that have it as a shortcut
    // keeps it, as KeyDispatcher matches a key first.
    const auto key = getShortcutKey (pressed, keys.getLayout());
    const auto others = findOthersWithKey (commands, commandID, key);

    // A key no other command has as such can run one another way now (as the digit of its place, or by the character it
    // types): given to this command, it runs this one first, and those lose it.
    if (others.isEmpty())
    {
        juce::StringArray losing;
        juce::String how;

        for (const auto& match : keys.findMatches (pressed))
        {
            if (match.command == command)
                continue;

            losing.add (match.command->name);
            how = match.way == KeyDispatcher::Way::asDigit
                      ? "as " + formatKey (match.shortcut.getTextDescription())
                      : "by the " + juce::String::charToString (pressed.getTextCharacter()) + " it types";
        }

        if (! losing.isEmpty())
            lines.add ("Now this key runs " + losing.joinIntoString (", ") + ", " + how + "; given to " + command->name + ", it runs "
                       + command->name + " instead, and " + losing.joinIntoString (", ") + (losing.size() == 1 ? " loses" : " lose")
                       + " this key.");
    }

    if (! others.isEmpty())
    {
        juce::StringArray names;

        for (const auto* other : others)
            names.add (other->name);

        lines.add ((others.size() == 1 ? juce::String ("1 other command has this key: ")
                                       : juce::String (others.size()) + " other commands have this key: ")
                   + names.joinIntoString (", ") + ".");

        // Which runs, as KeyDispatcher::findCommandForKey() picks one: the enabled command of the highest priority, and of
        // those with the same, the one registered first.
        for (const auto* other : others)
        {
            if (other->keyPriority != command->keyPriority)
            {
                const auto* first = other->keyPriority > command->keyPriority ? other : command;
                const auto* second = first == other ? command : other;
                lines.add ("Both keep it: " + first->name + " runs whenever it can, " + second->name + " otherwise.");
            }
            else
            {
                const auto otherFirst = registry.indexOf (other->id) < registry.indexOf (commandID);
                const auto* first = otherFirst ? other : command;
                const auto* second = otherFirst ? command : other;
                lines.add ("Neither comes first: where both can run, " + first->name + " runs, and " + second->name + " only where "
                           + first->name + " can't.");
            }
        }
    }

    // While the computer keyboard plays notes, it takes its letters; Shift and a letter run the letter's command then,
    // unless Shift and the letter have a command of their own (KeyDispatcher::invokeWithoutShift()).
    const auto modifiers = key.getModifiers();
    const auto keyText = formatKey (key.getTextDescription());
    const auto onlyShift = modifiers.isShiftDown() && ! modifiers.isCtrlDown() && ! modifiers.isCommandDown() && ! modifiers.isAltDown();
    const juce::KeyPress plain (key.getKeyCode());
    const juce::KeyPress shifted (key.getKeyCode(), juce::ModifierKeys::shiftModifier, 0);

    if (keyboard.usesKey (key))
    {
        const auto shiftedText = formatKey (shifted.getTextDescription());

        if (const auto* own = keys.findShortcutOf (shifted); own != nullptr && own != command)
            lines.add ("While the computer keyboard plays notes, " + keyText + " plays a note and " + shiftedText + " runs " + own->name
                       + ", so " + command->name + " has no key then.");
        else
            lines.add ("While the computer keyboard plays notes, " + keyText + " plays a note, and " + shiftedText + " runs what " + keyText
                       + " runs otherwise.");
    }
    else if (onlyShift && keyboard.usesKey (plain))
    {
        if (const auto* letters = keys.findShortcutOf (plain); letters != nullptr && letters != command)
            lines.add ("While the computer keyboard plays notes, " + keyText + " runs " + command->name + ", not " + letters->name
                       + " as Shift and a letter do otherwise.");
    }

    return lines;
}
} // namespace phazite::ui

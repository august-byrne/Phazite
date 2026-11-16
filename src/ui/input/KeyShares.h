// SPDX-License-Identifier: AGPL-3.0-or-later
// What else a key does, as the shortcuts editor says it before a command is given the key: the other commands it is a
// shortcut of, which of them runs when (KeyDispatcher: the enabled one with the highest keyPriority, the one registered
// first where they have the same), and what the computer keyboard does with it while it plays notes (Shift and a letter
// run the letter's shortcut then).
#pragma once

#include "ComputerKeyboard.h"
#include "KeyDispatcher.h"

namespace phazite::ui
{
// The commands other than one that have a key among their shortcuts, in the registry's order.
juce::Array<const Command*> findOthersWithKey (const CommandBridge&, const juce::String& commandID, const juce::KeyPress&);

// What a command given a key pressed shares it with, a sentence a line: empty when nothing else uses the key. For example
// "1 other command has this key: Select Track Above." and "Both keep it: Transpose Up runs whenever it can, Select Track
// Above otherwise."; or, for a key that runs another command now by the character it types and would run this one first,
// which commands lose it. The key is taken as the command would keep it (getShortcutKey()).
juce::StringArray describeKeyShare (const CommandBridge&, const KeyDispatcher&, const ComputerKeyboard&, const juce::String& commandID,
                                    const juce::KeyPress& pressed);
} // namespace phazite::ui

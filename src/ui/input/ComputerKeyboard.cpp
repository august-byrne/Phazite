// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ComputerKeyboard.h"

#include <algorithm>
#include <string_view>

namespace phazite::ui
{
ComputerKeyboard::ComputerKeyboard (std::function<void (const juce::MidiMessage&)> send, const KeyboardLayout& keyboardLayout)
    : sendMessage (std::move (send)),
      layout (keyboardLayout)
{
    static_assert (std::string_view (noteKeys).size() == numNoteKeys);
    heldNotes.fill (-1);
}

ComputerKeyboard::~ComputerKeyboard()
{
    releaseAll();
}

void ComputerKeyboard::leaveKeysOf (const CommandBridge* bridge)
{
    commands = bridge;
}

bool ComputerKeyboard::isLeft (const juce::KeyPress& key) const
{
    return commands != nullptr && commands->getKeyPresses ("input.computerKeyboard").contains (key);
}

bool ComputerKeyboard::isLeft (char place) const
{
    const auto typed = layout.getCharacter (place, false);
    return typed != 0 && isLeft (juce::KeyPress ((int) typed));
}

juce::String ComputerKeyboard::describe (const char* places) const
{
    return describePlaces (places, layout, [this] (char place) { return isLeft (place); });
}

void ComputerKeyboard::setEnabled (bool shouldBeEnabled)
{
    if (enabled == shouldBeEnabled)
        return;

    releaseAll();
    enabled = shouldBeEnabled;
    changed();
}

void ComputerKeyboard::releaseAll()
{
    for (auto& note : heldNotes)
    {
        if (note >= 0)
        {
            sendMessage (juce::MidiMessage::noteOff (midiChannel, note));
            note = -1;
        }
    }

    // Keys still down when the window comes back count as newly pressed.
    noteKeysDown.fill (false);
    controlKeysDown.fill (false);
}

bool ComputerKeyboard::update (const std::function<bool (char)>& keyIsDown, juce::ModifierKeys modifiers, bool keyWentDown)
{
    if (! enabled)
        return false;

    // A key that goes down with Shift, Ctrl, Alt or Cmd held is part of a shortcut: it plays nothing,
    // not even once the modifier is let go before the key. Held notes still end. So does a key that went
    // down into a text field.
    const bool plays = keyWentDown && ! isShortcut (modifiers);
    bool used = false;

    for (size_t i = 0; i < heldNotes.size(); ++i)
    {
        const bool down = keyIsDown (noteKeys[i]) && ! isLeft (noteKeys[i]);
        const bool pressed = down && ! noteKeysDown[i];
        noteKeysDown[i] = down;
        auto& held = heldNotes[i];

        if (pressed && plays && held < 0 && lowestNote + (int) i <= 127)
        {
            held = lowestNote + (int) i;
            sendMessage (juce::MidiMessage::noteOn (midiChannel, held, (juce::uint8) velocity));
            used = true;

            if (onNotePlayed != nullptr)
                onNotePlayed();
        }
        else if (! down && held >= 0)
        {
            // The note the key started, even if the octave has changed since.
            sendMessage (juce::MidiMessage::noteOff (midiChannel, held));
            held = -1;
            used = true;
        }
    }

    // Z, X, C and V act once per press, not again while held down.
    for (size_t i = 0; i < controlKeys.size(); ++i)
    {
        const bool down = keyIsDown (controlKeys[i]) && ! isLeft (controlKeys[i]);
        const bool pressed = down && ! controlKeysDown[i];
        controlKeysDown[i] = down;

        if (pressed && plays)
        {
            switch (controlKeys[i])
            {
                case 'z':
                    lowestNote = std::max (0, lowestNote - 12);
                    break;
                case 'x':
                    lowestNote = std::min (120, lowestNote + 12);
                    break;
                case 'c':
                    velocity = std::max (1, velocity - 20);
                    break;
                case 'v':
                    velocity = std::min (127, velocity + 20);
                    break;
                default:
                    break;
            }

            changed();
            used = true;
        }
    }

    return used;
}

bool ComputerKeyboard::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    // Takes the layout's keys, so that they don't also run a shortcut.
    return takesKey (key);
}

bool ComputerKeyboard::takesKey (const juce::KeyPress& key) const
{
    return enabled && usesKey (key);
}

bool ComputerKeyboard::usesKey (const juce::KeyPress& key) const
{
    if (isShortcut (key.getModifiers()) || isLeft (key))
        return false;

    // What the layout types at each place is the key code JUCE gives its key (JUCE workarounds W47 and W48).
    const auto character = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
    // Or, on Linux, the place's keysym, which JUCE gives where it can't make a character of it (W48).
    const auto isCharacter = [this, character, code = key.getKeyCode()] (char place)
    {
        const auto other = layout.getOtherKeyCode (place);
        return layout.getCharacter (place, false) == character || (other != 0 && other == code);
    };

    return std::any_of (noteKeys, noteKeys + numNoteKeys, isCharacter) || std::any_of (controlKeys.begin(), controlKeys.end(), isCharacter);
}

std::vector<juce::KeyPress> ComputerKeyboard::getKeys() const
{
    std::vector<juce::KeyPress> keys;

    if (! enabled)
        return keys;

    const auto add = [this, &keys] (char place)
    {
        if (const auto typed = layout.getCharacter (place, false); typed != 0 && ! isLeft (place))
            keys.emplace_back ((int) typed);
    };

    std::for_each (noteKeys, noteKeys + numNoteKeys, add);
    std::for_each (controlKeys.begin(), controlKeys.end(), add);
    return keys;
}

bool ComputerKeyboard::keyStateChanged (bool keyWentDown, juce::Component*)
{
    // A text field takes the keys that go down into it, but lets those that come up pass on to the window, here
    // (juce::TextEditor::keyStateChanged()): a letter typed before the last one was let go, as quick typing does, is
    // down as that one comes up, and played its note. So keys start notes only as they go down, and never while a text
    // field has the keys.
    return update ([this] (char place) { return layout.isDown (place); }, juce::ModifierKeys::getCurrentModifiersRealtime(),
                   keyWentDown && ! isTyping());
}

bool ComputerKeyboard::isShortcut (juce::ModifierKeys modifiers)
{
    return modifiers.isShiftDown() || modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown();
}

bool ComputerKeyboard::isTyping()
{
    return dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

void ComputerKeyboard::changed()
{
    if (onChange != nullptr)
        onChange();
}
} // namespace phazite::ui

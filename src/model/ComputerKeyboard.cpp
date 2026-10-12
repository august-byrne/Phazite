// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ComputerKeyboard.h"

#include <algorithm>
#include <string_view>

namespace phazite
{
ComputerKeyboard::ComputerKeyboard (std::function<void (const juce::MidiMessage&)> send)
    : sendMessage (std::move (send))
{
    static_assert (std::string_view (noteKeys).size() == numNoteKeys);
    heldNotes.fill (-1);
}

ComputerKeyboard::~ComputerKeyboard()
{
    releaseAll();
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

    controlKeysDown.fill (false);
}

bool ComputerKeyboard::update (const std::function<bool (int)>& isKeyDown, juce::ModifierKeys modifiers)
{
    if (! enabled)
        return false;

    // With Ctrl, Alt or Cmd held the keys are shortcuts: held notes can still end, but none start.
    const bool canStart = ! isShortcut (modifiers);
    bool used = false;

    for (size_t i = 0; i < heldNotes.size(); ++i)
    {
        const bool down = isKeyDown (noteKeys[i]);
        auto& held = heldNotes[i];

        if (down && held < 0 && canStart && lowestNote + (int) i <= 127)
        {
            held = lowestNote + (int) i;
            sendMessage (juce::MidiMessage::noteOn (midiChannel, held, (juce::uint8) velocity));
            used = true;
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
        const bool down = canStart && isKeyDown (controlKeys[i]);

        if (down && ! controlKeysDown[i])
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

        controlKeysDown[i] = down;
    }

    return used;
}

bool ComputerKeyboard::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    // Takes the layout's keys, so that they don't also run a shortcut.
    if (! enabled || isShortcut (key.getModifiers()))
        return false;

    const auto character = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
    const auto isCharacter = [character] (char c) { return (juce::juce_wchar) c == character; };

    return std::any_of (noteKeys, noteKeys + numNoteKeys, isCharacter) || std::any_of (controlKeys.begin(), controlKeys.end(), isCharacter);
}

bool ComputerKeyboard::keyStateChanged (bool, juce::Component*)
{
    return update ([] (int keyCode) { return juce::KeyPress::isKeyCurrentlyDown (keyCode); },
                   juce::ModifierKeys::getCurrentModifiersRealtime());
}

bool ComputerKeyboard::isShortcut (juce::ModifierKeys modifiers)
{
    return modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown();
}

void ComputerKeyboard::changed()
{
    if (onChange != nullptr)
        onChange();
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ComputerKeyboard.h"

#include <algorithm>
#include <string_view>

// Last, so that its macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace phazite::ui
{
namespace
{
    // Whether a key was down when the key message being handled was sent. JUCE's KeyPress::isKeyCurrentlyDown() reads the
    // key as it is now (GetAsyncKeyState() on Windows): a quick tap is over by then if the UI thread was busy until after
    // it, and played no note (docs/performance-audit.md, U6). GetKeyState() keeps step with the messages instead, so the
    // tap plays its note late rather than never. Elsewhere JUCE keeps the keys' state from the messages itself.
    bool wasKeyDown (int keyCode)
    {
#if JUCE_WINDOWS
        const auto scanned = VkKeyScanW ((WCHAR) keyCode) & 0xff;
        return (GetKeyState (scanned != 0xff ? scanned : keyCode) & 0x8000) != 0;
#else
        return juce::KeyPress::isKeyCurrentlyDown (keyCode);
#endif
    }
} // namespace

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

    // Keys still down when the window comes back count as newly pressed.
    noteKeysDown.fill (false);
    controlKeysDown.fill (false);
}

bool ComputerKeyboard::update (const std::function<bool (int)>& isKeyDown, juce::ModifierKeys modifiers)
{
    if (! enabled)
        return false;

    // A key that goes down with Shift, Ctrl, Alt or Cmd held is part of a shortcut: it plays nothing,
    // not even once the modifier is let go before the key. Held notes still end.
    const bool shortcut = isShortcut (modifiers);
    bool used = false;

    for (size_t i = 0; i < heldNotes.size(); ++i)
    {
        const bool down = isKeyDown (noteKeys[i]);
        const bool pressed = down && ! noteKeysDown[i];
        noteKeysDown[i] = down;
        auto& held = heldNotes[i];

        if (pressed && ! shortcut && held < 0 && lowestNote + (int) i <= 127)
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
        const bool down = isKeyDown (controlKeys[i]);
        const bool pressed = down && ! controlKeysDown[i];
        controlKeysDown[i] = down;

        if (pressed && ! shortcut)
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
    if (! enabled || isShortcut (key.getModifiers()))
        return false;

    const auto character = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
    const auto isCharacter = [character] (char c) { return (juce::juce_wchar) c == character; };

    return std::any_of (noteKeys, noteKeys + numNoteKeys, isCharacter) || std::any_of (controlKeys.begin(), controlKeys.end(), isCharacter);
}

bool ComputerKeyboard::keyStateChanged (bool, juce::Component*)
{
    return update (wasKeyDown, juce::ModifierKeys::getCurrentModifiersRealtime());
}

bool ComputerKeyboard::isShortcut (juce::ModifierKeys modifiers)
{
    return modifiers.isShiftDown() || modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown();
}

void ComputerKeyboard::changed()
{
    if (onChange != nullptr)
        onChange();
}
} // namespace phazite::ui

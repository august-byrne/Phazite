// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "ui/input/ComputerKeyboard.h"

#include <set>

namespace
{
// A ComputerKeyboard with a pretend keyboard: the keys in `down` are held.
struct Keyboard
{
    std::vector<juce::MidiMessage> sent;
    std::set<int> down;
    juce::ModifierKeys modifiers;

    phazite::ui::ComputerKeyboard keyboard { [this] (const juce::MidiMessage& m) { sent.push_back (m); } };

    bool press (char key)
    {
        down.insert (key);
        return update();
    }

    bool release (char key)
    {
        down.erase (key);
        return update();
    }

    bool update()
    {
        return keyboard.update ([this] (int keyCode) { return down.count (keyCode) > 0; }, modifiers);
    }
};

bool isNoteOn (const juce::MidiMessage& m, int note, int velocity)
{
    return m.isNoteOn() && m.getNoteNumber() == note && m.getVelocity() == velocity;
}

bool isNoteOff (const juce::MidiMessage& m, int note)
{
    return m.isNoteOff() && m.getNoteNumber() == note;
}
} // namespace

TEST_CASE ("The middle row plays the white keys from middle C, the row above the black keys")
{
    Keyboard k;

    CHECK (k.press ('a'));
    CHECK (k.press ('w'));
    CHECK (k.press (';'));
    REQUIRE (k.sent.size() == 3);
    CHECK (isNoteOn (k.sent[0], 60, 100)); // A: C4
    CHECK (isNoteOn (k.sent[1], 61, 100)); // W: C#4
    CHECK (isNoteOn (k.sent[2], 76, 100)); // ;: E5

    // Holding a key doesn't repeat the note; releasing it ends the note.
    CHECK_FALSE (k.update());
    CHECK (k.release ('a'));
    REQUIRE (k.sent.size() == 4);
    CHECK (isNoteOff (k.sent[3], 60));
}

TEST_CASE ("Z and X change the octave, C and V the velocity, once per press")
{
    Keyboard k;

    k.press ('z');
    k.update(); // still held: no second octave down
    CHECK (k.keyboard.getLowestNote() == 48);
    k.release ('z');

    for (int i = 0; i < 3; ++i)
    {
        k.press ('x');
        k.release ('x');
    }

    CHECK (k.keyboard.getLowestNote() == 84);

    k.press ('v');
    k.release ('v');
    k.press ('v');
    CHECK (k.keyboard.getVelocity() == 127);
    k.release ('v');

    for (int i = 0; i < 10; ++i)
    {
        k.press ('c');
        k.release ('c');
    }

    CHECK (k.keyboard.getVelocity() == 1);
    CHECK (k.sent.empty()); // none of these play notes
}

TEST_CASE ("A held note ends on the note it started, even after an octave change")
{
    Keyboard k;

    k.press ('a');
    k.press ('x');
    k.release ('a');

    REQUIRE (k.sent.size() == 2);
    CHECK (isNoteOn (k.sent[0], 60, 100));
    CHECK (isNoteOff (k.sent[1], 60));
}

TEST_CASE ("Keys pressed with Ctrl, Alt or Cmd are left to shortcuts")
{
    Keyboard k;
    k.modifiers = juce::ModifierKeys (juce::ModifierKeys::ctrlModifier);

    CHECK_FALSE (k.press ('s')); // Ctrl+S saves, it doesn't play D
    CHECK (k.sent.empty());

    CHECK_FALSE (k.keyboard.keyPressed (juce::KeyPress ('s', juce::ModifierKeys::ctrlModifier, 0), nullptr));
    CHECK (k.keyboard.keyPressed (juce::KeyPress ('s', 0, 0), nullptr));
    CHECK (k.keyboard.keyPressed (juce::KeyPress ('z', 0, 0), nullptr));
    CHECK_FALSE (k.keyboard.keyPressed (juce::KeyPress ('m', 0, 0), nullptr));
    CHECK_FALSE (k.keyboard.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), nullptr)); // play/stop
}

TEST_CASE ("With Shift held, letters are shortcuts, not notes, even once Shift is let go")
{
    Keyboard k;
    k.modifiers = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);

    CHECK_FALSE (k.press ('s')); // Shift+S is S's shortcut
    CHECK_FALSE (k.press ('z')); // and Shift+Z isn't an octave down
    CHECK_FALSE (k.keyboard.keyPressed (juce::KeyPress ('s', juce::ModifierKeys::shiftModifier, 0), nullptr));

    // Letting go of Shift before the keys doesn't turn them into a note or an octave change.
    k.modifiers = {};
    CHECK_FALSE (k.update());
    k.release ('s');
    k.release ('z');
    CHECK (k.sent.empty());
    CHECK (k.keyboard.getLowestNote() == 60);

    // Pressed again on their own, they play and move as usual.
    CHECK (k.press ('s'));
    REQUIRE (k.sent.size() == 1);
    CHECK (isNoteOn (k.sent[0], 62, 100));
}

TEST_CASE ("Turning the computer keyboard off ends its notes and frees its keys")
{
    Keyboard k;
    k.press ('a');

    k.keyboard.setEnabled (false);
    REQUIRE (k.sent.size() == 2);
    CHECK (isNoteOff (k.sent[1], 60));

    CHECK_FALSE (k.update());
    CHECK_FALSE (k.keyboard.keyPressed (juce::KeyPress ('a', 0, 0), nullptr));
    CHECK (k.sent.size() == 2);
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "TestKeyboardLayouts.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/Keymap.h"

#include <map>

#include <set>

namespace
{
// A ComputerKeyboard with a pretend keyboard of the US layout: the keys in `down` are held, by their places.
struct Keyboard
{
    std::vector<juce::MidiMessage> sent;
    std::set<char> down;
    juce::ModifierKeys modifiers;

    phazite::ui::ComputerKeyboard keyboard { [this] (const juce::MidiMessage& m) { sent.push_back (m); }, phazite::ui::getUsLayout() };

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
        return keyboard.update ([this] (char place) { return down.count (place) > 0; }, modifiers);
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

TEST_CASE ("The computer keyboard plays by the keys' places, on AZERTY and QWERTZ as on the US layout")
{
    // Known bug 112 until 2026-11-15: the notes were the US layout's characters, so AZERTY broke the piano's shape and QWERTZ
    // swapped the octave key for a note. Now a key plays by where it is, as in Ableton Live and Bitwig Studio.
    for (auto layout : { phazite::test::SimulatedLayout::azerty(), phazite::test::SimulatedLayout::qwertz() })
    {
        const auto isAzerty = layout.getCharacter ('a', false) == 'q';
        CAPTURE (isAzerty);
        std::vector<juce::MidiMessage> sent;
        phazite::ui::ComputerKeyboard keyboard { [&sent] (const juce::MidiMessage& m) { sent.push_back (m); }, layout };

        const juce::String places = "awsedftgyhujkolp;";

        for (int i = 0; i < places.length(); ++i)
        {
            const auto place = (char) places[i];
            CAPTURE (place);

            // The key there, as JUCE gives it (what the layout types there), is the keyboard's; the note is the place's.
            CHECK (keyboard.takesKey (juce::KeyPress ((int) layout.getCharacter (place, false))));

            layout.down = { place };
            keyboard.keyStateChanged (true, nullptr);
            layout.down.clear();
            keyboard.keyStateChanged (false, nullptr);

            REQUIRE (sent.size() == 2);
            CHECK (isNoteOn (sent[0], 60 + i, 100));
            CHECK (isNoteOff (sent[1], 60 + i));
            sent.clear();
        }

        // Z's place goes an octave down, whatever it types (Y on QWERTZ, W on AZERTY).
        layout.down = { 'z' };
        keyboard.keyStateChanged (true, nullptr);
        layout.down.clear();
        keyboard.keyStateChanged (false, nullptr);
        CHECK (keyboard.getLowestNote() == 48);
        CHECK (sent.empty());

        // The keys the help names, and those left to the shortcuts.
        if (isAzerty)
        {
            CHECK (keyboard.getWhiteKeysText() == "Q S D F G H J K L M");
            CHECK (keyboard.getBlackKeysText() == "Z E T Y U O P");
            CHECK (keyboard.getOctaveKeysText() == "W and X");
            CHECK (keyboard.takesKey (juce::KeyPress ('m')));
            CHECK_FALSE (keyboard.takesKey (juce::KeyPress ('a'))); // A is where the US layout has Q: a shortcut's
        }
        else
        {
            CHECK (keyboard.getWhiteKeysText() == juce::String ("A S D F G H J K L ") + juce::String::charToString (0xd6));
            CHECK (keyboard.getBlackKeysText() == "W E T Z U O P");
            CHECK (keyboard.getOctaveKeysText() == "Y and X");
            CHECK (keyboard.takesKey (juce::KeyPress ('y')));
            CHECK (keyboard.takesKey (juce::KeyPress ('z')));
            CHECK (keyboard.takesKey (juce::KeyPress (0xf6)));
        }
    }
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

TEST_CASE ("The key that turns the computer keyboard off stays that command's, even where a note's place types it")
{
    // On AZERTY the place right of L types M, which turns the computer keyboard off in Phazite's keymap: M plays no note
    // there, and the help names the white keys without it.
    const auto azerty = phazite::test::SimulatedLayout::azerty();
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry }; // Phazite's keymap: M
    registry.add ({ .id = "input.computerKeyboard", .name = "Computer Keyboard", .perform = [] {} });

    std::vector<juce::MidiMessage> sent;
    auto layout = azerty;
    phazite::ui::ComputerKeyboard keyboard { [&sent] (const juce::MidiMessage& m) { sent.push_back (m); }, layout };
    keyboard.leaveKeysOf (&commands);

    CHECK_FALSE (keyboard.takesKey (juce::KeyPress ('m')));
    CHECK (keyboard.takesKey (juce::KeyPress ('q')));
    CHECK (keyboard.getWhiteKeysText() == "Q S D F G H J K L");

    for (const auto& key : keyboard.getKeys())
        CHECK (key != juce::KeyPress ('m'));

    layout.down = { ';' };
    keyboard.keyStateChanged (true, nullptr);
    layout.down.clear();
    keyboard.keyStateChanged (false, nullptr);
    CHECK (sent.empty());

    // Given another key, the place plays its note again.
    commands.removeKey ("input.computerKeyboard", juce::KeyPress ('M'));
    commands.addKey ("input.computerKeyboard", juce::KeyPress::createFromDescription ("command + M"));
    CHECK (keyboard.takesKey (juce::KeyPress ('m')));
    CHECK (keyboard.getWhiteKeysText() == "Q S D F G H J K L M");
}

namespace
{
// The X server's keys as a table (KeyboardLayout.h: KeysymTable): each key code's keysyms by group, without Shift and with
// it, and the group in use.
struct TestKeysymTable final : phazite::ui::KeysymTable
{
    std::map<int, std::vector<std::pair<unsigned long, unsigned long>>> keys; // per group
    std::map<unsigned long, juce::juce_wchar> legacy;                         // what libxkbcommon converts them to
    int group = 0;
    mutable int translations = 0;

    unsigned long getKeysym (int keyCode, int keyGroup, int level) const override
    {
        const auto found = keys.find (keyCode);

        if (found == keys.end() || keyGroup >= (int) found->second.size())
            return 0;

        const auto& [plain, shifted] = found->second[(size_t) keyGroup];
        return level == 0 ? plain : shifted;
    }

    int getGroup() const override { return group; }

    juce::juce_wchar translate (unsigned long keysym) const override
    {
        ++translations;
        const auto found = legacy.find (keysym);
        return found != legacy.end() ? found->second : 0;
    }

    // The key XKeysymToKeycode() finds for a keysym: the first with it in the first group without Shift.
    int findKeyCode (unsigned long keysym) const
    {
        for (const auto& [keyCode, groups] : keys)
            if (! groups.empty() && groups[0].first == keysym)
                return keyCode;

        return 0;
    }
};
} // namespace

TEST_CASE ("On Linux a place's key is asked of JUCE by its own keysym, whichever layout group is in use")
{
    // With us,de and German in use, asked by what German types there, JUCE's XKeysymToKeycode() found the US layout's key:
    // Y's place read Z's. A legacy keysym (Czech's ů) is written by the X server's converter.
    TestKeysymTable table;
    const auto y = phazite::ui::getX11KeyCode ('y'), z = phazite::ui::getX11KeyCode ('z'), l = phazite::ui::getX11KeyCode (';');
    table.keys[y] = { { 'y', 'Y' }, { 'z', 'Z' } };
    table.keys[z] = { { 'z', 'Z' }, { 'y', 'Y' } };
    table.keys[l] = { { ';', ':' }, { 0x1f9, 0x1d9 } }; // the second group's is Czech's ů, an older keysym
    table.legacy[0x1f9] = 0x16f;
    table.group = 1;

    std::set<int> held;
    const phazite::ui::KeysymLayout layout { &table, [&table, &held] (int keysym)
                                             { return held.count (table.findKeyCode ((unsigned long) keysym)) > 0; } };

    CHECK (layout.getCharacter ('y', false) == 'z');
    CHECK (layout.getCharacter ('z', false) == 'y');
    CHECK (layout.getCharacter (';', false) == 0x16f);

    held = { y };
    CHECK (layout.isDown ('y'));
    CHECK_FALSE (layout.isDown ('z'));
    held = { l };
    CHECK (layout.isDown (';'));

    // Keysyms that are their characters, and the others.
    CHECK (phazite::ui::keysymToCharacter ('a') == 'a');
    CHECK (phazite::ui::keysymToCharacter (0xe9) == 0xe9);
    CHECK (phazite::ui::keysymToCharacter (0x100016f) == 0x16f);
    CHECK (phazite::ui::keysymToCharacter (0x1f9) == 0);

    // Without a display, the US layout's.
    const phazite::ui::KeysymLayout none { nullptr, [] (int) { return false; } };
    CHECK (none.getCharacter ('y', false) == 'y');
}

TEST_CASE ("On Linux the places are read once for the group in use, and a key JUCE gives by its keysym is the place's")
{
    // Converting a keysym at each call, the four-times-a-second fingerprint included, did work the layout needn't.
    TestKeysymTable table;
    const auto l = phazite::ui::getX11KeyCode (';'), a = phazite::ui::getX11KeyCode ('a');
    table.keys[l] = { { 0x1f9, 0x1d9 } }; // Czech's ů, an older keysym
    table.keys[a] = { { 'a', 'A' }, { 'q', 'Q' } };
    table.legacy[0x1f9] = 0x16f;
    const phazite::ui::KeysymLayout layout { &table, [] (int) { return false; } };

    CHECK (layout.getCharacter (';', false) == 0x16f);
    const auto converted = table.translations;

    for (int i = 0; i < 10; ++i)
    {
        CHECK (layout.getCharacter (';', false) == 0x16f);
        layout.getFingerprint();
    }

    CHECK (table.translations == converted);

    // Another group: read again.
    table.group = 1;
    CHECK (layout.getCharacter ('a', false) == 'q');

    // Where JUCE can't make a character of the keysym, it gives the keysym as the key code: the place's still.
    table.group = 0;
    CHECK (layout.getOtherKeyCode (';') == 0x1f9);
    CHECK (layout.getOtherKeyCode ('a') == 0);
    const phazite::ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {}, layout };
    CHECK (keyboard.takesKey (juce::KeyPress (0x1f9)));

    // The fingerprint is unsigned: it wraps over the places rather than overflow (UndefinedBehaviorSanitizer, linux-asan).
    CHECK (phazite::ui::getUsLayout().getFingerprint() != phazite::test::SimulatedLayout::azerty().getFingerprint());
}
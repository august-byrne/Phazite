// SPDX-License-Identifier: AGPL-3.0-or-later
// The computer keyboard's keys by their places, and what the layout in use types at each. The computer keyboard's notes
// play by places, as Ableton Live and Bitwig Studio play them, so that the piano keeps its shape on AZERTY, QWERTZ or any
// other layout; and the shortcuts' digits are the digit row's keys, as most apps take them (AZERTY types & where the US
// layout types 1, and 1 only with Shift). A place is named by the character the US layout types there without Shift:
// 'a' is the key right of Caps Lock, ';' the one right of L, '1' the digit row's first; '<' is the key ISO keyboards
// have right of left Shift, which the US layout's keyboards don't.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>

namespace phazite::ui
{
class KeyboardLayout
{
public:
    virtual ~KeyboardLayout() = default;

    // What the layout types at a place, lower case, without Shift or with it; 0 where it types nothing there, or for a
    // place it doesn't know (on Windows, with Shift: JUCE's key codes there are without it). Without Shift it is the key
    // code JUCE gives a key on Windows; with Shift, the one it gives on Linux (JUCE workarounds W47 and W48,
    // docs/juce-workarounds.md).
    virtual juce::juce_wchar getCharacter (char place, bool shifted) const = 0;

    // Whether the key at a place was down as the key message being handled was sent.
    virtual bool isDown (char place) const = 0;

    // Another key code JUCE can give the key at a place, besides its character: on Linux, the key's keysym, where JUCE
    // can't make a character of it (an older keysym, in a locale without that character); 0 for none.
    virtual int getOtherKeyCode (char) const { return 0; }

    // A number that changes when what the layout types at the computer keyboard's places changes (a switch of layouts),
    // for the help that names those keys.
    juce::uint32 getFingerprint() const;

    static constexpr const char* digitRow = "1234567890";

    // Every place a layout types a character at, by the US layout's character there.
    static constexpr const char* places = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./<";
};

// A place's scan code (PC set 1, as Windows gives it; 0 for a place it doesn't know), and the place of a scan code (0 for
// none): what a key is told by between processes whose layouts may differ (a plug-in's helper's: PluginKeys.h).
unsigned int getPlaceScanCode (char place);
char findPlaceOfScanCode (unsigned int scanCode);

// The layout the operating system uses now, following a switch of layouts as it is made; the US layout where it can't
// tell (Linux without a display; macOS, until Phazite builds there). Only the app's own parts read it: tests give the US
// layout, or a layout of their own (tests/TestKeyboardLayouts.h).
const KeyboardLayout& getSystemLayout();

// The US layout, with no key down: what a place types where nothing else can tell.
const KeyboardLayout& getUsLayout();

// A key as a shortcut is kept and first matched (KeyDispatcher, the shortcuts editor): the key JUCE gives, which is the
// character printed on it (what the layout types there without Shift), with its modifiers, but
// - a character typed with Shift is the key's own character without it, Shift held, as Windows gives it (Linux gives
//   what was typed: "?" for Shift and the US layout's /);
// - without the text it typed, which JUCE compares too: with Caps Lock on, A typed "A", another key than A, and a key
//   that typed "/" was written as "/" alone, without its modifiers.
// - Shift with a key of the digit row, where the layout types the digits with Shift (AZERTY, Czech), is the digit typed,
//   Shift held: Shift and AZERTY's & key is "shift + 1", Ctrl, Shift and it "ctrl + shift + 1", on Windows and Linux
//   alike (Windows gives the key's own character with Ctrl, Linux the digit).
juce::KeyPress getShortcutKey (const juce::KeyPress& pressed, const KeyboardLayout&);

// A key of the digit row as its digit, with its modifiers, whatever the layout prints there: AZERTY's & key is 1, Ctrl
// with it Ctrl+1; where the layout types the digits with Shift, as AZERTY does, a digit typed with Shift is the digit
// alone. KeyDispatcher matches a key so when no shortcut matches it as it is. Any other key as getShortcutKey() makes it.
juce::KeyPress getDigitRowKey (const juce::KeyPress& pressed, const KeyboardLayout&);

// The character a shortcut's key types, as the US layout types it, for a key that isn't a letter or a digit and has no
// Ctrl, Alt or Cmd: '=' for "=", '+' for "+", '?' for "shift + /". 0 for any other. A key typed on another layout matches
// the shortcut by it (KeyDispatcher), whatever Shift it took there: "=" is Shift+0 on QWERTZ, "?" Shift+ß.
juce::juce_wchar getTypedCharacter (const juce::KeyPress& shortcut);

// Whether a key pressed with Alt typed a character of its own, as AltGr types one: JUCE reports AltGr as Alt, with the
// key's own character as its code and what AltGr typed as its text (~ for AltGr and QWERTZ's + key), with Shift too.
// Typing, not a shortcut. Its text is compared with the key's characters with and without Shift in the layout, as key
// caps write them (Alt and QWERTZ's ö key, whose code Windows can give as Ö, is Alt+ö).
bool isTypedWithAltGr (const juce::KeyPress&, const KeyboardLayout&);

// The places as the layout types them, upper case, apart by spaces: "A S D F G H J K L ;" on QWERTY, "Q S D F G H J K L
// M" on AZERTY. A place that types nothing is named as on the US layout; the places `leftOut` says are left out.
juce::String describePlaces (const juce::String& places, const KeyboardLayout&, const std::function<bool (char)>& leftOut = {});

//==============================================================================
// The X server's keys (Linux), as a table: a key's keysym by its key code (a place's scan code and 8), its layout group
// and its level (0 without Shift, 1 with it). KeysymLayout reads the layout from one; the Linux part of getSystemLayout()
// gives it the X server's, and a test a table of its own.
class KeysymTable
{
public:
    virtual ~KeysymTable() = default;
    virtual unsigned long getKeysym (int keyCode, int group, int level) const = 0;
    virtual int getGroup() const = 0; // the layout group in use

    // The character of a keysym that is neither Latin-1's nor Unicode's (an older one: Czech's ů, Cyrillic, Greek):
    // libxkbcommon's xkb_keysym_to_utf32(), where it is there; 0 where it can't tell.
    virtual juce::juce_wchar translate (unsigned long) const { return 0; }
};

// A key's place by the X server's key code (its scan code and 8); 0 for a place it doesn't know.
int getX11KeyCode (char place);

// A keysym's character: Latin-1's are their own, Unicode's carry theirs from 0x1000000 on; 0 for any other.
juce::juce_wchar keysymToCharacter (unsigned long keysym);

// A layout from the X server's keys: what a place types is its key's keysym in the group in use, as a character
// (keysymToCharacter(), else the table's translate()). Whether its key was down is asked of isKeyDown, JUCE's
// KeyPress::isKeyCurrentlyDown(), with the key's own keysym in the first group without Shift, which JUCE (through
// XKeysymToKeycode(), the first key with that keysym in the first column) finds that key by again: asked with what the
// group in use types, it found another key (QWERTZ's Z, the US layout's Y, with us,de and German in use). Without a
// table, the US layout's. The places' characters and keysyms are read once, and again only when the group in use
// changes: asking for a character converts nothing.
class KeysymLayout : public KeyboardLayout
{
public:
    KeysymLayout (const KeysymTable*, std::function<bool (int keyCode)> isKeyDown);

    juce::juce_wchar getCharacter (char place, bool shifted) const override;
    bool isDown (char place) const override;
    int getOtherKeyCode (char place) const override; // the key's own keysym in the first group, where it has no character

private:
    struct Place
    {
        juce::juce_wchar plain = 0, shifted = 0;
        unsigned long ownKeysym = 0; // in the first group, without Shift: what JUCE finds the key by
    };

    const KeysymTable* table;
    std::function<bool (int)> isKeyDown;
    mutable std::map<char, Place> places;
    mutable int readGroup = -1;

    const Place* findPlace (char) const; // read again if the group in use changed
};
} // namespace phazite::ui

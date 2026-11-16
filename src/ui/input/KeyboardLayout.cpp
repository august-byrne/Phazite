// SPDX-License-Identifier: AGPL-3.0-or-later
#include "KeyboardLayout.h"
#include "ui/theme/LookAndFeel.h"

#include <array>
#include <cstring>
#include <utility>

namespace phazite::ui
{
namespace
{
    // The places, by the character the US layout types there, with their scan codes (PC set 1, which Windows gives; on
    // Linux each key's code is its scan code and 8).
    // clang-format off
    constexpr std::array<std::pair<char, unsigned int>, 48> scanCodes { {
        { '1', 0x02 }, { '2', 0x03 }, { '3', 0x04 }, { '4', 0x05 }, { '5', 0x06 }, { '6', 0x07 }, { '7', 0x08 }, { '8', 0x09 },
        { '9', 0x0a }, { '0', 0x0b }, { '-', 0x0c }, { '=', 0x0d },
        { 'q', 0x10 }, { 'w', 0x11 }, { 'e', 0x12 }, { 'r', 0x13 }, { 't', 0x14 }, { 'y', 0x15 }, { 'u', 0x16 }, { 'i', 0x17 },
        { 'o', 0x18 }, { 'p', 0x19 }, { '[', 0x1a }, { ']', 0x1b },
        { 'a', 0x1e }, { 's', 0x1f }, { 'd', 0x20 }, { 'f', 0x21 }, { 'g', 0x22 }, { 'h', 0x23 }, { 'j', 0x24 }, { 'k', 0x25 },
        { 'l', 0x26 }, { ';', 0x27 }, { '\'', 0x28 }, { '`', 0x29 }, { '\\', 0x2b },
        { 'z', 0x2c }, { 'x', 0x2d }, { 'c', 0x2e }, { 'v', 0x2f }, { 'b', 0x30 }, { 'n', 0x31 }, { 'm', 0x32 }, { ',', 0x33 },
        { '.', 0x34 }, { '/', 0x35 }, { '<', 0x56 }, // '<': the ISO keyboards' key right of left Shift
    } };
    // clang-format on

    // What the US layout types with Shift, at the places that aren't letters.
    constexpr const char* usUnshifted = "1234567890-=[];'`\\,./";
    constexpr const char* usShifted = "!@#$%^&*()_+{}:\"~|<>?";

    // The computer keyboard's places, whose characters the fingerprint covers.
    constexpr const char* keyboardPlaces = "awsedftgyhujkolp;zxcv";

    unsigned int getScanCode (char place)
    {
        for (const auto& [character, code] : scanCodes)
            if (character == place)
                return code;

        return 0;
    }

    juce::juce_wchar toLower (juce::juce_wchar c)
    {
        return juce::CharacterFunctions::toLowerCase (c);
    }

    juce::juce_wchar getUsCharacter (char place, bool shifted)
    {
        if (getScanCode (place) == 0 || place == '<') // the US layout's keyboards have no ISO key
            return 0;

        if (! shifted)
            return (juce::juce_wchar) place;

        if (const auto* found = std::strchr (usUnshifted, place); found != nullptr && *found != 0)
            return (juce::juce_wchar) usShifted[found - usUnshifted];

        return (juce::juce_wchar) place; // a letter, which Shift types in upper case
    }

    struct UsLayout final : KeyboardLayout
    {
        juce::juce_wchar getCharacter (char place, bool shifted) const override { return getUsCharacter (place, shifted); }
        bool isDown (char) const override { return false; }
    };

    bool isLetterOrDigit (juce::juce_wchar c)
    {
        return juce::CharacterFunctions::isLetterOrDigit (c);
    }

    // juce::KeyPress's own, before the X11 headers below take its name for a macro.
    [[maybe_unused]] bool isJuceKeyDown (int keyCode)
    {
        return juce::KeyPress::isKeyCurrentlyDown (keyCode);
    }
} // namespace

unsigned int getPlaceScanCode (char place)
{
    return getScanCode (place);
}

char findPlaceOfScanCode (unsigned int scanCode)
{
    for (const auto& [character, code] : scanCodes)
        if (code == scanCode && scanCode != 0)
            return character;

    return 0;
}

juce::uint32 KeyboardLayout::getFingerprint() const
{
    // Unsigned, so that it wraps rather than overflows.
    juce::uint32 hash = 17;

    for (const auto* place = keyboardPlaces; *place != 0; ++place)
        hash = hash * 31u + (juce::uint32) getCharacter (*place, false);

    return hash;
}

const KeyboardLayout& getUsLayout()
{
    static const UsLayout layout;
    return layout;
}

juce::KeyPress getShortcutKey (const juce::KeyPress& pressed, const KeyboardLayout& layout)
{
    const auto code = pressed.getKeyCode();
    const auto modifiers = pressed.getModifiers();

    if (code <= 0 || code > 0xffff)
        return juce::KeyPress (code, modifiers, 0);

    const auto typed = toLower ((juce::juce_wchar) code);

    if (modifiers.isShiftDown())
    {
        // Shift with a digit-row key where the layout types the digits with Shift: the digit typed, Shift held. Windows
        // gives the key's own character with Ctrl (AZERTY's &) and the digit without; Linux the digit either way.
        for (const auto* d = KeyboardLayout::digitRow; *d != 0; ++d)
        {
            const auto digit = (juce::juce_wchar) *d;
            const auto unshifted = layout.getCharacter (*d, false);

            if (unshifted != 0 && unshifted != digit && (typed == digit || typed == unshifted))
                return juce::KeyPress ((int) digit, modifiers, 0);
        }
    }

    // What a key typed with Shift, as the key's own character (Linux; Windows gives that already). Not a digit: Windows
    // gives a digit typed as the digit too.
    if (modifiers.isShiftDown() && ! juce::CharacterFunctions::isDigit (typed))
    {
        for (const auto& entry : scanCodes)
        {
            const auto shifted = layout.getCharacter (entry.first, true), plain = layout.getCharacter (entry.first, false);

            if (shifted != 0 && plain != 0 && shifted != plain && typed == shifted)
                return juce::KeyPress ((int) plain, modifiers, 0);
        }
    }

    return juce::KeyPress (code, modifiers, 0);
}

juce::KeyPress getDigitRowKey (const juce::KeyPress& pressed, const KeyboardLayout& layout)
{
    const auto code = pressed.getKeyCode();
    const auto modifiers = pressed.getModifiers();

    if (code > 0 && code <= 0xffff)
    {
        const auto typed = toLower ((juce::juce_wchar) code);

        for (const auto* d = KeyboardLayout::digitRow; *d != 0; ++d)
        {
            const auto digit = (juce::juce_wchar) *d;
            const auto unshifted = layout.getCharacter (*d, false);
            const auto typesDigitWithShift = unshifted != 0 && unshifted != digit;

            // Where the layout types the digits with Shift, Shift is how a digit is typed: with it, the key is the digit
            // alone (Shift and AZERTY's & key is 1, Ctrl, Shift and Czech's + key Ctrl+1).
            const auto digitModifiers =
                typesDigitWithShift && modifiers.isShiftDown() ? modifiers.withoutFlags (juce::ModifierKeys::shiftModifier) : modifiers;

            // The shifted character only with Shift: Linux gives the number pad's * and / as themselves, with no Shift, which
            // the US layout's 8 and QWERTZ's 7 type with it.
            if (typed == digit || typed == unshifted || (modifiers.isShiftDown() && typed == layout.getCharacter (*d, true)))
                return juce::KeyPress ((int) digit, digitModifiers, 0);
        }
    }

    return getShortcutKey (pressed, layout);
}

juce::juce_wchar getTypedCharacter (const juce::KeyPress& shortcut)
{
    const auto modifiers = shortcut.getModifiers();
    const auto code = shortcut.getKeyCode();

    if (modifiers.isCtrlDown() || modifiers.isCommandDown() || modifiers.isAltDown() || code <= ' ' || code > 0x7e)
        return 0;

    const auto c = (juce::juce_wchar) code;

    if (isLetterOrDigit (c))
        return 0;

    if (! modifiers.isShiftDown())
        return c;

    if (const auto* found = std::strchr (usUnshifted, (char) code); found != nullptr && *found != 0)
        return (juce::juce_wchar) usShifted[found - usUnshifted];

    return 0;
}

bool isTypedWithAltGr (const juce::KeyPress& key, const KeyboardLayout& layout)
{
    const auto modifiers = key.getModifiers();
    const auto typed = key.getTextCharacter();
    const auto code = key.getKeyCode();

    if (! modifiers.isAltDown() || modifiers.isCtrlDown() || modifiers.isCommandDown() || typed <= ' ' || code <= 0 || code > 0xffff)
        return false;

    // Compared as key caps write them, on both sides: the C locale's case functions leave Ö and ö apart.
    const auto text = toKeyCapCase (typed);
    const auto own = toKeyCapCase ((juce::juce_wchar) code);

    for (const auto& entry : scanCodes)
    {
        if (const auto plain = layout.getCharacter (entry.first, false); plain != 0 && toKeyCapCase (plain) == own)
        {
            const auto shifted = layout.getCharacter (entry.first, true);
            return text != own && (shifted == 0 || text != toKeyCapCase (shifted));
        }
    }

    return text != own;
}

juce::String describePlaces (const juce::String& places, const KeyboardLayout& layout, const std::function<bool (char)>& leftOut)
{
    juce::StringArray names;

    for (const auto place : places)
    {
        if (leftOut != nullptr && leftOut ((char) place))
            continue;

        const auto typed = layout.getCharacter ((char) place, false);
        names.add (juce::String::charToString (toKeyCapCase (typed != 0 ? typed : place)));
    }

    return names.joinIntoString (" ");
}

//==============================================================================
int getX11KeyCode (char place)
{
    const auto scanCode = getScanCode (place);
    return scanCode != 0 ? (int) scanCode + 8 : 0;
}

juce::juce_wchar keysymToCharacter (unsigned long keysym)
{
    if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff))
        return (juce::juce_wchar) keysym;

    if (keysym >= 0x1000100 && keysym <= 0x110ffff)
        return (juce::juce_wchar) (keysym - 0x1000000);

    return 0;
}

KeysymLayout::KeysymLayout (const KeysymTable* keysyms, std::function<bool (int)> keyDown)
    : table (keysyms),
      isKeyDown (std::move (keyDown))
{
}

const KeysymLayout::Place* KeysymLayout::findPlace (char place) const
{
    if (table == nullptr)
        return nullptr;

    // Every place read at once, for the group in use, and again only when it changes.
    if (const auto group = table->getGroup(); group != readGroup)
    {
        readGroup = group;
        places.clear();

        const auto toCharacter = [this] (unsigned long keysym)
        {
            const auto character = keysymToCharacter (keysym);
            return toLower (character != 0 || keysym == 0 ? character : table->translate (keysym));
        };

        for (const auto& entry : scanCodes)
        {
            const auto keyCode = (int) entry.second + 8;
            places[entry.first] = { toCharacter (table->getKeysym (keyCode, group, 0)), toCharacter (table->getKeysym (keyCode, group, 1)),
                                    table->getKeysym (keyCode, 0, 0) };
        }
    }

    const auto found = places.find (place);
    return found != places.end() ? &found->second : nullptr;
}

juce::juce_wchar KeysymLayout::getCharacter (char place, bool shifted) const
{
    if (table == nullptr)
        return getUsCharacter (place, shifted);

    const auto* found = findPlace (place);
    return found != nullptr ? (shifted ? found->shifted : found->plain) : 0;
}

bool KeysymLayout::isDown (char place) const
{
    if (isKeyDown == nullptr || getX11KeyCode (place) == 0)
        return false;

    // JUCE takes a key code below 0x10000 for a keysym of its own (a Latin-1 one, or a legacy one such as Czech's ů).
    const auto* found = findPlace (place);
    const auto keysym = found != nullptr ? found->ownKeysym : 0;
    return isKeyDown ((int) (keysym != 0 ? keysym : (unsigned long) place));
}

int KeysymLayout::getOtherKeyCode (char place) const
{
    // What JUCE gives as the key code where XLookupString() gives no character: the key's keysym in the first group.
    const auto* found = findPlace (place);
    return found != nullptr && found->ownKeysym != 0 && keysymToCharacter (found->ownKeysym) == 0 && found->ownKeysym <= 0xffff
               ? (int) found->ownKeysym
               : 0;
}
} // namespace phazite::ui

//==============================================================================
// The operating system's layout. Last, so that the system's headers and their macros can't change the code above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>

namespace phazite::ui
{
namespace
{
    // JUCE gives a key no place: its key code is what the layout types at the key without Shift, read through
    // MapVirtualKey() as the key's message comes (doKeyChar(), juce_gui_basics/native/juce_Windowing_windows.cpp), which
    // it doesn't promise. A place's key is its scan code's virtual key in the layout in use; what it types there the same
    // as JUCE reads it; and whether it was down GetKeyState()'s, which keeps step with the key messages: JUCE's
    // KeyPress::isKeyCurrentlyDown() reads GetAsyncKeyState(), the key as it is now, so a quick tap was over by the time a
    // busy message thread asked, and played no note. JUCE workarounds W23 and W47 (docs/juce-workarounds.md).
    struct WindowsLayout final : KeyboardLayout
    {
        juce::juce_wchar getCharacter (char place, bool shifted) const override
        {
            const auto key = getVirtualKey (place);

            if (key == 0 || shifted) // JUCE's key codes on Windows are without Shift
                return 0;

            return toLower ((juce::juce_wchar) LOWORD (MapVirtualKeyW (key, MAPVK_VK_TO_CHAR)));
        }

        bool isDown (char place) const override
        {
            const auto key = getVirtualKey (place);
            return key != 0 && (GetKeyState ((int) key) & 0x8000) != 0;
        }

        static UINT getVirtualKey (char place)
        {
            const auto scanCode = getScanCode (place);
            return scanCode != 0 ? MapVirtualKeyW (scanCode, MAPVK_VSC_TO_VK) : 0;
        }
    };
} // namespace

const KeyboardLayout& getSystemLayout()
{
    static const WindowsLayout layout;
    return layout;
}
} // namespace phazite::ui

#elif JUCE_LINUX || JUCE_BSD
// As juce_gui_basics.h includes them (JUCE_GUI_BASICS_INCLUDE_XHEADERS).
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wvariadic-macros")
 #include <X11/Xlib.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
 #include <X11/XKBlib.h>
 #undef KeyPress

 #include <cstdint>

namespace phazite::ui
{
namespace
{
    // JUCE gives a key no place: its key code is what the layout types at the key, Shift included, from XLookupString()
    // (handleKeyPressEvent(), juce_gui_basics/native/juce_XWindowSystem_linux.cpp), which it doesn't promise, and its X
    // display is its own. The X server's keys are read through a display of Phazite's own (libX11, loaded as JUCE loads
    // it, at run time), the layout group in use at most four times a second, for KeysymLayout, which asks JUCE whether a
    // key was down. An older keysym's character comes from libxkbcommon (loaded at run time too, where it is there; never
    // linked nor shipped), with no locale. Without a display (a test machine's), the US layout's. A layout switched to by
    // its group is followed; a keymap replaced while Phazite runs isn't, until it starts again. JUCE workaround W48
    // (docs/juce-workarounds.md).
    class X11Table final : public KeysymTable
    {
    public:
        X11Table()
        {
            if (auto* openDisplay = reinterpret_cast<decltype (&::XOpenDisplay)> (library.getFunction ("XOpenDisplay")))
                display = openDisplay (nullptr);

            toKeysym = reinterpret_cast<decltype (&::XkbKeycodeToKeysym)> (library.getFunction ("XkbKeycodeToKeysym"));
            getState = reinterpret_cast<decltype (&::XkbGetState)> (library.getFunction ("XkbGetState"));
            toUtf32 = reinterpret_cast<KeysymToUtf32> (xkbcommon.getFunction ("xkb_keysym_to_utf32"));
        }

        ~X11Table() override
        {
            if (display != nullptr)
                if (auto* closeDisplay = reinterpret_cast<decltype (&::XCloseDisplay)> (library.getFunction ("XCloseDisplay")))
                    closeDisplay (display);
        }

        bool isOpen() const noexcept { return display != nullptr && toKeysym != nullptr; }

        unsigned long getKeysym (int keyCode, int keyGroup, int level) const override
        {
            return toKeysym (display, (KeyCode) keyCode, keyGroup, level);
        }

        int getGroup() const override
        {
            const auto now = juce::Time::getMillisecondCounter();

            if (getState != nullptr && (groupReadAt == 0 || now - groupReadAt > 250))
            {
                XkbStateRec state {};

                if (getState (display, XkbUseCoreKbd, &state) == Success)
                    group = state.group;

                groupReadAt = now;
            }

            return group;
        }

        juce::juce_wchar translate (unsigned long keysym) const override
        {
            return toUtf32 != nullptr && keysym <= 0xffffffffu ? (juce::juce_wchar) toUtf32 ((std::uint32_t) keysym) : 0;
        }

    private:
        using KeysymToUtf32 = std::uint32_t (*) (std::uint32_t); // xkb_keysym_to_utf32(), xkbcommon/xkbcommon.h

        juce::DynamicLibrary library { "libX11.so.6" }, xkbcommon { "libxkbcommon.so.0" };
        Display* display = nullptr;
        decltype (&::XkbKeycodeToKeysym) toKeysym = nullptr;
        decltype (&::XkbGetState) getState = nullptr;
        KeysymToUtf32 toUtf32 = nullptr;
        mutable juce::uint32 groupReadAt = 0;
        mutable int group = 0;
    };
} // namespace

const KeyboardLayout& getSystemLayout()
{
    static const X11Table table;
    static const KeysymLayout layout (table.isOpen() ? &table : nullptr, isJuceKeyDown);
    return layout;
}
} // namespace phazite::ui

#else
namespace phazite::ui
{
const KeyboardLayout& getSystemLayout()
{
    return getUsLayout();
}
} // namespace phazite::ui
#endif

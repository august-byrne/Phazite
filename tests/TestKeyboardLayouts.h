// SPDX-License-Identifier: AGPL-3.0-or-later
// Keyboard layouts for tests (src/ui/input/KeyboardLayout.h): the US layout, French AZERTY, German QWERTZ, Czech, Spanish
// and Swedish, as what each types at a place without Shift, with it and with AltGr, their dead keys (which give no key
// press, without Shift, with it or with AltGr), the ISO key right of left Shift ('<') where the layout's keyboards have
// it, and the places whose keys are held.
#pragma once

#include "ui/input/KeyboardLayout.h"

#include <map>
#include <set>
#include <utility>

namespace phazite::test
{
struct SimulatedLayout final : ui::KeyboardLayout
{
    // What the layout types at the places where it isn't the US layout: without Shift, then with it (letters in lower case).
    std::map<char, std::pair<juce::juce_wchar, juce::juce_wchar>> differs;
    std::map<char, juce::juce_wchar> altGr;      // what AltGr types, at the places the keymaps' Ctrl+Alt keys use
    std::set<char> dead, deadShifted, deadAltGr; // the dead keys, without Shift, with it, and with AltGr
    std::set<char> down;

    juce::juce_wchar getCharacter (char place, bool shifted) const override
    {
        if (const auto found = differs.find (place); found != differs.end())
            return shifted ? found->second.second : found->second.first;

        return ui::getUsLayout().getCharacter (place, shifted);
    }

    bool isDown (char place) const override { return down.count (place) > 0; }

    juce::juce_wchar getAltGrCharacter (char place) const
    {
        const auto found = altGr.find (place);
        return found != altGr.end() ? found->second : 0;
    }

    // French AZERTY: A and Q, Z and W swapped, M right of L, ² left of 1, and the digits typed with Shift.
    static SimulatedLayout azerty()
    {
        SimulatedLayout layout;
        layout.differs = { { 'q', { 'a', 'a' } },   { 'a', { 'q', 'q' } },  { 'w', { 'z', 'z' } },  { 'z', { 'w', 'w' } },
                           { ';', { 'm', 'm' } },   { 'm', { ',', '?' } },  { ',', { ';', '.' } },  { '.', { ':', '/' } },
                           { '/', { '!', 0xa7 } },  { '1', { '&', '1' } },  { '2', { 0xe9, '2' } }, { '3', { '"', '3' } },
                           { '4', { '\'', '4' } },  { '5', { '(', '5' } },  { '6', { '-', '6' } },  { '7', { 0xe8, '7' } },
                           { '8', { '_', '8' } },   { '9', { 0xe7, '9' } }, { '0', { 0xe0, '0' } }, { '-', { ')', 0xb0 } },
                           { '=', { '=', '+' } },   { '[', { '^', 0xa8 } }, { ']', { '$', 0xa3 } }, { '\'', { 0xf9, '%' } },
                           { '\\', { '*', 0xb5 } }, { '`', { 0xb2, 0 } },   { '<', { '<', '>' } } };
        layout.dead = { '[' };
        layout.deadShifted = { '[' };
        layout.altGr = { { '2', '~' },  { '3', '#' }, { '4', '{' }, { '5', '[' }, { '6', '|' }, { '7', '`' },
                         { '8', '\\' }, { '9', '^' }, { '0', '@' }, { '-', ']' }, { '=', '}' }, { 'e', 0x20ac } };
        layout.deadAltGr = { '2', '7' };
        return layout;
    }

    // German QWERTZ: Y and Z swapped, the umlauts right of L, P and the L row's end, "=" with Shift on 0, "+" a key of its own.
    static SimulatedLayout qwertz()
    {
        SimulatedLayout layout;
        layout.differs = { { 'y', { 'z', 'z' } },   { 'z', { 'y', 'y' } }, { ';', { 0xf6, 0xf6 } }, { '\'', { 0xe4, 0xe4 } },
                           { '[', { 0xfc, 0xfc } }, { ']', { '+', '*' } }, { '-', { 0xdf, '?' } },  { '=', { 0xb4, '`' } },
                           { '/', { '-', '_' } },   { ',', { ',', ';' } }, { '.', { '.', ':' } },   { '\\', { '#', '\'' } },
                           { '`', { '^', 0xb0 } },  { '1', { '1', '!' } }, { '2', { '2', '"' } },   { '3', { '3', 0xa7 } },
                           { '4', { '4', '$' } },   { '5', { '5', '%' } }, { '6', { '6', '&' } },   { '7', { '7', '/' } },
                           { '8', { '8', '(' } },   { '9', { '9', ')' } }, { '0', { '0', '=' } },   { '<', { '<', '>' } } };
        layout.dead = { '=', '`' };
        layout.deadShifted = { '=' };
        layout.altGr = { { '2', 0xb2 }, { '3', 0xb3 }, { '7', '{' },    { '8', '[' }, { '9', ']' },  { '0', '}' },
                         { '-', '\\' }, { 'q', '@' },  { 'e', 0x20ac }, { ']', '~' }, { 'm', 0xb5 }, { '<', '|' } };
        return layout;
    }

    // Czech QWERTZ: the digit row types +, ě, š, č, ř, ž, ý, á, í, é, and the digits with Shift; = right of the 0's; the
    // digit row's AltGr level all dead keys (~ ˇ ^ ˘ ° ˛ ` ˙ ´ ˝).
    static SimulatedLayout czech()
    {
        SimulatedLayout layout;
        layout.differs = { { '`', { ';', 0xb0 } },   { '1', { '+', '1' } },   { '2', { 0x11b, '2' } }, { '3', { 0x161, '3' } },
                           { '4', { 0x10d, '4' } },  { '5', { 0x159, '5' } }, { '6', { 0x17e, '6' } }, { '7', { 0xfd, '7' } },
                           { '8', { 0xe1, '8' } },   { '9', { 0xed, '9' } },  { '0', { 0xe9, '0' } },  { '-', { '=', '%' } },
                           { '=', { 0xb4, 0x2c7 } }, { 'y', { 'z', 'z' } },   { 'z', { 'y', 'y' } },   { '[', { 0xfa, '/' } },
                           { ']', { ')', '(' } },    { ';', { 0x16f, '"' } }, { '\'', { 0xa7, '!' } }, { '\\', { 0xa8, '\'' } },
                           { ',', { ',', '?' } },    { '.', { '.', ':' } },   { '/', { '-', '_' } },   { '<', { '\\', '|' } } };
        layout.dead = { '=', '\\' };
        layout.deadShifted = { '=' };
        layout.altGr = { { '1', '~' },    { '2', 0x2c7 }, { '3', '^' },  { '4', 0x2d8 }, { '5', 0xb0 }, { '6', 0x2db },
                         { '7', '`' },    { '8', 0x2d9 }, { '9', 0xb4 }, { '0', 0x2dd }, { 'q', '\\' }, { 'w', '|' },
                         { 'e', 0x20ac }, { 'b', '{' },   { 'n', '}' },  { 'v', '@' },   { 'c', '&' },  { 'x', '#' } };
        layout.deadAltGr = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' };
        return layout;
    }

    // Spanish: ñ right of L, + right of P, the apostrophe and ¡ right of the 0's, "=" with Shift on 0, AltGr and 4 dead (~).
    static SimulatedLayout spanish()
    {
        SimulatedLayout layout;
        layout.differs = { { '`', { 0xba, 0xaa } },  { '1', { '1', '!' } },    { '2', { '2', '"' } }, { '3', { '3', 0xb7 } },
                           { '4', { '4', '$' } },    { '5', { '5', '%' } },    { '6', { '6', '&' } }, { '7', { '7', '/' } },
                           { '8', { '8', '(' } },    { '9', { '9', ')' } },    { '0', { '0', '=' } }, { '-', { '\'', '?' } },
                           { '=', { 0xa1, 0xbf } },  { '[', { '`', '^' } },    { ']', { '+', '*' } }, { ';', { 0xf1, 0xf1 } },
                           { '\'', { 0xb4, 0xa8 } }, { '\\', { 0xe7, 0xe7 } }, { ',', { ',', ';' } }, { '.', { '.', ':' } },
                           { '/', { '-', '_' } },    { '<', { '<', '>' } } };
        layout.dead = { '[', '\'' };
        layout.deadShifted = { '[', '\'' };
        layout.altGr = { { '`', '\\' }, { '1', '|' },    { '2', '@' }, { '3', '#' }, { '4', '~' },  { '5', 0x20ac },
                         { '6', 0xac }, { 'e', 0x20ac }, { '[', '[' }, { ']', ']' }, { '\'', '{' }, { '\\', '}' } };
        layout.deadAltGr = { '4' };
        return layout;
    }

    // Swedish (as the Nordic layouts): + and ´ right of the 0's, å, ö and ä, "=" with Shift on 0.
    static SimulatedLayout swedish()
    {
        SimulatedLayout layout;
        layout.differs = { { '`', { 0xa7, 0xbd } },  { '1', { '1', '!' } },   { '2', { '2', '"' } },  { '3', { '3', '#' } },
                           { '4', { '4', 0xa4 } },   { '5', { '5', '%' } },   { '6', { '6', '&' } },  { '7', { '7', '/' } },
                           { '8', { '8', '(' } },    { '9', { '9', ')' } },   { '0', { '0', '=' } },  { '-', { '+', '?' } },
                           { '=', { 0xb4, '`' } },   { '[', { 0xe5, 0xe5 } }, { ']', { 0xa8, '^' } }, { ';', { 0xf6, 0xf6 } },
                           { '\'', { 0xe4, 0xe4 } }, { '\\', { '\'', '*' } }, { ',', { ',', ';' } },  { '.', { '.', ':' } },
                           { '/', { '-', '_' } },    { '<', { '<', '>' } } };
        layout.dead = { '=', ']' };
        layout.deadShifted = { '=', ']' };
        layout.altGr = { { '2', '@' }, { '3', 0xa3 }, { '4', '$' },    { '5', 0x20ac }, { '7', '{' },  { '8', '[' }, { '9', ']' },
                         { '0', '}' }, { '-', '\\' }, { 'e', 0x20ac }, { ']', '~' },    { 'm', 0xb5 }, { '<', '|' } };
        layout.deadAltGr = { ']' };
        return layout;
    }
};
} // namespace phazite::test

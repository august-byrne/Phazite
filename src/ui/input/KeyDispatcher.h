// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "CommandBridge.h"
#include "KeyboardLayout.h"

#include <functional>
#include <utility>
#include <vector>

namespace phazite::ui
{
// Runs commands from keys, with the keys the bridge gives them: when commands share a key, the enabled one with the
// highest keyPriority runs; a held key runs its command once, unless it repeatsWhileHeld; a command hears when its key
// is let go (keyReleased); some keys work over plug-in windows (overPlugins). While a mouse button is down, a key runs only
// the few commands named in KeyDispatcher.cpp that change nothing in the song, its history or its files (play and stop, the
// tools, the editors' zoom): what the mouse is doing (a clip dragged, a knob turned) is one undo step from the press to the
// release, which any other command could land in or take apart. The rest wait for the mouse to go up.
//
// A key matches a shortcut in three ways, the first that finds shortcuts deciding, among every command, enabled or not:
// as the key it is, the character printed on it (getShortcutKey(): AZERTY's - key zooms out, Czech's + key zooms in);
// else, at a digit's place, as its digit (getDigitRowKey(): AZERTY's & key is 1, Ctrl with it Ctrl+1, and a digit typed
// with Shift there the digit alone); else, for a shortcut that isn't a letter or a digit and has no Ctrl, Alt or Cmd, by
// the character it types, whatever Shift that took on the layout in use ("=" is Shift+0 on QWERTZ: getTypedCharacter()).
// Of the commands found the first way that finds any, the enabled one with the highest priority runs. A key AltGr typed a
// character with matches nothing.
class KeyDispatcher : public juce::KeyListener
{
public:
    // The layout tells a key's place (getSystemLayout() in the app; tests give the US layout or one of their own).
    KeyDispatcher (CommandBridge&, const KeyboardLayout&);

    // The command a key runs: of the enabled commands it is a shortcut of, the one with the highest
    // keyPriority (the one registered first, if several have it). nullptr if there is none.
    const Command* findCommandForKey (const juce::KeyPress&) const;

    // The same, of every command it is a shortcut of, enabled or not: the one that runs where all of them can.
    const Command* findShortcutOf (const juce::KeyPress& key) const { return findCommand (key, false); }

    // The shortcuts a key pressed matches, the first way that finds any, with their commands, enabled or not, in the
    // registry's order: what the key runs, as the shortcuts editor's Record Keys lists it.
    enum class Way
    {
        asItIs,
        asDigit,
        byCharacter
    };

    struct Match
    {
        const Command* command = nullptr;
        juce::KeyPress shortcut;
        Way way = Way::asItIs; // how the key matched it
    };

    std::vector<Match> findMatches (const juce::KeyPress&) const;

    const KeyboardLayout& getLayout() const noexcept { return layout; }

    // Runs the key's command (findCommandForKey()). Windows add the dispatcher as a key listener, so the
    // shortcuts work in them. While the key is held, its repeats run the command again only if it
    // repeatsWhileHeld; keyStateChanged() sees the key let go.
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

    // While a plug-in's window has the keyboard focus: whether a key runs a command that works over plug-ins
    // (overPlugins), and running it. The plug-in gets the other keys.
    bool runsOverPlugins (const juce::KeyPress&) const;
    bool keyPressedOverPlugin (const juce::KeyPress&);
    std::vector<juce::KeyPress> getKeysOverPlugins() const; // every key of those commands, enabled or not

    // Forgets the held keys, as if they were let go: when the window loses the keyboard, their release goes
    // elsewhere.
    void releaseHeldKeys();

    // While the computer keyboard plays notes, single-key shortcuts are typed with Shift held, as in
    // Ableton Live: for Shift and a key, this runs the command of the key alone, unless Shift and the
    // key have a command of their own. Returns true if it ran a command.
    bool invokeWithoutShift (const juce::KeyPress&);

private:
    CommandBridge& commands;
    const KeyboardLayout& layout;
    // The keys that ran commands, while they are down, with those commands' ids.
    std::vector<std::pair<juce::KeyPress, juce::String>> heldKeys;

    // Of the commands a key is a shortcut of (enabled ones only, when asked), the one with the highest priority.
    const Command* findCommand (const juce::KeyPress&, bool enabledOnly) const;
    bool runForKey (const Command&, const juce::KeyPress&);
    void releaseHeldKeys (const std::function<bool (const juce::KeyPress&)>& isReleased);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyDispatcher)
};
} // namespace phazite::ui

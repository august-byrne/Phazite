// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "CommandBridge.h"

#include <functional>
#include <utility>
#include <vector>

namespace phazite::ui
{
// Runs commands from keys, with the keys the bridge gives them: when commands share a key, the enabled one with the
// highest keyPriority runs; a held key runs its command once, unless it repeatsWhileHeld; a command hears when its key
// is let go (keyReleased); some keys work over plug-in windows (overPlugins).
class KeyDispatcher : public juce::KeyListener
{
public:
    explicit KeyDispatcher (CommandBridge&);

    // The command a key runs: of the enabled commands it is a shortcut of, the one with the highest
    // keyPriority (the one registered first, if several have it). nullptr if there is none.
    const Command* findCommandForKey (const juce::KeyPress&) const;

    // Runs the key's command (findCommandForKey()). Windows add the dispatcher as a key listener, so the
    // shortcuts work in them. While the key is held, its repeats run the command again only if it
    // repeatsWhileHeld; keyStateChanged() sees the key let go.
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

    // While a plug-in's window has the keyboard focus: whether a key runs a command that works over plug-ins
    // (overPlugins), and running it. The plug-in gets the other keys.
    bool runsOverPlugins (const juce::KeyPress&) const;
    bool keyPressedOverPlugin (const juce::KeyPress&);

    // Forgets the held keys, as if they were let go: when the window loses the keyboard, their release goes
    // elsewhere.
    void releaseHeldKeys();

    // While the computer keyboard plays notes, single-key shortcuts are typed with Shift held, as in
    // Ableton Live: for Shift and a key, this runs the command of the key alone, unless Shift and the
    // key have a command of their own. Returns true if it ran a command.
    bool invokeWithoutShift (const juce::KeyPress&);

private:
    CommandBridge& commands;
    // The keys that ran commands, while they are down, with those commands' ids.
    std::vector<std::pair<juce::KeyPress, juce::String>> heldKeys;

    bool runForKey (const Command&, const juce::KeyPress&);
    void releaseHeldKeys (const std::function<bool (const juce::KeyPress&)>& isReleased);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyDispatcher)
};
} // namespace phazite::ui

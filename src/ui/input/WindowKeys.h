// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ComputerKeyboard.h"
#include "KeyDispatcher.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// The keys of the main window: its shortcuts, and the computer keyboard, which plays notes while it is on.
class WindowKeys
{
public:
    // Adds the shortcuts and then the computer keyboard as the window's key listeners. JUCE asks the listener added last
    // first (ComponentPeer::handleKeyPress), so the computer keyboard gets its letters before the shortcuts do; it leaves
    // alone keys pressed with Shift, Ctrl, Alt or Cmd.
    WindowKeys (juce::Component& window, KeyDispatcher& shortcuts, ComputerKeyboard&);
    ~WindowKeys(); // removes them, the computer keyboard first

    // For the window's keyPressed(), which JUCE calls when no key listener took the key: while the computer keyboard
    // plays notes, Shift and a letter run that letter's single-key shortcut, as in Ableton Live. True if it ran one.
    bool keyPressedAfterListeners (const juce::KeyPress&);

    // The window lost the keyboard, so keys let go elsewhere never reach it: ends the notes, and lets the tools that
    // spring back do so.
    void releaseKeys();

private:
    juce::Component& window;
    KeyDispatcher& shortcuts;
    ComputerKeyboard& computerKeyboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WindowKeys)
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WindowKeys.h"

namespace phazite::ui
{
WindowKeys::WindowKeys (juce::Component& w, KeyDispatcher& s, ComputerKeyboard& keyboard)
    : window (w),
      shortcuts (s),
      computerKeyboard (keyboard)
{
    // Shortcuts work anywhere in the window. Key listeners added later are asked first, so the computer keyboard gets
    // its letters before shortcuts do; it leaves alone keys pressed with Ctrl, Alt or Cmd.
    window.addKeyListener (&shortcuts);
    window.addKeyListener (&computerKeyboard);
}

WindowKeys::~WindowKeys()
{
    window.removeKeyListener (&computerKeyboard);
    window.removeKeyListener (&shortcuts);
}

bool WindowKeys::keyPressedAfterListeners (const juce::KeyPress& key)
{
    // While the computer keyboard plays notes, Shift and a letter run that letter's single-key shortcut, as in Ableton
    // Live. The key listeners have tried Shift's own shortcuts by now.
    return computerKeyboard.isEnabled() && shortcuts.invokeWithoutShift (key);
}

void WindowKeys::releaseKeys()
{
    computerKeyboard.releaseAll();
    shortcuts.releaseHeldKeys();
}
} // namespace phazite::ui

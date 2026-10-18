// SPDX-License-Identifier: AGPL-3.0-or-later
// The keymap the user chose, and their own changes to its keys, kept in Phazite's settings (services/Settings.h) by
// command id: JUCE's own format names commands by number, and the numbers change when commands are added.
#pragma once

#include "CommandBridge.h"
#include "services/Settings.h"

namespace phazite::ui
{
// The user's changes to the bridge's keymap, as XML, and back. Restoring skips what names no command.
juce::String getKeyChanges (const CommandBridge&);
void restoreKeyChanges (CommandBridge&, const juce::String& xml);

// The keymap and the user's changes to it, from and to the settings.
void loadKeySettings (CommandBridge&, const Settings&);
void saveKeySettings (const CommandBridge&, Settings&);

// Keeps the user's changes to the keymap in use, then uses another, with the user's changes to that one.
void switchKeymap (CommandBridge&, const juce::String& keymapName, Settings&);
} // namespace phazite::ui

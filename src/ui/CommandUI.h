// SPDX-License-Identifier: AGPL-3.0-or-later
// Menus and tooltips that show commands, with their shortcuts, and the color menu that tracks and sections
// share.
#pragma once

#include "model/Commands.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// The text with the command's first shortcut after it, such as "Play or stop (Space)".
juce::String withShortcut (const CommandRegistry&, const juce::String& text, const juce::String& commandID);

// Adds commands to a menu, by id, with their shortcuts; ids that aren't registered are left out.
void addCommandItem (juce::PopupMenu&, CommandRegistry&, const juce::String& id);
void addCommandItems (juce::PopupMenu&, CommandRegistry&, std::initializer_list<const char*> ids);

// A menu of the track palette's colors (phazite::getTrackColours()), with the current one ticked. Calls back
// with the color chosen, unless the menu is dismissed or the target has been deleted by then.
void showColourMenu (juce::Component& target, juce::Colour current, std::function<void (juce::Colour)> onChosen);
} // namespace phazite::ui

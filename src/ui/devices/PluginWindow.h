// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's own window (docs/design.md, section 9): a plug-in's editor, or a knob for each parameter of
// Tracktion's built-in devices, which have no editor of their own. The windows stay above the main window and
// remember where they were, and play and stop, and the computer keyboard, work in them (ui/input/PluginKeys.h). Tracktion
// opens and closes them (te::PluginWindowState) and asks phazite::UIBehaviour for them, which calls
// createPluginWindow().
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
namespace te = tracktion;

// The window for a device, shown, or nullptr if the device has nothing to show (a plug-in that didn't load).
std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState&);

// Rebuilds a device's window after the device was reloaded, if one is open.
void recreatePluginWindowContent (te::Plugin&);
} // namespace phazite::ui

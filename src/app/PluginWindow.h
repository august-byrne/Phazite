// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's own window (docs/design.md, section 9): a plug-in's editor, or a knob for each parameter of Tracktion's
// built-in devices, which have no editor of their own. Phazite's own devices and containers have none: the Devices view
// shows them (ui/mixer/DevicesView.h). The windows stay above the main window and remember where they were, and play and
// stop, and the computer keyboard, work in them (ui/input/PluginKeys.h). Tracktion opens and closes them
// (te::PluginWindowState) and asks the app's AppUIBehaviour (AppUIBehaviour.h) for them, which calls
// createPluginWindow(). They are the app's: they are where Tracktion's plug-ins meet Phazite's controls.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
// The window for a device, shown, or nullptr if the device has nothing to show (a plug-in that didn't load, or one of
// Phazite's own devices or a container).
std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState&);

// Rebuilds a device's window after the device was reloaded, if one is open.
void recreatePluginWindowContent (te::Plugin&);
} // namespace phazite

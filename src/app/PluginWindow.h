// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's own window (docs/design.md, section 9): a plug-in's editor, the editor of one of Phazite's own devices
// (ui/devices/DeviceEditors.h), a container's (ui/mixer/ContainerEditor.h), or a knob for each parameter of Tracktion's
// built-in devices, which have no editor of their own. The windows stay above the main window and remember where they
// were, and play and stop, and the computer keyboard, work in them (ui/input/PluginKeys.h). Tracktion opens and closes
// them (te::PluginWindowState) and asks the app's AppUIBehaviour (AppUIBehaviour.h) for them, which calls
// createPluginWindow(). They are the app's: they are where Tracktion's plug-ins meet Phazite's controls.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class ProjectContext;

namespace ui
{
    class CommandBridge;
}

// What a container's window shows its devices and macros with: the open project, and the commands for its menus. A
// container's window needs both, which the app gives once it has made them.
struct PluginWindowServices
{
    ProjectContext* context = nullptr;
    ui::CommandBridge* commands = nullptr;
};

// The window for a device, shown, or nullptr if the device has nothing to show (a plug-in that didn't load).
std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState&, const PluginWindowServices& = {});

// Rebuilds a device's window after the device was reloaded, if one is open.
void recreatePluginWindowContent (te::Plugin&);
} // namespace phazite

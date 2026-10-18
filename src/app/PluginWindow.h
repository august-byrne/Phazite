// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's own window (docs/design.md, section 9), with the plug-in's editor. Phazite's own devices and containers have
// none: the Devices view shows them (ui/mixer/DevicesView.h). The windows stay above the main window and open where they
// were last, which the project keeps as view state, and play and stop, and the computer keyboard, work in them
// (ui/input/PluginKeys.h). Each has a menu bar with one menu, Plug-in (roadmap step 2.12): Save Preset..., Load Preset (the
// presets saved for the plug-in) and Send All Keys to the Plug-in. Tracktion opens and closes them
// (te::PluginWindowState) and asks the app's AppUIBehaviour (AppUIBehaviour.h) for them, which calls
// createPluginWindow(). They are the app's: they are where Tracktion's plug-ins meet Phazite's controls.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class Library;
class ProjectContext;
class Settings;

// The window for a plug-in, shown, or nullptr if the device has nothing to show (a plug-in that didn't load, or one of
// Phazite's own devices or a container).
std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState&);

// Rebuilds a device's window after the device was reloaded, if one is open.
void recreatePluginWindowContent (te::Plugin&);

// What the windows' menus work with: the open project, the user's presets, and the settings, which keep for each plug-in
// whether it gets every key. The main window sets them while it is open; until then the menus' items are off.
struct PluginWindowServices
{
    ProjectContext& context;
    Library& library;
    Settings& settings;
};

void setPluginWindowServices (const PluginWindowServices*);

// The menu bar of a plug-in's window, the window given: its keys (ui/input/PluginKeys.h) follow the menu's switch.
std::unique_ptr<juce::MenuBarModel> createPluginMenu (te::ExternalPlugin&, juce::Component& window);
} // namespace phazite

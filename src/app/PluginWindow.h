// SPDX-License-Identifier: AGPL-3.0-or-later
// Devices' windows (docs/design.md, section 9): a plug-in's own window, with the plug-in's editor (SandboxedEditor.h); and,
// when the user wants one (roadmap step 2.17), one of Phazite's own devices' editors out of the Devices view
// (ui/mixer/DevicesView.h), named after it and its channel ("EQ - Bass"), which can be made larger or smaller; containers
// show in the Devices view only. The windows stay above the main window and open where they were last, which the project
// keeps as view state. Play and stop, and the computer keyboard, work in a plug-in's (ui/input/PluginKeys.h), and every
// shortcut in one of Phazite's own, as in the main window. A plug-in's has a menu bar with one menu, Plug-in (roadmap step
// 2.12): Save Preset..., Load Preset (the presets saved for the plug-in) and Send All Keys to the Plug-in. They are the
// app's: the model asks for them by the devices' IDs (model/mixer/MixerModel.h: showDeviceWindow()), through the project
// context's DeviceWindowHost, which DeviceWindows is.
#pragma once

#include "model/base/IDs.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>

namespace phazite
{
class Library;
class Settings;

// The windows of a project context's devices, one for each device that shows one, which this owns: it shows them for the
// model from when it is made (it is the context's DeviceWindowHost until it goes) and closes them all as it goes and as
// another project opens. A window closes by itself when the song no longer has its device, when the device's track is
// frozen, and, a plug-in's, when its instance is gone; a plug-in's makes its editor again after the plug-in was reloaded.
// Closing a plug-in's window stores what the plug-in changed in it into the song, outside the undo history. The main
// window owns the app's. Message thread only.
class DeviceWindows final : public DeviceWindowHost,
                            private ProjectContext::Listener
{
public:
    // New windows open over a window (the main window), the first time; without one, over the window in front.
    explicit DeviceWindows (ProjectContext&, juce::Component* openOver = nullptr);
    ~DeviceWindows() override;

    // A device's window, shown, or brought to the front if it shows: nothing for a device that has none to show (a plug-in
    // that didn't load, or makes no editor, a frozen device, a container).
    void show (DeviceID) override;
    bool isOpen (DeviceID) const override;
    void close (DeviceID) override;
    void closeAll();

    // A device's window while it shows, or nullptr.
    juce::DocumentWindow* findWindow (DeviceID) const;

private:
    ProjectContext& context;
    juce::Component* const openOver;
    std::map<DeviceID, std::unique_ptr<juce::DocumentWindow>> windows;

    juce::Point<int> choosePosition (const juce::Component& window) const;
    void projectChanged() override { closeAll(); }

    JUCE_DECLARE_NON_COPYABLE (DeviceWindows)
};

// What the plug-in windows' menus work with: the open project, the user's presets, and the settings, which keep for each
// plug-in whether it gets every key; and what a drum sampler's Swap in its own window asks (the browser's, which tries its
// sounds on the pad: ui/browser/Browser.h). The main window sets them while it is open; until then the menus' items are off,
// and the pads have no Swap.
struct PluginWindowServices
{
    ProjectContext& context;
    Library& library;
    Settings& settings;
    std::function<void (DeviceID, int pad)> swap {};
};

void setPluginWindowServices (const PluginWindowServices*);

// The menu bar of a plug-in's window, for a plug-in of a project context's song, the window given: its keys
// (ui/input/PluginKeys.h) follow the menu's switch. Its presets are those of the services' project only.
std::unique_ptr<juce::MenuBarModel> createPluginMenu (ProjectContext&, DeviceID, juce::Component& window);
} // namespace phazite

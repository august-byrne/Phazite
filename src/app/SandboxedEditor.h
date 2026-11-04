// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's editor in its helper process as Phazite's window shows it: a component for the plug-in's instance
// (model/plugins/internal/SandboxedInstance.h), which a plug-in's window shows (PluginWindow.h). The helper's window for
// the plug-in's editor is a child of the window this is in, which the helper places over it, again as it moves or is
// resized; the two are as large as the plug-in makes its editor, and the plug-in's as large as this, where the plug-in can
// be resized. Phazite never calls into the helper's window, so that a helper that hangs can't hang Phazite's window with
// it: everything goes through the control channel. The keys Phazite takes over a plug-in (ui/input/PluginKeys.h) the
// helper hands back, which run here, unless the window's plug-in gets every key; the helper is told which they are, and
// again as they change (the computer keyboard switched, Send All Keys to the Plug-in). A plug-in whose helper has gone
// shows nothing of its own. It keeps its instance while it shows it, and closes the editor in the helper as it goes.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace phazite
{
class SandboxedInstance;

class SandboxedEditor final : public juce::Component,
                              private juce::ComponentMovementWatcher,
                              private juce::Timer
{
public:
    ~SandboxedEditor() override;

    // Whether the plug-in lets its editor be resized, as it said as it made it.
    bool isResizable() const noexcept { return resizable; }

private:
    friend std::unique_ptr<SandboxedEditor> createSandboxedEditor (std::shared_ptr<SandboxedInstance>);
    SandboxedEditor (std::shared_ptr<SandboxedInstance>, int width, int height, bool canResize);

    std::shared_ptr<SandboxedInstance> instance;
    const bool resizable;
    std::int64_t placedWindow = 0; // where the helper was told to show it
    juce::Rectangle<int> placedArea;
    bool toldKeys = false, toldEveryKey = false;
    std::vector<juce::KeyPress> toldWanted;

    void paint (juce::Graphics&) override;
    void place();
    void componentMovedOrResized (bool, bool) override { place(); }
    void componentPeerChanged() override { place(); }
    void componentVisibilityChanged() override { place(); }
    using juce::ComponentMovementWatcher::componentMovedOrResized;
    using juce::ComponentMovementWatcher::componentVisibilityChanged;
    void timerCallback() override;
    void runKey (bool isDown, int keyCode, int modifiers);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedEditor)
};

// The editor of a plug-in in a helper, made there: nullptr for a plug-in that has none, one whose helper has gone, or one
// that didn't make it in time.
std::unique_ptr<SandboxedEditor> createSandboxedEditor (std::shared_ptr<SandboxedInstance>);
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's editor in its helper process as Phazite's window shows it: a component for the plug-in's instance
// (model/plugins/internal/SandboxedInstance.h), which a plug-in's window shows (PluginWindow.h). The helper's window for
// the plug-in's editor is a child of the window this is in, which the helper places over it, again as it moves or is
// resized; the two are as large on the screen as the plug-in makes its editor, whatever the Interface Size (this one's size
// is the plug-in's divided by it, fitted again as it changes), and the plug-in's as large as this, where the plug-in can be
// resized. Phazite never calls into the helper's window, so that a helper that hangs can't hang Phazite's window with
// it: everything goes through the control channel. The helper's window is a child of a window of this one's own (its
// socket), a child of the window this is in, which this places: as it goes, it takes the socket out of that window,
// hidden, and asks the helper to close its editor without waiting; the socket goes once the helper's window has left it
// (closed, or with its helper). Windows destroys a window's children with it, and a child of another process's thread
// only as that thread answers: destroyed with Phazite's window, a helper's that hangs kept Phazite waiting until the
// helper was ended (known bug 165). On Linux the socket is an X window of Phazite's own, made through a display of its
// own (libX11, loaded at run time as JUCE loads it), so that the helper's window is never destroyed under it (known bug
// 166: the close waited up to 2 seconds for the helper to take it out); without a display, and on macOS (where Phazite
// doesn't build yet), there is none, and the close waits for nothing either. The keys Phazite takes over a plug-in
// (ui/input/PluginKeys.h) the helper hands back, which run here, unless the window's plug-in gets every key; the helper is
// told which they are, in every form they come in for the layout in use (known bug 177), and again as they change (the
// computer keyboard switched, a layout switched to, Send All Keys to the Plug-in). A plug-in whose helper has gone
// shows nothing of its own. It keeps its instance while it shows it, and closes the editor in the helper as it goes.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace phazite
{
class SandboxedInstance;
class SandboxedEditorRequest;

class SandboxedEditor final : public juce::Component,
                              private juce::ComponentMovementWatcher,
                              private juce::Timer
{
public:
    ~SandboxedEditor() override;

    // Whether the plug-in lets its editor be resized, as it said as it made it.
    bool isResizable() const noexcept { return resizable; }

    // Its instance.
    const std::shared_ptr<SandboxedInstance>& getInstance() const noexcept { return instance; }

    // A key the helper handed back, run as Phazite's own hook runs one over a plug-in's window, with the character the layout
    // in use types at the key, as the window's own keys come (ui/input/PluginKeys.h: PluginKeys::makeHandedBackKey()).
    void runKey (bool isDown, int keyCode, int modifiers, int scanCode = 0);

    // What its timer does a few times a second, now: the keys Phazite takes told to the helper if they changed (for a test,
    // which then waits for the helper to have taken them, rather than for a time).
    void tickNow() { timerCallback(); }

private:
    friend class SandboxedEditorRequest;
    SandboxedEditor (std::shared_ptr<SandboxedInstance>, int width, int height, bool canResize);

    std::shared_ptr<SandboxedInstance> instance;
    const bool resizable;
    juce::Point<int> pluginSize;   // the plug-in's editor's, as the helper told it, in the screen's units
    float fittedScale = 1.0f;      // getScale() as it was last fitted to it
    std::int64_t placedWindow = 0; // where the helper was told to show it
    juce::Rectangle<int> placedArea;
    void* socket = nullptr;       // the window the helper's is in (its native handle), made as it first shows; none without one
    void* socketParent = nullptr; // the window that is in (on Windows watched as it goes)

    void lookAndFeelChanged() override; // the socket's colour, the theme's again
    bool toldKeys = false, toldEveryKey = false;
    std::vector<juce::KeyPress> toldWanted;
    std::vector<std::pair<unsigned int, int>> toldPlaces;

    void paint (juce::Graphics&) override;
    float getScale() const; // of its units in the screen's: the Interface Size, as its window has it
    void fitPlugin();       // its size, the plug-in's in its own units
    void place();
    void componentMovedOrResized (bool, bool) override { place(); }
    void componentPeerChanged() override { place(); }
    void componentVisibilityChanged() override { place(); }
    using juce::ComponentMovementWatcher::componentMovedOrResized;
    using juce::ComponentMovementWatcher::componentVisibilityChanged;
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedEditor)
};

// The editor of a plug-in in a helper, asked of its helper without waiting (known bug 166: the message thread waited for
// the helper to make it, up to 10 seconds), then given to what asked, on the message thread, once it is made: nullptr for a
// plug-in that has none, or whose helper went. Waited for as long as the helper lives: it isn't taken for hung for the first
// 30 seconds the plug-in takes, and after that only as its message thread doesn't answer. Let go of before then, it calls
// nothing, and the helper closes the editor it makes. requestSandboxedEditor() asks nothing, and gives nullptr, for an
// instance that has crashed or has no editor.
class SandboxedEditorRequest final : private juce::Timer
{
public:
    using Made = std::function<void (std::unique_ptr<SandboxedEditor>)>;

    ~SandboxedEditorRequest() override;

private:
    friend std::unique_ptr<SandboxedEditorRequest> requestSandboxedEditor (std::shared_ptr<SandboxedInstance>, Made);
    SandboxedEditorRequest (std::shared_ptr<SandboxedInstance>, Made);

    struct Asked; // the helper's answer to come
    std::shared_ptr<SandboxedInstance> instance;
    std::unique_ptr<Asked> asked;
    Made made;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedEditorRequest)
};

std::unique_ptr<SandboxedEditorRequest> requestSandboxedEditor (std::shared_ptr<SandboxedInstance>, SandboxedEditorRequest::Made);
} // namespace phazite

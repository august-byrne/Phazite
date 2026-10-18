// SPDX-License-Identifier: AGPL-3.0-or-later
// Keys that work while a plug-in's window has the keyboard focus, so that a sound can be played while it is tweaked
// (the owner's request, 2026-10-13): the keys of the commands that work over plug-ins (play and stop;
// Command::overPlugins) and, while it plays notes, the computer keyboard's. The plug-in gets every other key.
//
// A plug-in window's editor takes every key that reaches it through JUCE, so these listen on each component in the
// window, which hear a key before the component does. A plug-in's own native window gets its keys before JUCE sees
// them at all, so on Windows a message hook on the message thread catches these keys on their way to it. The hook is
// made with the app, before any plug-in's window: Windows calls the hook installed last first, so a plug-in built with
// JUCE, whose own hook offers each key to its focused control (a preset name being typed, say) and passes on the keys
// that control doesn't use, gets them first. A key typed into a native text field goes to the plug-in too.
//
// A plug-in that draws its own text fields, which none of this can see, gets every key while its window's "Send All
// Keys to the Plug-in" switch is on (docs/design.md, section 9; roadmap step 2.12): then play, stop and the computer
// keyboard don't work over it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace phazite::ui
{
class ComputerKeyboard;
class KeyDispatcher;

class PluginKeys : public juce::KeyListener
{
public:
    PluginKeys (KeyDispatcher&, ComputerKeyboard&);
    ~PluginKeys() override;

    // Whether a key goes to Phazite rather than to the plug-in.
    bool wants (const juce::KeyPress&) const;

    // Listens to a plug-in's window and everything in it (call it again when the window's content changes), or
    // stops.
    void watch (juce::Component& window);
    void unwatch (juce::Component& window);

    // Whether a watched window's plug-in gets every key: its "Send All Keys to the Plug-in" switch.
    void setSendsAllKeys (juce::Component& window, bool);
    bool sendsAllKeys (const juce::Component& window) const;

    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

private:
    struct NativeHook;

    struct Watched
    {
        juce::Component::SafePointer<juce::Component> window {};
        bool allKeys = false; // its plug-in gets every key
    };

    KeyDispatcher& shortcuts;
    ComputerKeyboard& computerKeyboard;
    std::vector<Watched> windows;
    std::unique_ptr<NativeHook> nativeHook; // Windows only

    const Watched* findWindow (void* nativeHandle) const;      // the watched one a native window is in
    const Watched* findWatched (const juce::Component*) const; // the watched one a component is in
    void removeFrom (juce::Component&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginKeys)
};

// The app's, which the plug-in windows that Tracktion asks for use (createPluginWindow()); nullptr until the main
// window makes it.
void setPluginKeys (PluginKeys*);
PluginKeys* getPluginKeys();
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Keys that work while a plug-in's window has the keyboard focus, so that a sound can be played while it is tweaked
// (the owner's request, 2026-10-13): the keys of the commands that work over plug-ins (play and stop;
// Command::overPlugins) and, while it plays notes, the computer keyboard's. The plug-in gets every other key.
//
// A plug-in window's editor takes every key that reaches it through JUCE, so these listen on each component in the
// window, which hear a key before the component does. A plug-in's own window is its helper's, in another process, whose
// keys never reach Phazite's: the helper catches these keys on their way to it and hands them back (host/core/Editor.h,
// app/SandboxedEditor.h), unless a text field of the plug-in's has the keys.
//
// A plug-in that draws its own text fields, which none of this can see, gets every key while its window's "Send All
// Keys to the Plug-in" switch is on (docs/design.md, section 9; roadmap step 2.12): then play, stop and the computer
// keyboard don't work over it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

namespace phazite::ui
{
class ComputerKeyboard;
class KeyDispatcher;
class WindowKeys;

class PluginKeys : public juce::KeyListener
{
public:
    PluginKeys (KeyDispatcher&, ComputerKeyboard&);
    ~PluginKeys() override;

    // Whether a key goes to Phazite rather than to the plug-in; and those keys, for a plug-in's window in a helper
    // process, which catches them there (app/SandboxedEditor.h): the keys as the keymaps and the computer keyboard have them,
    // and every key of the layout in use, in each form the helper names a key by (its character without Shift, as Windows
    // gives it; with Shift, as Linux does), that the dispatcher's own matching takes for one of them (known bug 177: AZERTY's
    // & key for a shortcut on 1, by its digit's place). They change as the computer keyboard is switched, the keymap
    // changes, and another layout is switched to; worked out again only then.
    bool wants (const juce::KeyPress&) const;
    std::vector<juce::KeyPress> getWantedKeys() const;

    // The same keys by their places (scan codes, as KeyboardLayout.h has them) and modifiers (JUCE's raw flags), which a
    // helper on Windows matches a key by, whatever its own thread's layout (the review of known bug 177); and every other
    // place with placeWantedByNone for its modifiers, as a helper matches a key that has a place by its place alone (its
    // second review: by a code its own layout gave it, a key could be taken that meant another to Phazite).
    static constexpr int placeWantedByNone = -1;
    std::vector<std::pair<unsigned int, int>> getWantedPlaces() const;

    // A key a helper hands back, by its code and modifiers, and its place where the helper told it (its scan code; 0 for
    // none), as a key pressed in Phazite's own window is: by what this layout types at its place, where it has one, and with
    // the character the layout types at it, where it types one (no Ctrl, Alt or Command held), so that a shortcut matched by
    // the character a key types matches it too.
    juce::KeyPress makeHandedBackKey (int keyCode, int modifiers, unsigned int scanCode = 0) const;

    // Listens to a plug-in's window and everything in it (call it again when the window's content changes), or
    // stops.
    void watch (juce::Component& window);
    void unwatch (juce::Component& window);

    // Whether a watched window's plug-in gets every key: its "Send All Keys to the Plug-in" switch.
    void setSendsAllKeys (juce::Component& window, bool);
    bool sendsAllKeys (const juce::Component& window) const;

    // The main window's keys (WindowKeys: every shortcut, and the computer keyboard) for another of Phazite's own
    // windows, which is no plug-in's: a built-in device's (app/PluginWindow.h), until unwatch(), or until these keys go
    // with the main window. Its keyPressed() asks keyPressedAfterListeners(), and it lets go of the keys held as another
    // window becomes the active one.
    void watchAsOwnWindow (juce::Component& window);
    bool keyPressedAfterListeners (juce::Component& window, const juce::KeyPress&);
    void releaseKeys (juce::Component& window);

    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

private:
    struct Watched
    {
        juce::Component::SafePointer<juce::Component> window {};
        bool allKeys = false; // its plug-in gets every key
    };

    struct OwnWindow
    {
        juce::Component::SafePointer<juce::Component> window {};
        std::unique_ptr<WindowKeys> keys;
    };

    KeyDispatcher& shortcuts;
    ComputerKeyboard& computerKeyboard;
    std::vector<Watched> windows;
    std::vector<OwnWindow> ownWindows;

    // The keys wanted, as last worked out, and what they were worked out from: the keys as written, and the layout's
    // characters at its places.
    mutable std::vector<juce::KeyPress> wanted, wantedFrom;
    mutable std::vector<std::pair<unsigned int, int>> wantedPlaces;
    mutable juce::String wantedLayout;

    const Watched* findWatched (const juce::Component*) const; // the watched one a component is in
    WindowKeys* findOwnWindow (const juce::Component&) const;
    void removeFrom (juce::Component&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginKeys)
};

// The app's, which devices' windows use (app/PluginWindow.h); nullptr until the main window makes it.
void setPluginKeys (PluginKeys*);
PluginKeys* getPluginKeys();
} // namespace phazite::ui

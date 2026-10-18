// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginKeys.h"
#include "ComputerKeyboard.h"
#include "KeyDispatcher.h"

// Last, so that its macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace phazite::ui
{
namespace
{
    PluginKeys* appPluginKeys = nullptr;

    // A window and everything in it.
    void forEachComponent (juce::Component& component, const std::function<void (juce::Component&)>& action)
    {
        action (component);

        for (auto* child : component.getChildren())
            forEachComponent (*child, action);
    }
} // namespace

void setPluginKeys (PluginKeys* keys)
{
    appPluginKeys = keys;
}

PluginKeys* getPluginKeys()
{
    return appPluginKeys;
}

//==============================================================================
#if JUCE_WINDOWS
// A hook on the messages the message thread takes from its queue, before they go to their windows: a plug-in's own
// window included, which JUCE doesn't see.
struct PluginKeys::NativeHook
{
    explicit NativeHook (PluginKeys& o)
        : owner (o)
    {
        jassert (current == nullptr); // the app makes one PluginKeys
        current = this;
        hook = SetWindowsHookExW (WH_GETMESSAGE, &NativeHook::getMessage, nullptr, GetCurrentThreadId());
    }

    ~NativeHook()
    {
        if (hook != nullptr)
            UnhookWindowsHookEx (hook);

        current = nullptr;
    }

    static LRESULT CALLBACK getMessage (int code, WPARAM removed, LPARAM message)
    {
        if (code == HC_ACTION && removed == PM_REMOVE && current != nullptr)
            current->filter (*reinterpret_cast<MSG*> (message));

        return CallNextHookEx (nullptr, code, removed, message);
    }

    void filter (MSG& message)
    {
        const auto isDown = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
        const auto isUp = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;

        if (! (isDown || isUp))
            return;

        // Keys in a watched window, unless its plug-in gets them all or a native text field has the focus.
        const auto* watched = owner.findWindow (message.hwnd);

        if (watched == nullptr || watched->allKeys || isTextField (GetFocus()))
            return;

        const auto key = toKeyPress ((UINT) message.wParam);

        if (! owner.wants (key))
            return;

        // Phazite gets the key, as the main window would, and the plug-in doesn't.
        owner.keyStateChanged (isDown, nullptr);

        if (isDown)
            owner.keyPressed (key, nullptr);

        message.message = WM_NULL;
    }

    // A native text field, where Space and letters are typed.
    static bool isTextField (HWND window)
    {
        wchar_t className[64] = {};

        if (window == nullptr || GetClassNameW (window, className, 64) == 0)
            return false;

        return juce::String (className).containsIgnoreCase ("edit");
    }

    // The key as JUCE names it, with the modifier keys held now: Space by its code, and a key that types a character
    // (the computer keyboard's letters and ;) by its unshifted character.
    static juce::KeyPress toKeyPress (UINT virtualKey)
    {
        const auto modifiers = juce::ModifierKeys::getCurrentModifiersRealtime().withoutMouseButtons();

        if (virtualKey == VK_SPACE)
            return { juce::KeyPress::spaceKey, modifiers, 0 };

        const auto character = (int) (MapVirtualKeyW (virtualKey, MAPVK_VK_TO_CHAR) & 0x7fff); // the top bit marks dead keys
        return { character != 0 ? character : (int) virtualKey, modifiers, 0 };
    }

    PluginKeys& owner;
    HHOOK hook = nullptr;
    static inline NativeHook* current = nullptr;
};
#else
struct PluginKeys::NativeHook
{
    explicit NativeHook (PluginKeys&) {}
};
#endif

//==============================================================================
PluginKeys::PluginKeys (KeyDispatcher& dispatcher, ComputerKeyboard& keyboard)
    : shortcuts (dispatcher),
      computerKeyboard (keyboard),
      nativeHook (std::make_unique<NativeHook> (*this))
{
}

PluginKeys::~PluginKeys()
{
    for (auto& watched : windows)
        if (auto* window = watched.window.getComponent())
            removeFrom (*window);

    nativeHook = nullptr;
}

bool PluginKeys::wants (const juce::KeyPress& key) const
{
    return shortcuts.runsOverPlugins (key) || computerKeyboard.takesKey (key);
}

void PluginKeys::watch (juce::Component& window)
{
    // A component's key listeners hear a key before the component itself, so the editor can't take these keys first.
    forEachComponent (window,
                      [this] (juce::Component& component)
                      {
                          component.removeKeyListener (this);
                          component.addKeyListener (this);
                      });

    if (std::none_of (windows.begin(), windows.end(), [&window] (const Watched& w) { return w.window == &window; }))
        windows.push_back ({ &window, false });
}

void PluginKeys::unwatch (juce::Component& window)
{
    removeFrom (window);
    windows.erase (std::remove_if (windows.begin(), windows.end(),
                                   [&window] (const Watched& w) { return w.window == nullptr || w.window == &window; }),
                   windows.end());
}

void PluginKeys::setSendsAllKeys (juce::Component& window, bool allKeys)
{
    for (auto& watched : windows)
        if (watched.window == &window)
            watched.allKeys = allKeys;
}

bool PluginKeys::sendsAllKeys (const juce::Component& window) const
{
    const auto* watched = findWatched (&window);
    return watched != nullptr && watched->allKeys;
}

const PluginKeys::Watched* PluginKeys::findWatched (const juce::Component* component) const
{
    const auto* window = component != nullptr ? component->getTopLevelComponent() : nullptr;

    for (const auto& watched : windows)
        if (window != nullptr && watched.window == window)
            return &watched;

    return nullptr;
}

void PluginKeys::removeFrom (juce::Component& window)
{
    forEachComponent (window, [this] (juce::Component& component) { component.removeKeyListener (this); });
}

bool PluginKeys::keyPressed (const juce::KeyPress& key, juce::Component* component)
{
    // A component's key (the native hook passes none): not for a plug-in that gets every key.
    if (const auto* watched = findWatched (component); watched != nullptr && watched->allKeys)
        return false;

    if (! wants (key))
        return false;

    return shortcuts.keyPressedOverPlugin (key) || computerKeyboard.keyPressed (key, component);
}

bool PluginKeys::keyStateChanged (bool isKeyDown, juce::Component* component)
{
    if (const auto* watched = findWatched (component); watched != nullptr && watched->allKeys)
        return false;

    // Keys let go release the commands they ran, and the computer keyboard starts and ends its notes.
    shortcuts.keyStateChanged (isKeyDown, component);
    return computerKeyboard.keyStateChanged (isKeyDown, component);
}

const PluginKeys::Watched* PluginKeys::findWindow ([[maybe_unused]] void* nativeHandle) const
{
#if JUCE_WINDOWS
    const auto root = GetAncestor (static_cast<HWND> (nativeHandle), GA_ROOT);

    for (const auto& watched : windows)
    {
        auto* peer = watched.window != nullptr ? watched.window->getPeer() : nullptr;

        if (peer != nullptr && static_cast<HWND> (peer->getNativeHandle()) == root)
            return &watched;
    }
#endif

    return nullptr;
}
} // namespace phazite::ui

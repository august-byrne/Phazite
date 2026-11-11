// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginKeys.h"
#include "ComputerKeyboard.h"
#include "KeyDispatcher.h"
#include "WindowKeys.h"

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
PluginKeys::PluginKeys (KeyDispatcher& dispatcher, ComputerKeyboard& keyboard)
    : shortcuts (dispatcher),
      computerKeyboard (keyboard)
{
}

PluginKeys::~PluginKeys()
{
    for (auto& watched : windows)
        if (auto* window = watched.window.getComponent())
            removeFrom (*window);

    ownWindows.clear(); // their keys leave them, before the keys themselves go
}

bool PluginKeys::wants (const juce::KeyPress& key) const
{
    return shortcuts.runsOverPlugins (key) || computerKeyboard.takesKey (key);
}

std::vector<juce::KeyPress> PluginKeys::getWantedKeys() const
{
    auto keys = shortcuts.getKeysOverPlugins();
    const auto playing = computerKeyboard.getKeys();
    keys.insert (keys.end(), playing.begin(), playing.end());
    return keys;
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
    ownWindows.erase (std::remove_if (ownWindows.begin(), ownWindows.end(), [&window] (const OwnWindow& w) { return w.window == &window; }),
                      ownWindows.end());
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

void PluginKeys::watchAsOwnWindow (juce::Component& window)
{
    if (findOwnWindow (window) == nullptr)
        ownWindows.push_back ({ &window, std::make_unique<WindowKeys> (window, shortcuts, computerKeyboard) });
}

WindowKeys* PluginKeys::findOwnWindow (const juce::Component& window) const
{
    for (const auto& own : ownWindows)
        if (own.window == &window)
            return own.keys.get();

    return nullptr;
}

bool PluginKeys::keyPressedAfterListeners (juce::Component& window, const juce::KeyPress& key)
{
    auto* keys = findOwnWindow (window);
    return keys != nullptr && keys->keyPressedAfterListeners (key);
}

void PluginKeys::releaseKeys (juce::Component& window)
{
    if (auto* keys = findOwnWindow (window))
        keys->releaseKeys();
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
    // A component's key (the plug-in's editor's for one its helper hands back, app/SandboxedEditor.h): not for a plug-in
    // that gets every key.
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
} // namespace phazite::ui

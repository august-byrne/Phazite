// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginKeys.h"
#include "ComputerKeyboard.h"
#include "KeyDispatcher.h"
#include "KeyboardLayout.h"
#include "WindowKeys.h"

#include <algorithm>

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

    // The layout as its places type, without Shift and with it.
    const auto& layout = shortcuts.getLayout();
    juce::String typed;

    for (const auto* place = KeyboardLayout::places; *place != 0; ++place)
        typed << juce::String::charToString (layout.getCharacter (*place, false))
              << juce::String::charToString (layout.getCharacter (*place, true));

    if (keys == wantedFrom && typed == wantedLayout)
        return wanted;

    // Every key of a place, with the modifiers of a key wanted, and Shift too or not (Shift and AZERTY's & is 1), in the forms a
    // helper names it by, kept where the dispatcher's matching takes it for a command that works over plug-ins, or the computer
    // keyboard takes it (known bug 177).
    std::vector<int> modifierSets;

    for (const auto& key : keys)
    {
        const auto flags = key.getModifiers().getRawFlags();

        for (const auto set : { flags, flags ^ juce::ModifierKeys::shiftModifier })
            if (std::find (modifierSets.begin(), modifierSets.end(), set) == modifierSets.end())
                modifierSets.push_back (set);
    }

    auto found = keys;
    std::vector<std::pair<unsigned int, int>> places;

    const auto isWanted = [this] (const juce::KeyPress& candidate)
    {
        const auto asHandedBack = makeHandedBackKey (candidate.getKeyCode(), candidate.getModifiers().getRawFlags());
        const auto matches = shortcuts.findMatches (asHandedBack);
        const auto overPlugins =
            std::any_of (matches.begin(), matches.end(), [] (const auto& match) { return match.command->overPlugins; });

        return overPlugins || computerKeyboard.takesKey (asHandedBack);
    };

    const auto add = [&found] (const juce::KeyPress& candidate)
    {
        if (std::find (found.begin(), found.end(), candidate) == found.end())
            found.push_back (candidate);
    };

    for (const auto* place = KeyboardLayout::places; *place != 0; ++place)
    {
        const auto plain = layout.getCharacter (*place, false), shifted = layout.getCharacter (*place, true);
        const auto scanCode = getPlaceScanCode (*place);
        auto anyWanted = false;

        for (const auto set : modifierSets)
        {
            if (plain == 0)
                break;

            // By its character as this layout types it, and by its place, which a helper whose layout is another matches
            // it by (the review of known bug 177); a Shift form by its place too (its second review: it was in the code
            // list alone, which a helper whose layout is another matches as another key).
            const juce::ModifierKeys modifiers (set);
            auto byPlace = false;

            if (const juce::KeyPress byCharacter ((int) plain, modifiers, 0); isWanted (byCharacter))
            {
                add (byCharacter);
                byPlace = true;
            }

            if (const juce::KeyPress byShifted ((int) shifted, modifiers, 0);
                modifiers.isShiftDown() && shifted != 0 && shifted != plain && isWanted (byShifted))
            {
                add (byShifted);
                byPlace = true;
            }

            if (byPlace && scanCode != 0)
                places.emplace_back (scanCode, set);

            anyWanted = anyWanted || byPlace;
        }

        // Every other place said to be one, wanted with no modifiers (placeWantedByNone): a helper matches a key with a place by
        // its place alone, never by a code its own layout gives it.
        if (! anyWanted && scanCode != 0)
            places.emplace_back (scanCode, placeWantedByNone);
    }

    wantedFrom = std::move (keys);
    wantedLayout = typed;
    wanted = std::move (found);
    wantedPlaces = std::move (places);
    return wanted;
}

std::vector<std::pair<unsigned int, int>> PluginKeys::getWantedPlaces() const
{
    getWantedKeys(); // worked out with them
    return wantedPlaces;
}

juce::KeyPress PluginKeys::makeHandedBackKey (int keyCode, int modifiers, unsigned int scanCode) const
{
    const juce::ModifierKeys held (modifiers);
    juce::juce_wchar typed = 0;

    // A key told by its place: what this layout types there, as Phazite's own window would have it, whatever the helper's
    // layout typed (the review of known bug 177: a layout switched to while the plug-in's window had the keys).
    if (const auto place = findPlaceOfScanCode (scanCode); place != 0)
        if (const auto plain = shortcuts.getLayout().getCharacter (place, false); plain != 0)
            keyCode = (int) plain;

    if (! held.isCtrlDown() && ! held.isAltDown() && ! held.isCommandDown() && keyCode > ' ' && keyCode <= 0xffff)
    {
        const auto& layout = shortcuts.getLayout();
        const auto code = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) keyCode);

        for (const auto* place = KeyboardLayout::places; *place != 0 && typed == 0; ++place)
        {
            const auto plain = layout.getCharacter (*place, false), shifted = layout.getCharacter (*place, true);

            if (plain != 0 && (code == plain || (shifted != 0 && code == shifted)))
                typed = held.isShiftDown() ? shifted : plain;
        }
    }

    return juce::KeyPress (keyCode, held, typed);
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

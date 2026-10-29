// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <optional>

namespace phazite::ui
{
// A set of shortcuts: command id -> key descriptions in JUCE's format ("command + Z", "spacebar",
// "F5"). "command" is Ctrl on Windows and Linux and Cmd on macOS. A keymap also says how the playlist's mouse works.
struct Keymap
{
    static constexpr const char* defaultName = "Phazite";

    juce::String name, description;
    std::map<juce::String, juce::StringArray> keys;
    bool rightClickDeletes = false; // in the playlist, as in FL Studio; otherwise right-click opens a menu
    bool clickPlacesClips = false;  // in the playlist's empty space, with the Smart Tool, as FL Studio's Draw tool does
    bool selectionLoops = false;    // a time range selected in the playlist loops while it is selected, as in FL Studio

    static std::optional<Keymap> fromJSON (const juce::String&);

    // The keymaps that ship with Phazite (resources/keymaps): "Phazite" (the default), "FL Studio" and "Ableton Live".
    static juce::Array<Keymap> getBuiltIn();
    static std::optional<Keymap> findBuiltIn (const juce::String& name);
};
} // namespace phazite::ui

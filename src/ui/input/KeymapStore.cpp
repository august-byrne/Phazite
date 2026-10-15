// SPDX-License-Identifier: AGPL-3.0-or-later
#include "KeymapStore.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    constexpr const char* keyChangesTag = "PHAZITEKEYCHANGES";
} // namespace

juce::String getKeyChanges (const CommandBridge& bridge)
{
    // By command id: JUCE's own format names commands by number, and the numbers change when commands are added.
    juce::XmlElement changes (keyChangesTag);
    const auto& registry = bridge.getRegistry();
    const auto& keymap = bridge.getKeymap();

    for (int i = 0; i < registry.getNumCommands(); ++i)
    {
        const auto& id = registry.getCommand (i)->id;
        const auto keys = bridge.getKeyPresses (id);
        juce::Array<juce::KeyPress> defaults;

        if (const auto found = keymap.keys.find (id); found != keymap.keys.end())
            for (const auto& description : found->second)
                if (const auto key = juce::KeyPress::createFromDescription (description); key.isValid())
                    defaults.add (key);

        const auto same =
            keys.size() == defaults.size()
            && std::all_of (keys.begin(), keys.end(), [&defaults] (const juce::KeyPress& key) { return defaults.contains (key); });

        if (same)
            continue;

        auto* change = changes.createNewChildElement ("COMMAND");
        change->setAttribute ("id", id);

        for (const auto& key : keys)
            change->createNewChildElement ("KEY")->setAttribute ("key", key.getTextDescription());
    }

    return changes.getNumChildElements() > 0 ? changes.toString() : juce::String();
}

void restoreKeyChanges (CommandBridge& bridge, const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);

    if (parsed == nullptr || ! parsed->hasTagName (keyChangesTag))
        return;

    auto& mappings = bridge.getKeyMappings();

    for (auto* change : parsed->getChildWithTagNameIterator ("COMMAND"))
    {
        const auto commandID = bridge.getCommandID (change->getStringAttribute ("id"));

        if (commandID == 0)
            continue;

        mappings.clearAllKeyPresses (commandID);

        for (auto* key : change->getChildWithTagNameIterator ("KEY"))
            if (const auto press = juce::KeyPress::createFromDescription (key->getStringAttribute ("key")); press.isValid())
                mappings.addKeyPress (commandID, press);
    }
}

void loadKeySettings (CommandBridge& bridge, const Settings& settings)
{
    if (auto savedKeymap = Keymap::findBuiltIn (settings.get (Settings::keymap)))
        bridge.setKeymap (*savedKeymap);

    restoreKeyChanges (bridge, settings.getKeyChanges (bridge.getKeymap().name));
}

void saveKeySettings (const CommandBridge& bridge, Settings& settings)
{
    settings.set (Settings::keymap, bridge.getKeymap().name);
    settings.setKeyChanges (bridge.getKeymap().name, getKeyChanges (bridge));
}

void switchKeymap (CommandBridge& bridge, const juce::String& keymapName, Settings& settings)
{
    saveKeySettings (bridge, settings); // keeps the user's changes to the old keymap
    settings.set (Settings::keymap, keymapName);
    loadKeySettings (bridge, settings); // the new keymap, with the user's changes to it
    saveKeySettings (bridge, settings);
}
} // namespace phazite::ui

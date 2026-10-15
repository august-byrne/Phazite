// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Keymap.h"

#include <PhaziteData.h>

#include <utility>

namespace phazite::ui
{
std::optional<Keymap> Keymap::fromJSON (const juce::String& text)
{
    const auto json = juce::JSON::parse (text);
    Keymap keymap;
    keymap.name = json["name"].toString();
    keymap.description = json["description"].toString();
    keymap.rightClickDeletes = json["rightClickDeletes"];
    keymap.clickPlacesClips = json["clickPlacesClips"];
    keymap.selectionLoops = json["selectionLoops"];

    if (keymap.name.isEmpty())
        return {};

    if (auto* keys = json["keys"].getDynamicObject())
    {
        for (const auto& property : keys->getProperties())
        {
            juce::StringArray descriptions;

            if (auto* array = property.value.getArray())
                for (const auto& key : *array)
                    descriptions.add (key.toString());

            keymap.keys[property.name.toString()] = descriptions;
        }
    }

    return keymap;
}

juce::Array<Keymap> Keymap::getBuiltIn()
{
    juce::Array<Keymap> keymaps;

    for (const auto& [data, size] : { std::pair { PhaziteData::phazite_json, PhaziteData::phazite_jsonSize },
                                      std::pair { PhaziteData::fl_studio_json, PhaziteData::fl_studio_jsonSize } })
        if (auto keymap = fromJSON (juce::String::fromUTF8 (data, size)))
            keymaps.add (*keymap);

    return keymaps;
}

std::optional<Keymap> Keymap::findBuiltIn (const juce::String& name)
{
    for (const auto& keymap : getBuiltIn())
        if (keymap.name == name)
            return keymap;

    return {};
}
} // namespace phazite::ui

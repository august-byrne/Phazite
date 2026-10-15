// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LibraryDrag.h"

namespace phazite::ui
{
juce::var toDragDescription (const LibraryItem& item)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("phaziteItem", item.getKey());

    if (item.device.has_value())
    {
        if (const auto xml = item.device->description.createXml())
            object->setProperty ("device", xml->toString());
    }
    else
    {
        object->setProperty ("file", item.file.getFullPathName());
    }

    return juce::var (object);
}

std::optional<juce::File> getDraggedFile (const juce::var& description)
{
    const auto path = description.getProperty ("file", {}).toString();

    if (path.isEmpty() || ! juce::File::isAbsolutePath (path))
        return std::nullopt;

    return juce::File (path);
}

std::optional<DeviceType> getDraggedDevice (const juce::var& description)
{
    const auto text = description.getProperty ("device", {}).toString();
    juce::PluginDescription plugin;

    if (const auto xml = text.isNotEmpty() ? juce::parseXML (text) : nullptr; xml == nullptr || ! plugin.loadFromXml (*xml))
        return std::nullopt;

    return makeDeviceType (plugin);
}

juce::String getDraggedKey (const juce::var& description)
{
    return description.getProperty ("phaziteItem", {}).toString();
}

void markPlaced (const juce::var& description)
{
    // The description's object is shared by every copy of it, the browser's included.
    if (auto* object = description.getDynamicObject())
        object->setProperty ("placed", true);
}

bool wasPlaced (const juce::var& description)
{
    return description.getProperty ("placed", false);
}
} // namespace phazite::ui

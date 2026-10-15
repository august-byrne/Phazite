// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Settings.h"

namespace phazite
{
Settings::Settings (juce::PropertiesFile* propertiesFile)
    : inMemory (propertiesFile == nullptr ? std::make_unique<juce::PropertySet>() : nullptr),
      storage (propertiesFile != nullptr ? static_cast<juce::PropertySet*> (propertiesFile) : inMemory.get()),
      file (propertiesFile)
{
}

bool Settings::get (const BoolSetting& setting) const
{
    return storage->getBoolValue (setting.key, setting.defaultValue);
}

double Settings::get (const NumberSetting& setting) const
{
    return storage->getDoubleValue (setting.key, setting.defaultValue);
}

double Settings::get (const NumberSettingWithoutDefault& setting, double defaultValue) const
{
    return storage->getDoubleValue (setting.key, defaultValue);
}

juce::String Settings::get (const TextSetting& setting) const
{
    return storage->getValue (setting.key, setting.defaultValue);
}

juce::StringArray Settings::get (const ListSetting& setting) const
{
    auto lines = juce::StringArray::fromLines (storage->getValue (setting.key));
    lines.removeEmptyStrings();
    return lines;
}

void Settings::set (const BoolSetting& setting, bool value)
{
    storage->setValue (setting.key, value);
    changed (setting.save);
}

void Settings::set (const NumberSetting& setting, double value)
{
    storage->setValue (setting.key, value);
    changed (setting.save);
}

void Settings::set (const NumberSettingWithoutDefault& setting, double value)
{
    storage->setValue (setting.key, value);
    changed (setting.save);
}

void Settings::set (const TextSetting& setting, const juce::String& value)
{
    storage->setValue (setting.key, value);
    changed (setting.save);
}

void Settings::set (const ListSetting& setting, const juce::StringArray& lines)
{
    storage->setValue (setting.key, lines.joinIntoString ("\n"));
    changed (setting.save);
}

juce::String Settings::getKeyChanges (const juce::String& keymapName) const
{
    return storage->getValue (keyChangesPrefix + keymapName);
}

void Settings::setKeyChanges (const juce::String& keymapName, const juce::String& xml)
{
    storage->setValue (keyChangesPrefix + keymapName, xml);
    changed (SaveTiming::now);
}

void Settings::saveNow()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (file != nullptr)
        file->saveIfNeeded();
}

void Settings::changed (SaveTiming timing)
{
    // A setting saved soon waits for the file's own timer (PropertiesFile saves a while after the last change).
    if (timing == SaveTiming::now)
        saveNow();
}
} // namespace phazite

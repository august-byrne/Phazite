// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Settings.h"
#include "model/base/Json.h"
#include "model/engine/AppInfo.h"

#include <cmath>

namespace phazite
{
Settings::Settings (juce::PropertiesFile* propertiesFile)
    : inMemory (propertiesFile == nullptr ? std::make_unique<juce::PropertySet>() : nullptr),
      storage (propertiesFile != nullptr ? static_cast<juce::PropertySet*> (propertiesFile) : inMemory.get()),
      file (propertiesFile)
{
}

juce::PropertiesFile::Options Settings::getFileOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = appName;
    options.filenameSuffix = "settings";
    options.folderName = appName;
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 2000;
    return options;
}

juce::File Settings::getFolder()
{
    return getFileOptions().getDefaultFile().getParentDirectory();
}

bool Settings::get (const BoolSetting& setting) const
{
    return storage->getBoolValue (setting.key, setting.defaultValue);
}

// A number from the file, which anything could have written: its default for what is no finite number.
double Settings::get (const NumberSetting& setting) const
{
    // Read without the overflow of JUCE's reader of numbers ("1e12345678901"), as getDoubleValue() would read it.
    return storage->containsKey (setting.key) ? readDouble (storage->getValue (setting.key), setting.defaultValue) : setting.defaultValue;
}

double Settings::get (const NumberSettingWithoutDefault& setting, double defaultValue) const
{
    return storage->containsKey (setting.key) ? readDouble (storage->getValue (setting.key), defaultValue) : defaultValue;
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
    store (setting.key, value, setting.save);
}

void Settings::set (const NumberSetting& setting, double value)
{
    store (setting.key, value, setting.save);
}

void Settings::set (const NumberSettingWithoutDefault& setting, double value)
{
    store (setting.key, value, setting.save);
}

void Settings::set (const TextSetting& setting, const juce::String& value)
{
    store (setting.key, value, setting.save);
}

void Settings::set (const ListSetting& setting, const juce::StringArray& lines)
{
    store (setting.key, lines.joinIntoString ("\n"), setting.save);
}

juce::String Settings::getKeyChanges (const juce::String& keymapName) const
{
    return storage->getValue (keyChangesPrefix + keymapName);
}

void Settings::setKeyChanges (const juce::String& keymapName, const juce::String& xml)
{
    store (keyChangesPrefix + keymapName, xml, SaveTiming::now);
}

void Settings::saveNow()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (file != nullptr)
        file->saveIfNeeded();
}

void Settings::store (const juce::String& key, const juce::var& value, SaveTiming timing)
{
    // A setting saved now is written at once, with whatever waits to be saved soon; one saved soon waits for the file's own
    // timer (PropertiesFile saves a while after the last change). A setting set to what it holds already writes nothing:
    // what sets several as one of them changes (the audio device's settings, app/AppServices.cpp) would write the file, and
    // what waits with it, at each step of a drag of one saved soon.
    const auto before = storage->getValue (key);
    storage->setValue (key, value);

    if (timing == SaveTiming::now && storage->getValue (key) != before)
        saveNow();

    listeners.call ([&key] (Listener& listener) { listener.settingChanged (key); });
}

void Settings::addListener (Listener* listener)
{
    JUCE_ASSERT_MESSAGE_THREAD
    listeners.add (listener);
}

void Settings::removeListener (Listener* listener)
{
    listeners.remove (listener);
}
} // namespace phazite

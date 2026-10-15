// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's settings: every key it keeps in Settings.xml (next to the Engine's own settings), with its default and when
// a change is saved. Code reads and writes Phazite's settings only through this list.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
// When a changed setting reaches the file.
enum class SaveTiming
{
    now, // saved at once
    soon // within 2 s (the file's timer) and on close: values that change during drags or with every placement
};

struct BoolSetting
{
    const char* key = "";
    bool defaultValue = false;
    SaveTiming save = SaveTiming::now;
};

struct NumberSetting
{
    const char* key = "";
    double defaultValue = 0.0;
    SaveTiming save = SaveTiming::now;
};

// A number whose default depends on where it is used, so each read gives it.
struct NumberSettingWithoutDefault
{
    const char* key = "";
    SaveTiming save = SaveTiming::now;
};

struct TextSetting
{
    const char* key = "";
    const char* defaultValue = "";
    SaveTiming save = SaveTiming::now;
};

// Lines of text, empty by default.
struct ListSetting
{
    const char* key = "";
    SaveTiming save = SaveTiming::now;
};

class Settings
{
public:
    // Every setting Phazite keeps. A key is part of Settings.xml's format: never rename one.
    static constexpr TextSetting theme { "phaziteTheme", "dark", SaveTiming::now }; // dark, light, highContrast
    static constexpr BoolSetting hoverHelp { "phaziteHoverHelp", true, SaveTiming::now };
    static constexpr BoolSetting computerKeyboard { "phaziteComputerKeyboard", true, SaveTiming::now };
    static constexpr NumberSetting interfaceScale { "phaziteInterfaceScale", 1.0, SaveTiming::now };
    static constexpr BoolSetting welcomeShown { "phaziteWelcomeShown", false, SaveTiming::now };
    static constexpr TextSetting keymap { "phaziteKeymap", "Phazite", SaveTiming::now };
    static constexpr const char* keyChangesPrefix = "phaziteKeyChanges."; // + a keymap's name; saved at once
    static constexpr ListSetting recentProjects { "phaziteRecentProjects", SaveTiming::now };
    static constexpr ListSetting browserFolders { "phaziteBrowserFolders", SaveTiming::soon };
    static constexpr ListSetting browserFavorites { "phaziteBrowserFavorites", SaveTiming::soon };
    static constexpr ListSetting browserRecent { "phaziteBrowserRecent", SaveTiming::soon };
    static constexpr BoolSetting browserPreview { "phaziteBrowserPreview", true, SaveTiming::soon };
    static constexpr NumberSettingWithoutDefault browserPreviewVolume { "phaziteBrowserPreviewVolume", SaveTiming::soon };
    static constexpr TextSetting browserOpenness { "phaziteBrowserOpenness", "", SaveTiming::soon };

    // Kept in that file (the Engine's, which must outlive this), or, without one, in memory (tests). The parameter
    // isn't named 'file': it would hide the member (C4458, -Wshadow).
    explicit Settings (juce::PropertiesFile* propertiesFile = nullptr);

    bool get (const BoolSetting&) const;
    double get (const NumberSetting&) const;
    double get (const NumberSettingWithoutDefault&, double defaultValue) const;
    juce::String get (const TextSetting&) const;
    juce::StringArray get (const ListSetting&) const; // without empty lines

    void set (const BoolSetting&, bool);
    void set (const NumberSetting&, double);
    void set (const NumberSettingWithoutDefault&, double);
    void set (const TextSetting&, const juce::String&);
    void set (const ListSetting&, const juce::StringArray&); // stored as lines

    // The user's changes to a keymap's keys, as XML (the UI's KeymapStore writes them), saved at once.
    juce::String getKeyChanges (const juce::String& keymapName) const;
    void setKeyChanges (const juce::String& keymapName, const juce::String& xml);

    // Writes what has changed to the file now, the changes waiting to be saved soon included. Nothing in memory.
    void saveNow();

private:
    std::unique_ptr<juce::PropertySet> inMemory;
    juce::PropertySet* storage = nullptr;
    juce::PropertiesFile* file = nullptr;

    void changed (SaveTiming);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Settings)
};
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// The services the app owns and hands to what needs them: what Phazite keeps outside any project (docs/design.md,
// section 24). Made right after the Engine, and gone just before it: after the window and the project's session.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/engine/EngineContext.h"
#include "model/engine/Preview.h"
#include "services/Library.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"
#include "services/SoundPack.h"

namespace phazite
{
// At namespace scope, not nested in AppServices: GCC refuses a nested struct with default member initialisers as a
// default argument of its enclosing class (CWG 1397).
struct AppServicesOptions
{
    bool settingsInMemory = false;                                 // for tests: nothing is read from or written to Phazite.settings
    juce::File packsFolder = SoundPack::getDefaultPacksFolder();   // where Phazite Sounds are installed
    juce::File presetsFolder = Library::getDefaultPresetsFolder(); // where the user's presets are saved
    juce::File factoryPresetsFolder = Library::getDefaultFactoryPresetsFolder(); // the presets that come with Phazite
    SoundPack::Release soundPackRelease = SoundPack::getPinnedRelease();         // the release of them that is downloaded
};

struct AppServices
{
    explicit AppServices (te::Engine&, AppServicesOptions = {});

    // In this order: each may use those before it. The browser's library indexes Phazite Sounds' folder, and writes its
    // settings until the window that shows it goes; the previewer's Edit goes before the Engine.
    std::unique_ptr<EngineContext> engineContext;       // the Engine as the UI reaches it
    std::unique_ptr<juce::PropertiesFile> settingsFile; // Phazite's own (Settings::getFileOptions()), or none in memory
    Settings settings;
    RecentProjects recentProjects;
    SoundPack soundPack;                  // Phazite Sounds, downloaded when the user asks
    Library library;                      // what the browser and the plug-in picker show
    std::unique_ptr<Previewer> previewer; // the browser's

    // The plug-ins the scan found, and those that crashed their helpers as they loaded, kept off, at the next start too:
    // read from the settings as the services start, and kept there as they change.
    struct PluginLists;
    std::unique_ptr<PluginLists> pluginLists;

    // Opens the audio device and the MIDI inputs as the settings saved them (or as a first run chooses), and keeps them
    // there from then on as they change (model/engine/internal/AudioIO.h): the app's, as it starts. Tests open devices of
    // their own, and their services save none.
    void openAudioDevices();
    struct DeviceSettings;
    std::unique_ptr<DeviceSettings> deviceSettings;
    ~AppServices();

    JUCE_DECLARE_NON_COPYABLE (AppServices)
};
} // namespace phazite

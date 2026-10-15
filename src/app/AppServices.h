// SPDX-License-Identifier: AGPL-3.0-or-later
// The services the app owns and hands to what needs them: what Phazite keeps outside any project (docs/design.md,
// section 24). Made right after the Engine, and gone just before it: after the window and the project's session.
#pragma once

#include "model/base/Tracktion.h"
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
    bool settingsInMemory = false;                                       // for tests: nothing is read from or written to Settings.xml
    juce::File packsFolder = SoundPack::getDefaultPacksFolder();         // where Phazite Sounds are installed
    SoundPack::Release soundPackRelease = SoundPack::getPinnedRelease(); // the release of them that is downloaded
};

struct AppServices
{
    explicit AppServices (te::Engine&, AppServicesOptions = {});

    // In this order: each may use those before it. The browser's library indexes Phazite Sounds' folder, and writes its
    // settings until the window that shows it goes; the previewer's Edit goes before the Engine.
    Settings settings; // in the Engine's settings file
    RecentProjects recentProjects;
    SoundPack soundPack; // Phazite Sounds, downloaded when the user asks
    Library library;     // what the browser and the plug-in picker show
    Previewer previewer; // the browser's

    JUCE_DECLARE_NON_COPYABLE (AppServices)
};
} // namespace phazite

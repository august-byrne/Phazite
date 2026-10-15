// SPDX-License-Identifier: AGPL-3.0-or-later
// The services the app owns and hands to what needs them: what Phazite keeps outside any project (docs/design.md,
// section 24). Made right after the Engine, and gone just before it.
#pragma once

#include "model/base/Tracktion.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"

namespace phazite
{
// At namespace scope, not nested in AppServices: GCC refuses a nested struct with default member initialisers as a
// default argument of its enclosing class (CWG 1397).
struct AppServicesOptions
{
    bool settingsInMemory = false; // for tests: nothing is read from or written to Settings.xml
};

struct AppServices
{
    explicit AppServices (te::Engine&, AppServicesOptions = {});

    Settings settings; // in the Engine's settings file
    RecentProjects recentProjects;

    JUCE_DECLARE_NON_COPYABLE (AppServices)
};
} // namespace phazite

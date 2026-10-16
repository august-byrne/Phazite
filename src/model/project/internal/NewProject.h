// SPDX-License-Identifier: AGPL-3.0-or-later
// A new project's Tracktion side (model/project/NewProject.h), for the app and tests: what its settings add to its Edit.
// Not for the UI.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
struct NewProjectSettings;

// Adds what the settings choose to a new project's Edit, as Project::create()'s content function, so that it is part of
// the first save: the tempo and time signature, the instrument tracks, the key (each track's, and new tracks') and the
// outline's sections, in bars of that time signature.
void addNewProjectContent (te::Edit&, const NewProjectSettings&);
} // namespace phazite

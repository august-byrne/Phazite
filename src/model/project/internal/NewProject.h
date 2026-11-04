// SPDX-License-Identifier: AGPL-3.0-or-later
// What a new project's settings (model/project/NewProject.h) add to its song, for the app and tests. Not for the UI.
#pragma once

#include "model/base/Song.h"

namespace phazite
{
struct NewProjectSettings;

// Adds what the settings choose to a new project's song, as Project::create()'s content function, so that it is part of
// the first save: the tempo and time signature, the key that tracks added later get, and the outline's sections, in bars
// of that time signature. It takes away the track a new song has: a new project starts with none.
void addNewProjectContent (Song&, const NewProjectSettings&);
} // namespace phazite

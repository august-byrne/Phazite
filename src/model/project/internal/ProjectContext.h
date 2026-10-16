// SPDX-License-Identifier: AGPL-3.0-or-later
// The project context's Tracktion side (model/project/ProjectContext.h), for the model, the app and tests: the open
// Edit, Tracktion's selection manager, which the model's commands act on, and the time range selected across tracks.
// Not for the UI.
#pragma once

#include "model/arrangement/Selection.h"
#include "model/base/Tracktion.h"

namespace phazite
{
class ProjectContext;

te::Edit* getEdit (const ProjectContext&); // nullptr while no project is open
te::SelectionManager& getSelection (ProjectContext&);
const te::SelectionManager& getSelection (const ProjectContext&);
RangeSelection& getRangeSelection (ProjectContext&);

// The Edit open now, or none: ProjectSession's (and tests'). Nothing of the previous Edit stays selected. Then tells the
// listeners.
void setEdit (ProjectContext&, te::Edit*);
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// The project context's Tracktion side (model/project/ProjectContext.h), for the model, the app and tests: the open
// Edit, Tracktion's selection manager, which the model's commands act on, and the time range selected across tracks.
// Not for the UI.
#pragma once

#include "model/arrangement/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

namespace phazite
{
class AutomationWatcher;
class ClipRenders;
class ProjectContext;
struct Clipboard;

te::Edit* getEdit (const ProjectContext&); // nullptr while no project is open
te::SelectionManager& getSelection (ProjectContext&);
const te::SelectionManager& getSelection (const ProjectContext&);
RangeSelection& getRangeSelection (ProjectContext&);

// What watches the open Edit's automation (model/automation/internal/Automation.h): nullptr while no project is open.
AutomationWatcher* getAutomationWatcher (const ProjectContext&);

// What renders the open Edit's audio clips stretched, pitched or reversed (model/arrangement/internal/ClipRenders.h):
// nullptr while no project is open.
ClipRenders* getClipRenders (const ProjectContext&);

// What Cut and Copy keep for Paste (model/arrangement/internal/Clipboard.h), of the open Edit: empty after another opens.
Clipboard& getClipboard (ProjectContext&);

// The Edit open now, or none: ProjectSession's (and tests'). Nothing of the previous Edit stays selected, and the
// clipboard is empty. Then tells the listeners.
void setEdit (ProjectContext&, te::Edit*);
} // namespace phazite

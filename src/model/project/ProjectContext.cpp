// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContext.h"
#include "model/arrangement/internal/SongSelection.h"
#include "model/engine/EngineSetup.h"

namespace phazite
{
ProjectContext::ProjectContext (te::Engine& engine, UIBehaviour* tracktionFocus)
    : focus (tracktionFocus),
      selection (engine),
      songSelection (createSongSelection (selection))
{
    if (focus != nullptr)
        focus->setSelectionManager (&selection);
}

ProjectContext::~ProjectContext()
{
    if (focus != nullptr)
    {
        focus->setFocusedEdit (nullptr);
        focus->setSelectionManager (nullptr);
    }
}

void ProjectContext::setEdit (te::Edit* edit)
{
    if (focus != nullptr)
        focus->setFocusedEdit (nullptr);

    selection.deselectAll();
    selection.edit = nullptr;
    rangeSelection.clear();
    selection.edit = edit;

    if (focus != nullptr)
        focus->setFocusedEdit (edit);

    listeners.call ([] (Listener& l) { l.projectChanged(); });
}

void ProjectContext::addListener (Listener* listener)
{
    listeners.add (listener);
}

void ProjectContext::removeListener (Listener* listener)
{
    listeners.remove (listener);
}
} // namespace phazite

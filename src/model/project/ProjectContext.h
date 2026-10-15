// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project, as commands, panels and dialogs find it: its Edit, what is selected in it, and the time range
// selected across tracks. The app's ProjectSession owns the one ProjectContext and is the only code (besides tests) that
// calls setEdit(). Code that holds the context reads getEdit(); model code given the selection may read
// selection.getEdit(): the same Edit. Tracktion's focused Edit and selection (UIBehaviour, which live MIDI and
// controller mappings use) follow this; Phazite's code never reads them. Message thread only.
#pragma once

#include "model/arrangement/Selection.h"

namespace phazite
{
class UIBehaviour;

class ProjectContext
{
public:
    // Keeps Tracktion's focus (the Engine's UIBehaviour) in step, if given one.
    explicit ProjectContext (te::Engine&, UIBehaviour* tracktionFocus = nullptr);
    ~ProjectContext();

    te::Edit* getEdit() const noexcept { return selection.getEdit(); } // nullptr while no project is open

    te::SelectionManager& getSelection() noexcept { return selection; }
    const te::SelectionManager& getSelection() const noexcept { return selection; }
    RangeSelection& getRangeSelection() noexcept { return rangeSelection; }

    // The Edit open now, or none. Nothing of the previous Edit stays selected.
    void setEdit (te::Edit*);

private:
    UIBehaviour* const focus;
    te::SelectionManager selection;
    RangeSelection rangeSelection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectContext)
};
} // namespace phazite

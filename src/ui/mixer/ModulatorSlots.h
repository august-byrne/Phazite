// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's modulators and macros, as the mixer's strips show them (docs/design.md, section 10; roadmap step 2.3).
#pragma once

#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"

#include <vector>

namespace phazite::ui
{
// Each modulator in a slot: its name (click to open its editor, right-click for more), a meter of its output, and a
// handle to drag onto a knob of its track's devices, which it then modulates; a macro's slot has its knob instead of
// the meter. A button adds a modulator or a macro.
class ModulatorSlots : public juce::Component,
                       public juce::DragAndDropContainer
{
public:
    explicit ModulatorSlots (ProjectContext&);
    ~ModulatorSlots() override;

    // The track whose modulators to show (an empty ID for none; the master's: AutomationModel.h, getMasterTrack()).
    // Shows them anew, so call it again for a channel of another project, even one with the same ID.
    void setTrack (TrackID);

    // Shows the modulators as they are now: new slots if they changed. Call it when the song changes.
    void refresh();

    // Shows the meters and the macros' knobs. Call it from a timer.
    void updateValues();

    // Opens a modulator's editor, over its slot.
    void openEditor (ModulatorID);

    int getPreferredHeight() const;
    void resized() override;

    struct Slot;

private:
    ProjectContext& context;
    TrackID track;
    juce::OwnedArray<Slot> slots;
    juce::TextButton addButton { "+ Modulator" };
    std::vector<ModulatorID> shown;

    void rebuild (const std::vector<ModulatorInfo>&);
    void showAddMenu();
    void showSlotMenu (const ModulatorInfo&);
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's modulators and macros, as the Devices view's column shows them (docs/design.md, section 10; roadmap step 2.3).
#pragma once

#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"

#include <vector>

namespace phazite::ui
{
// Each modulator as a chip: a grip to drag onto a knob of its track's devices, which it then modulates; its name (click to
// open its editor, right-click for more); and a meter of its output, faint while it is off. A macro's chip has its knob
// instead of the meter. A button adds a modulator or a macro. A modulator's editor pops out over the window from its chip,
// on the side away from the dock where there is room, so that the knobs it moves stay in view, one at a time: a click
// anywhere else or Escape closes it, as does a click on its chip.
class ModulatorSlots : public juce::Component,
                       public juce::DragAndDropContainer
{
public:
    explicit ModulatorSlots (ProjectContext&);
    ~ModulatorSlots() override;

    // The track whose modulators to show (an empty ID for none; the master's: AutomationModel.h, getMasterTrack()).
    // Shows them anew, so call it again for a channel of another project, even one with the same ID.
    void setTrack (TrackID);

    // Shows the modulators as they are now: new chips if they changed, and the editor of one that is gone closed. Call it
    // when the song changes.
    void refresh();

    // Shows the meters and the macros' knobs. Call it from a timer.
    void updateValues();

    // Opens a modulator's editor beside its chip, or closes it if it is open; and the one open (for tests).
    void openEditor (ModulatorID);
    juce::Component* getOpenEditor() const;

    int getPreferredHeight() const;
    void resized() override;

    // While one of its modulators is dragged, the knobs it could modulate light up (Knob::showModulatorTargets()).
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails&) override;
    void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;

    struct Slot;

private:
    ProjectContext& context;
    TrackID track;
    juce::OwnedArray<Slot> slots;
    juce::TextButton addButton { "+ Modulator" };
    std::vector<ModulatorID> shown;
    juce::Component::SafePointer<juce::CallOutBox> openBox; // the editor's, while it shows
    ModulatorID openModulator;                              // whose it is

    void rebuild (const std::vector<ModulatorInfo>&);
    void closeEditor();
    void showOpenEditor(); // lights the chip whose editor shows
    void showAddMenu();
    void showSlotMenu (const ModulatorInfo&);
};
} // namespace phazite::ui

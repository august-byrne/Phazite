// SPDX-License-Identifier: AGPL-3.0-or-later
// A container's window (docs/design.md, section 9; roadmap step 2.4): its name, its eight macros and its devices in a
// row. A macro is a knob, with a name that a double-click renames and a grip to drag onto a knob of one of the
// container's devices, which it then turns too (right-click a knob for the same, under Modulate). The devices are slots
// as the mixer shows them, with a button that adds one. The app's device windows show it for a container
// (app/PluginWindow.h).
#pragma once

#include "DeviceSlots.h"
#include "model/project/ProjectContext.h"

namespace phazite::ui
{
class ContainerEditor : public juce::Component,
                        public juce::DragAndDropContainer,
                        private ProjectContext::Listener,
                        private juce::Timer
{
public:
    ContainerEditor (ProjectContext&, CommandBridge&, DeviceID container);
    ~ContainerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // For tests.
    juce::Label& getNameLabel() noexcept { return name; }
    int getNumMacros() const noexcept { return macros.size(); }
    Knob& getMacroKnob (int index);
    juce::Label& getMacroName (int index);
    juce::Component& getMacroGrip (int index);
    DeviceSlots& getDevices() noexcept { return devices; }

private:
    struct Macro;

    ProjectContext& context;
    const DeviceID container;
    juce::Label name;
    juce::OwnedArray<Macro> macros;
    DeviceSlots devices;

    void showNames();
    void updateSize();
    void projectChanged() override {} // the window closes with its container
    void songChanged (const SongChanges&) override;
    void timerCallback() override; // the knobs' values
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// The editors of Phazite's own devices (roadmap step 1.6), which device windows (src/app/PluginWindow.h) show: a
// device's own editor, or until it has one, a knob for each of its parameters. They work through the model's
// DeviceHandle and ParameterHandles, so the model owns every undo step and the editors reach the song by IDs alone.
#pragma once

#include "model/base/IDs.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
// The editor for one of Phazite's devices, or nullptr if the handle has none.
std::unique_ptr<juce::Component> createDeviceEditor (const DeviceHandle&);

// The same for a device of the open project, by its ID, as the Devices view and a device's window show it: given the
// project, the editors that choose what the device's sidechain listens to have their picker (SidechainMenus.h).
std::unique_ptr<juce::Component> createDeviceEditor (ProjectContext&, DeviceID);

// An editor that also fits a shorter space than the size it gives itself, as the Devices view's panels are (a graph
// shrinks, or parts show one at a time under tabs): the least height it needs, and how wide it is at a height; and how tall
// it is at a width, everything showing (narrower, its parts wrap), and the least width it wraps to (its widest part), or
// -1 for both if it doesn't wrap. And the least height at a narrower width than its own, where it may show its parts in
// pages (DeviceLayout.h), with their bar.
class FlexibleHeight
{
public:
    virtual ~FlexibleHeight() = default;
    virtual int getMinimumHeight() const = 0;
    virtual int getWidthForHeight (int height) const = 0;
    virtual int getHeightForWidth (int) const { return -1; }
    virtual int getMinimumWidth() const { return -1; }
    virtual int getMinimumHeightAtWidth (int) const { return getMinimumHeight(); }
};

//==============================================================================
// A knob for each of a device's parameters, titled with its name and showing its value in its unit.
class DeviceKnobs : public juce::Component,
                    private juce::Timer
{
public:
    explicit DeviceKnobs (const DeviceHandle&);
    ~DeviceKnobs() override;

    int getNumKnobs() const noexcept { return knobs.size(); }
    Knob& getKnob (int index) { return *knobs[index]; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int columns = 6;

    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    juce::OwnedArray<juce::Label> labels;

    void timerCallback() override; // shows changes from automation, undo and elsewhere
};
} // namespace phazite::ui

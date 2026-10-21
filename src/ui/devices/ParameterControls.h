// SPDX-License-Identifier: AGPL-3.0-or-later
// Controls for some of a device's parameters, as its editor shows them: a knob with its name and value under it for one
// that turns, a list for one that picks a choice, a switch for one that is on or off, in a grid of a given number of
// columns, in the order given. Knobs are compact (compactKnobSize), or large for a device's main controls. A name that
// starts with the part's title drops it ("Filter Cutoff" is "Cutoff" under "Filter"); the control's title, which screen
// readers read, keeps the whole name. The DeviceTicker shows changes made elsewhere (automation, undo).
#pragma once

#include "DeviceTicker.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string_view>
#include <vector>

namespace phazite::ui
{
class ParameterControls : public juce::Component,
                          private DeviceTicker::Display
{
public:
    enum class Size
    {
        compact,
        large
    };

    // The parameters by ID, in a grid of columns (0 for one row of all of them). A name starting with dropFromNames drops it,
    // unless shownNames gives the name to show under each control instead (in the same order).
    ParameterControls (const DeviceHandle&, std::vector<std::string_view> parameterIDs, int columns = 0, Size = Size::compact,
                       const juce::String& dropFromNames = {}, const std::vector<juce::String>& shownNames = {});
    ~ParameterControls() override;

    // The size it takes: its grid's.
    int getPreferredWidth() const noexcept { return preferredWidth; }
    int getPreferredHeight() const noexcept { return preferredHeight; }

    // A parameter's control: a Knob, a juce::ComboBox or a juce::ToggleButton; nullptr if it has none.
    juce::Component* findControl (std::string_view parameterID) const;

    // Shows the values as they are now; the ticker does it.
    void refresh();

    void resized() override;

    static int getCellWidth (Size);
    static int getCellHeight (Size);

private:
    struct Control;

    std::vector<std::unique_ptr<Control>> controls;
    const int columns;
    const Size size;
    int preferredWidth = 0, preferredHeight = 0;

    int getWidthOf (const Control&) const;
    void tick() override { refresh(); }
};

// A parameter's name without the words before it that its part's title has already said.
juce::String getShownName (const juce::String& name, const juce::String& partTitle);
} // namespace phazite::ui

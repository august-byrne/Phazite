// SPDX-License-Identifier: AGPL-3.0-or-later
// Controls for some of a device's parameters, as its editor shows them: a knob with its name and value under it for one
// that turns, a list for one that picks a choice, a switch for one that is on or off, in a grid of a given number of
// columns, in the order given. Knobs are compact (compactKnobSize), or large for a device's main controls. A name that
// starts with the part's title drops it ("Filter Cutoff" is "Cutoff" under "Filter"); the control's title, which screen
// readers read, keeps the whole name. The DisplayTicker shows changes made elsewhere (automation, undo). Given more width
// than its grid, its columns spread; given less (a dense editor's, the multiband's), they narrow alike, each at least as
// wide as its knob, names and values squeezed to fit.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace phazite::ui
{
class ParameterControls : public juce::Component,
                          private DisplayTicker::Display
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

    // Shows other parameters of the device in the same controls, in the same order, each of the same kind as the one it
    // replaces (the drum sampler's pad shown, say): each control then changes its new parameter, titled after it.
    void showParameters (const DeviceHandle&, const std::vector<std::string_view>& parameterIDs);

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

//==============================================================================
// A switch as a button with its text, down while the switch is on, for a row of a part's own controls (an FM operator's
// Fixed, the sampler's Reverse): a click switches it, as one undo step. Titled with the parameter's name; the
// DisplayTicker shows changes made elsewhere (automation, undo).
class ParameterSwitch : public juce::TextButton,
                        private DisplayTicker::Display
{
public:
    ParameterSwitch (const DeviceHandle&, std::string_view parameterID, const juce::String& buttonText, const juce::String& tooltip);
    ~ParameterSwitch() override;

    int getPreferredWidth();

private:
    ToggleAttachment attachment { *this };

    void tick() override { attachment.update(); }
};

//==============================================================================
// A choice's options as buttons, side by side, one above another (a switch's positions) or two to a row, the one chosen lit:
// a click picks one, as one undo step, as a compressor's characters and a multiband's band modes are picked. The
// DisplayTicker shows changes made elsewhere (automation, undo).
class ChoiceButtons : public juce::Component,
                      private DisplayTicker::Display
{
public:
    enum class Direction
    {
        across,
        down,
        twoAcross // in rows of two, the first two over the next two
    };

    // Each option's tooltip, in the choice's order, when given; and the text each button shows, where shorter than its
    // choice's name (a slope's "24" for "24 dB"), which the buttons' titles keep.
    ChoiceButtons (const DeviceHandle&, std::string_view parameterID, Direction = Direction::across,
                   const std::vector<juce::String>& tooltips = {}, const std::vector<juce::String>& shownTexts = {});
    ~ChoiceButtons() override;

    juce::Button& getButton (int index) { return *buttons[index]; }
    int getNumButtons() const noexcept { return buttons.size(); }

    // The size it takes: each button as wide as its text needs, controlHeight tall.
    int getPreferredWidth() const;
    int getPreferredHeight() const;

    // The button of the option chosen now lit; the ticker does it.
    void refresh();

    void resized() override;

private:
    ParameterHandle parameter;
    const Direction direction;
    juce::OwnedArray<juce::TextButton> buttons;

    void tick() override { refresh(); }
};

//==============================================================================
// A choice's options as tabs along the top of an editor, the one chosen in front, as pills like the dock's tabs: a device's
// modes, each showing parts of its own (DeviceEditorBase::setModes(); the compressor's Compress and Duck, as Fruity Limiter's
// tabs switch its own). A click picks one, as one undo step; onChange then says which, as it does for a change made elsewhere
// (an undo, automation), which the DisplayTicker brings in. Each tab is titled with the choice's name and its option's.
class ChoiceTabs : public juce::Component,
                   private DisplayTicker::Display
{
public:
    ChoiceTabs (const DeviceHandle&, std::string_view parameterID, const std::vector<juce::String>& tooltips = {});
    ~ChoiceTabs() override;

    juce::Button& getTab (int index) { return *tabs[index]; }
    int getNumTabs() const noexcept { return tabs.size(); }

    // The option chosen, as it last showed it.
    int getChosen() const noexcept { return chosen; }

    // Called with the option chosen as it changes: by a click, or by a change made elsewhere at the ticker's next refresh.
    std::function<void (int)> onChange;

    static constexpr int height = 24; // as the dock's tabs' pills are tall

    // Its tab in front: the option chosen now. The ticker does it.
    void refresh();

    void resized() override;

private:
    ParameterHandle parameter;
    juce::OwnedArray<juce::Button> tabs;
    int chosen = -1;

    void tick() override { refresh(); }
};
} // namespace phazite::ui

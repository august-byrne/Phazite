// SPDX-License-Identifier: AGPL-3.0-or-later
// What the editors of Phazite's own devices share (docs/design.md, section 9): their parts in a DeviceLayout (columns of
// titled parts that stack, or show one at a time under tabs, so that the editor is as wide at any height), the controls of
// their parameters (ParameterControls), found by ID, and the device's colour (DeviceTint), which their knobs and graphs
// take. A device's mix and its switch are its row's in the Devices view's column, not its editor's: a window of its own shows
// the editor, and the row keeps them. What its sidechain listens to is its editor's to choose (SidechainMenus.h).
#pragma once

#include "DeviceEditors.h"
#include "DeviceLayout.h"
#include "ParameterControls.h"
#include "model/devices/DeviceHandle.h"
#include "ui/theme/DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string_view>
#include <vector>

namespace phazite::ui
{
class DeviceEditorBase : public juce::Component,
                         public FlexibleHeight
{
public:
    explicit DeviceEditorBase (const DeviceHandle&);
    ~DeviceEditorBase() override;

    const DeviceHandle& getDevice() const noexcept { return device; }

    // A parameter's control (a Knob, a juce::ComboBox or a juce::ToggleButton), or nullptr.
    juce::Component* findControl (std::string_view parameterID) const;

    // How its parts are laid out: its mode's, for a device with modes (setModes()).
    DeviceLayout& getLayout() const noexcept { return *layouts[(size_t) shownMode]; }

    // For a device with modes: its modes' tabs along its top (nullptr for one without), and the mode whose parts show.
    ChoiceTabs* getModeTabs() const noexcept { return modeTabs; }
    int getShownMode() const noexcept { return shownMode; }

    int getMinimumHeight() const override;
    int getWidthForHeight (int) const override;
    int getHeightForWidth (int) const override;
    int getMinimumWidth() const override;
    int getMinimumHeightAtWidth (int) const override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override; // another theme: its device's colour in it

    // Its edge, around its parts: at its sides, and at its top and bottom, less, as the dock is short. And the widest it is at
    // first, in a window of its own, its parts wrapped into rows where they are wider side by side.
    static constexpr int margin = Space::s, marginY = Space::xs;
    static constexpr int maximumWindowWidth = 1096;

protected:
    DeviceHandle device;

    // Controls for some parameters, which the editor then places in a part.
    ParameterControls& makeControls (std::vector<std::string_view> parameterIDs, int columns,
                                     ParameterControls::Size = ParameterControls::Size::compact, const juce::String& dropFromNames = {},
                                     const std::vector<juce::String>& shownNames = {});

    // A part of controls, as wide and tall as they are, named as ParameterControls names them, or as given.
    DeviceLayout::Part makeControlsPart (const juce::String& title, std::vector<std::string_view> parameterIDs, int columns,
                                         ParameterControls::Size = ParameterControls::Size::compact,
                                         const std::vector<juce::String>& shownNames = {});

    // A control the editor places itself (a knob in a graph), so that findControl() finds it too.
    void addControl (std::string_view parameterID, juce::Component&);

    // A part's on/off switch, for its title's row (DeviceLayout::Part::titleSwitch): a PowerSwitch attached to the parameter
    // that switches the part, which shows the part's content faint while it is off, as Serum greys a section that is off.
    // findControl() finds it by the parameter's ID.
    juce::Component& makePartSwitch (std::string_view parameterID, juce::Component& partContent);

    // A part showing a picture over its controls: the picture as tall as given (shrinking to its least, growing with what the
    // column has to spare), the controls under it at their own size.
    DeviceLayout::Part makePicturePart (const juce::String& title, juce::Component& picture, int pictureHeight, int pictureMinimumHeight,
                                        std::vector<std::string_view> parameterIDs, int columns,
                                        ParameterControls::Size = ParameterControls::Size::compact,
                                        const std::vector<juce::String>& shownNames = {});

    // Takes a component the editor shows in a part (a graph), keeping it for the editor's life.
    template <typename ComponentType>
    ComponentType& keep (std::unique_ptr<ComponentType> component)
    {
        auto& kept = *component;
        keptComponents.push_back (std::move (component));
        return kept;
    }

    // Lays the parts out: call once, at the end of the editor's constructor, with the columns that list their parts as side
    // tabs (DeviceLayout::setSideTabs()). The editor's size is then its width at the height it wants, at most
    // maximumWindowWidth wide.
    void setParts (std::vector<DeviceLayout::Column>, std::vector<int> sideTabColumns = {});

    // For a device with modes that each show parts of their own (the compressor's Compress and Duck), in place of setParts():
    // each mode's parts, in the order of the choice parameter that picks the mode, under a row of tabs along the top, one a
    // mode (ChoiceTabs, with each mode's tooltip), the mode's parts showing. A click on a tab sets the mode, as one undo step,
    // and shows its parts; the mode set elsewhere (an undo, automation) shows them as the ticker refreshes the tabs. A part
    // may be in several modes (a graph they share). The editor is as wide and as tall as its widest and tallest mode, so that
    // it keeps its size from mode to mode.
    void setModes (std::string_view modeParameterID, std::vector<std::vector<DeviceLayout::Column>> modes,
                   const std::vector<juce::String>& tooltips = {});

    // Called as a mode's parts show, after they are laid out (and once as setModes() shows the first).
    virtual void modeShown (int) {}

private:
    std::vector<std::unique_ptr<DeviceLayout>> layouts; // one, or one for each mode
    std::vector<std::unique_ptr<juce::Component>> keptComponents;
    std::vector<ParameterControls*> controls;
    std::vector<std::pair<std::string_view, juce::Component*>> ownControls;
    ChoiceTabs* modeTabs = nullptr;
    int shownMode = 0;

    void showMode (int);
    juce::Rectangle<int> getPartsArea() const; // inside its margins, under the modes' tabs
    int getTabsHeight() const noexcept;
};
} // namespace phazite::ui

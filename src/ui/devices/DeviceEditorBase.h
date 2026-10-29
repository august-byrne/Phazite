// SPDX-License-Identifier: AGPL-3.0-or-later
// What the editors of Phazite's own devices share (docs/design.md, section 9): their parts in a DeviceLayout (columns of
// titled parts that stack, or show one at a time under tabs, so that the editor is as wide at any height), the controls of
// their parameters (ParameterControls), found by ID, and the device's colour (DeviceTint), which their knobs and graphs
// take. A device's mix, its switch and its sidechain are its card's header's in the Devices view, not its editor's: a window of
// its own shows the editor, and the card stays, folded, with its header.
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
                         public FlexibleHeight,
                         public ShowsPictureOnly
{
public:
    explicit DeviceEditorBase (const DeviceHandle&);
    ~DeviceEditorBase() override;

    // Its picture alone, filling it: its first part's (a graph, a history), unless it says otherwise (getFoldedPicture()).
    void showPictureOnly() override;
    bool isPictureOnly() const noexcept override { return pictureShown != nullptr; }

    const DeviceHandle& getDevice() const noexcept { return device; }

    // A parameter's control (a Knob, a juce::ComboBox or a juce::ToggleButton), or nullptr.
    juce::Component* findControl (std::string_view parameterID) const;

    DeviceLayout& getLayout() const noexcept { return *layout; }

    int getMinimumHeight() const override;
    int getWidthForHeight (int) const override;
    int getHeightForWidth (int) const override;
    int getMinimumWidth() const override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override; // another theme: its device's colour in it

    // Its edge, around its parts.
    static constexpr int margin = Space::s;

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
    // tabs (DeviceLayout::setSideTabs()). The editor's size is then its width at the height it wants.
    void setParts (std::vector<DeviceLayout::Column>, std::vector<int> sideTabColumns = {});

    // What showPictureOnly() shows: the first part's content, or another of the editor's (one part of a part).
    virtual juce::Component* getFoldedPicture() const;

private:
    std::unique_ptr<DeviceLayout> layout;
    juce::Component* pictureShown = nullptr; // alone, filling the editor
    std::vector<std::unique_ptr<juce::Component>> keptComponents;
    std::vector<ParameterControls*> controls;
    std::vector<std::pair<std::string_view, juce::Component*>> ownControls;
};
} // namespace phazite::ui

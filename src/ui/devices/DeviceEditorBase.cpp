// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceEditorBase.h"
#include "DeviceTint.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
DeviceEditorBase::DeviceEditorBase (const DeviceHandle& d)
    : device (d)
{
    if (const auto* info = device.getInfo())
        setTitle (juce::String (info->name.data(), info->name.size()));

    applyDeviceTint (*this, device);
}

DeviceEditorBase::~DeviceEditorBase() = default;

juce::Component* DeviceEditorBase::findControl (std::string_view parameterID) const
{
    for (auto* each : controls)
        if (auto* control = each->findControl (parameterID))
            return control;

    for (const auto& [id, control] : ownControls)
        if (id == parameterID)
            return control;

    return nullptr;
}

void DeviceEditorBase::addControl (std::string_view parameterID, juce::Component& control)
{
    ownControls.emplace_back (parameterID, &control);
}

ParameterControls& DeviceEditorBase::makeControls (std::vector<std::string_view> parameterIDs, int columns, ParameterControls::Size size,
                                                   const juce::String& dropFromNames, const std::vector<juce::String>& shownNames)
{
    auto& made = keep (std::make_unique<ParameterControls> (device, std::move (parameterIDs), columns, size, dropFromNames, shownNames));
    controls.push_back (&made);
    return made;
}

DeviceLayout::Part DeviceEditorBase::makeControlsPart (const juce::String& title, std::vector<std::string_view> parameterIDs, int columns,
                                                       ParameterControls::Size size, const std::vector<juce::String>& shownNames)
{
    auto& made = makeControls (std::move (parameterIDs), columns, size, title, shownNames);
    return { title, &made, made.getPreferredWidth(), made.getPreferredHeight(), made.getPreferredHeight() };
}

namespace
{
    // A picture over controls, as one part's content.
    struct PictureAndControls : public juce::Component
    {
        PictureAndControls (juce::Component& p, ParameterControls& c)
            : picture (p),
              controls (c)
        {
            addAndMakeVisible (picture);
            addAndMakeVisible (controls);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            controls.setBounds (area.removeFromBottom (controls.getPreferredHeight()));
            area.removeFromBottom (Space::xs);
            picture.setBounds (area);
        }

        juce::Component& picture;
        ParameterControls& controls;
    };
} // namespace

DeviceLayout::Part DeviceEditorBase::makePicturePart (const juce::String& title, juce::Component& picture, int pictureHeight,
                                                      int pictureMinimumHeight, std::vector<std::string_view> parameterIDs, int columns,
                                                      ParameterControls::Size size, const std::vector<juce::String>& shownNames)
{
    auto& made = makeControls (std::move (parameterIDs), columns, size, title, shownNames);
    auto& part = keep (std::make_unique<PictureAndControls> (picture, made));
    const auto below = Space::xs + made.getPreferredHeight();
    return { title,
             &part,
             std::max (made.getPreferredWidth(), 3 * ParameterControls::getCellWidth (ParameterControls::Size::compact)),
             pictureHeight + below,
             pictureMinimumHeight + below,
             true };
}

void DeviceEditorBase::setParts (std::vector<DeviceLayout::Column> columns, std::vector<int> sideTabColumns)
{
    layout = std::make_unique<DeviceLayout> (*this, std::move (columns));

    for (const auto column : sideTabColumns)
        layout->setSideTabs (column);

    setSize (getWidthForHeight (0), 2 * margin + layout->getHeightForWidth (layout->getWidth()));
}

int DeviceEditorBase::getMinimumHeight() const
{
    return 2 * margin + (layout != nullptr ? layout->getMinimumHeight() : 0);
}

int DeviceEditorBase::getWidthForHeight (int) const
{
    return 2 * margin + (layout != nullptr ? layout->getWidth() : 0);
}

int DeviceEditorBase::getHeightForWidth (int width) const
{
    return 2 * margin + (layout != nullptr ? layout->getHeightForWidth (width - 2 * margin) : 0);
}

int DeviceEditorBase::getMinimumWidth() const
{
    return 2 * margin + (layout != nullptr ? layout->getMinimumWidth() : 0);
}

void DeviceEditorBase::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);

    if (layout != nullptr)
        layout->paint (g);
}

void DeviceEditorBase::lookAndFeelChanged()
{
    applyDeviceTint (*this, device);
}

void DeviceEditorBase::resized()
{
    if (layout != nullptr)
        layout->layOut (getLocalBounds().reduced (margin));
}
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceEditorBase.h"
#include "DeviceTint.h"
#include "ui/components/PowerSwitch.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite::ui
{
DeviceEditorBase::DeviceEditorBase (const DeviceHandle& d)
    : device (d)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
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
    // A part's switch: its parameter's, which the ticker brings up to date (an undo, automation), the part faint while it is off.
    class PartSwitch : public PowerSwitch,
                       private DisplayTicker::Display
    {
    public:
        PartSwitch (const DeviceHandle& device, std::string_view parameterID, juce::Component& content)
            : PowerSwitch (getParameterName (device, parameterID)),
              DisplayTicker::Display (static_cast<juce::Component&> (*this)),
              part (&content)
        {
            setTooltip ("Switches it on or off: off, it does nothing to the sound, and shows faint");
            attachment.setParameter (device.getParameter (parameterID));
            showState();
        }

        using PowerSwitch::clicked;              // the overload with the keys held, which this leaves as it is
        void clicked() override { showState(); } // as it switches, before the parameter has

    private:
        ToggleAttachment attachment { *this };
        juce::Component::SafePointer<juce::Component> part;

        static juce::String getParameterName (const DeviceHandle& device, std::string_view parameterID)
        {
            if (const auto* info = device.getInfo())
                if (const auto index = devices::findParameter (*info, parameterID); index >= 0)
                    return { info->parameters[(size_t) index].name.data(), info->parameters[(size_t) index].name.size() };

            return "On";
        }

        void showState()
        {
            if (part != nullptr)
                part->setAlpha (getToggleState() ? 1.0f : SwitchTint::partOff);
        }

        void tick() override
        {
            attachment.update();
            showState();
        }
    };

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

juce::Component& DeviceEditorBase::makePartSwitch (std::string_view parameterID, juce::Component& partContent)
{
    auto& made = keep (std::make_unique<PartSwitch> (device, parameterID, partContent));
    addControl (parameterID, made);
    return made;
}

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

    // Its own size, as a window of its own first shows it: its parts side by side, or where that is wider than such a window
    // should be, wrapped into rows that wide.
    const auto width = std::min (getWidthForHeight (0), maximumWindowWidth);
    setSize (width, getHeightForWidth (width));
}

int DeviceEditorBase::getMinimumHeight() const
{
    return 2 * marginY + (layout != nullptr ? layout->getMinimumHeight() : 0);
}

int DeviceEditorBase::getWidthForHeight (int) const
{
    return 2 * margin + (layout != nullptr ? layout->getWidth() : 0);
}

int DeviceEditorBase::getHeightForWidth (int width) const
{
    return 2 * marginY + (layout != nullptr ? layout->getHeightForWidth (width - 2 * margin) : 0);
}

int DeviceEditorBase::getMinimumWidth() const
{
    return 2 * margin + (layout != nullptr ? layout->getMinimumWidth() : 0);
}

int DeviceEditorBase::getMinimumHeightAtWidth (int width) const
{
    return 2 * marginY + (layout != nullptr ? layout->getMinimumHeightForWidth (width - 2 * margin) : 0);
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
        layout->layOut (getLocalBounds().reduced (margin, marginY));
}
} // namespace phazite::ui

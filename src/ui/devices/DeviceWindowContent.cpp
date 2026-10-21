// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceWindowContent.h"
#include "DeviceEditors.h"

#include <algorithm>

namespace phazite::ui
{
DeviceWindowContent::DeviceWindowContent (const DeviceHandle& device)
    : editor (createDeviceEditor (device))
{
    setTitle ("Device");
    view.setScrollBarsShown (true, false);
    view.setViewedComponent (editor.get(), false);
    addAndMakeVisible (view);

    if (editor != nullptr)
    {
        naturalWidth = editor->getWidth();
        setSize (naturalWidth + view.getScrollBarThickness(), editor->getHeight());
    }
}

int DeviceWindowContent::getMinimumWidth() const
{
    auto* flexible = dynamic_cast<const FlexibleHeight*> (editor.get());
    const auto wrapped = flexible != nullptr ? flexible->getMinimumWidth() : -1;
    return (wrapped >= 0 ? wrapped : naturalWidth) + view.getScrollBarThickness();
}

int DeviceWindowContent::getMinimumHeight() const
{
    auto* flexible = dynamic_cast<const FlexibleHeight*> (editor.get());
    return flexible != nullptr ? flexible->getMinimumHeight() : getHeight();
}

void DeviceWindowContent::resized()
{
    view.setBounds (getLocalBounds());

    if (editor == nullptr)
        return;

    const auto width = getWidth() - view.getScrollBarThickness();

    if (auto* flexible = dynamic_cast<const FlexibleHeight*> (editor.get()))
        editor->setSize (width, std::max ({ flexible->getHeightForWidth (width), flexible->getMinimumHeight(), getHeight() }));
}
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices' editors in a window of its own, out of the Devices view (app/PluginWindow.h; roadmap step
// 2.17): as wide as the window, its parts wrapping into rows where that is narrower than they are side by side, and
// scrolling up and down when the window is shorter than they are.
#pragma once

#include "model/devices/DeviceHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace phazite::ui
{
class DeviceWindowContent : public juce::Component
{
public:
    explicit DeviceWindowContent (const DeviceHandle&); // as large as the editor wants, at first

    bool hasEditor() const noexcept { return editor != nullptr; }
    juce::Component* getEditor() const noexcept { return editor.get(); }

    // The least size the editor takes, for the window's limits: as narrow as its widest part where its parts wrap, or else
    // as wide as it was at first; and its shortest layout.
    int getMinimumWidth() const;
    int getMinimumHeight() const;

    void resized() override;

private:
    std::unique_ptr<juce::Component> editor;
    juce::Viewport view;
    int naturalWidth = 0;
};
} // namespace phazite::ui

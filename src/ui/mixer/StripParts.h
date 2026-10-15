// SPDX-License-Identifier: AGPL-3.0-or-later
// What the mixer's strips share (ChannelStrip.h, MasterStrip.h, DeviceSlots.h): the sizes of their parts, a heading
// over a group of controls, a button that tells a right-click from a click, and knobs for a device's parameters.
#pragma once

#include "ui/components/Controls.h"
#include "ui/theme/DesignTokens.h"

#include <functional>
#include <vector>

namespace phazite::ui
{
namespace strip
{
    inline constexpr int slotNameHeight = 22, slotKnobHeight = 30, sendHeight = 26, buttonHeight = 24, headingHeight = 16;
    inline constexpr int keyControlsPerDevice = 2;
    inline constexpr int panKnobSize = 44, meterWidth = 10, headerHeight = 46, colourBarHeight = 4, outputHeight = 18;
    inline constexpr int minFaderHeight = 72, minSlotsHeight = headingHeight + buttonHeight;
    inline constexpr int loudnessHeight = 4 * 16 + buttonHeight + Space::xs; // the master's readout: four lines and Reset
    inline constexpr float relatedOutlineAlpha = 0.5f;                       // a channel the selected one feeds, or is fed by

    // The fader's height: half of what there is, at least the minimum, but leaving the rest of the strip what it needs.
    int getFaderHeight (int available, int forTheRest);

    // What a channel's slots show, so that a timer can tell when it changes.
    template <typename Item>
    std::vector<te::EditItemID> getIDs (const juce::Array<Item*>& items)
    {
        std::vector<te::EditItemID> ids;

        for (auto* item : items)
            ids.push_back (item->itemID);

        return ids;
    }

    // A button that tells a right-click (which opens a menu) from a click.
    struct MenuButton : public juce::TextButton
    {
        using juce::TextButton::clicked;
        std::function<void (bool isMenuClick)> onClicked;

        void clicked (const juce::ModifierKeys& mods) override
        {
            if (onClicked != nullptr)
                onClicked (mods.isPopupMenu());
        }
    };

    // A knob for a parameter of a plug-in, attached to it.
    Knob* addParameterKnob (juce::Component& parent, juce::OwnedArray<Knob>& knobs, juce::OwnedArray<ParameterAttachment>& attachments,
                            te::Plugin& plugin, const juce::String& parameterID);
} // namespace strip

//==============================================================================
// A small heading over a group of a strip's controls, such as "Effects", in the theme's muted text color. Screen
// readers skip it: the group it heads has the same name.
class StripHeading : public juce::Component
{
public:
    explicit StripHeading (const juce::String& text);
    void paint (juce::Graphics&) override;

private:
    juce::String text;
};
} // namespace phazite::ui

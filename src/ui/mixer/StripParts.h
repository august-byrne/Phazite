// SPDX-License-Identifier: AGPL-3.0-or-later
// What the mixer's strips share (ChannelStrip.h, MasterStrip.h, DeviceSlots.h): the sizes of their parts, a heading
// over a group of controls, a button that tells a right-click from a click, knobs for a device's parameters, and a grip
// to drag a modulator by.
#pragma once

#include "model/base/IDs.h"
#include "model/mixer/MixerModel.h"
#include "ui/components/Controls.h"
#include "ui/theme/DesignTokens.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite::ui
{
class CommandBridge;

namespace strip
{
    inline constexpr int slotNameHeight = 22, slotKnobHeight = 30, sendHeight = 26, buttonHeight = 24, headingHeight = 16;
    inline constexpr int keyControlsPerDevice = 2;
    inline constexpr int slotPowerWidth = 32;           // a slot's On switch
    inline constexpr int sidechainStripeWidth = 3;      // a slot whose sidechain listens to a track: that track's colour
    inline constexpr int reductionMeterWidth = 4;       // a slot whose device turns the sound down: how far, along its right edge
    inline constexpr float reductionMeterRange = 24.0f; // in decibels, as the compressor's editor shows it
    inline constexpr int panKnobSize = 44, meterWidth = 10, headerHeight = 46, colourBarHeight = 4, outputHeight = 18;
    inline constexpr int minFaderHeight = 72, minSlotsHeight = headingHeight + buttonHeight;
    inline constexpr int loudnessHeight = 4 * 16 + buttonHeight + Space::xs; // the master's readout: four lines and Reset
    inline constexpr float relatedOutlineAlpha = 0.5f;                       // a channel the selected one feeds, or is fed by

    // The fader's height: half of what there is, at least the minimum, but leaving the rest of the strip what it needs.
    int getFaderHeight (int available, int forTheRest);

    // The devices or sends a channel's slots show (model/mixer/MixerModel.h), to tell when they change.
    template <typename Item>
    std::vector<DeviceID> getIDs (const std::vector<Item>& items)
    {
        std::vector<DeviceID> ids;

        for (const auto& item : items)
            ids.push_back (item.id);

        return ids;
    }

    // A button that tells a right-click (which opens a menu) from a click.
    struct MenuButton : public juce::TextButton
    {
        using juce::TextButton::clicked;
        std::function<void (bool isMenuClick)> onClicked;
        std::function<void (const juce::ModifierKeys&)> onClickedWith; // in place of onClicked, for clicks with Ctrl or Shift

        void clicked (const juce::ModifierKeys& mods) override
        {
            if (onClickedWith != nullptr)
                onClickedWith (mods);
            else if (onClicked != nullptr)
                onClicked (mods.isPopupMenu());
        }
    };

    // The menu of where a channel's sound goes (MixerModel.h: getOutputChoices()): the master, or a bus, which the track
    // then goes into; and a new bus from the selected tracks, this one among them.
    void showOutputMenu (ProjectContext&, CommandBridge&, TrackID, juce::Component& target);

    // "Out: Master", or the bus's name; a plain folder has no sound of its own.
    juce::String describeOutput (const std::optional<Channel>&);

    // A slim bar along a strip's bottom: how much of each block's time its devices take (MixerModel.h: getCpuLoad()), in
    // the warning color above half, with the share in its tooltip.
    class CpuBar : public juce::Component,
                   public juce::SettableTooltipClient
    {
    public:
        static constexpr int height = 4;
        CpuBar();
        void setLoad (float); // 0 to 1
        float getLoad() const noexcept { return load; }
        void paint (juce::Graphics&) override;

    private:
        float load = 0.0f;
    };

    // A knob for a parameter of a device, attached to it; nullptr for a handle of nothing.
    Knob* addParameterKnob (juce::Component& parent, juce::OwnedArray<Knob>& knobs, juce::OwnedArray<ParameterAttachment>& attachments,
                            ParameterHandle);

    // Links a knob to an effect's mix through the model's MixHandle, as ParameterAttachment links one to a parameter: one
    // named undo step per drag or key press.
    class MixAttachment
    {
    public:
        MixAttachment (juce::Slider&, MixHandle);
        void update(); // shows the mix as it is now, unless the knob is being dragged

    private:
        juce::Slider& slider;
        MixHandle handle;
        bool dragging = false;
    };

    // A knob for an effect's mix, 100% at a double-click, attached to it; its tooltip warns when the effect delays its sound.
    Knob* addMixKnob (juce::Component& parent, juce::OwnedArray<Knob>& knobs, std::unique_ptr<MixAttachment>&, ProjectContext&,
                      const DeviceSlot&);

    // Dots to drag a modulator or a macro by, onto a knob it can modulate (ui/components/ModulatorDrag.h), in devices'
    // windows too. Its drag-and-drop container is the nearest around it.
    struct ModulatorGrip : public juce::Component,
                           public juce::SettableTooltipClient
    {
        ModulatorGrip (ModulatorID, const juce::String& tooltip);
        void paint (juce::Graphics&) override;
        void mouseDrag (const juce::MouseEvent&) override;

        const ModulatorID modulator;
        static constexpr int width = 12;
    };
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StripParts.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace strip
{
    int getFaderHeight (int available, int forTheRest)
    {
        return juce::jlimit (minFaderHeight, std::max (minFaderHeight, available - forTheRest), std::max (120, available / 2));
    }

    Knob* addParameterKnob (juce::Component& parent, juce::OwnedArray<Knob>& knobs, juce::OwnedArray<ParameterAttachment>& attachments,
                            ParameterHandle parameter)
    {
        if (! parameter.isValid())
            return nullptr;

        const auto name = parameter.getName();
        auto* knob = knobs.add (new Knob (name, parameter.getDefaultValue().value_or (0.0f)));
        knob->setTooltip (name);
        auto* attachment = attachments.add (new ParameterAttachment (*knob));
        attachment->setParameter (std::move (parameter));
        attachment->showDeviceText();
        parent.addAndMakeVisible (knob);
        return knob;
    }

    //==============================================================================
    MixAttachment::MixAttachment (juce::Slider& s, MixHandle mix)
        : slider (s),
          handle (std::move (mix))
    {
        slider.setRange (0.0, 1.0);
        slider.setDoubleClickReturnValue (true, 1.0);
        slider.onDragStart = [this]
        {
            dragging = true;
            handle.beginGesture();
        };

        slider.onDragEnd = [this]
        {
            dragging = false;
            handle.endGesture();
        };

        slider.onValueChange = [this] { handle.setValue ((float) slider.getValue()); }; // a key press: a step of its own
        update();
    }

    void MixAttachment::update()
    {
        if (handle.isValid() && ! dragging)
            slider.setValue (handle.getValue(), juce::dontSendNotification);
    }

    Knob* addMixKnob (juce::Component& parent, juce::OwnedArray<Knob>& knobs, std::unique_ptr<MixAttachment>& attachment,
                      ProjectContext& context, const DeviceSlot& device)
    {
        auto* knob = knobs.add (new Knob ("Mix", 1.0));
        knob->textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)) + "% wet"; };
        juce::String tooltip ("Mix: how much of the sound is the effect. At 100% it is the effect alone; lower lets the sound "
                              "that comes in through as well. Double-click for 100%");

        // Tracktion adds the dry sound back without waiting for the effect, as FL Studio's mix knob does.
        if (device.latencySeconds > 0.0)
            tooltip << ". This effect delays its sound by " << juce::jmax (1, juce::roundToInt (device.latencySeconds * 1000.0))
                    << " ms, so below 100% the two blur together: use the effect's own mix control if it has one";

        knob->setTooltip (tooltip);
        attachment = std::make_unique<MixAttachment> (*knob, getMix (context, device.id));
        parent.addAndMakeVisible (knob);
        return knob;
    }

    //==============================================================================
    ModulatorGrip::ModulatorGrip (ModulatorID m, const juce::String& tooltip)
        : modulator (m)
    {
        setTitle ("Drag to modulate");
        setTooltip (tooltip);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void ModulatorGrip::paint (juce::Graphics& g)
    {
        constexpr int dots = 3;
        constexpr float dotSize = 2.0f;
        g.setColour (getPalette (*this).textMuted);
        const auto area = getLocalBounds().toFloat();

        for (int row = 0; row < dots; ++row)
            for (int column = 0; column < 2; ++column)
                g.fillEllipse (juce::Rectangle<float> (dotSize, dotSize)
                                   .withCentre ({ area.getCentreX() + ((float) column - 0.5f) * 2.0f * dotSize,
                                                  area.getCentreY() + (float) (row - 1) * 2.0f * dotSize }));
    }

    void ModulatorGrip::mouseDrag (const juce::MouseEvent& e)
    {
        auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this);

        if (container != nullptr && e.getDistanceFromDragStart() > 4 && ! container->isDragAndDropActive())
            container->startDragging (makeModulatorDrag (modulator), this, juce::ScaledImage(), true);
    }
} // namespace strip

StripHeading::StripHeading (const juce::String& headingText)
    : text (headingText)
{
    setAccessible (false);
    setInterceptsMouseClicks (false, false);
}

void StripHeading::paint (juce::Graphics& g)
{
    g.setColour (getPalette (*this).textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (text, getLocalBounds(), juce::Justification::centredLeft, true);
}
} // namespace phazite::ui

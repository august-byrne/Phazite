// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StripParts.h"
#include "model/project/ProjectContext.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace strip
{
    void showOutputMenu (ProjectContext& context, CommandBridge& commands, TrackID track, juce::Component& target)
    {
        juce::PopupMenu menu;
        auto& projectContext = context;
        menu.addSectionHeader ("Where the sound goes");

        for (const auto& choice : getOutputChoices (context, track))
            menu.addItem (choice.bus.isValid() ? choice.name + " (bus)" : choice.name, true, choice.chosen,
                          [&projectContext, track, bus = choice.bus] { setOutput (projectContext, track, bus); });

        // A folder that is a bus, made of the selected tracks: this one, unless it is one of several selected.
        menu.addSeparator();
        menu.addItem ("New Bus from the Selected Tracks",
                      [&projectContext, &commands, track]
                      {
                          auto& selection = projectContext.getSongSelection();

                          if (! selection.isSelected (track))
                              selection.selectOnly (track);

                          commands.invoke ("track.groupIntoFolder");
                      });

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
    }

    juce::String describeOutput (const std::optional<Channel>& channel)
    {
        if (! channel.has_value())
            return {};

        if (channel->kind == ChannelKind::plainFolder)
            return "No sound of its own";

        return "Out: " + (channel->outputBus.isNotEmpty() ? channel->outputBus : juce::String ("Master"));
    }

    //==============================================================================
    CpuBar::CpuBar()
    {
        setTitle ("CPU");
        setTooltip ("How much of the processor's time the track's devices take");
    }

    void CpuBar::setLoad (float newLoad)
    {
        newLoad = juce::jlimit (0.0f, 1.0f, newLoad);

        if (juce::approximatelyEqual (newLoad, load))
            return;

        load = newLoad;
        const auto percent = juce::roundToInt (load * 100.0f);
        setTooltip ("CPU: the track's devices take " + juce::String (percent) + "% of each block's time");
        setDescription (juce::String (percent) + "%");
        repaint();
    }

    void CpuBar::paint (juce::Graphics& g)
    {
        const auto& p = getPalette (*this);
        auto area = getLocalBounds().toFloat();
        g.setColour (p.border);
        g.fillRoundedRectangle (area, area.getHeight() / 2.0f);

        if (load > 0.0f)
        {
            g.setColour (load > 0.5f ? p.warning : p.textMuted);
            g.fillRoundedRectangle (area.withWidth (std::max (area.getHeight(), area.getWidth() * load)), area.getHeight() / 2.0f);
        }
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

        // An effect that delays its sound says by how much: the sound that comes in is delayed as much, so the two stay
        // together below 100% too.
        if (device.latencySeconds > 0.0)
            tooltip << ". This effect delays its sound by " << juce::jmax (1, juce::roundToInt (device.latencySeconds * 1000.0))
                    << " ms, and what comes in with it";

        knob->setTooltip (tooltip);
        attachment = std::make_unique<MixAttachment> (*knob, getMix (context, device.id));
        parent.addAndMakeVisible (knob);
        return knob;
    }

    //==============================================================================
    ModulatorGrip::ModulatorGrip (ModulatorID m, const juce::String& name, const juce::String& tooltip)
        : modulator (m),
          chipText (name)
    {
        setTitle ("Drag to modulate");
        setTooltip (tooltip);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void ModulatorGrip::paint (juce::Graphics& g)
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat();

        // Lit under the pointer and while dragged: this is what drags.
        if (dragging || isMouseOver())
        {
            g.setColour (p.modulation.withAlpha (ModulatorDragTint::gripWash));
            g.fillRoundedRectangle (area, cornerRadius);
        }

        constexpr int dots = 3;
        constexpr float dotSize = 2.0f;
        g.setColour (p.modulation);

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
        {
            // The image's top left at the pointer: the chip in it is a little right of and below it.
            setDragging (true);
            const juce::Point<int> chipAtPointer;
            container->startDragging (makeModulatorDrag (modulator), this, makeDragImage(), true, &chipAtPointer);
        }
    }

    void ModulatorGrip::mouseUp (const juce::MouseEvent&)
    {
        setDragging (false);
    }

    void ModulatorGrip::setDragging (bool isDragging)
    {
        if (dragging == isDragging)
            return;

        dragging = isDragging;
        repaint();

        if (auto* slot = getParentComponent())
            slot->repaint();
    }

    juce::ScaledImage ModulatorGrip::makeDragImage() const
    {
        // Drawn at twice the size, sharp on a screen scaled up, in an image in memory (not one the screen draws with, which
        // takes long to make).
        constexpr float scale = 2.0f;
        const auto& p = getPalette (*this);
        const juce::Font font (juce::FontOptions (TextSize::small, juce::Font::bold));
        const auto chipWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, chipText)) + 2 * Space::s;
        const juce::Rectangle<float> chip ((float) ModulatorDragTint::chipOffsetX, (float) ModulatorDragTint::chipOffsetY,
                                           (float) chipWidth, (float) ModulatorDragTint::chipHeight);

        juce::Image image (juce::Image::ARGB, juce::roundToInt (chip.getRight() * scale), juce::roundToInt (chip.getBottom() * scale), true,
                           juce::SoftwareImageType());
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.setColour (p.modulation.withAlpha (ModulatorDragTint::chipAlpha));
        g.fillRoundedRectangle (chip, cornerRadius);
        g.setColour (getInkFor (p.modulation));
        g.setFont (font);
        g.drawText (chipText, chip, juce::Justification::centred, false);
        return { image, scale };
    }

    void outlineDragSource (juce::Graphics& g, const juce::Component& slot)
    {
        g.setColour (getPalette (slot).modulation);
        g.drawRoundedRectangle (slot.getLocalBounds().toFloat().reduced (ModulatorDragTint::sourceOutline / 2.0f), cornerRadius,
                                ModulatorDragTint::sourceOutline);
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StripParts.h"
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

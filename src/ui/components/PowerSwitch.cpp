// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PowerSwitch.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
PowerSwitch::PowerSwitch (const juce::String& name)
    : juce::Button (name)
{
    setTitle (name);
    setClickingTogglesState (true);
}

void PowerSwitch::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    // As a track's switch is drawn (TrackSwitch::paintButton()): the light in the middle, a wash under the pointer.
    const auto& p = getPalette (*this);
    const auto size = (float) std::min (std::min (getWidth(), getHeight()), SwitchTint::size) * SwitchTint::light;
    const auto light = getLocalBounds().toFloat().withSizeKeepingCentre (size, size);

    if ((isHighlighted || isDown) && isEnabled())
    {
        g.setColour (isDown ? p.background : p.surface);
        g.fillRoundedRectangle (light.expanded (size * 0.5f), cornerRadius);
    }

    if (hasKeyboardFocus (false))
    {
        g.setColour (p.focus);
        g.drawRoundedRectangle (light.expanded (size * 0.5f).reduced (0.5f), cornerRadius, 1.0f);
    }

    const auto alpha = isEnabled() ? 1.0f : SwitchTint::silenced;

    // Off: a hollow ring. On: lit, on an edge that keeps it apart from a tinted background.
    if (! getToggleState())
    {
        g.setColour ((isHighlighted ? p.text : p.textMuted).withMultipliedAlpha (alpha));
        g.drawEllipse (light.reduced (SwitchTint::ring / 2.0f), SwitchTint::ring);
        return;
    }

    g.setColour (p.background.withMultipliedAlpha (alpha));
    g.fillEllipse (light.expanded (SwitchTint::edge));
    g.setColour (p.playing.withMultipliedAlpha (alpha));
    g.fillEllipse (light);
}
} // namespace phazite::ui

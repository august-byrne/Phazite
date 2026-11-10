// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NoticeCard.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int shadowDrop = 2; // the shadow a little lower than the card, as the popups' is
} // namespace

int NoticeCard::getMargin() noexcept
{
    return Popup::shadowRadius;
}

juce::Rectangle<int> NoticeCard::getCardArea() const
{
    return getLocalBounds().reduced (getMargin());
}

int NoticeCard::getWidthIn (int areaWidth) noexcept
{
    return juce::jmax (0, juce::jmin (NoticeTint::width + 2 * getMargin(), areaWidth));
}

void NoticeCard::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto card = getCardArea().toFloat();
    juce::DropShadow (juce::Colours::black.withAlpha (Popup::shadowAlpha), Popup::shadowRadius, { 0, shadowDrop })
        .drawForRectangle (g, getCardArea());
    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (card, cardCornerRadius);
    g.setColour (p.border);
    g.drawRoundedRectangle (card.reduced (0.5f), cardCornerRadius, 1.0f);
}

bool NoticeCard::hitTest (int x, int y)
{
    return getCardArea().contains (x, y);
}
} // namespace phazite::ui

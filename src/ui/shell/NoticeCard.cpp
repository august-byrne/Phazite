// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NoticeCard.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

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

int NoticeCard::getTextHeight (const juce::String& text, const juce::FontOptions& font, int width)
{
    if (text.isEmpty())
        return 0;

    juce::AttributedString attributed (text);
    attributed.setFont (juce::Font (font));
    attributed.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout (attributed, (float) juce::jmax (1, width));
    return (int) std::ceil (layout.getHeight() + font.getHeight() / 4.0f);
}

bool NoticeCard::isUnderPointer (const juce::Component& component)
{
    for (const auto& source : juce::Desktop::getInstance().getMouseSources())
        if (const auto* under = source.getComponentUnderMouse(); under != nullptr && (under == &component || component.isParentOf (under)))
            return true;

    return false;
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

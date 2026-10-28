// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HintBar.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
HintBar::HintBar()
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Hint bar");
    startTimerHz (10);
}

void HintBar::setDefaultHint (std::function<juce::String()> getText)
{
    defaultHint = std::move (getText);
    timerCallback();
}

void HintBar::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromTop (1));

    g.setColour (alert.isNotEmpty() ? p.warning : p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (alert.isNotEmpty() ? alert : hint, getLocalBounds().reduced (Space::s, 0), juce::Justification::centredLeft, true);
}

void HintBar::setAlert (const juce::String& text, std::function<void()> fix)
{
    if (text == alert)
        return;

    alert = text;
    fixAlert = std::move (fix);
    setMouseCursor (alert.isNotEmpty() && fixAlert != nullptr ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    setDescription (alert);

    // Screen readers say it as it comes.
    if (alert.isNotEmpty())
        if (auto* handler = getAccessibilityHandler())
            handler->postAnnouncement (alert, juce::AccessibilityHandler::AnnouncementPriority::high);

    repaint();
}

void HintBar::mouseUp (const juce::MouseEvent& e)
{
    if (alert.isNotEmpty() && fixAlert != nullptr && e.mouseWasClicked())
        fixAlert();
}

void HintBar::timerCallback()
{
    juce::String text;

    // The nearest component under the pointer that explains itself.
    for (auto* c = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse(); c != nullptr && text.isEmpty();
         c = c->getParentComponent())
    {
        if (auto* client = dynamic_cast<juce::TooltipClient*> (c))
            text = client->getTooltip();

        if (text.isEmpty() && c->getDescription().isNotEmpty())
            text = c->getTitle().isNotEmpty() ? c->getTitle() + ": " + c->getDescription() : c->getDescription();
    }

    if (text.isEmpty() && defaultHint != nullptr)
        text = defaultHint();

    if (text != hint)
    {
        hint = text;

        if (alert.isEmpty())
            repaint();
    }
}
} // namespace phazite::ui

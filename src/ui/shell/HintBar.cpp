// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HintBar.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/timeline/TimelineEditor.h"

namespace phazite::ui
{
HintBar::HintBar()
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Hint bar");
    startTimerHz (hintHz);
}

void HintBar::setDefaultHint (std::function<juce::String()> getText)
{
    defaultHint = std::move (getText);
    timerCallback();
}

void HintBar::paint (juce::Graphics& g)
{
    // On the status bar's background, the window's frame.
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

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

void HintBar::showOutcome (const juce::String& text)
{
    outcome = text;
    outcomeUntil = juce::Time::getMillisecondCounterHiRes() + outcomeSeconds * 1000.0;

    if (outcome.isNotEmpty())
        if (auto* handler = getAccessibilityHandler())
            handler->postAnnouncement (outcome, juce::AccessibilityHandler::AnnouncementPriority::medium);

    timerCallback();
}

void HintBar::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;

    if (alert.isNotEmpty())
    {
        if (fixAlert != nullptr)
            fixAlert();
    }
    else if (onClick != nullptr)
    {
        onClick();
    }
}

void HintBar::timerCallback()
{
    juce::String text;
    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();

    // While a drag in an editor moves something, where it is now (TimelineEditor::getDragReadout()), followed more often than
    // the hints, as the drag goes.
    for (auto* c = under; c != nullptr && text.isEmpty(); c = c->getParentComponent())
        if (auto* editor = dynamic_cast<TimelineEditor*> (c))
            text = editor->getDragReadout();

    if (const auto rate = text.isNotEmpty() ? dragReadoutHz : hintHz; getTimerInterval() != 1000 / rate)
        startTimerHz (rate);

    // Else what a command just made, for a while.
    if (outcome.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() >= outcomeUntil)
        outcome.clear();

    if (text.isEmpty())
        text = outcome;

    // The nearest component under the pointer that explains itself.
    for (auto* c = under; c != nullptr && text.isEmpty(); c = c->getParentComponent())
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

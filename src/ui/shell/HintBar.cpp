// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HintBar.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
HintBar::HintBar()
{
    setTitle ("Hint bar");
    startTimerHz (10);
}

void HintBar::setDefaultHint (const juce::String& text)
{
    defaultHint = text;
    timerCallback();
}

void HintBar::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromTop (1));

    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (hint, getLocalBounds().reduced (Space::s, 0), juce::Justification::centredLeft, true);
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

    if (text.isEmpty())
        text = defaultHint;

    if (text != hint)
    {
        hint = text;
        repaint();
    }
}
} // namespace phazite::ui

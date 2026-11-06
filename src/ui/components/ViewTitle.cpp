// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ViewTitle.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
ViewTitle::ViewTitle (const juce::String& viewName)
    : name (viewName)
{
    setTitle (name);
    setInterceptsMouseClicks (false, true); // its actions take theirs
}

void ViewTitle::addAction (juce::Button& action)
{
    actions.emplace_back (&action);
    addAndMakeVisible (action);
    resized();
}

void ViewTitle::paint (juce::Graphics& g)
{
    // As VS Code's Modern UI writes a side bar's title: semibold, in Title Case, so far from the part's edge, clear of the
    // actions.
    auto area = getLocalBounds().withTrimmedLeft (SideBar::titleInset);

    for (const auto& action : actions)
        if (action != nullptr && action->isVisible())
            area.setRight (juce::jmin (area.getRight(), action->getX() - Space::xs));

    g.setColour (getPalette (*this).text);
    g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    g.drawText (name, area, juce::Justification::centredLeft, true);
}

void ViewTitle::resized()
{
    // From the right edge, inset as VS Code's title actions are, each so far from the next.
    auto area = getLocalBounds().withTrimmedRight (SideBar::titlePadding);

    for (auto action = actions.rbegin(); action != actions.rend(); ++action)
    {
        if (*action == nullptr)
            continue;

        (*action)->setBounds (area.removeFromRight (SideBar::actionSize).withSizeKeepingCentre (SideBar::actionSize, SideBar::actionSize));
        area.removeFromRight (SideBar::actionGap);
    }
}
} // namespace phazite::ui

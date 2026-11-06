// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SectionHeader.h"
#include "ui/theme/LookAndFeel.h"

#include <utility>

namespace phazite::ui
{
int getBadgeWidth (const juce::String& text)
{
    const auto font = juce::Font (juce::FontOptions (TextSize::badge, juce::Font::bold));
    return juce::roundToInt (juce::jmax (SideBar::badgeHeight, juce::TextLayout::getStringWidth (font, text) + SideBar::badgeHeight));
}

int drawBadge (juce::Graphics& g, juce::Rectangle<int> rightOf, const juce::String& text, juce::Colour fill, juce::Colour textColour)
{
    // The font set, then measured, as the headings are: measured first, a bold one came out in a serif face.
    g.setFont (juce::FontOptions (TextSize::badge, juce::Font::bold));
    const auto width = juce::roundToInt (
        juce::jmax (SideBar::badgeHeight, juce::TextLayout::getStringWidth (g.getCurrentFont(), text) + SideBar::badgeHeight));
    const auto pill = rightOf.removeFromRight (width).toFloat().withSizeKeepingCentre ((float) width, SideBar::badgeHeight);
    g.setColour (fill);
    g.fillRoundedRectangle (pill, SideBar::badgeHeight / 2.0f);
    g.setColour (textColour);
    g.drawText (text, pill, juce::Justification::centred, false);
    return width;
}

//==============================================================================
// The pointer on the heading or on an action shows the actions, as VS Code shows a group's.
struct SectionHeader::Hover : public juce::MouseListener
{
    explicit Hover (SectionHeader& s)
        : section (s)
    {
    }

    void mouseEnter (const juce::MouseEvent&) override { section.showActions (true); }
    void mouseExit (const juce::MouseEvent&) override { section.showActions (section.isMouseOver (true)); }

    SectionHeader& section;
};

SectionHeader::SectionHeader (const juce::String& name, Style shownAs)
    : juce::Button (name),
      style (shownAs),
      hover (std::make_unique<Hover> (*this))
{
    setTitle (name);
    setClickingTogglesState (true);
    setToggleable (true);
    setToggleState (true, juce::dontSendNotification); // open
    addMouseListener (hover.get(), false);
}

SectionHeader::~SectionHeader()
{
    removeMouseListener (hover.get());

    for (auto& action : actions)
        action->removeMouseListener (hover.get());
}

void SectionHeader::set (const juce::String& newHeading, const juce::String& newDetail, int newCount)
{
    if (heading == newHeading && detail == newDetail && count == newCount)
        return;

    heading = newHeading;
    detail = newDetail;
    count = newCount;
    setDescription (count > 0 ? juce::String (count) : juce::String());
    resized();
    repaint();
}

void SectionHeader::addAction (Icon icon, const juce::String& name, std::function<void()> run)
{
    auto action = std::make_unique<IconButton> (name, icon);
    action->setQuiet (true);
    action->setTooltip (name);
    action->onClick = std::move (run);
    action->addMouseListener (hover.get(), false);
    addChildComponent (*action);
    actions.push_back (std::move (action));
}

void SectionHeader::setActionsAllowed (bool allowed)
{
    actionsAllowed = allowed;
    showActions (isMouseOver (true));
}

int SectionHeader::getPreferredHeight() const noexcept
{
    return style == Style::pane ? SideBar::paneHeaderHeight : ListRow::height;
}

void SectionHeader::setLineAbove (bool shown)
{
    if (std::exchange (lineAbove, shown) != shown)
        repaint();
}

juce::Rectangle<int> SectionHeader::getContentArea() const
{
    return getLocalBounds().withTrimmedRight ((int) ListRow::inset + Space::xs);
}

void SectionHeader::showActions (bool shown)
{
    for (auto& action : actions)
        action->setVisible (shown && actionsAllowed);

    repaint();
}

void SectionHeader::resized()
{
    // From the right, inside the row's inset: the count's badge, then the actions.
    auto area = getContentArea();
    area.removeFromRight (count > 0 ? getBadgeWidth (juce::String (count)) + Space::xs : 0);

    for (auto& action : actions)
        action->setBounds (area.removeFromRight (SideBar::rowActionWidth)
                               .withSizeKeepingCentre (SideBar::rowActionWidth - 2, SideBar::rowActionWidth - 2));
}

void SectionHeader::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    const auto& p = getPalette (*this);
    const auto pane = style == Style::pane;

    // A pane's line above it, inset at each end; the hover wash on a row's box, inset as the list's rows are.
    if (pane && lineAbove)
    {
        g.setColour (p.border);
        g.fillRect (SideBar::paneLineInset, 0, getWidth() - 2 * SideBar::paneLineInset, 1);
    }

    if (isHighlighted || isDown)
    {
        g.setColour (isDown ? p.selected : p.hover);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (ListRow::inset, 0.0f), cornerRadius);
    }

    const auto chevronX = pane ? SideBar::paneChevron : SideBar::rowChevron;
    (getToggleState() ? open : folded)
        .draw (g,
               juce::Rectangle<float> ((float) chevronX, 0.0f, (float) SideBar::chevronSize, (float) getHeight())
                   .withSizeKeepingCentre ((float) SideBar::chevronSize, (float) SideBar::chevronSize),
               p.text);

    auto area = getContentArea().withLeft (pane ? SideBar::paneText : SideBar::rowChevron + SideBar::chevronSize + SideBar::gap);

    if (count > 0)
        area.removeFromRight (drawBadge (g, area, juce::String (count), p.accent, p.textOnAccent) + Space::xs); // VS Code's badge colours

    if (! actions.empty() && actions.front()->isVisible())
        area.removeFromRight ((int) actions.size() * SideBar::rowActionWidth);

    // Its name, semibold on a pane's header, as a row's text on a group's; what it is about after it, muted.
    g.setColour (p.text);
    g.setFont (pane ? juce::FontOptions (TextSize::small, juce::Font::bold) : juce::FontOptions (TextSize::body));
    const auto headingWidth = juce::jmin (area.getWidth(), juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), heading))
                                                               + SideBar::detailGap);
    g.drawText (heading, area.removeFromLeft (headingWidth), juce::Justification::centredLeft, true);
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (detail, area, juce::Justification::centredLeft, true);
}
} // namespace phazite::ui

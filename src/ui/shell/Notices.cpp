// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Notices.h"
#include "ui/components/Controls.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    const auto closeSize = juce::roundToInt (TextSize::title) + Space::xs; // as the device notices' and the lesson card's
    constexpr int timerHz = 4;

    juce::FontOptions getTitleFont()
    {
        return juce::FontOptions (TextSize::body, juce::Font::bold);
    }

    juce::FontOptions getTextFont()
    {
        return juce::FontOptions (TextSize::body);
    }
} // namespace

//==============================================================================
class Notices::Card final : public NoticeCard
{
public:
    Card (Notices& notices, Notice toShow)
        : notice (std::move (toShow)),
          owner (notices),
          icon (notice.isProblem ? Icon::error : Icon::info)
    {
        setTitle (notice.title);
        setDescription (notice.text); // what screen readers say of the notice

        title.setText (notice.title, juce::dontSendNotification);
        title.setFont (getTitleFont());
        message.setText (notice.text, juce::dontSendNotification);
        message.setFont (getTextFont());
        setThemeColour (message, juce::Label::textColourId, &Palette::textMuted);

        for (auto* label : { &title, &message })
        {
            label->setJustificationType (juce::Justification::topLeft);
            label->setMinimumHorizontalScale (1.0f);
            label->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (label);
        }

        // What to do next, which closes it too; and a button that closes it. A click on either leaves the keys where they
        // were (the editor's, for Space to play).
        if (notice.actionName.isNotEmpty())
        {
            actionButton.setButtonText (notice.actionName);
            setPrimary (actionButton, true);
            actionButton.onClick = [this]
            {
                if (notice.action != nullptr)
                    notice.action();

                owner.close (*this);
            };
            addAndMakeVisible (actionButton);
        }

        closeButton.setTooltip ("Closes the notice");
        closeButton.onClick = [this]
        {
            if (notice.closed != nullptr)
                notice.closed();

            owner.close (*this);
        };
        addAndMakeVisible (closeButton);

        for (auto* button : std::initializer_list<juce::Button*> { &actionButton, &closeButton })
            button->setMouseClickGrabsKeyboardFocus (false);
    }

    const Notice notice;
    double shown = 0.0; // seconds it has shown while Phazite was in front and the pointer wasn't over it

    juce::Button* getActionButton() noexcept { return actionButton.isVisible() ? &actionButton : nullptr; }
    juce::Button& getCloseButton() noexcept { return closeButton; }

    int getHeightFor (int width) const
    {
        const auto textWidth = getTextWidth (width);
        auto height = std::max (closeSize, getTextHeight (notice.title, getTitleFont(), textWidth));

        if (notice.text.isNotEmpty())
            height += Space::xs + getTextHeight (notice.text, getTextFont(), textWidth);

        if (actionButton.isVisible())
            height += Space::s + controlHeight;

        return 2 * (getMargin() + Space::m) + height;
    }

    void paint (juce::Graphics& g) override
    {
        NoticeCard::paint (g);

        // Its icon by its first line: news in the focus colour, a problem in the danger colour, as VS Code marks its
        // notifications.
        const auto& p = getPalette (*this);
        const auto first = getCardArea().reduced (Space::m).removeFromTop (closeSize);
        icon.draw (
            g, first.withWidth ((int) NoticeTint::iconSize).toFloat().withSizeKeepingCentre (NoticeTint::iconSize, NoticeTint::iconSize),
            notice.isProblem ? p.danger : p.focus);
    }

    void resized() override
    {
        auto area = getCardArea().reduced (Space::m);

        if (actionButton.isVisible())
        {
            auto footer = area.removeFromBottom (controlHeight);
            const auto buttonWidth = std::max (NoticeTint::buttonWidth, actionButton.getBestWidthForHeight (controlHeight));
            actionButton.setBounds (footer.removeFromRight (std::min (buttonWidth, footer.getWidth())));
            area.removeFromBottom (Space::s);
        }

        area.removeFromLeft ((int) NoticeTint::iconSize + Space::s);
        closeButton.setBounds (area.removeFromRight (closeSize).removeFromTop (closeSize));
        area.removeFromRight (Space::xs);
        const auto textWidth = area.getWidth();
        title.setBounds (area.removeFromTop (std::max (closeSize, getTextHeight (notice.title, getTitleFont(), textWidth))));

        if (notice.text.isNotEmpty())
        {
            area.removeFromTop (Space::xs);
            message.setBounds (area);
        }
    }

private:
    Notices& owner;
    TintedIcon icon;
    juce::Label title, message;
    juce::TextButton actionButton;
    IconButton closeButton { "Close the Notice", Icon::close };

    // The text's width in a card so wide: the card's, but its padding, the icon and the close button.
    static int getTextWidth (int width)
    {
        return width - 2 * (getMargin() + Space::m) - (int) NoticeTint::iconSize - Space::s - closeSize - Space::xs;
    }
};

//==============================================================================
Notices::Notices()
{
    setTitle ("Notices");
    setInterceptsMouseClicks (false, true); // between the cards, what is under them takes clicks
    setVisible (false);
}

Notices::~Notices() = default;

void Notices::show (Notice notice)
{
    // The same notice again takes the old one's place rather than piling up; the latest at the bottom.
    std::erase_if (cards, [&notice] (const std::unique_ptr<Card>& card)
                   { return card->notice.title == notice.title && card->notice.text == notice.text; });
    auto& card = *cards.emplace_back (std::make_unique<Card> (*this, std::move (notice)));
    addAndMakeVisible (card);

    // At most so many: the oldest news goes first, then the oldest problem. One that lasts while a state holds (MIDI learn
    // waiting for a knob) stays, as the state does.
    for (auto shown = getShown(); (int) shown.size() > NoticeTint::mostShown; shown = getShown())
    {
        const auto first = [&shown] (bool problem)
        {
            return std::find_if (shown.begin(), shown.end(),
                                 [problem] (const Card* c) { return c->notice.lasts == nullptr && c->notice.isProblem == problem; });
        };

        auto gone = first (false);

        if (gone == shown.end())
            gone = first (true);

        if (gone == shown.end())
            break;

        std::erase_if (cards, [card = *gone] (const std::unique_ptr<Card>& c) { return c.get() == card; });
    }

    cardsChanged();
}

void Notices::close (const Card& closed)
{
    // Gone from the column at once; deleted once the click that closed it has finished with it.
    for (auto& card : cards)
        if (card.get() == &closed)
            card->setVisible (false);

    cardsChanged();
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<Notices> (this)]
        {
            if (safe != nullptr)
                std::erase_if (safe->cards, [] (const std::unique_ptr<Card>& card) { return ! card->isVisible(); });
        });
}

std::vector<Notices::Card*> Notices::getShown() const
{
    std::vector<Card*> shown;

    for (const auto& card : cards)
        if (card->isVisible())
            shown.push_back (card.get());

    return shown;
}

int Notices::getNumNotices() const
{
    return (int) getShown().size();
}

juce::String Notices::getTitle (int index) const
{
    return getShown()[(size_t) index]->notice.title;
}

juce::String Notices::getText (int index) const
{
    return getShown()[(size_t) index]->notice.text;
}

bool Notices::isProblem (int index) const
{
    return getShown()[(size_t) index]->notice.isProblem;
}

juce::Button* Notices::getActionButton (int index)
{
    return getShown()[(size_t) index]->getActionButton();
}

juce::Button& Notices::getCloseButton (int index)
{
    return getShown()[(size_t) index]->getCloseButton();
}

void Notices::cardsChanged()
{
    const auto any = ! getShown().empty();
    setVisible (any);

    if (any && ! isTimerRunning())
    {
        lastTick = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (timerHz);
    }
    else if (! any)
    {
        stopTimer();
    }

    resized();

    if (onChange != nullptr)
        onChange();
}

int Notices::getHeightFor (int width) const
{
    // One over another, so far apart, their shadows' room overlapping.
    auto height = 0, shownCount = 0;

    for (const auto* card : getShown())
    {
        height += card->getHeightFor (width);
        ++shownCount;
    }

    return shownCount == 0 ? 0 : height - (shownCount - 1) * (2 * NoticeCard::getMargin() - NoticeTint::gap);
}

void Notices::resized()
{
    // From the bottom up, the latest at the bottom.
    auto bottom = getHeight();

    for (auto it = cards.rbegin(); it != cards.rend(); ++it)
    {
        auto& card = **it;

        if (! card.isVisible())
            continue;

        const auto height = card.getHeightFor (getWidth());
        card.setBounds (0, bottom - height, getWidth(), height);
        bottom = card.getY() + 2 * NoticeCard::getMargin() - NoticeTint::gap;
    }
}

void Notices::countShown (double seconds)
{
    // News under the pointer waits; the rest goes once it has shown long enough. A problem stays until it is closed.
    auto closed = false;

    for (auto it = cards.begin(); it != cards.end();)
    {
        auto& card = **it;

        if (card.isVisible() && ! card.notice.isProblem && card.notice.lasts == nullptr && ! NoticeCard::isUnderPointer (card))
            card.shown += seconds;

        // One whose state ended goes as a closed one does, hidden now and deleted after: its own button's click may have ended it.
        if (card.isVisible() && card.notice.lasts != nullptr && ! card.notice.lasts())
        {
            close (card);
            ++it;
            continue;
        }

        if (card.shown >= NoticeTint::shownSeconds)
        {
            it = cards.erase (it);
            closed = true;
        }
        else
        {
            ++it;
        }
    }

    if (closed)
        cardsChanged();
}

void Notices::timerCallback()
{
    // Counted while Phazite is in front, as VS Code counts while its window has the focus.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = (now - lastTick) / 1000.0;
    lastTick = now;

    countShown (juce::Process::isForegroundProcess() ? seconds : 0.0); // a lasting one goes as its state ends, in front or not
}
} // namespace phazite::ui

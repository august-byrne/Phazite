// SPDX-License-Identifier: AGPL-3.0-or-later
// A card over the bottom right corner of the window's centre, where VS Code shows its notifications: the lesson card
// (LessonCard.h) and the notices of audio devices plugged in (DeviceNotices.h) are such cards, in one column. Raised over the
// window as the popups are, on their shadow, in the raised surface's colour, rounded as the panels' cards and edged in the
// border colour as they and the popups are (DesignTokens.h: NoticeTint). Its bounds hold the shadow around the card, where
// clicks pass through to what is under it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class NoticeCard : public juce::Component
{
public:
    // The shadow's room around the card, inside its bounds, on each side; and the card, inside that.
    static int getMargin() noexcept;
    juce::Rectangle<int> getCardArea() const;

    // How wide a card is in an area so wide (as wide as NoticeTint::width, or the area), its shadow's room included.
    static int getWidthIn (int areaWidth) noexcept;

    // How tall text is, wrapped at a width, with a little to spare, as a label breaks lines a little differently.
    static int getTextHeight (const juce::String&, const juce::FontOptions&, int width);

    // Whether the pointer is over a component or what it holds, as the mouse's events last found it: a notice that goes by
    // itself stays while it is. (Component::isMouseOver() asks also where the system's pointer is now, which the tests' events
    // don't move.) JUCE workaround W26 (docs/juce-workarounds.md).
    static bool isUnderPointer (const juce::Component&);

    void paint (juce::Graphics&) override; // the card, under what a card paints over it after
    bool hitTest (int x, int y) override;  // the card alone
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// The lesson that runs (Lessons.h), in a card over the bottom right corner of the window's centre (the owner, 2026-11-01:
// lessons in the hint bar were too easy to miss), a notice card as the others there (NoticeCard.h), at the bottom of their
// column, with a ring around the control its step is about: the lesson's name, how far it has come, the step in large
// text, Back, and Next (or Skip, past a step not done yet), and a button that stops it.
// It covers nothing but its corner, and everything goes on working while it shows: help beside the task, which the user
// does, works better than a tutorial that takes over (Nielsen Norman Group), and so does a mark on the real control. The
// ring pulses a few times as a step starts, then stays still, so that nothing repaints while nothing changes; a few times a
// second the card looks whether the control moved, and whether the song shows the step done.
#pragma once

#include "Lessons.h"
#include "NoticeCard.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace phazite::ui
{
class LessonCard : public NoticeCard,
                   private juce::Timer
{
public:
    // The ring around a step's control, over the whole of the card's parent, which clicks pass through.
    class Ring : public juce::Component
    {
    public:
        Ring();

        // Where the control is in the ring's parent, or nothing; and how far through its pulse the ring is (0 to 1), then 1.
        void show (std::optional<juce::Rectangle<int>> target, float pulse);
        std::optional<juce::Rectangle<int>> getTarget() const noexcept { return target; }

        void paint (juce::Graphics&) override;

    private:
        std::optional<juce::Rectangle<int>> target;
        float pulse = 1.0f;
        juce::Rectangle<int> getPaintedArea() const; // what the ring paints now
    };

    // findTarget: the control a step's target names, if it shows (AppShell::findLessonTarget()).
    LessonCard (Lessons&, std::function<juce::Component*(const juce::String& target)> findTarget);
    ~LessonCard() override;

    Ring& getRing() noexcept { return ring; }

    // Shows the lesson as it is now: its step, or nothing (hidden) while none runs. Lessons::onChange calls it.
    void update();

    // How tall the card is at a width, its text wrapped.
    int getHeightFor (int width) const;

    void paint (juce::Graphics&) override;
    void resized() override;

    // For tests.
    juce::String getText() const { return text.getText(); }
    juce::Button& getBackButton() noexcept { return backButton; }
    juce::Button& getNextButton() noexcept { return nextButton; }
    juce::Button& getCloseButton() noexcept { return closeButton; }

private:
    Lessons& lessons;
    const std::function<juce::Component*(const juce::String&)> findTarget;
    Ring ring;
    juce::Label kind, name, progress, text;
    juce::TextButton backButton { "Back" }, nextButton;
    IconButton closeButton { "Stop the Lesson", Icon::close };
    juce::String shownStep;  // the lesson and step shown, whose ring pulses as it first shows
    double pulseStart = 0.0; // when the step started, in milliseconds (juce::Time::getMillisecondCounterHiRes())

    void updateRing(); // where the step's control is now, and the pulse
    void timerCallback() override;
};
} // namespace phazite::ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite tells without asking anything (the owner's UX audit, 2026-11-12): a song exported, recordings recovered after
// a crash, a file imported, a project open with its plug-ins off; and what went wrong, a save or an autosave that failed.
// Each is a notice card (NoticeCard.h) over the bottom right corner of the window's centre, where VS Code shows its
// notifications, in a column over the audio devices' notices (DeviceNotices.h), with its icon, its title, what it says, a
// button for what to do next where there is one ("Show in Folder"), and one that closes it. Everything goes on working while
// they show: news goes by itself once it has shown for NoticeTint::shownSeconds while Phazite is in front and the pointer
// isn't over it, as VS Code's go, and a problem stays until it is closed. At most NoticeTint::mostShown show, the latest at
// the bottom, the oldest news going first. Until 2026-11-13 each was a dialog that stopped everything until it was answered,
// which NN/g keeps for decisions that can't wait, as Phazite still asks "Save changes?" in one.
#pragma once

#include "NoticeCard.h"

#include <functional>
#include <memory>
#include <vector>

namespace phazite::ui
{
class Notices : public juce::Component,
                private juce::Timer
{
public:
    struct Notice
    {
        juce::String title, text;
        bool isProblem = false;       // what went wrong: it stays until it is closed
        juce::String actionName;      // a button for what to do next, if any
        std::function<void()> action; // which closes the notice too
    };

    Notices();
    ~Notices() override;

    void show (Notice);

    // How tall the column is at a width, the notices one over another (none while none shows); told when one comes or goes,
    // so that the parent places the column again.
    int getHeightFor (int width) const;
    std::function<void()> onChange;

    void resized() override;

    // For tests: the notices that show, the latest last, what each says, its buttons, and the time each has shown, counted on
    // by so many seconds, as the timer counts it while Phazite is in front and the pointer isn't over it.
    int getNumNotices() const;
    juce::String getTitle (int index) const;
    juce::String getText (int index) const;
    bool isProblem (int index) const;
    juce::Button* getActionButton (int index); // nullptr without one
    juce::Button& getCloseButton (int index);
    void countShown (double seconds);

private:
    class Card;

    std::vector<std::unique_ptr<Card>> cards;
    double lastTick = 0.0; // when the timer counted last, in milliseconds (juce::Time::getMillisecondCounterHiRes())

    std::vector<Card*> getShown() const; // closed ones wait to be deleted, hidden
    void close (const Card&);
    void cardsChanged(); // shown or hidden, laid out, the parent told
    void timerCallback() override;
};
} // namespace phazite::ui

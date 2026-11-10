// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio devices plugged in while Phazite runs, each offered in a notice over the bottom right corner of the window's
// centre, where VS Code shows its notifications (the owner, 2026-11-09): "Headphones (USB) is connected. Play through it?",
// Switch, and a button that closes it; for an input, "Record from it?". They are notice cards (NoticeCard.h), in a column
// over the lesson card's place. Everything goes on working while they show, and they can be left alone: each goes by itself
// once it has shown for NoticeTint::shownSeconds while Phazite is in front and the pointer isn't over it, as VS Code's go,
// and as its device goes again or another driver is chosen. At most NoticeTint::mostShown show, the latest at the bottom.
#pragma once

#include "NoticeCard.h"
#include "model/engine/AudioDeviceWatch.h"

#include <functional>
#include <memory>
#include <vector>

namespace phazite::ui
{
class DeviceNotices : public juce::Component,
                      private juce::Timer
{
public:
    explicit DeviceNotices (juce::AudioDeviceManager&);
    ~DeviceNotices() override;

    // How tall the column is at a width, the notices one over another (none while none shows); told when one comes or goes,
    // so that the parent places the column again.
    int getHeightFor (int width) const;
    std::function<void()> onChange;

    void resized() override;

    // For tests: the notices, the latest last, and what each says; and the time each has shown, counted on by so many
    // seconds, as the timer counts it while Phazite is in front and the pointer isn't over it.
    int getNumNotices() const;
    juce::String getText (int index) const;
    juce::Button& getSwitchButton (int index);
    juce::Button& getCloseButton (int index);
    void countShown (double seconds);

private:
    class Notice;

    juce::AudioDeviceManager& devices;
    AudioDeviceWatch watch;
    std::vector<std::unique_ptr<Notice>> notices;
    double lastTick = 0.0; // when the timer counted last, in milliseconds (juce::Time::getMillisecondCounterHiRes())

    void devicesChanged (const std::vector<AudioDeviceName>& plugged, const std::vector<AudioDeviceName>& gone);
    std::vector<Notice*> getShown() const; // closed ones wait to be deleted, hidden
    void close (const Notice&);
    void noticesChanged(); // shown or hidden, laid out, the parent told
    void timerCallback() override;
};
} // namespace phazite::ui

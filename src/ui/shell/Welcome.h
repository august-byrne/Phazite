// SPDX-License-Identifier: AGPL-3.0-or-later
// The welcome window (docs/design.md, section 13): what Phazite asks when it first opens, a page at a time, all of it
// optional. Which app's shortcuts to use ("Coming from...?"), the audio output, with a test sound, and Phazite Sounds,
// the free sounds that aren't part of Phazite's own download. Help > Welcome opens it again, and Help > Phazite Sounds
// at its last page.
#pragma once

#include "services/SoundPack.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
//==============================================================================
// "Coming from...?": FL Studio's keymap for FL Studio's users, Ableton Live's for Live's, Phazite's own for everyone else. A
// choice takes effect at once; Options > Keymap changes it later.
class ComingFromPage : public juce::Component
{
public:
    ComingFromPage (const juce::String& keymapName, std::function<void (const juce::String& keymapName)> chooseKeymap);

    void resized() override;
    void lookAndFeelChanged() override;

    // The choices, in order: FL Studio, Ableton Live, another app, and none. Nothing is chosen until one is clicked, unless
    // FL Studio's or Live's keymap is in use already.
    static constexpr int numChoices = 4;
    juce::ToggleButton& getChoice (int index) noexcept { return *choices[index]; }
    int getChosen() const; // -1 for none

private:
    const std::function<void (const juce::String&)> chooseKeymap;
    juce::Label title, about;
    juce::OwnedArray<juce::ToggleButton> choices;
    juce::OwnedArray<juce::Label> descriptions;
};

//==============================================================================
// The audio output: JUCE's device chooser (made when the page first shows: it scans the devices), a button that plays a
// test sound through the output chosen, and what to do when nothing is heard; and a warning while the output is
// Bluetooth (docs/design.md, section 20), as isOutputBluetooth says.
class SoundPage : public juce::Component,
                  private juce::ChangeListener
{
public:
    SoundPage (juce::AudioDeviceManager&, std::function<bool()> isOutputBluetooth);
    ~SoundPage() override;

    void resized() override;
    void visibilityChanged() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::TextButton& getTestButton() noexcept { return testButton; }
    juce::Component* getDeviceChooser() noexcept { return deviceChooser.get(); }
    bool warnsOfBluetooth() const noexcept { return warning.isVisible(); }

private:
    juce::AudioDeviceManager& deviceManager;
    const std::function<bool()> isOutputBluetooth;
    juce::Label title, about, help, warning;
    juce::TextButton testButton { "Play a Test Sound" };
    std::unique_ptr<juce::Component> deviceChooser;

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the output changed
};

//==============================================================================
// The Phazite Sounds page: what they are, a button that downloads them (or, where Phazite can't download, opens their
// page in a browser), how far the download is, and a button that installs them from a zip downloaded by hand.
class SoundPackPage : public juce::Component,
                      private juce::ChangeListener
{
public:
    explicit SoundPackPage (SoundPack&);
    ~SoundPackPage() override;

    void resized() override;
    void lookAndFeelChanged() override { update(); }

    // For tests.
    juce::TextButton& getMainButton() noexcept { return mainButton; }
    juce::TextButton& getShowButton() noexcept { return showButton; }
    juce::String getStatus() const { return status.getText(); }

private:
    SoundPack& soundPack;
    juce::Label title, about, status;
    juce::TextButton mainButton, fileButton { "Install from a File..." }, showButton;
    double progress = 0.0; // what the progress bar shows; -1 while installing, which it can't measure
    juce::ProgressBar progressBar { progress };
    std::unique_ptr<juce::FileChooser> chooser;

    void update();
    void mainButtonClicked(); // download, try again, cancel, or open the page
    void chooseFile();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { update(); }
};

//==============================================================================
class WelcomePanel : public juce::Component,
                     private juce::ChangeListener
{
public:
    // Its pages, in order.
    enum Page
    {
        comingFrom,
        sound,
        sounds
    };

    static constexpr int numPages = 3;

    // keymapName: the keymap in use. chooseKeymap: switches to another (the main window's). isOutputBluetooth: whether
    // the output is Bluetooth (EngineContext::getAudioOutput()); none, for an output never Bluetooth.
    WelcomePanel (SoundPack&, juce::AudioDeviceManager&, const juce::String& keymapName,
                  std::function<void (const juce::String& keymapName)> chooseKeymap, std::function<bool()> isOutputBluetooth = {});
    ~WelcomePanel() override;

    void showPage (Page);
    Page getPage() const noexcept { return page; }

    // Not Now, or Done, which also closes the dialog window it is in.
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::TextButton& getCloseButton() noexcept { return closeButton; }
    juce::TextButton& getNextButton() noexcept { return nextButton; }
    juce::TextButton& getBackButton() noexcept { return backButton; }
    ComingFromPage& getComingFromPage() noexcept { return comingFromPage; }
    SoundPage& getSoundPage() noexcept { return soundPage; }
    SoundPackPage& getSoundPackPage() noexcept { return soundPackPage; }

private:
    SoundPack& soundPack;
    Page page = comingFrom;
    juce::Label heading, intro, stepLabel;
    ComingFromPage comingFromPage;
    SoundPage soundPage;
    SoundPackPage soundPackPage;
    juce::TextButton backButton { "Back" }, closeButton, nextButton { "Next" };

    void updateButtons();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateButtons(); }
};
} // namespace phazite::ui

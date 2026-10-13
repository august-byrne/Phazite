// SPDX-License-Identifier: AGPL-3.0-or-later
// The welcome window (docs/design.md, section 13): what Phazite offers when it first opens, a page at a time. For now
// its one page offers Phazite Sounds, the free sounds that aren't part of Phazite's own download; later pages will set
// up the keymap, the audio device and the look. Help > Welcome opens it again, and Help > Phazite Sounds at its page.
#pragma once

#include "model/SoundPack.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
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
        sounds
    };

    explicit WelcomePanel (SoundPack&);
    ~WelcomePanel() override;

    void showPage (Page);

    // Not Now, or Done, which also closes the dialog window it is in.
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::TextButton& getCloseButton() noexcept { return closeButton; }
    SoundPackPage& getSoundPackPage() noexcept { return soundPackPage; }

private:
    SoundPack& soundPack;
    juce::Label heading, intro;
    SoundPackPage soundPackPage;
    juce::TextButton closeButton;

    void updateCloseButton();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateCloseButton(); }
};
} // namespace phazite::ui

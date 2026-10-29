// SPDX-License-Identifier: AGPL-3.0-or-later
// The welcome window, which Phazite opens the first time it starts (docs/design.md, section 13): the keymap of the app
// the user comes from, the audio output with a test sound, and Phazite Sounds.
#include "SoundPackTestHelpers.h"
#include "UiTestHelpers.h"
#include "ui/shell/Welcome.h"

namespace
{
// A welcome window, as the main window makes it, that remembers the keymaps it chose.
struct Welcome
{
    explicit Welcome (const phazite::SoundPack::Release& release, const juce::String& keymapName = "Phazite")
        : soundPack (packs.folder, release),
          panel (
              soundPack, deviceManager, keymapName, [this] (const juce::String& name) { chosenKeymaps.add (name); },
              [this] { return bluetooth; })
    {
    }

    bool bluetooth = false; // what the output is, as the panel asks

    phazite::test::TemporaryFolder packs;
    phazite::SoundPack soundPack;
    juce::AudioDeviceManager deviceManager; // never opened
    juce::StringArray chosenKeymaps;
    phazite::ui::WelcomePanel panel;
    phazite::test::TestWindow window { panel, 600, 560 };

    void click (juce::Component& c) { phazite::test::Mouse (c).click (c.getLocalBounds().getCentre()); }
};
} // namespace

TEST_CASE ("The welcome window asks which app the user comes from, and uses that app's keys")
{
    phazite::test::PackRelease source;
    Welcome w (source.release);
    auto& page = w.panel.getComingFromPage();
    CHECK (w.panel.getPage() == phazite::ui::WelcomePanel::comingFrom);
    CHECK (page.isVisible());
    CHECK (page.getChosen() == -1); // Phazite's keys are in use, which two choices give
    CHECK_FALSE (w.panel.getBackButton().isVisible());
    CHECK (w.panel.getCloseButton().getButtonText() == juce::String ("Not Now"));
    phazite::test::checkChildrenApart (page);
    phazite::test::checkChildrenApart (w.panel);

    w.click (page.getChoice (0));
    CHECK (page.getChosen() == 0);
    CHECK (w.chosenKeymaps.joinIntoString (", ") == "FL Studio");

    w.click (page.getChoice (1));
    CHECK (page.getChosen() == 1);
    CHECK (w.chosenKeymaps.joinIntoString (", ") == "FL Studio, Ableton Live");

    w.click (page.getChoice (2));
    CHECK (page.getChosen() == 2);
    CHECK (w.chosenKeymaps.joinIntoString (", ") == "FL Studio, Ableton Live, Phazite");

    // FL Studio's keymap, or Live's, shows as chosen when the window opens again.
    phazite::test::PackRelease again;
    Welcome flStudio (again.release, "FL Studio");
    CHECK (flStudio.panel.getComingFromPage().getChosen() == 0);

    phazite::test::PackRelease third;
    Welcome live (third.release, "Ableton Live");
    CHECK (live.panel.getComingFromPage().getChosen() == 1);
}

TEST_CASE ("The welcome window goes page by page, and its sound page plays a test sound through the chosen output")
{
    phazite::test::PackRelease source;
    Welcome w (source.release);
    CHECK (w.panel.getSoundPage().getDeviceChooser() == nullptr); // not until it shows: it scans the devices

    w.click (w.panel.getNextButton());
    CHECK (w.panel.getPage() == phazite::ui::WelcomePanel::sound);
    CHECK (w.panel.getSoundPage().isVisible());
    CHECK_FALSE (w.panel.getComingFromPage().isVisible());
    CHECK (w.panel.getSoundPage().getDeviceChooser() != nullptr);
    CHECK (w.panel.getBackButton().isVisible());
    CHECK (w.panel.getSoundPage().getTestButton().isVisible());
    phazite::test::checkChildrenApart (w.panel.getSoundPage());
    w.click (w.panel.getSoundPage().getTestButton()); // no device is open here: nothing to hear, and nothing breaks

    w.click (w.panel.getNextButton());
    CHECK (w.panel.getPage() == phazite::ui::WelcomePanel::sounds);
    CHECK (w.panel.getSoundPackPage().isVisible());
    CHECK_FALSE (w.panel.getNextButton().isVisible());
    CHECK (w.panel.getCloseButton().getButtonText() == juce::String ("Not Now"));
    phazite::test::checkChildrenApart (w.panel);

    w.click (w.panel.getBackButton());
    CHECK (w.panel.getPage() == phazite::ui::WelcomePanel::sound);

    bool closed = false;
    w.panel.onClose = [&closed] { closed = true; };
    w.click (w.panel.getCloseButton());
    CHECK (closed);
}

TEST_CASE ("The welcome window offers Phazite Sounds, shows the download, and then says where they went")
{
    phazite::test::PackRelease source;
    Welcome w (source.release);
    w.panel.showPage (phazite::ui::WelcomePanel::sounds);
    bool closed = false;
    w.panel.onClose = [&closed] { closed = true; };

    auto& soundPack = w.soundPack;
    auto& page = w.panel.getSoundPackPage();
    auto& mainButton = page.getMainButton();
    auto& closeButton = w.panel.getCloseButton();
    CHECK (closeButton.getButtonText() == juce::String ("Not Now"));
    phazite::test::checkChildrenApart (page);

    // Download where Phazite can; elsewhere, the page that downloads them in a browser, then the file.
    if (phazite::SoundPack::canDownload())
    {
        CHECK (page.getStatus().endsWith (w.packs.folder.getFullPathName() + ".")); // where they will go
        CHECK (mainButton.getButtonText().startsWith ("Download ("));
        w.click (mainButton);
    }
    else
    {
        CHECK (mainButton.getButtonText() == juce::String ("Open the Download Page"));
        soundPack.installFromFile (source.zip);
    }

    REQUIRE (phazite::test::waitForSoundPack (soundPack));
    // The pack's change message: its thread posts it after it sets the state, so it may not be queued yet.
    CHECK (phazite::test::dispatchMessagesUntil ([&page] { return page.getStatus().startsWith ("Installed in "); }));
    CHECK (soundPack.isInstalled());
    CHECK (page.getStatus().startsWith ("Installed in " + soundPack.getFolder().getFullPathName()));
    CHECK_FALSE (mainButton.isVisible());
    CHECK (page.getShowButton().isVisible());
    CHECK (closeButton.getButtonText() == juce::String ("Done"));
    phazite::test::checkChildrenApart (page);

    w.click (closeButton);
    CHECK (closed);
}

TEST_CASE ("The welcome window says why a download failed, and offers it again")
{
    phazite::test::PackRelease source;
    auto release = source.release;
    release.sha256 = juce::String::repeatedString ("0", 64);
    Welcome w (release);
    w.panel.showPage (phazite::ui::WelcomePanel::sounds);

    auto& page = w.panel.getSoundPackPage();
    w.soundPack.download();
    REQUIRE (phazite::test::waitForSoundPack (w.soundPack));
    CHECK (phazite::test::dispatchMessagesUntil (
        [&page, &w] { return w.soundPack.getError().isNotEmpty() && page.getStatus() == w.soundPack.getError(); }));

    CHECK (page.getStatus() == w.soundPack.getError());
    CHECK (page.getMainButton().getButtonText()
           == juce::String (phazite::SoundPack::canDownload() ? "Try Again" : "Open the Download Page"));
    CHECK (w.panel.getCloseButton().getButtonText() == juce::String ("Not Now"));
}

TEST_CASE ("The welcome window's sound page warns while the output is Bluetooth")
{
    phazite::test::PackRelease source;
    Welcome w (source.release);
    w.click (w.panel.getNextButton());
    CHECK_FALSE (w.panel.getSoundPage().warnsOfBluetooth());

    // The output changes to Bluetooth headphones: the device manager says so.
    w.bluetooth = true;
    w.deviceManager.sendSynchronousChangeMessage();
    CHECK (w.panel.getSoundPage().warnsOfBluetooth());
    phazite::test::checkChildrenApart (w.panel.getSoundPage());

    w.bluetooth = false;
    w.deviceManager.sendSynchronousChangeMessage();
    CHECK_FALSE (w.panel.getSoundPage().warnsOfBluetooth());
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The welcome window, which offers Phazite Sounds when Phazite first opens (docs/design.md, section 13).
#include "SoundPackTestHelpers.h"
#include "UiTestHelpers.h"
#include "ui/Welcome.h"

TEST_CASE ("The welcome window offers Phazite Sounds, shows the download, and then says where they went")
{
    phazite::test::PackRelease source;
    phazite::test::TemporaryFolder packs;
    phazite::SoundPack soundPack (packs.folder, source.release);
    phazite::ui::WelcomePanel panel (soundPack);
    phazite::test::TestWindow window (panel, 560, 460);
    bool closed = false;
    panel.onClose = [&closed] { closed = true; };

    auto& page = panel.getSoundPackPage();
    auto& mainButton = page.getMainButton();
    auto& closeButton = panel.getCloseButton();
    CHECK (closeButton.getButtonText() == juce::String ("Not Now"));
    phazite::test::checkChildrenApart (page);

    // Download where Phazite can; elsewhere, the page that downloads them in a browser, then the file.
    if (phazite::SoundPack::canDownload())
    {
        CHECK (page.getStatus().endsWith (packs.folder.getFullPathName() + ".")); // where they will go
        CHECK (mainButton.getButtonText().startsWith ("Download ("));
        phazite::test::Mouse (mainButton).click (mainButton.getLocalBounds().getCentre());
    }
    else
    {
        CHECK (mainButton.getButtonText() == juce::String ("Open the Download Page"));
        soundPack.installFromFile (source.zip);
    }

    REQUIRE (phazite::test::waitForSoundPack (soundPack));
    phazite::test::dispatchPendingMessages(); // the pack's change message
    CHECK (soundPack.isInstalled());
    CHECK (page.getStatus().startsWith ("Installed in " + soundPack.getFolder().getFullPathName()));
    CHECK_FALSE (mainButton.isVisible());
    CHECK (page.getShowButton().isVisible());
    CHECK (closeButton.getButtonText() == juce::String ("Done"));
    phazite::test::checkChildrenApart (page);

    phazite::test::Mouse (closeButton).click (closeButton.getLocalBounds().getCentre());
    CHECK (closed);
}

TEST_CASE ("The welcome window says why a download failed, and offers it again")
{
    phazite::test::PackRelease source;
    phazite::test::TemporaryFolder packs;
    auto release = source.release;
    release.sha256 = juce::String::repeatedString ("0", 64);
    phazite::SoundPack soundPack (packs.folder, release);
    phazite::ui::WelcomePanel panel (soundPack);
    phazite::test::TestWindow window (panel, 560, 460);

    soundPack.download();
    REQUIRE (phazite::test::waitForSoundPack (soundPack));
    phazite::test::dispatchPendingMessages();

    auto& page = panel.getSoundPackPage();
    CHECK (page.getStatus() == soundPack.getError());
    CHECK (page.getMainButton().getButtonText()
           == juce::String (phazite::SoundPack::canDownload() ? "Try Again" : "Open the Download Page"));
    CHECK (panel.getCloseButton().getButtonText() == juce::String ("Not Now"));
}

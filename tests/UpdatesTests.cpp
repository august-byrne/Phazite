// SPDX-License-Identifier: AGPL-3.0-or-later
// The update check (src/services/Updates.h) and its window (src/ui/shell/UpdatePanel.h): releases in a temporary folder, as
// GitHub would serve them, read through local files' addresses as the sound pack's tests do. Nothing here reaches the
// internet, and no installer runs: the tests' launch only says what would have started.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "services/Updates.h"
#include "ui/shell/UpdatePanel.h"

#include <juce_cryptography/juce_cryptography.h>

namespace
{
using phazite::Updates;
using State = Updates::State;

// Phazite's releases in a temporary folder: the latest one's update file, and its installer, a file of 200 KiB (several
// reads of a download), named and placed as the installer target and GitHub name and place them.
struct Releases
{
    phazite::test::TemporaryFolder temp;
    const juce::File releases = temp.folder.getChildFile ("releases");
    const juce::File installer = writeInstaller (releases.getChildFile ("download/v0.2.0/Phazite-0.2.0-Setup.exe"));
    const juce::File updateFile = releases.getChildFile ("latest/download/phazite-update.json");
    const juce::File downloads = temp.folder.getChildFile ("Downloads");
    const Updates::Source source { .updateFile = juce::URL (updateFile),
                                   .installerPrefix = juce::URL (releases.getChildFile ("download")).toString (false) + "/",
                                   .releasesPage = juce::URL (releases) };

    Releases() { writeUpdateFile ("0.2.0"); }

    static juce::File writeInstaller (const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        juce::MemoryBlock bytes (200 * 1024);
        juce::Random random (42);

        for (size_t i = 0; i < bytes.getSize(); ++i)
            bytes[i] = (char) random.nextInt (256);

        file.replaceWithData (bytes.getData(), bytes.getSize());
        return file;
    }

    // As cmake/WriteUpdateFile.cmake writes it, unless a test says otherwise.
    void writeUpdateFile (const juce::String& version, std::optional<juce::int64> size = {}, std::optional<juce::String> sha256 = {},
                          std::optional<juce::String> address = {})
    {
        updateFile.getParentDirectory().createDirectory();
        updateFile.replaceWithText ("{ \"app\": \"Phazite\", \"version\": \"" + version + "\", \"notes\": \""
                                    + juce::URL (releases).toString (false) + "/tag/v" + version + "\", \"windows\": { \"installer\": \""
                                    + address.value_or (juce::URL (installer).toString (false))
                                    + "\", \"size\": " + juce::String (size.value_or (installer.getSize())) + ", \"sha256\": \""
                                    + sha256.value_or (juce::SHA256 (installer).toHexString()) + "\" } }");
    }
};

// Waits for a check or a download to end, as the message thread would.
bool waitForUpdates (const Updates& updates)
{
    return phazite::test::runTimersUntil (
        [&updates]
        {
            const auto state = updates.getState();
            return state != State::checking && state != State::downloading;
        },
        20000);
}

// A JSON update file for a version, with an installer's address, size and SHA-256.
juce::String makeUpdateFile (const juce::String& app, const juce::String& version, const juce::String& installer, const juce::String& size,
                             const juce::String& sha256)
{
    return "{ \"app\": \"" + app + "\", \"version\": \"" + version + "\", \"windows\": { \"installer\": \"" + installer
           + "\", \"size\": " + size + ", \"sha256\": \"" + sha256 + "\" } }";
}
} // namespace

TEST_CASE ("Phazite asks its own releases on GitHub for updates, over HTTPS, with installers from those releases only")
{
    const auto releases = Updates::getReleases();
    CHECK (releases.updateFile.toString (false) == "https://github.com/august-byrne/Phazite/releases/latest/download/phazite-update.json");
    CHECK (releases.installerPrefix == "https://github.com/august-byrne/Phazite/releases/download/");
    CHECK (releases.releasesPage.toString (false) == "https://github.com/august-byrne/Phazite/releases");
    CHECK (Updates::getInstallerArguments().contains ("/relaunch=1"));
    CHECK (Updates::getInstallerArguments().contains ("/SILENT"));

    // The app's starts the installer with the system, and is idle until asked: making it touches nothing.
    phazite::Settings settings;
    const Updates updates (settings);
    CHECK (updates.launch != nullptr);
    CHECK (updates.canCheckSource() == Updates::canCheck()); // over HTTPS: not in Linux builds without curl
    CHECK (updates.getState() == State::idle);
    CHECK (updates.getRunningVersion() == phazite::getVersionString());
    CHECK_FALSE (updates.checksAtStart());
}

TEST_CASE ("Versions compare by their numbers, and a text that isn't one comes before every version")
{
    CHECK (Updates::compareVersions ("0.10.0", "0.9.1") > 0);
    CHECK (Updates::compareVersions ("0.9.1", "0.10.0") < 0);
    CHECK (Updates::compareVersions ("0.2", "0.2.0") == 0);
    CHECK (Updates::compareVersions ("1.0.0", "0.99.99") > 0);
    CHECK (Updates::compareVersions ("0.1.0", "0.1.0") == 0);
    CHECK (Updates::compareVersions ("1.2.3.4", "1.2.3") > 0);
    CHECK (Updates::compareVersions ("0.2.0-beta", "0.1.0") < 0); // not a version
    CHECK (Updates::compareVersions ("", "0.0.1") < 0);
    CHECK (Updates::compareVersions ("0.0.1", "v0.0.2") > 0);
    CHECK (Updates::compareVersions ("1..2", "0.1") < 0);
}

TEST_CASE ("Redirects are followed to HTTPS only")
{
    CHECK (Updates::isAllowedRedirect ("https://release-assets.githubusercontent.com/github-production-release-asset/1/2"));
    CHECK_FALSE (Updates::isAllowedRedirect ("http://example.com/Phazite-0.2.0-Setup.exe"));
    CHECK_FALSE (Updates::isAllowedRedirect ("ftp://example.com/Phazite-0.2.0-Setup.exe"));
    CHECK_FALSE (Updates::isAllowedRedirect ("/august-byrne/Phazite/releases/download/v0.2.0/Phazite-0.2.0-Setup.exe"));
    CHECK_FALSE (Updates::isAllowedRedirect ("https://"));
    CHECK_FALSE (Updates::isAllowedRedirect ({}));
}

TEST_CASE ("An update file names a release: its version and page, and on Windows its installer from Phazite's releases, size and SHA-256")
{
    const auto source = Updates::getReleases();
    const juce::String sha256 ("9C73C3BAE7ED48D44112A0F48E66742C00090BDB5BEF71D9D3C056C66E97B732");
    const juce::String installer ("https://github.com/august-byrne/Phazite/releases/download/v0.2.0/Phazite-0.2.0-Setup.exe");
    juce::String error;

    const auto release = Updates::parseUpdateFile (
        "{ \"app\": \"Phazite\", \"version\": \"0.2.0\", \"notes\": \"https://github.com/august-byrne/Phazite/releases/tag/v0.2.0\", "
        "\"windows\": { \"installer\": \""
            + installer + "\", \"size\": 31457280, \"sha256\": \"" + sha256 + "\" } }",
        source, error);
    REQUIRE (release.has_value());
    CHECK (release->version == "0.2.0");
    CHECK (release->notes.toString (false) == "https://github.com/august-byrne/Phazite/releases/tag/v0.2.0");

#if JUCE_WINDOWS
    CHECK (release->installer.toString (false) == installer);
    CHECK (release->size == 31457280);
    CHECK (release->sha256 == sha256.toLowerCase());
#else
    CHECK (release->installer.isEmpty()); // the release's page has the download
#endif

    // Notes from anywhere else are the list of releases.
    const auto elsewhere = Updates::parseUpdateFile (
        "{ \"app\": \"Phazite\", \"version\": \"0.2.0\", \"notes\": \"https://example.com/phazite\", \"windows\": { \"installer\": \""
            + installer + "\", \"size\": 1, \"sha256\": \"" + sha256 + "\" } }",
        source, error);
    REQUIRE (elsewhere.has_value());
    CHECK (elsewhere->notes.toString (false) == source.releasesPage.toString (false));

    // Not one Phazite takes.
    const auto refuses = [&source] (const juce::String& json)
    {
        juce::String why;
        const auto refused = ! Updates::parseUpdateFile (json, source, why).has_value();
        CHECK (why.isNotEmpty());
        return refused;
    };

    CHECK (refuses ("not JSON"));
    CHECK (refuses ("[ 1, 2 ]"));
    CHECK (refuses (makeUpdateFile ("Another App", "0.2.0", installer, "100", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0-beta", installer, "100", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "", installer, "100", sha256)));

#if JUCE_WINDOWS
    CHECK (refuses ("{ \"app\": \"Phazite\", \"version\": \"0.2.0\" }"));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", "https://example.com/Phazite-0.2.0-Setup.exe", "100", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", installer.replace ("https://", "http://"), "100", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", source.installerPrefix, "100", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", source.installerPrefix + "../../evil.exe", "100", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", installer, "0", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", installer, "-5", sha256)));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", installer, "2147483648000", sha256))); // 2 TB
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", installer, "100", sha256.dropLastCharacters (1))));
    CHECK (refuses (makeUpdateFile ("Phazite", "0.2.0", installer, "100", sha256.replaceCharacter ('9', 'g'))));
#endif
}

TEST_CASE ("A check finds a newer release, or says this Phazite is the latest")
{
    Releases releases;
    phazite::Settings settings;

    Updates older (settings, releases.downloads, releases.source, "0.1.0");
    CHECK (older.getState() == State::idle);
    older.check();
    REQUIRE (waitForUpdates (older));
    CHECK (older.getState() == State::available);
    REQUIRE (older.getAvailable().has_value());
    CHECK (older.getAvailable()->version == "0.2.0");
    CHECK (older.getLatestVersion() == "0.2.0");

    Updates same (settings, releases.downloads, releases.source, "0.2.0");
    same.check();
    REQUIRE (waitForUpdates (same));
    CHECK (same.getState() == State::upToDate);
    CHECK_FALSE (same.getAvailable().has_value());

    Updates newer (settings, releases.downloads, releases.source, "0.3.0"); // a build ahead of its releases
    newer.check();
    REQUIRE (waitForUpdates (newer));
    CHECK (newer.getState() == State::upToDate);
    CHECK (newer.getLatestVersion() == "0.2.0");

    // No update file: it says so, and nothing is found.
    releases.updateFile.deleteFile();
    Updates nothing (settings, releases.downloads, releases.source, "0.1.0");
    nothing.check();
    REQUIRE (waitForUpdates (nothing));
    CHECK (nothing.getState() == State::failed);
    CHECK (nothing.getError().startsWith ("There is no "));
    CHECK_FALSE (nothing.getAvailable().has_value());
}

TEST_CASE ("The check as Phazite opens is off until the user turns it on; starting up takes away an earlier update's installer")
{
    Releases releases;
    phazite::Settings settings;
    Updates updates (settings, releases.downloads, releases.source, "0.1.0");
    CHECK_FALSE (updates.checksAtStart());
    CHECK_FALSE (settings.get (phazite::Settings::updateCheck));

    REQUIRE (releases.downloads.createDirectory().wasOk());
    REQUIRE (releases.downloads.getChildFile ("Phazite-0.1.0-Setup.exe").replaceWithText ("left by the last update"));
    updates.startUp();
    CHECK (updates.getState() == State::idle); // nothing asked
    CHECK_FALSE (releases.downloads.getChildFile ("Phazite-0.1.0-Setup.exe").exists());

    updates.setChecksAtStart (true);
    CHECK (settings.get (phazite::Settings::updateCheck));
    updates.startUp();
    REQUIRE (waitForUpdates (updates));
    CHECK (updates.getState() == State::available);
}

#if JUCE_WINDOWS
TEST_CASE ("An update's installer downloads, is checked by its size and SHA-256, and starts again checked, to update Phazite as it closes")
{
    Releases releases;
    phazite::Settings settings;
    Updates updates (settings, releases.downloads, releases.source, "0.1.0");
    juce::File launched;
    juce::String arguments;
    updates.launch = [&launched, &arguments] (const juce::File& file, const juce::String& with)
    {
        launched = file;
        arguments = with;
        return true;
    };

    // Nothing to install before a check found a release, nor before its download.
    CHECK (updates.startInstaller().failed());
    updates.download();
    CHECK (updates.getState() == State::idle);

    updates.check();
    REQUIRE (waitForUpdates (updates));
    REQUIRE (updates.getState() == State::available);
    CHECK (updates.startInstaller().failed());

    updates.download();
    REQUIRE (waitForUpdates (updates));
    REQUIRE (updates.getState() == State::ready);
    CHECK (updates.getProgress() == doctest::Approx (1.0));
    const auto downloaded = updates.getInstallerFile (*updates.getAvailable());
    CHECK (downloaded == releases.downloads.getChildFile ("Phazite-0.2.0-Setup.exe"));
    CHECK (downloaded.hasIdenticalContentTo (releases.installer));
    CHECK_FALSE (downloaded.getSiblingFile (downloaded.getFileName() + ".part").exists());

    const auto started = updates.startInstaller();
    INFO (started.getErrorMessage().toStdString());
    CHECK (started.wasOk());
    CHECK (launched == downloaded);
    CHECK (arguments == Updates::getInstallerArguments());
}

TEST_CASE ("A download that isn't the release's installer is refused, and nothing of it is kept")
{
    Releases releases;
    phazite::Settings settings;
    Updates updates (settings, releases.downloads, releases.source, "0.1.0");
    auto launches = 0;
    updates.launch = [&launches] (const juce::File&, const juce::String&) { return ++launches > 0; };
    const auto onlyLeftovers = [&releases] { return releases.downloads.findChildFiles (juce::File::findFiles, false).isEmpty(); };

    // Another SHA-256: the server sent something else.
    releases.writeUpdateFile ("0.2.0", {}, juce::String::repeatedString ("ab", 32));
    updates.check();
    REQUIRE (waitForUpdates (updates));
    REQUIRE (updates.getState() == State::available);
    updates.download();
    REQUIRE (waitForUpdates (updates));
    CHECK (updates.getState() == State::failed);
    CHECK (updates.getError().contains ("SHA-256"));
    CHECK (onlyLeftovers());
    CHECK (updates.startInstaller().failed());

    // Larger than the release says: stopped at its size.
    releases.writeUpdateFile ("0.2.0", releases.installer.getSize() - 1);
    updates.check();
    REQUIRE (waitForUpdates (updates));
    updates.download();
    REQUIRE (waitForUpdates (updates));
    CHECK (updates.getState() == State::failed);
    CHECK (updates.getError().contains ("larger"));
    CHECK (onlyLeftovers());

    // Shorter: it stopped before it finished.
    releases.writeUpdateFile ("0.2.0", releases.installer.getSize() + 1);
    updates.check();
    REQUIRE (waitForUpdates (updates));
    updates.download();
    REQUIRE (waitForUpdates (updates));
    CHECK (updates.getState() == State::failed);
    CHECK (onlyLeftovers());
    CHECK (launches == 0);
}

TEST_CASE ("An installer changed after it was checked doesn't start, and goes")
{
    Releases releases;
    phazite::Settings settings;
    Updates updates (settings, releases.downloads, releases.source, "0.1.0");
    auto launches = 0;
    updates.launch = [&launches] (const juce::File&, const juce::String&) { return ++launches > 0; };

    updates.check();
    REQUIRE (waitForUpdates (updates));
    updates.download();
    REQUIRE (waitForUpdates (updates));
    REQUIRE (updates.getState() == State::ready);

    const auto downloaded = updates.getInstallerFile (*updates.getAvailable());
    juce::MemoryBlock bytes;
    REQUIRE (downloaded.loadFileAsData (bytes));
    bytes[1000] = (char) (bytes[1000] ^ 1); // one bit, the size the same
    REQUIRE (downloaded.replaceWithData (bytes.getData(), bytes.getSize()));

    const auto started = updates.startInstaller();
    CHECK (started.failed());
    CHECK (started.getErrorMessage().contains ("changed"));
    CHECK (launches == 0);
    CHECK_FALSE (downloaded.exists());
    CHECK (updates.getState() == State::available); // to download again
}

TEST_CASE ("The Updates window says what the check found and takes the next step, up to Install and Restart")
{
    Releases releases;
    phazite::Settings settings;
    Updates updates (settings, releases.downloads, releases.source, "0.1.0");
    updates.launch = [] (const juce::File&, const juce::String&) { return true; };
    auto installs = 0;
    phazite::ui::UpdatePanel panel (updates, [&installs] { ++installs; });
    phazite::test::TestWindow window (panel, phazite::ui::UpdatePanel::width, phazite::ui::UpdatePanel::height);

    CHECK (panel.getStatus().contains ("hasn't looked"));
    CHECK (panel.getActionButton().getButtonText() == "Check Now");
    CHECK (phazite::ui::getUpdateCommandName (updates) == "Check for Updates...");
    CHECK_FALSE (phazite::ui::isUpdateWaiting (updates));

    // Check Now finds the release.
    phazite::test::Mouse mouse (panel);
    mouse.click (panel.getActionButton().getBounds().getCentre());
    REQUIRE (waitForUpdates (updates));
    phazite::test::dispatchPendingMessages();
    CHECK (panel.getStatus().contains ("Phazite 0.2.0 is out"));
    CHECK (panel.getActionButton().getButtonText() == "Download and Install");
    CHECK (panel.getReleaseButton().getButtonText() == "Release Notes");
    CHECK (phazite::ui::getUpdateCommandName (updates) == "Update to Phazite 0.2.0...");
    CHECK (phazite::ui::isUpdateWaiting (updates));

    // Download and Install downloads it and checks it; Install and Restart hands over to the window that quits.
    mouse.pause();
    mouse.click (panel.getActionButton().getBounds().getCentre());
    REQUIRE (waitForUpdates (updates));
    phazite::test::dispatchPendingMessages();
    CHECK (updates.getState() == State::ready);
    CHECK (panel.getStatus().contains ("SHA-256"));
    CHECK_FALSE (panel.showsProgress());
    CHECK (panel.getActionButton().getButtonText() == "Install and Restart");
    mouse.pause();
    mouse.click (panel.getActionButton().getBounds().getCentre());
    CHECK (installs == 1);

    // The switch for a check each time Phazite opens, off until turned on.
    CHECK_FALSE (panel.getCheckAtStartButton().getToggleState());
    mouse.pause();
    mouse.click (panel.getCheckAtStartButton().getBounds().getCentre());
    CHECK (settings.get (phazite::Settings::updateCheck));
}
#endif

TEST_CASE ("The Updates window says when this Phazite is the latest, and when a check failed")
{
    Releases releases;
    phazite::Settings settings;
    Updates updates (settings, releases.downloads, releases.source, "0.2.0");
    phazite::ui::UpdatePanel panel (updates, [] {});
    phazite::test::TestWindow window (panel, phazite::ui::UpdatePanel::width, phazite::ui::UpdatePanel::height);
    CHECK (updates.canCheckSource()); // a local file, in every build
    CHECK (panel.getActionButton().getButtonText() == "Check Now");

    updates.check();
    REQUIRE (waitForUpdates (updates));
    phazite::test::dispatchPendingMessages();
    CHECK (panel.getStatus() == "Phazite 0.2.0 is the latest version.");
    CHECK (panel.getActionButton().getButtonText() == "Check Again");
    CHECK (panel.getReleaseButton().getButtonText() == "All Releases");

    releases.updateFile.replaceWithText ("not an update file");
    updates.check();
    REQUIRE (waitForUpdates (updates));
    phazite::test::dispatchPendingMessages();
    CHECK (panel.getStatus().startsWith ("The latest release's update file isn't JSON"));
    CHECK (panel.getActionButton().getButtonText() == "Try Again");
}

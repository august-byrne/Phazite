// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite Sounds: downloaded only when the user asks, checked against the release Phazite pins, and unpacked into the
// folder of packs, where the browser finds them (docs/design.md, section 11).
#include "SoundPackTestHelpers.h"

using phazite::SoundPack;
using phazite::test::TemporaryFolder;
using phazite::test::waitForSoundPack;

TEST_CASE ("The release of Phazite Sounds that Phazite pins is a GitHub release over HTTPS, with its SHA-256")
{
    const auto release = SoundPack::getPinnedRelease();
    CHECK (release.name == juce::String ("Phazite Sounds"));
    CHECK (release.version >= 1);
    CHECK (release.zip.toString (false).startsWith ("https://github.com/august-byrne/phazite-sounds/releases/download/v"
                                                    + juce::String (release.version) + "/"));
    CHECK (release.page.toString (false).startsWith ("https://github.com/august-byrne/phazite-sounds/releases/"));
    CHECK (release.size > 0);
    CHECK (release.sha256.length() == 64);
    CHECK (release.sha256.containsOnly ("0123456789abcdef"));

    // Documents/Phazite/Packs.
    CHECK (SoundPack::getDefaultPacksFolder().getFileName() == juce::String ("Packs"));
}

TEST_CASE ("Phazite Sounds download from the release's address and install into the folder of packs")
{
    phazite::test::PackRelease source;
    TemporaryFolder packs;
    SoundPack pack (packs.folder, source.release);
    CHECK (pack.getState() == SoundPack::State::notInstalled);
    CHECK_FALSE (pack.isInstalled());

    pack.download();
    REQUIRE (waitForSoundPack (pack));
    CHECK (pack.getError() == juce::String());
    CHECK (pack.getState() == SoundPack::State::installed);
    CHECK (pack.isInstalled());
    CHECK (pack.getFolder() == packs.folder.getChildFile ("Phazite Sounds"));
    CHECK (pack.getFolder().getChildFile ("Drums/Kick.wav").existsAsFile());

    // Nothing else is left: neither the download nor what it was unpacked into.
    juce::StringArray left;

    for (const auto& entry : juce::RangedDirectoryIterator (packs.folder, false, "*", juce::File::findFilesAndDirectories))
        left.add (entry.getFile().getFileName());

    CHECK (left == juce::StringArray { "Phazite Sounds" });

    // The next time Phazite opens, it is installed.
    const SoundPack again (packs.folder, source.release);
    CHECK (again.getState() == SoundPack::State::installed);
    CHECK (again.isInstalled());
}

TEST_CASE ("A download that isn't the release Phazite pins is refused, and nothing is installed")
{
    phazite::test::PackRelease source;
    TemporaryFolder packs;
    auto release = source.release;
    release.sha256 = juce::String::repeatedString ("0", 64); // as if the file had changed on the way
    SoundPack pack (packs.folder, release);

    pack.download();
    REQUIRE (waitForSoundPack (pack));
    CHECK (pack.getState() == SoundPack::State::failed);
    CHECK (pack.getError().contains ("damaged"));
    CHECK_FALSE (pack.isInstalled());
    CHECK_FALSE (pack.getFolder().exists());

    // A missing file is refused too.
    release = source.release;
    release.zip = juce::URL (source.temp.folder.getChildFile ("Missing.zip"));
    SoundPack missing (packs.folder, release);
    missing.download();
    REQUIRE (waitForSoundPack (missing));
    CHECK (missing.getState() == SoundPack::State::failed);
    CHECK_FALSE (missing.isInstalled());
}

TEST_CASE ("Phazite Sounds install from a zip downloaded by hand, if it is the release's own file")
{
    phazite::test::PackRelease source;
    TemporaryFolder packs;
    SoundPack pack (packs.folder, source.release);

    // Another zip is refused, even one with the pack in it.
    phazite::test::writeSound (source.folder.getChildFile ("Drums/Snare.wav"), 0.3);
    const auto other = phazite::test::zipFolder (source.folder, source.temp.folder.getChildFile ("Other.zip"), "Phazite Sounds/");
    pack.installFromFile (other);
    REQUIRE (waitForSoundPack (pack));
    CHECK (pack.getState() == SoundPack::State::failed);
    CHECK (pack.getError().contains ("isn't Phazite Sounds 1"));
    CHECK_FALSE (pack.isInstalled());

    // The release's own file installs.
    pack.installFromFile (source.zip);
    REQUIRE (waitForSoundPack (pack));
    CHECK (pack.getState() == SoundPack::State::installed);
    CHECK (pack.getFolder().getChildFile ("Loops/Break 120 BPM.wav").existsAsFile());
}

TEST_CASE ("A release whose zip doesn't hold the pack, or reaches outside its folder, installs nothing")
{
    TemporaryFolder temp, packs;

    // The right file, but with another pack in it.
    const auto otherPack = phazite::test::writePack (temp.folder.getChildFile ("other"), "Other Sounds");
    const auto wrongZip = phazite::test::zipFolder (otherPack, temp.folder.getChildFile ("Wrong.zip"), "Phazite Sounds/");
    SoundPack wrong (packs.folder, phazite::test::makeRelease (wrongZip));
    wrong.installFromFile (wrongZip);
    REQUIRE (waitForSoundPack (wrong));
    CHECK (wrong.getState() == SoundPack::State::failed);
    CHECK (wrong.getError().contains ("doesn't hold Phazite Sounds 1"));
    CHECK_FALSE (wrong.getFolder().exists());

    // A file in it that would land outside the folder it is unpacked into.
    const auto escapingZip = temp.folder.getChildFile ("Escaping.zip");

    {
        juce::ZipFile::Builder builder;
        builder.addEntry (std::make_unique<juce::MemoryInputStream> ("outside", 7, false), 0, "../outside.txt",
                          juce::Time::getCurrentTime());
        juce::FileOutputStream out (escapingZip);
        REQUIRE (builder.writeToStream (out, nullptr));
    }

    SoundPack escaping (packs.folder, phazite::test::makeRelease (escapingZip));
    escaping.installFromFile (escapingZip);
    REQUIRE (waitForSoundPack (escaping));
    CHECK (escaping.getState() == SoundPack::State::failed);
    CHECK_FALSE (packs.folder.getChildFile ("outside.txt").exists());
    CHECK_FALSE (packs.folder.getChildFile ("Phazite Sounds (installing)").exists());
    CHECK_FALSE (escaping.isInstalled());
}

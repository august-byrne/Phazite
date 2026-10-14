// SPDX-License-Identifier: AGPL-3.0-or-later
// A small Phazite Sounds for tests: a pack's folder, zipped as the release zips it, and a release that pins the zip.
#pragma once

#include "TestHelpers.h"
#include "services/SoundPack.h"

#include <juce_cryptography/juce_cryptography.h>

namespace phazite::test
{
// A pack's folder: two sounds, and the sources.json that names the pack and its version.
inline juce::File writePack (const juce::File& folder, const juce::String& name = "Phazite Sounds", int version = 1)
{
    writeSound (folder.getChildFile ("Drums/Kick.wav"), 0.3);
    writeSound (folder.getChildFile ("Loops/Break 120 BPM.wav"), 4.0);
    folder.getChildFile ("sources.json").replaceWithText ("{ \"pack\": \"" + name + "\", \"version\": " + juce::String (version) + " }");
    return folder;
}

// Zips a folder's files, each under the prefix, as phazite_soundkit --zip does with "Phazite Sounds/".
inline juce::File zipFolder (const juce::File& folder, const juce::File& zip, const juce::String& prefix)
{
    juce::ZipFile::Builder builder;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        builder.addFile (entry.getFile(), 0, prefix + entry.getFile().getRelativePathFrom (folder).replaceCharacter ('\\', '/'));

    zip.deleteFile();
    juce::FileOutputStream out (zip);
    builder.writeToStream (out, nullptr);
    return zip;
}

// The release a zip is: its address (a local file), size and SHA-256.
inline SoundPack::Release makeRelease (const juce::File& zip)
{
    return { .name = "Phazite Sounds",
             .version = 1,
             .zip = juce::URL (zip),
             .page = juce::URL ("https://example.com/phazite-sounds"),
             .size = zip.getSize(),
             .sha256 = juce::SHA256 (zip).toHexString() };
}

// A pack in a zip, as the release has it, and the release that pins it.
struct PackRelease
{
    TemporaryFolder temp;
    const juce::File folder = writePack (temp.folder.getChildFile ("pack"));
    const juce::File zip = zipFolder (folder, temp.folder.getChildFile ("Phazite-Sounds-1.zip"), "Phazite Sounds/");
    const SoundPack::Release release = makeRelease (zip);
};

// Waits for a download or an installation to end, as the message thread would.
inline bool waitForSoundPack (const SoundPack& pack)
{
    return runTimersUntil (
        [&pack]
        {
            const auto state = pack.getState();
            return state != SoundPack::State::downloading && state != SoundPack::State::installing;
        },
        20000);
}
} // namespace phazite::test

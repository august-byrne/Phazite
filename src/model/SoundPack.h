// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite Sounds, the free sounds Phazite offers to download when it first opens (docs/design.md, section 11). They
// aren't part of Phazite's own download: they are a zip of CC0 sounds, released from the phazite-sounds repository and
// pinned here by its address, size and SHA-256. Phazite downloads it only when the user asks, checks it, and unpacks it
// into Documents/Phazite/Packs, where the browser shows it. A zip downloaded in a browser installs the same way.
#pragma once

#include <juce_events/juce_events.h>

namespace phazite
{
class SoundPack : public juce::ChangeBroadcaster
{
public:
    // A release of the pack: what to download, and what it must be.
    struct Release
    {
        juce::String name {}; // the pack's folder, in the zip and in Packs: "Phazite Sounds"
        int version = 0;
        juce::URL zip {};       // where it downloads from
        juce::URL page {};      // the release's page, to download it in a browser
        juce::int64 size = 0;   // the zip's, in bytes
        juce::String sha256 {}; // the zip's, in lowercase hexadecimal
    };

    // The release this version of Phazite installs.
    static Release getPinnedRelease();

    // Documents/Phazite/Packs.
    static juce::File getDefaultPacksFolder();

    // Tests give a folder and a release of their own.
    explicit SoundPack (juce::File packsFolder = getDefaultPacksFolder(), Release = getPinnedRelease());
    ~SoundPack() override;

    const Release& getRelease() const noexcept { return release; }
    juce::String getTitle() const;                                             // "Phazite Sounds 1"
    juce::File getFolder() const { return packs.getChildFile (release.name); } // the pack, once it is installed

    enum class State
    {
        notInstalled,
        downloading,
        installing,
        installed,
        failed // the last attempt: getError() says why
    };

    // A change message follows every change of these, from download() and installFromFile() too.
    State getState() const;
    double getProgress() const; // of a download, from 0 to 1
    juce::String getError() const;

    // Whether the release (or a later one) is in its folder, whatever the last attempt to install it did.
    bool isInstalled() const;

    // Downloads the release and installs it, on a thread of its own. Does nothing while it is busy.
    void download();

    // Installs the release from a zip downloaded by hand, which must be the release's own file.
    void installFromFile (const juce::File& zip);

    // Stops a download; an installation that has begun finishes.
    void cancel();

    // Whether Phazite can download over HTTPS here. Linux builds without curl can't: there, the release's page opens in a
    // browser, and the zip installs from the file.
    static bool canDownload();

private:
    struct Job;

    const juce::File packs;
    const Release release;
    std::unique_ptr<Job> job;

    mutable juce::CriticalSection lock; // guards what follows, which the job's thread changes
    State state = State::notInstalled;
    double progress = 0.0;
    juce::String error;

    void start (const juce::File& zip); // none: download
    void update (State, double newProgress, const juce::String& newError = {});
    bool isRelease (const juce::File& zip) const; // the release's own file, byte for byte
    juce::Result install (const juce::File& zip); // on the job's thread
    int findInstalledVersion() const;             // 0 if none
};
} // namespace phazite

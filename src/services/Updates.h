// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's update check (docs/design.md, section 22): whether a newer Phazite is out, asked of its releases on GitHub only
// when the user asks (Help > Check for Updates...) or has turned on the check as Phazite opens, which is off until they do.
// A release's update file (phazite-update.json, which CMake's installer target writes next to the installer) names its
// version and its page, and on Windows its installer, with the installer's size and SHA-256. Phazite downloads the
// installer only when asked, from Phazite's releases and over HTTPS only (redirects too), stops at that size, and checks the
// SHA-256 once it has it and again just before it runs it. The installer then updates Phazite once it has closed, and opens
// it again (packaging/windows/Phazite.iss).
#pragma once

#include "model/engine/AppInfo.h"
#include "services/Settings.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <optional>

namespace phazite
{
class Updates : public juce::ChangeBroadcaster
{
public:
    // Where the releases are: the latest one's update file, what every installer's address must start with, and the page that
    // lists the releases.
    struct Source
    {
        juce::URL updateFile {};
        juce::String installerPrefix {};
        juce::URL releasesPage {};
    };

    // Phazite's GitHub releases.
    static Source getReleases();

    // What a release's update file says.
    struct Release
    {
        juce::String version {}; // "0.2.0"
        juce::URL notes {};      // the release's page
        juce::URL installer {};  // Windows' installer: none elsewhere, where the release's page has the download
        juce::int64 size = 0;    // the installer's, in bytes
        juce::String sha256 {};  // the installer's, in lowercase hexadecimal
    };

    // An update file's text as a release, or none, with why: it must be Phazite's, with a version, and on Windows an
    // installer from the source's releases over HTTPS, its size and its SHA-256.
    static std::optional<Release> parseUpdateFile (const juce::String& json, const Source&, juce::String& error);

    // Versions by their numbers ("0.10.0" comes after "0.9.1"; "0.2" is "0.2.0"): below 0 if a comes first, 0 if they are
    // the same, above 0 if a comes after b. A text that isn't a version comes before every version.
    static int compareVersions (const juce::String& a, const juce::String& b);

    // Whether a redirect may be followed: to HTTPS only, never from HTTPS to HTTP.
    static bool isAllowedRedirect (const juce::String& location);

    enum class State
    {
        idle,        // not asked yet
        checking,    // reading the latest release's update file
        upToDate,    // the latest release is this version, or an older one
        available,   // a newer release is out: getAvailable()
        downloading, // its installer: getProgress()
        ready,       // its installer is downloaded and checked: startInstaller()
        failed       // the last check or download: getError() says why
    };

    // Downloads go into a folder of their own; tests give a folder, a source and a version of their own.
    explicit Updates (Settings&, juce::File downloadsFolder = getDefaultDownloadsFolder(), Source = getReleases(),
                      juce::String runningVersion = getVersionString());
    ~Updates() override;

    // "Phazite Updates" in the system's temporary folder.
    static juce::File getDefaultDownloadsFolder();

    // Whether Phazite checks as it opens (Settings::updateCheck): off until the user turns it on.
    bool checksAtStart() const;
    void setChecksAtStart (bool);

    // As Phazite opens: takes away installers left by an earlier update, and checks, if the user turned that on.
    void startUp();

    // Reads the latest release's update file, on a thread of its own. Nothing while a check or a download is under way.
    void check();

    // Downloads the newer release's installer, on a thread of its own, and checks it: only while one is available.
    void download();

    // Stops a check or a download.
    void cancel();

    // A change message follows every change of these.
    State getState() const;
    double getProgress() const; // of a download, from 0 to 1
    juce::String getError() const;
    std::optional<Release> getAvailable() const; // the newer release, from available on
    juce::String getLatestVersion() const;       // the latest release's version, once a check found it

    const juce::String& getRunningVersion() const noexcept { return runningVersion; }
    const Source& getSource() const noexcept { return source; }

    // Where a release's installer is downloaded to.
    juce::File getInstallerFile (const Release&) const;

    // Starts the downloaded installer, after checking it again, to update Phazite once it has closed and open it again; the
    // caller then quits. Fails, saying why, if there is none ready or it changed since it was checked.
    juce::Result startInstaller();

    // How an installer is started, with its arguments: the system's, unless tests set their own.
    std::function<bool (const juce::File&, const juce::String& arguments)> launch =
        [] (const juce::File& installer, const juce::String& arguments) { return installer.startAsProcess (arguments); };

    // The installer's arguments for an update: silent, with Phazite opened again once it is installed.
    static juce::String getInstallerArguments();

    // Whether this build can check over HTTPS. Linux builds without curl can't (JUCE's sockets have no TLS): there, the
    // releases page opens in a browser.
    static bool canCheck();

private:
    struct Job;

    Settings& settings;
    const juce::File downloads;
    const Source source;
    const juce::String runningVersion;
    std::unique_ptr<Job> job;

    mutable juce::CriticalSection lock; // guards what follows, which the job's thread changes
    State state = State::idle;
    double progress = 0.0;
    juce::String error, latestVersion;
    std::optional<Release> available;

    void start (bool downloading);
    void update (State, double newProgress, const juce::String& newError = {});
    bool isInstaller (const juce::File&, const Release&) const; // the release's own installer, byte for byte

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Updates)
};
} // namespace phazite

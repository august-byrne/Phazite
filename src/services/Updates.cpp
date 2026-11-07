// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Updates.h"

#include <juce_cryptography/juce_cryptography.h>

namespace phazite
{
namespace
{
    constexpr int maxUpdateFileSize = 64 * 1024; // an update file is a few hundred bytes
    constexpr int maxRedirects = 5;              // GitHub's downloads take one
    constexpr int connectionTimeout = 20000;     // milliseconds

    // What an installer must be, which only Windows' builds download (the release's page has the others' downloads).
#if JUCE_WINDOWS
    constexpr juce::int64 maxInstallerSize = 1024LL * 1024LL * 1024; // Phazite's is tens of megabytes

    bool isSha256 (const juce::String& text)
    {
        return text.length() == 64 && text.containsOnly ("0123456789abcdef");
    }
#endif

    // A version's numbers, or none if it isn't a version: one to four numbers, with dots between them.
    std::optional<std::vector<int>> getNumbers (const juce::String& version)
    {
        juce::StringArray parts;
        parts.addTokens (version, ".", {});

        if (parts.isEmpty() || parts.size() > 4)
            return std::nullopt;

        std::vector<int> numbers;

        for (const auto& part : parts)
        {
            if (part.isEmpty() || part.length() > 6 || ! part.containsOnly ("0123456789"))
                return std::nullopt;

            numbers.push_back (part.getIntValue());
        }

        return numbers;
    }

    bool isRedirect (int status)
    {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }
} // namespace

//==============================================================================
// Reads the latest release's update file, or downloads a release's installer and checks it.
struct Updates::Job : public juce::Thread
{
    Job (Updates& u, std::optional<Release> toDownload)
        : juce::Thread ("Phazite updates"),
          owner (u),
          release (std::move (toDownload))
    {
    }

    ~Job() override
    {
        cancel();
        stopThread (20000);
    }

    void cancel()
    {
        signalThreadShouldExit();
        const juce::ScopedLock sl (streamLock);

        if (stream != nullptr)
            stream->cancel(); // stops a read that waits for the network
    }

    void run() override
    {
        if (release.has_value())
            downloadInstaller (*release);
        else
            checkLatest();
    }

    void checkLatest()
    {
        juce::MemoryOutputStream text;
        const auto read = fetch (owner.source.updateFile, maxUpdateFileSize, 0, text);

        if (threadShouldExit())
        {
            owner.update (State::idle, 0.0); // cancelled
            return;
        }

        if (read.failed())
        {
            owner.update (State::failed, 0.0, read.getErrorMessage());
            return;
        }

        juce::String why;
        const auto latest = parseUpdateFile (text.toUTF8(), owner.source, why);

        if (! latest.has_value())
        {
            owner.update (State::failed, 0.0, "The latest release's update file " + why);
            return;
        }

        const auto newer = compareVersions (latest->version, owner.runningVersion) > 0;

        {
            const juce::ScopedLock sl (owner.lock);
            owner.latestVersion = latest->version;
            owner.available = newer ? latest : std::nullopt;
        }

        owner.update (newer ? State::available : State::upToDate, 0.0);
    }

    void downloadInstaller (const Release& r)
    {
        const auto target = owner.getInstallerFile (r);
        const auto partial = target.getSiblingFile (target.getFileName() + ".part");

        if (const auto made = owner.downloads.createDirectory(); made.failed())
        {
            owner.update (State::failed, 0.0, "Phazite couldn't make the folder " + owner.downloads.getFullPathName() + ".");
            return;
        }

        target.deleteFile();
        partial.deleteFile();
        auto downloaded = juce::Result::ok();

        {
            juce::FileOutputStream out (partial);

            if (! out.openedOk())
            {
                owner.update (State::failed, 0.0, "Phazite couldn't write to " + owner.downloads.getFullPathName() + ".");
                return;
            }

            downloaded = fetch (r.installer, r.size, r.size, out);
            out.flush();

            if (downloaded.wasOk() && ! out.getStatus().wasOk())
                downloaded = juce::Result::fail ("Phazite couldn't write to " + owner.downloads.getFullPathName() + ".");
        }

        if (threadShouldExit())
        {
            partial.deleteFile();
            owner.update (State::available, 0.0); // cancelled: the release is still there to download
            return;
        }

        if (downloaded.wasOk() && partial.getSize() != r.size)
            downloaded = juce::Result::fail ("The download stopped before it finished. Check the internet connection and try again.");

        // Byte for byte the installer the release names, or nothing.
        if (downloaded.wasOk() && ! owner.isInstaller (partial, r))
            downloaded = juce::Result::fail ("The download isn't the installer the release names: its SHA-256 differs, so Phazite deleted "
                                             "it. Try again, or download it from the release's page.");

        if (downloaded.wasOk() && ! partial.moveFileTo (target))
            downloaded = juce::Result::fail ("Phazite couldn't put the installer in " + owner.downloads.getFullPathName() + ".");

        if (downloaded.failed())
        {
            partial.deleteFile();
            owner.update (State::failed, 0.0, downloaded.getErrorMessage());
            return;
        }

        owner.update (State::ready, 1.0);
    }

    // Reads an address (a local file, as tests give it) into out: no more than limit bytes, telling the progress against
    // expected (none: 0). Over HTTPS only, following redirects that stay on HTTPS.
    juce::Result fetch (const juce::URL& url, juce::int64 limit, juce::int64 expected, juce::OutputStream& out)
    {
        const juce::ScopeGuard closeStream { [this]
                                             {
                                                 const juce::ScopedLock sl (streamLock);
                                                 stream = nullptr;
                                             } };
        std::unique_ptr<juce::InputStream> localFile;
        juce::InputStream* in = nullptr;

        if (url.isLocalFile())
        {
            localFile = url.getLocalFile().createInputStream();

            if (localFile == nullptr)
                return juce::Result::fail ("There is no " + url.getLocalFile().getFullPathName() + ".");

            in = localFile.get();
        }
        else
        {
            auto address = url;

            for (int redirects = 0;; ++redirects)
            {
                if (! address.toString (false).startsWithIgnoreCase ("https://"))
                    return juce::Result::fail ("Phazite asks for updates over HTTPS only, and " + address.toString (false) + " isn't.");

                {
                    const juce::ScopedLock sl (streamLock);
                    stream = std::make_unique<juce::WebInputStream> (address, false);
                }

                // Redirects are followed here, not by JUCE, which would follow one to HTTP.
                stream->withConnectionTimeout (connectionTimeout).withNumRedirectsToFollow (0);

                if (threadShouldExit())
                    return juce::Result::ok();

                if (! stream->connect (nullptr))
                    return juce::Result::fail ("Phazite couldn't connect to " + address.getDomain()
                                               + ". Check the internet connection and try again.");

                const auto status = stream->getStatusCode();

                if (isRedirect (status))
                {
                    const auto location = stream->getResponseHeaders()["Location"];

                    if (redirects == maxRedirects || ! isAllowedRedirect (location))
                        return juce::Result::fail (address.getDomain()
                                                   + " sent Phazite somewhere it doesn't download from, so it stopped.");

                    address = juce::URL (location);
                    continue;
                }

                if (status == 404)
                    return juce::Result::fail ("Phazite's releases on " + address.getDomain() + " have no update file yet.");

                if (status != 200)
                    return juce::Result::fail (address.getDomain() + " answered " + juce::String (status) + ". Try again later.");

                in = stream.get();
                break;
            }
        }

        constexpr int bufferSize = 65536;
        juce::HeapBlock<char> buffer (bufferSize);
        juce::int64 written = 0;

        while (! threadShouldExit())
        {
            const auto read = in->read (buffer, bufferSize);

            if (read <= 0)
                break;

            written += read;

            // Never more than the file should be, whatever the server sends.
            if (written > limit)
                return juce::Result::fail ("The download was larger than it should be, so Phazite stopped it.");

            if (! out.write (buffer, (size_t) read))
                return juce::Result::fail ("Phazite couldn't write what it downloaded.");

            if (expected > 0)
                owner.update (State::downloading, (double) written / (double) expected);
        }

        return juce::Result::ok();
    }

    Updates& owner;
    const std::optional<Release> release; // the one to download, or none: check
    juce::CriticalSection streamLock;
    std::unique_ptr<juce::WebInputStream> stream; // while reading; set and reset under streamLock, read without it
};

//==============================================================================
Updates::Source Updates::getReleases()
{
    const auto releases = getSourceCodeAddress() + "/releases";
    return { .updateFile = juce::URL (releases + "/latest/download/phazite-update.json"),
             .installerPrefix = releases + "/download/",
             .releasesPage = juce::URL (releases) };
}

std::optional<Updates::Release> Updates::parseUpdateFile (const juce::String& json, const Source& from, juce::String& error)
{
    juce::var parsed;

    if (juce::JSON::parse (json, parsed).failed() || ! parsed.isObject())
    {
        error = "isn't JSON.";
        return std::nullopt;
    }

    if (parsed["app"].toString() != appName)
    {
        error = "isn't Phazite's.";
        return std::nullopt;
    }

    Release release;
    release.version = parsed["version"].toString();

    if (! getNumbers (release.version).has_value())
    {
        error = "has no version.";
        return std::nullopt;
    }

    // Its page, if it is one of the releases'; otherwise the list of them.
    const auto notes = parsed["notes"].toString();
    release.notes = notes.startsWith (from.releasesPage.toString (false) + "/") ? juce::URL (notes) : from.releasesPage;

#if JUCE_WINDOWS
    const auto windows = parsed["windows"];

    if (! windows.isObject())
    {
        error = "has no installer for Windows.";
        return std::nullopt;
    }

    const auto installer = windows["installer"].toString();

    if (from.installerPrefix.isEmpty() || installer.length() <= from.installerPrefix.length()
        || ! installer.startsWith (from.installerPrefix) || installer.contains (".."))
    {
        error = "names an installer that isn't one of Phazite's releases.";
        return std::nullopt;
    }

    release.installer = juce::URL (installer);
    release.size = (juce::int64) windows["size"];

    if (release.size <= 0 || release.size > maxInstallerSize)
    {
        error = "gives the installer no size.";
        return std::nullopt;
    }

    release.sha256 = windows["sha256"].toString().toLowerCase();

    if (! isSha256 (release.sha256))
    {
        error = "gives the installer no SHA-256.";
        return std::nullopt;
    }
#endif

    return release;
}

int Updates::compareVersions (const juce::String& a, const juce::String& b)
{
    const auto x = getNumbers (a), y = getNumbers (b);

    if (! x.has_value() || ! y.has_value())
        return (x.has_value() ? 1 : 0) - (y.has_value() ? 1 : 0);

    for (size_t i = 0; i < 4; ++i)
    {
        const auto p = i < x->size() ? (*x)[i] : 0, q = i < y->size() ? (*y)[i] : 0;

        if (p != q)
            return p < q ? -1 : 1;
    }

    return 0;
}

bool Updates::isAllowedRedirect (const juce::String& location)
{
    return location.startsWithIgnoreCase ("https://") && location.length() > 8;
}

juce::String Updates::getInstallerArguments()
{
    // Inno Setup's: no questions, no restart, Phazite's helpers closed if any is left, and Phazite opened again once it is
    // installed (/relaunch=1, which also makes the installer wait for this Phazite to close: packaging/windows/Phazite.iss).
    return "/SILENT /SP- /NORESTART /CLOSEAPPLICATIONS /relaunch=1";
}

bool Updates::canCheck()
{
#if JUCE_LINUX && ! JUCE_USE_CURL
    return false; // JUCE's own sockets have no TLS
#else
    return true;
#endif
}

bool Updates::canCheckSource() const
{
    return canCheck() || source.updateFile.isLocalFile();
}

juce::File Updates::getDefaultDownloadsFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (juce::String (appName) + " Updates");
}

Updates::Updates (Settings& s, juce::File downloadsFolder, Source from, juce::String version)
    : settings (s),
      downloads (std::move (downloadsFolder)),
      source (std::move (from)),
      runningVersion (std::move (version))
{
}

Updates::~Updates()
{
    job = nullptr; // before the change messages it sends can't go anywhere
}

bool Updates::checksAtStart() const
{
    return settings.get (Settings::updateCheck);
}

void Updates::setChecksAtStart (bool on)
{
    settings.set (Settings::updateCheck, on);
    sendChangeMessage();
}

void Updates::startUp()
{
    // An earlier update's installer has run by now, or its download stopped.
    if (downloads.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator (downloads, false, "*", juce::File::findFiles))
            entry.getFile().deleteFile();

    if (checksAtStart())
        check();
}

void Updates::check()
{
    if (const auto now = getState(); now == State::checking || now == State::downloading)
        return;

    if (! canCheckSource())
    {
        update (State::failed, 0.0, "This build of Phazite can't check for updates: its releases page lists them.");
        return;
    }

    start (false);
}

void Updates::download()
{
    if (const auto now = getState(); now == State::checking || now == State::downloading || ! getAvailable().has_value())
        return;

    start (true);
}

void Updates::start (bool downloading)
{
    job = nullptr; // the last one, which has finished
    update (downloading ? State::downloading : State::checking, 0.0);
    job = std::make_unique<Job> (*this, downloading ? getAvailable() : std::nullopt);
    job->startThread();
}

void Updates::cancel()
{
    if (job != nullptr)
        if (const auto now = getState(); now == State::checking || now == State::downloading)
            job->cancel();
}

Updates::State Updates::getState() const
{
    const juce::ScopedLock sl (lock);
    return state;
}

double Updates::getProgress() const
{
    const juce::ScopedLock sl (lock);
    return progress;
}

juce::String Updates::getError() const
{
    const juce::ScopedLock sl (lock);
    return error;
}

std::optional<Updates::Release> Updates::getAvailable() const
{
    const juce::ScopedLock sl (lock);
    return available;
}

juce::String Updates::getLatestVersion() const
{
    const juce::ScopedLock sl (lock);
    return latestVersion;
}

void Updates::update (State newState, double newProgress, const juce::String& newError)
{
    {
        const juce::ScopedLock sl (lock);
        state = newState;
        progress = newProgress;
        error = newError;
    }

    sendChangeMessage(); // to the message thread
}

juce::File Updates::getInstallerFile (const Release& release) const
{
    // Named by Phazite, never by the server: the version is numbers and dots only (parseUpdateFile()).
    return downloads.getChildFile (juce::String (appName) + "-" + release.version + "-Setup.exe");
}

bool Updates::isInstaller (const juce::File& file, const Release& release) const
{
    return file.existsAsFile() && file.getSize() == release.size && juce::SHA256 (file).toHexString() == release.sha256;
}

juce::Result Updates::startInstaller()
{
    const auto release = getAvailable();

    if (getState() != State::ready || ! release.has_value())
        return juce::Result::fail ("There is no update downloaded to install.");

    // Checked again: anything could have changed the file since it was downloaded.
    const auto file = getInstallerFile (*release);

    if (! isInstaller (file, *release))
    {
        file.deleteFile();
        update (State::available, 0.0);
        return juce::Result::fail ("The downloaded installer changed after Phazite checked it, so Phazite deleted it. Download it again.");
    }

    if (launch == nullptr || ! launch (file, getInstallerArguments()))
        return juce::Result::fail ("Phazite couldn't start " + file.getFileName() + ".");

    return juce::Result::ok();
}
} // namespace phazite

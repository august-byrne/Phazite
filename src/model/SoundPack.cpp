// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SoundPack.h"
#include "AppInfo.h"

#include <juce_cryptography/juce_cryptography.h>

namespace phazite
{
namespace
{
    // The pack's own list of sounds, sources.json, names the pack and its version.
    int readVersion (const juce::File& folder, const juce::String& name)
    {
        const auto manifest = juce::JSON::parse (folder.getChildFile ("sources.json"));
        return manifest["pack"].toString() == name ? (int) manifest["version"] : 0;
    }
} // namespace

//==============================================================================
// Downloads the release, or takes a zip downloaded by hand, then checks it and installs it.
struct SoundPack::Job : public juce::Thread
{
    Job (SoundPack& p, juce::File zipToInstall)
        : juce::Thread ("Phazite Sounds"),
          owner (p),
          file (std::move (zipToInstall))
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
        const auto& pinned = owner.release;

        if (file != juce::File())
        {
            finish (owner.isRelease (file) ? owner.install (file)
                                           : juce::Result::fail (file.getFileName() + " isn't " + owner.getTitle() + ". Download "
                                                                 + pinned.zip.getFileName() + " from its page, and install that file."));
            return;
        }

        juce::TemporaryFile temp (owner.packs.getChildFile (pinned.name + ".zip"));
        const auto downloaded = fetch (temp.getFile());

        if (threadShouldExit())
            owner.update (owner.findInstalledVersion() >= pinned.version ? State::installed : State::notInstalled, 0.0); // cancelled
        else if (downloaded.failed())
            finish (downloaded);
        else
            finish (owner.isRelease (temp.getFile()) ? owner.install (temp.getFile())
                                                     : juce::Result::fail ("The download was damaged. Try again."));
    }

    juce::Result fetch (const juce::File& target)
    {
        const auto& pinned = owner.release;

        if (const auto made = target.getParentDirectory().createDirectory(); made.failed())
            return juce::Result::fail ("Phazite couldn't make the folder " + target.getParentDirectory().getFullPathName() + ".");

        const juce::ScopeGuard closeStream { [this]
                                             {
                                                 const juce::ScopedLock sl (streamLock);
                                                 stream = nullptr;
                                             } };
        std::unique_ptr<juce::InputStream> localFile;
        juce::InputStream* in = nullptr;

        if (pinned.zip.isLocalFile()) // as the tests give it
        {
            localFile = pinned.zip.getLocalFile().createInputStream();
            in = localFile.get();

            if (in == nullptr)
                return juce::Result::fail ("There is no " + pinned.zip.getLocalFile().getFullPathName() + ".");
        }
        else
        {
            {
                const juce::ScopedLock sl (streamLock);
                stream = std::make_unique<juce::WebInputStream> (pinned.zip, false);
            }

            stream->withConnectionTimeout (20000).withNumRedirectsToFollow (5);

            if (! stream->connect (nullptr))
                return juce::Result::fail ("Phazite couldn't connect to " + pinned.zip.getDomain()
                                           + ". Check the internet connection and try again.");

            if (const auto status = stream->getStatusCode(); status != 200)
                return juce::Result::fail (pinned.zip.getDomain() + " answered " + juce::String (status)
                                           + " instead of sending the sounds. Try again later, or download them in a browser.");

            in = stream.get();
        }

        juce::FileOutputStream out (target);

        if (! out.openedOk())
            return juce::Result::fail ("Phazite couldn't write to " + target.getParentDirectory().getFullPathName() + ".");

        constexpr int bufferSize = 65536;
        juce::HeapBlock<char> buffer (bufferSize);
        juce::int64 written = 0;

        while (! threadShouldExit())
        {
            const auto read = in->read (buffer, bufferSize);

            if (read <= 0)
                break;

            written += read;

            // Never more than the release, whatever the server sends.
            if (written > pinned.size)
                return juce::Result::fail ("The download was larger than " + owner.getTitle() + ", so Phazite stopped it.");

            if (! out.write (buffer, (size_t) read))
                return juce::Result::fail ("Phazite couldn't write to " + target.getParentDirectory().getFullPathName() + ".");

            owner.update (State::downloading, (double) written / (double) pinned.size);
        }

        out.flush();

        if (! threadShouldExit() && (written != pinned.size || ! out.getStatus().wasOk()))
            return juce::Result::fail ("The download stopped before it finished. Check the internet connection and try again.");

        return juce::Result::ok();
    }

    void finish (const juce::Result& result)
    {
        if (result.wasOk())
            owner.update (State::installed, 1.0);
        else
            owner.update (State::failed, 0.0, result.getErrorMessage());
    }

    SoundPack& owner;
    const juce::File file; // a zip to install, or none: download one
    juce::CriticalSection streamLock;
    std::unique_ptr<juce::WebInputStream> stream; // while downloading; set and reset under streamLock, read without it
};

//==============================================================================
SoundPack::Release SoundPack::getPinnedRelease()
{
    // Phazite-Sounds-1.zip, which phazite_soundkit --zip makes from the phazite-sounds repository at its tag v1.
    return { .name = "Phazite Sounds",
             .version = 1,
             .zip = juce::URL ("https://github.com/august-byrne/phazite-sounds/releases/download/v1/Phazite-Sounds-1.zip"),
             .page = juce::URL ("https://github.com/august-byrne/phazite-sounds/releases/tag/v1"),
             .size = 9801851,
             .sha256 = "f0a13ffc5b98b07e45a22b34ccfb7270922903813cec77f9620e1c29401217a3" };
}

juce::File SoundPack::getDefaultPacksFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Packs");
}

SoundPack::SoundPack (juce::File packsFolder, Release r)
    : packs (std::move (packsFolder)),
      release (std::move (r))
{
    if (findInstalledVersion() >= release.version)
        state = State::installed;
}

SoundPack::~SoundPack()
{
    job = nullptr; // before the change messages it sends can't go anywhere
}

juce::String SoundPack::getTitle() const
{
    return release.name + " " + juce::String (release.version);
}

int SoundPack::findInstalledVersion() const
{
    return readVersion (getFolder(), release.name);
}

bool SoundPack::isRelease (const juce::File& zip) const
{
    return zip.getSize() == release.size && juce::SHA256 (zip).toHexString() == release.sha256;
}

SoundPack::State SoundPack::getState() const
{
    const juce::ScopedLock sl (lock);
    return state;
}

bool SoundPack::isInstalled() const
{
    return findInstalledVersion() >= release.version;
}

double SoundPack::getProgress() const
{
    const juce::ScopedLock sl (lock);
    return progress;
}

juce::String SoundPack::getError() const
{
    const juce::ScopedLock sl (lock);
    return error;
}

void SoundPack::update (State newState, double newProgress, const juce::String& newError)
{
    {
        const juce::ScopedLock sl (lock);
        state = newState;
        progress = newProgress;
        error = newError;
    }

    sendChangeMessage(); // to the message thread
}

bool SoundPack::canDownload()
{
#if JUCE_LINUX && ! JUCE_USE_CURL
    return false; // JUCE's own sockets have no TLS
#else
    return true;
#endif
}

void SoundPack::download()
{
    if (! canDownload() && ! release.zip.isLocalFile())
    {
        update (State::failed, 0.0,
                "This build of Phazite can't download. Download the sounds in a browser, and install them from the file.");
        return;
    }

    start ({});
}

void SoundPack::installFromFile (const juce::File& zip)
{
    start (zip);
}

void SoundPack::start (const juce::File& zip)
{
    if (const auto now = getState(); now == State::downloading || now == State::installing)
        return;

    job = nullptr; // the last one, which has finished
    update (zip == juce::File() ? State::downloading : State::installing, 0.0);
    job = std::make_unique<Job> (*this, zip);
    job->startThread();
}

void SoundPack::cancel()
{
    if (job != nullptr && getState() == State::downloading)
        job->cancel();
}

juce::Result SoundPack::install (const juce::File& zip)
{
    update (State::installing, 1.0);

    // Unpacked next to where it goes, then moved there, so that the pack is either all there or not at all.
    const auto staging = packs.getChildFile (release.name + " (installing)");
    const auto target = getFolder();
    staging.deleteRecursively(); // left by an installation that didn't finish

    juce::ZipFile archive (zip);

    if (const auto unpacked = archive.uncompressTo (staging); unpacked.failed())
    {
        staging.deleteRecursively();
        return juce::Result::fail ("Phazite couldn't unpack " + release.name + ": " + unpacked.getErrorMessage());
    }

    const auto unpackedFolder = staging.getChildFile (release.name);

    if (readVersion (unpackedFolder, release.name) != release.version)
    {
        staging.deleteRecursively();
        return juce::Result::fail (zip.getFileName() + " doesn't hold " + getTitle() + ".");
    }

    // A pack there already, or what is left of one, goes to the trash, or aside where there is none.
    if (target.exists() && ! target.moveToTrash() && ! target.moveFileTo (packs.getNonexistentChildFile (release.name + " (old)", {})))
    {
        staging.deleteRecursively();
        return juce::Result::fail ("Phazite couldn't replace " + target.getFullPathName()
                                   + ". Close anything that uses a sound in it, and try again.");
    }

    const auto moved = unpackedFolder.moveFileTo (target);
    staging.deleteRecursively();

    if (! moved)
        return juce::Result::fail ("Phazite couldn't put " + release.name + " in " + packs.getFullPathName() + ".");

    return juce::Result::ok();
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginScanner.h"
#include "model/plugins/internal/PluginHost.h"
#include "sandbox/client/Helper.h"
#include "sandbox/core/PluginInfo.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace phazite
{
namespace
{
    class Scanner : public juce::KnownPluginList::CustomScanner
    {
    public:
        Scanner (PluginHost& h, int seconds)
            : host (h),
              secondsEach (seconds)
        {
        }

        // What it finds in a file; false blacklists the file (JUCE's KnownPluginList does), for a plug-in that crashed twice
        // or hung. A scan cancelled finds nothing, but blacklists nothing either: the next scan tries that file again.
        bool findPluginTypesFor (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                                 const juce::String& fileOrIdentifier) override
        {
            const auto found = findTypes (format, result, fileOrIdentifier);
            keepUpdateTimesOfUnchanged (result);
            return found;
        }

        void scanFinished() override
        {
            abort = false;

            {
                const std::lock_guard held (lock);
                helper = nullptr;
            }
        }

        // From another thread: the helper ended, so that the file it is on answers at once; first one that is still starting,
        // which the scan's thread holds the lock for (a cancel waited there for as long as a helper may take to start: the
        // plug-in list's window closed 20 seconds later, the window dead meanwhile).
        void cancel()
        {
            abort = true;

            if (const auto fresh = getStarting())
                fresh->kill();

            const std::lock_guard held (lock);

            if (helper != nullptr)
                helper->kill();
        }

    private:
        PluginHost& host;
        const int secondsEach;
        std::mutex lock; // the helper, which a cancel ends from another thread; held while one starts
        std::shared_ptr<sandbox::Helper> helper;
        std::atomic<bool> abort { false };

        // The helper that is starting, which a cancel ends without the lock its start holds (Helper::kill() may come from
        // another thread while it starts); kept by both while either uses it.
        std::mutex startingLock;
        std::shared_ptr<sandbox::Helper> starting;

        std::shared_ptr<sandbox::Helper> getStarting()
        {
            const std::lock_guard held (startingLock);
            return starting;
        }

        void setStarting (std::shared_ptr<sandbox::Helper> fresh)
        {
            const std::lock_guard held (startingLock);
            starting = std::move (fresh);
        }

        bool shouldAbort() const { return abort || shouldExit(); }

        bool findTypes (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                        const juce::String& fileOrIdentifier)
        {
            // A crash: once more with a fresh helper, in case the plug-in scanned before was to blame.
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (shouldAbort())
                    return true;

                auto* scanning = getHelper();

                if (scanning == nullptr)
                    return true; // no helper to scan in: nothing found, and nothing blacklisted

                auto request = scanning->makeRequest (sandbox::MessageType::scan);
                request.add (format.getName()).add (fileOrIdentifier);

                if (const auto answer = scanning->request (request, secondsEach * 1000))
                {
                    juce::String leftOut;
                    auto found = readScannedPlugins (*answer, &leftOut);

                    // Said where the scan's problems are shown (the plug-in list's window), never left out silently.
                    if (leftOut.isNotEmpty())
                    {
                        juce::Logger::writeToLog ("Plug-in scan: " + fileOrIdentifier + ": " + leftOut);
                        host.noteScanProblem (fileOrIdentifier + ": " + leftOut);
                    }

                    while (! found.isEmpty())
                        result.add (found.removeAndReturn (0));

                    return true;
                }

                if (shouldAbort())
                    return true;

                // Still running without an answer: stuck in the plug-in, which is left out, and the helper with it.
                if (scanning->isRunning())
                {
                    juce::Logger::writeToLog ("Plug-in scan: " + fileOrIdentifier + " took longer than " + juce::String (secondsEach)
                                              + " seconds, and is left out");
                    dropHelper();
                    return false;
                }

                juce::Logger::writeToLog ("Plug-in scan: crashed on " + fileOrIdentifier);
                dropHelper();
            }

            return false;
        }

        // The helper the scan uses, started as the first file comes and again after one that went; none without the
        // helper program.
        sandbox::Helper* getHelper()
        {
            const std::lock_guard held (lock);

            if (helper == nullptr || ! helper->isRunning())
            {
                helper = nullptr;
                auto fresh = std::make_shared<sandbox::Helper> (host.getHelperProgram());
                setStarting (fresh);

                // Not one a cancel came for before it was there to end; one cancelled as it starts fails to.
                const auto started = ! shouldAbort() && fresh->start (pluginScanStartMilliseconds);
                setStarting (nullptr);

                if (started)
                    helper = std::move (fresh);
            }

            return helper.get();
        }

        void dropHelper()
        {
            const std::lock_guard held (lock);
            helper = nullptr; // ended, if it still runs
        }

        // A plug-in described as it was before keeps the time it was described then, rather than
        // looking just updated (JUCE's formats describe every plug-in again at each scan, stamped with the time then).
        // JUCE workaround W6 (docs/juce-workarounds.md).
        void keepUpdateTimesOfUnchanged (juce::OwnedArray<juce::PluginDescription>& found) const
        {
            for (const auto& known : host.getKnownPlugins().getTypes())
                for (auto* description : found)
                    if (description != nullptr && description->isDuplicateOf (known)
                        && description->lastFileModTime == known.lastFileModTime && description->version == known.version)
                        description->lastInfoUpdateTime = known.lastInfoUpdateTime;
        }
    };
} // namespace

std::unique_ptr<juce::KnownPluginList::CustomScanner> createPluginScanner (PluginHost& host, int secondsEach)
{
    return std::make_unique<Scanner> (host, secondsEach);
}

void cancelPluginScan (juce::KnownPluginList::CustomScanner& scanner)
{
    if (auto* own = dynamic_cast<Scanner*> (&scanner))
        own->cancel();
}

void usePluginScannerWithTimeLimit (PluginHost& host)
{
    auto scanner = std::make_unique<Scanner> (host, pluginScanSecondsEach);
    host.setScanCanceller ([current = scanner.get()] { current->cancel(); });
    host.getKnownPlugins().setCustomScanner (std::move (scanner));
}

juce::OwnedArray<juce::PluginDescription> readScannedPlugins (const juce::MemoryBlock& answer, juce::String* leftOut)
{
    // A count no larger than a file holds nor than the answer's bytes could (each XML at least its end), each description
    // read only from XML of a size a description has, with its texts cut to what Phazite keeps (they go into the plug-in
    // list and the settings). What that leaves out is said, never left out silently (the review of known bug 181: a shell
    // file's hundreds of plug-ins past 256 went unsaid).
    sandbox::MessageReader reader (answer);
    const auto told = std::max (0, (int) reader.readInt());
    const auto count = std::min ({ told, maxScannedPlugins, (int) std::min<juce::int64> (reader.getNumBytesLeft(), 1 << 30) });
    juce::OwnedArray<juce::PluginDescription> found;
    auto unreadable = 0;

    for (int i = 0; i < count && reader.getNumBytesLeft() > 0; ++i)
    {
        const auto text = reader.readString();
        auto read = false;

        if (text.length() <= maxScannedXml)
        {
            if (const auto xml = juce::parseXML (text))
            {
                if (juce::PluginDescription description; description.loadFromXml (*xml))
                {
                    for (auto* field : { &description.name, &description.descriptiveName, &description.pluginFormatName,
                                         &description.category, &description.manufacturerName, &description.version })
                        *field = sandbox::keepText (*field);

                    description.fileOrIdentifier = sandbox::keepText (description.fileOrIdentifier, maxScannedPath);
                    found.add (new juce::PluginDescription (description));
                    read = true;
                }
            }
        }

        unreadable += read ? 0 : 1;
    }

    if (leftOut != nullptr)
    {
        *leftOut = {};

        if (const auto missing = told - found.size(); missing > 0)
        {
            *leftOut = juce::String (missing) + " of the " + juce::String (told) + " plug-ins it holds left out";

            if (told > maxScannedPlugins)
                *leftOut << " (Phazite takes " << maxScannedPlugins << " from a file at most)";
            else if (unreadable > 0)
                *leftOut << " (" << unreadable << " described in a way Phazite can't read)";
        }
    }

    return found;
}
} // namespace phazite

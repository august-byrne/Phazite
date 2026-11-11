// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginScanner.h"
#include "model/plugins/internal/PluginHost.h"
#include "sandbox/client/Helper.h"

#include <atomic>
#include <mutex>

namespace phazite
{
namespace
{
    constexpr int startMilliseconds = 20000; // how long a helper may take to start and answer

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

        // From another thread: the helper ended, so that the file it is on answers at once.
        void cancel()
        {
            abort = true;
            const std::lock_guard held (lock);

            if (helper != nullptr)
                helper->kill();
        }

    private:
        PluginHost& host;
        const int secondsEach;
        std::mutex lock; // the helper, which a cancel ends from another thread
        std::unique_ptr<sandbox::Helper> helper;
        std::atomic<bool> abort { false };

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
                    sandbox::MessageReader reader (*answer);

                    for (auto count = reader.readInt(); --count >= 0;)
                        if (const auto xml = juce::parseXML (reader.readString()))
                            if (juce::PluginDescription description; description.loadFromXml (*xml))
                                result.add (new juce::PluginDescription (description));

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
                helper = std::make_unique<sandbox::Helper> (host.getHelperProgram());

                if (helper != nullptr && ! helper->start (startMilliseconds))
                    helper = nullptr;
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

void usePluginScannerWithTimeLimit (PluginHost& host)
{
    auto scanner = std::make_unique<Scanner> (host, pluginScanSecondsEach);
    host.setScanCanceller ([current = scanner.get()] { current->cancel(); });
    host.getKnownPlugins().setCustomScanner (std::move (scanner));
}
} // namespace phazite

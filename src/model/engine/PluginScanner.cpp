// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginScanner.h"

#include <atomic>
#include <functional>

namespace phazite
{
namespace
{
    // What the child (Tracktion's PluginScanChildProcess, which the app starts for "--PluginScan:") reads: an element as
    // one line of XML.
    juce::MemoryBlock toMessage (const juce::XmlElement& xml)
    {
        juce::MemoryOutputStream stream;
        xml.writeTo (stream, juce::XmlElement::TextFormat().withoutHeader().singleLine());
        return stream.getMemoryBlock();
    }

    // The child copy that scans: a request for each file, answered with what it found. Destroyed, it stops the child.
    class Coordinator : private juce::ChildProcessCoordinator
    {
    public:
        enum class Reply
        {
            found,
            crashed,
            timedOut,
            cancelled
        };

        bool launch()
        {
            if (! launched)
                launched = launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), "PluginScan", 0, 0);

            return launched;
        }

        bool request (juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier, int id)
        {
            juce::XmlElement scan ("SCAN");
            scan.setAttribute ("id", id);
            scan.setAttribute ("type", format.getName());
            scan.setAttribute ("file", fileOrIdentifier);
            return sendMessageToWorker (toMessage (scan));
        }

        Reply waitForReply (int id, juce::OwnedArray<juce::PluginDescription>& result, const std::function<bool()>& shouldAbort)
        {
            const auto start = juce::Time::getMillisecondCounter();

            for (;;)
            {
                if (const auto reply = takeReply (id))
                {
                    for (const auto* element : reply->getChildIterator())
                        if (juce::PluginDescription description; description.loadFromXml (*element))
                            result.add (new juce::PluginDescription (description));

                    return Reply::found;
                }

                if (crashed)
                    return Reply::crashed;

                if (shouldAbort() || ! launched)
                    return Reply::cancelled;

                if (juce::Time::getMillisecondCounter() - start > (juce::uint32) pluginScanSecondsEach * 1000)
                    return Reply::timedOut;

                juce::Thread::sleep (10);
            }
        }

        bool hasCrashed() const noexcept { return crashed; }

    private:
        std::atomic<bool> launched { false }, crashed { false };
        juce::OwnedArray<juce::XmlElement> replies;
        juce::CriticalSection lock;

        std::unique_ptr<juce::XmlElement> takeReply (int id)
        {
            const juce::ScopedLock held (lock);

            for (int i = replies.size(); --i >= 0;)
                if (replies.getUnchecked (i)->getIntAttribute ("id") == id)
                    return std::unique_ptr<juce::XmlElement> (replies.removeAndReturn (i));

            return {};
        }

        void handleMessageFromWorker (const juce::MemoryBlock& message) override
        {
            if (auto xml = juce::parseXML (message.toString()); xml != nullptr && xml->hasTagName ("FOUND"))
            {
                const juce::ScopedLock held (lock);
                replies.add (xml.release());
            }
        }

        void handleConnectionLost() override { crashed = true; }
    };

    class Scanner : public juce::KnownPluginList::CustomScanner
    {
    public:
        explicit Scanner (te::Engine& e)
            : engine (e)
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
            coordinator = nullptr;

            if (auto callback = engine.getPluginManager().scanCompletedCallback)
                callback();
        }

        void cancel() { abort = true; }

    private:
        te::Engine& engine;
        std::unique_ptr<Coordinator> coordinator;
        std::atomic<bool> abort { false };

        bool shouldAbort() const { return abort || shouldExit(); }

        bool findTypes (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                        const juce::String& fileOrIdentifier)
        {
            // As Tracktion: the formats that load code, which can crash or hang, in the child; the others here.
            if (const auto name = format.getName(); ! (name.containsIgnoreCase ("VST") || name.containsIgnoreCase ("LADSPA")))
            {
                format.findAllTypesForFile (result, fileOrIdentifier);
                return true;
            }

            // A crash: once more with a fresh child, in case the plug-in scanned before was to blame.
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (coordinator == nullptr || coordinator->hasCrashed())
                    coordinator = std::make_unique<Coordinator>();

                // A child that can't start: here, as Tracktion falls back to.
                if (! coordinator->launch())
                {
                    coordinator = nullptr;
                    format.findAllTypesForFile (result, fileOrIdentifier);
                    return true;
                }

                if (shouldAbort())
                    return true;

                // A child that doesn't take the request has gone, as in a crash.
                const auto id = juce::Random().nextInt();

                if (! coordinator->request (format, fileOrIdentifier, id))
                {
                    coordinator = nullptr;
                    continue;
                }

                switch (coordinator->waitForReply (id, result, [this] { return shouldAbort(); }))
                {
                    case Coordinator::Reply::found:
                    case Coordinator::Reply::cancelled:
                        return true;

                    case Coordinator::Reply::timedOut:
                        juce::Logger::writeToLog ("Plug-in scan: " + fileOrIdentifier + " took longer than "
                                                  + juce::String (pluginScanSecondsEach) + " seconds, and is left out");
                        coordinator = nullptr; // stops the child, which is stuck in it
                        return false;

                    case Coordinator::Reply::crashed:
                        juce::Logger::writeToLog ("Plug-in scan: crashed on " + fileOrIdentifier);
                        coordinator = nullptr;
                        break;
                }
            }

            return false;
        }

        // As Tracktion's scanner: a plug-in described as it was before keeps the time it was described then, rather than
        // looking just updated (some formats describe every plug-in again at each scan).
        void keepUpdateTimesOfUnchanged (juce::OwnedArray<juce::PluginDescription>& found) const
        {
            for (const auto& known : engine.getPluginManager().knownPluginList.getTypes())
                for (auto* description : found)
                    if (description != nullptr && description->isDuplicateOf (known)
                        && description->lastFileModTime == known.lastFileModTime && description->version == known.version)
                        description->lastInfoUpdateTime = known.lastInfoUpdateTime;
        }
    };
} // namespace

void usePluginScannerWithTimeLimit (te::Engine& engine)
{
    auto& plugins = engine.getPluginManager();
    auto scanner = std::make_unique<Scanner> (engine);
    plugins.abortCurrentPluginScan = [current = scanner.get()] { current->cancel(); };
    plugins.knownPluginList.setCustomScanner (std::move (scanner));
}
} // namespace phazite

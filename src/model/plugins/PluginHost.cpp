// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginHost.h"
#include "engine/nodes/DeviceRunner.h"
#include "sandbox/client/Helper.h"

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    constexpr int startMilliseconds = 20000; // how long a helper may take to start and answer
    constexpr int smallestChannel = 4096;    // the longest block a channel takes at least
    constexpr int openingThreads = 8;        // the helpers plug-ins are opened in, in the background, at once
    constexpr int endingThreads = 8;         // and the helpers ended at once

    juce::File getDefaultHelperProgram()
    {
        const auto app = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
#if JUCE_WINDOWS
        return app.getSiblingFile ("PhaziteHost.exe");
#else
        return app.getSiblingFile ("PhaziteHost");
#endif
    }

    juce::String toXml (const juce::PluginDescription& description)
    {
        return description.createXml()->toString (juce::XmlElement::TextFormat().withoutHeader().singleLine());
    }
} // namespace

// A helper running, the instances opened in it by their IDs, and whether it has gone. Its helper goes first as it goes,
// so that its threads tell nothing of instances no longer there. Started once, by the first that needs it (on any thread:
// those that need it meanwhile wait for its start).
struct PluginHost::Running
{
    std::mutex lock;
    std::map<std::int64_t, SandboxedInstance*> instances;
    std::atomic<bool> lost { false };
    std::unique_ptr<sandbox::Helper> helper;
    std::atomic<int> loading { 0 };             // the plug-ins being opened in it now
    std::atomic<bool> crashedLoading { false }; // a plug-in crashed it as it loaded: others it took with it aren't to blame

    std::mutex startLock;
    bool startTried = false, started = false;

    ~Running() { helper = nullptr; }

    // What an instance in it calls as it goes: it is no longer told of.
    std::function<void()> unlist (std::int64_t id)
    {
        return [this, id]
        {
            const std::lock_guard held (lock);
            instances.erase (id);
        };
    }
};

PluginHost::PluginHost()
    : program (getDefaultHelperProgram()),
      messageHangMilliseconds (sandbox::Helper::messageHangMilliseconds),
      audioHangMilliseconds (sandbox::Helper::audioHangMilliseconds),
      loadMilliseconds (defaultLoadMilliseconds),
      quitMilliseconds (sandbox::Helper::quitMilliseconds),
      endings (std::make_shared<BackgroundJobs> ("Phazite helper ending", endingThreads, juce::Thread::Priority::low)),
      openings (std::make_unique<BackgroundJobs> ("Phazite plug-in opening", openingThreads, juce::Thread::Priority::normal))
{
    juce::addDefaultFormatsToManager (formats);
    knownPlugins.addChangeListener (this);
}

PluginHost::~PluginHost()
{
    knownPlugins.removeChangeListener (this);

    // Instances that outlive it tell it nothing more; and a plug-in still opening in the background (its project closed
    // first) isn't waited for: its helper is ended, which answers what waits for it, and one that hasn't started doesn't
    // (Helper::kill(); no other starts once it goes).
    {
        const std::lock_guard held (lock);
        stopping = true;
        waitingJobs.clear();

        for (const auto& [key, weak] : helpers)
        {
            if (const auto running = weak.lock())
            {
                const std::lock_guard instancesHeld (running->lock);

                for (const auto& [id, instance] : running->instances)
                    instance->onLost = nullptr;

                if (running->instances.empty() && running->helper != nullptr)
                    running->helper->kill();
            }
        }
    }

    openings = nullptr;    // each job waited for
    endings->waitForAll(); // and each helper that goes ended, none of them dropped
}

void PluginHost::cancelScan()
{
    if (scanCanceller != nullptr)
        scanCanceller();
}

void PluginHost::noteScanProblem (const juce::String& problem)
{
    const std::lock_guard held (scanProblemsLock);
    scanProblems.add (problem);
}

juce::StringArray PluginHost::takeScanProblems()
{
    const std::lock_guard held (scanProblemsLock);
    return std::exchange (scanProblems, {});
}

void PluginHost::setHelperProgram (const juce::File& helperProgram)
{
    const std::lock_guard held (lock);
    program = helperProgram;
}

void PluginHost::setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds)
{
    const std::lock_guard held (lock);
    messageHangMilliseconds = messageThreadMilliseconds;
    audioHangMilliseconds = audioThreadMilliseconds;
}

void PluginHost::setLoadMilliseconds (int milliseconds) noexcept
{
    const std::lock_guard held (lock);
    loadMilliseconds = milliseconds;
}

void PluginHost::setQuitMilliseconds (int milliseconds) noexcept
{
    const std::lock_guard held (lock);
    quitMilliseconds = milliseconds;
}

juce::String PluginHost::getHelperKey (const juce::PluginDescription& description)
{
    return description.pluginFormatName + ":" + description.fileOrIdentifier;
}

int PluginHost::getNumHelpers()
{
    const std::lock_guard held (lock);
    return (int) std::count_if (helpers.begin(), helpers.end(),
                                [] (const auto& entry)
                                {
                                    const auto running = entry.second.lock();
                                    return running != nullptr && ! running->lost.load();
                                });
}

int PluginHost::getNumHelpersEnding() const
{
    return endings->getNumJobs();
}

void PluginHost::waitForHelpersEnded() const
{
    endings->waitForAll();
}

void PluginHost::setCrashedWhileLoading (const juce::StringArray& identities)
{
    const std::lock_guard held (lock);
    crashedWhileLoading = identities;
}

juce::StringArray PluginHost::getCrashedWhileLoading() const
{
    const std::lock_guard held (lock);
    return crashedWhileLoading;
}

bool PluginHost::hasCrashedWhileLoading (const juce::PluginDescription& description) const
{
    const std::lock_guard held (lock);
    return crashedWhileLoading.contains (description.createIdentifierString());
}

void PluginHost::forgetCrashWhileLoading (const juce::PluginDescription& description)
{
    {
        const std::lock_guard held (lock);

        if (! crashedWhileLoading.contains (description.createIdentifierString()))
            return;

        crashedWhileLoading.removeString (description.createIdentifierString());
    }

    sendChangeMessage();
}

std::shared_ptr<PluginHost::Running> PluginHost::getHelper (const juce::String& key, juce::String& error, bool again)
{
    // The plug-in's helper, if it runs or starts; one started for it otherwise (or again, after one that went). It ends, as
    // its last instance lets go of it, on a thread of the host's, without anything waiting for it.
    std::shared_ptr<Running> running;
    juce::File helperProgram;

    {
        const std::lock_guard held (lock);
        helperProgram = program;

        if (stopping)
        {
            error = "Phazite is closing.";
            return nullptr;
        }

        running = helpers[key].lock();

        if (running == nullptr || running->lost.load())
        {
            running = std::shared_ptr<Running> (new Running(), [ends = endings] (Running* gone)
                                                { ends->add ([gone] (const BackgroundJobs::ShouldStop&) { delete gone; }); });
            running->helper = std::make_unique<sandbox::Helper> (helperProgram);
            running->helper->setHangLimits (messageHangMilliseconds, audioHangMilliseconds);
            running->helper->setQuitMilliseconds (quitMilliseconds);
            auto* raw = running.get();

            running->helper->onNotice = [raw] (sandbox::MessageReader& notice)
            {
                const auto id = sandbox::RemoteInstance::readNoticeInstance (notice);
                const std::lock_guard instancesHeld (raw->lock);

                if (const auto found = raw->instances.find (id); found != raw->instances.end())
                    found->second->getRemote().handleNotice (notice.getType(), notice);
            };

            running->helper->onLost = [raw]
            {
                raw->lost = true;
                const std::lock_guard instancesHeld (raw->lock);

                for (const auto& [id, instance] : raw->instances)
                    instance->helperLost();
            };

            helpers[key] = running;
        }
    }

    {
        const std::lock_guard starting (running->startLock);

        if (! running->startTried)
        {
            running->startTried = true;
            running->started = running->helper->start (startMilliseconds);

            if (! running->started)
                running->lost = true; // the next asks for another
        }
    }

    if (! running->started)
    {
        error = helperProgram.existsAsFile() ? "Phazite's plug-in host didn't start."
                                             : "Phazite's plug-in host (" + helperProgram.getFileName() + ") is missing.";
        return nullptr;
    }

    // Gone already, before its loss was told (a crash a moment ago): another, once.
    if (! running->helper->isRunning())
    {
        running->lost = true;

        if (! again)
            return getHelper (key, error, true);

        error = "Phazite's plug-in host stopped as it started.";
        return nullptr;
    }

    return running;
}

std::unique_ptr<sandbox::RemoteInstance> PluginHost::open (Running& running, const juce::PluginDescription& description,
                                                           const juce::MemoryBlock& state, double sampleRate,
                                                           const sandbox::BlockChannel::Limits& limits, juce::String& error)
{
    int limit = 0;

    {
        const std::lock_guard held (lock);
        limit = loadMilliseconds;
    }

    ++running.loading;
    auto remote = sandbox::RemoteInstance::open (*running.helper, toXml (description), state, sampleRate, limits, error, limit);
    --running.loading;
    return remote;
}

void PluginHost::keepOffAfterCrash (const juce::PluginDescription& description, juce::String& error)
{
    // Its helper gone as it loaded: a crash of its own, which keeps it off from now on.
    {
        const std::lock_guard held (lock);
        crashedWhileLoading.addIfNotAlreadyThere (description.createIdentifierString());
    }

    error = "It crashed as it loaded, and stays off until it is reloaded.";
    sendChangeMessage();
}

int PluginHost::getNumLoading (const juce::PluginDescription& description)
{
    const std::lock_guard held (lock);
    const auto found = helpers.find (getHelperKey (description));
    const auto running = found != helpers.end() ? found->second.lock() : nullptr;
    return running != nullptr ? running->loading.load() : 0;
}

void PluginHost::track (Running& running, std::int64_t id, SandboxedInstance& instance)
{
    instance.onLost = [this] { sendChangeMessage(); };
    const std::lock_guard held (running.lock);
    running.instances[id] = &instance;

    if (running.lost.load())
        instance.helperLost();
}

PluginHost::Opened PluginHost::open (const juce::PluginDescription& description, const juce::MemoryBlock& state, int programWanted,
                                     double sampleRate, int blockSize, const std::function<bool()>& abandoned)
{
    Opened opened { description, state, nullptr, nullptr, {} };
    const auto isAbandoned = [&abandoned, &opened]
    {
        if (abandoned == nullptr || ! abandoned())
            return false;

        opened.remote = nullptr; // closed in its helper, if it opened
        opened.running = nullptr;
        opened.error = "It wasn't wanted any more.";
        return true;
    };

    if (hasCrashedWhileLoading (description))
    {
        opened.error = "It crashed as it loaded before, and stays off until it is reloaded.";
        return opened;
    }

    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = std::max (smallestChannel, blockSize);
    limits.maxChanges = engine::DeviceRunner::maximumChanges; // what Phazite's engine moves in a block
    std::shared_ptr<Running> running;

    for (auto tries = 0;; ++tries)
    {
        if (isAbandoned())
            return opened;

        running = getHelper (getHelperKey (description), opened.error);

        if (running == nullptr)
            return opened;

        // Whether another instance was in the helper, or loading in it, as this was sent: one that takes the helper down
        // meanwhile would take this one's load with it.
        auto shared = running->loading.load() > 0;

        {
            const std::lock_guard instancesHeld (running->lock);
            shared = shared || ! running->instances.empty();
        }

        opened.remote = open (*running, description, state, sampleRate > 0.0 ? sampleRate : 44100.0, limits, opened.error);

        if (opened.remote != nullptr || running->helper->isRunning())
            break;

        // Its helper went as it loaded: its own crash, unless another may have taken it down (one in it, or one that crashed
        // it as it loaded); then once more, in a fresh helper.
        if ((! shared && ! running->crashedLoading.load()) || tries > 0)
        {
            running->crashedLoading = true;
            keepOffAfterCrash (description, opened.error);
            return opened;
        }
    }

    if (opened.remote == nullptr || isAbandoned())
        return opened;

    // Where its state left another program: that program, then the state again, so that the settings are never overridden
    // by a program set after them (a plug-in's state needn't say which program it is on).
    const auto stateMilliseconds = SandboxedInstance::getStateMilliseconds();

    if (programWanted >= 0 && juce::isPositiveAndBelow (programWanted, (int) opened.remote->getInfo().programs.size())
        && opened.remote->getProgram() != programWanted)
    {
        opened.remote->setProgram (programWanted, stateMilliseconds);

        if (state.getSize() > 0)
            opened.remote->setState (state, stateMilliseconds);
    }

    opened.running = std::move (running);
    isAbandoned();
    return opened;
}

std::unique_ptr<SandboxedInstance> PluginHost::makeInstance (Opened& opened)
{
    if (opened.remote == nullptr)
        return nullptr;

    auto running = std::static_pointer_cast<Running> (opened.running);
    const auto id = opened.remote->getID();
    auto instance =
        std::make_unique<SandboxedInstance> (std::move (opened.remote), opened.description, opened.state, running, running->unlist (id));
    track (*running, id, *instance);
    return instance;
}

void PluginHost::runInBackground (const juce::String& helperKey, std::function<void (const BackgroundJobs::ShouldStop&)> job)
{
    const std::lock_guard held (lock);

    if (stopping)
        return;

    // Behind the jobs for the same helper, if any wait: they run one after another on one thread.
    auto& waiting = waitingJobs[helperKey];
    waiting.push_back (std::move (job));

    if (waiting.size() == 1)
        openings->add ([this, helperKey] (const BackgroundJobs::ShouldStop& shouldStop) { runJobs (helperKey, shouldStop); });
}

void PluginHost::runJobs (const juce::String& helperKey, const BackgroundJobs::ShouldStop& shouldStop)
{
    for (;;)
    {
        std::function<void (const BackgroundJobs::ShouldStop&)> job;

        {
            const std::lock_guard held (lock);
            const auto found = waitingJobs.find (helperKey);

            if (found == waitingJobs.end() || found->second.empty())
                return;

            job = found->second.front(); // stays at the front while it runs, so that the next joins this thread
        }

        if (! shouldStop())
            job (shouldStop);

        const std::lock_guard held (lock);
        const auto found = waitingJobs.find (helperKey);

        if (found == waitingJobs.end())
            return;

        found->second.pop_front();

        if (found->second.empty())
        {
            waitingJobs.erase (found);
            return;
        }
    }
}

std::unique_ptr<SandboxedInstance> PluginHost::createInstance (const juce::PluginDescription& description, const juce::MemoryBlock& state,
                                                               int programWanted, double sampleRate, int blockSize, juce::String& error)
{
    auto opened = open (description, state, programWanted, sampleRate, blockSize);
    error = opened.error;
    return makeInstance (opened);
}

PluginHost::Reopening PluginHost::describeReopening (SandboxedInstance& instance, const juce::MemoryBlock& state)
{
    // As it was: at the rate it was last prepared for (crashed or not), for the longest block it was last prepared for or
    // its channel takes, from the state given (the values it tells as it opens are those); where that leaves another
    // program, its program, then the state again (as open() does).
    const auto blockSize =
        std::max ({ instance.getRemote().getChannel().getLimits().maxSamples, smallestChannel, instance.getPreparedBlockSize() });
    const auto rate = instance.getPreparedRate() > 0.0 ? instance.getPreparedRate() : 44100.0;
    return { instance.getDescription(), state, instance.getRemote().getProgram(), rate, blockSize };
}

bool PluginHost::replaceRemote (SandboxedInstance& instance, Opened& opened, double sampleRate, juce::String& error)
{
    if (opened.remote == nullptr)
    {
        error = opened.error;
        return false;
    }

    auto running = std::static_pointer_cast<Running> (opened.running);
    const auto id = opened.remote->getID();
    instance.replaceRemote (std::move (opened.remote), sampleRate, running, running->unlist (id));
    track (*running, id, instance);
    sendChangeMessage();
    return true;
}

} // namespace phazite

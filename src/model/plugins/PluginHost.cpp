// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginHost.h"
#include "engine/nodes/DeviceRunner.h"
#include "sandbox/client/Helper.h"

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr int startMilliseconds = 20000; // how long a helper may take to start and answer
    constexpr int loadMilliseconds = 60000;  // and a plug-in to load in it (a sample library can take long)
    constexpr int smallestChannel = 4096;    // the longest block a channel takes at least
    constexpr int openingThreads = 8;        // the plug-ins opened in the background at once, each in its own helper
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
    // first) isn't waited for: its helper is ended, which answers what waits for it.
    {
        const std::lock_guard held (lock);

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

std::shared_ptr<PluginHost::Running> PluginHost::getHelper (const juce::String& key, juce::String& error)
{
    // The plug-in's helper, if it runs or starts; one started for it otherwise (or again, after one that went). It ends, as
    // its last instance lets go of it, on a thread of the host's, without anything waiting for it.
    std::shared_ptr<Running> running;
    juce::File helperProgram;
    int messageLimit = 0, audioLimit = 0;

    {
        const std::lock_guard held (lock);
        running = helpers[key].lock();
        helperProgram = program;
        messageLimit = messageHangMilliseconds;
        audioLimit = audioHangMilliseconds;

        if (running == nullptr || running->lost.load())
        {
            running = std::shared_ptr<Running> (new Running(), [ends = endings] (Running* gone)
                                                { ends->add ([gone] (const BackgroundJobs::ShouldStop&) { delete gone; }); });
            running->helper = std::make_unique<sandbox::Helper> (helperProgram);
            running->helper->setHangLimits (messageLimit, audioLimit);
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

    return running;
}

std::unique_ptr<sandbox::RemoteInstance> PluginHost::open (Running& running, const juce::PluginDescription& description,
                                                           const juce::MemoryBlock& state, double sampleRate,
                                                           const sandbox::BlockChannel::Limits& limits, juce::String& error)
{
    auto remote = sandbox::RemoteInstance::open (*running.helper, toXml (description), state, sampleRate, limits, error, loadMilliseconds);

    // Its helper gone as it loaded: a crash of its own, which keeps it off from now on.
    if (remote == nullptr && ! running.helper->isRunning())
    {
        {
            const std::lock_guard held (lock);
            crashedWhileLoading.addIfNotAlreadyThere (description.createIdentifierString());
        }

        error = "It crashed as it loaded, and stays off until it is reloaded.";
        sendChangeMessage();
    }

    return remote;
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
                                     double sampleRate, int blockSize)
{
    Opened opened { description, state, nullptr, nullptr, {} };

    if (hasCrashedWhileLoading (description))
    {
        opened.error = "It crashed as it loaded before, and stays off until it is reloaded.";
        return opened;
    }

    auto running = getHelper (getHelperKey (description), opened.error);

    if (running == nullptr)
        return opened;

    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = std::max (smallestChannel, blockSize);
    limits.maxChanges = engine::DeviceRunner::maximumChanges; // what Phazite's engine moves in a block
    opened.remote = open (*running, description, state, sampleRate > 0.0 ? sampleRate : 44100.0, limits, opened.error);

    if (opened.remote == nullptr)
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

void PluginHost::runInBackground (std::function<void()> job)
{
    openings->add ([run = std::move (job)] (const BackgroundJobs::ShouldStop&) { run(); });
}

std::unique_ptr<SandboxedInstance> PluginHost::createInstance (const juce::PluginDescription& description, const juce::MemoryBlock& state,
                                                               int programWanted, double sampleRate, int blockSize, juce::String& error)
{
    auto opened = open (description, state, programWanted, sampleRate, blockSize);
    error = opened.error;
    return makeInstance (opened);
}

bool PluginHost::reload (SandboxedInstance& instance, juce::String& error)
{
    const auto& description = instance.getDescription();
    auto running = getHelper (getHelperKey (description), error);

    if (running == nullptr)
        return false;

    // As it was: at the rate it was last prepared for (crashed or not) with its channel's limits, for the longest block it
    // was last prepared for, from the state it last had (the values it tells as it opens are those); where that left
    // another program, its program, then the state again, so that the program never overrides the settings (as open()
    // does).
    auto limits = instance.getRemote().getChannel().getLimits();
    limits.maxSamples = std::max ({ limits.maxSamples, smallestChannel, instance.getPreparedBlockSize() });
    const auto programNow = instance.getRemote().getProgram();
    const auto& state = instance.getLastState();
    const auto rate = instance.getPreparedRate() > 0.0 ? instance.getPreparedRate() : 44100.0;
    auto remote = open (*running, description, state, rate, limits, error);

    if (remote == nullptr)
        return false;

    if (! remote->getInfo().programs.empty() && remote->getProgram() != programNow)
    {
        remote->setProgram (programNow, SandboxedInstance::getStateMilliseconds());

        if (state.getSize() > 0)
            remote->setState (state, SandboxedInstance::getStateMilliseconds());
    }

    const auto id = remote->getID();
    instance.replaceRemote (std::move (remote), rate, running, running->unlist (id));
    track (*running, id, instance);
    sendChangeMessage();
    return true;
}

} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginHost.h"
#include "engine/nodes/DeviceRunner.h"
#include "model/playback/internal/PlaybackSession.h"
#include "sandbox/client/Helper.h"

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr int startMilliseconds = 20000; // how long a helper may take to start and answer
    constexpr int loadMilliseconds = 60000;  // and a plug-in to load in it (a sample library can take long)
    constexpr int stateMilliseconds = 10000; // and a reloaded one to take its program and state
    constexpr int smallestChannel = 4096;    // the longest block a channel takes at least

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

    // Each Engine's host, for what reaches it from a device (message thread).
    std::map<const te::Engine*, PluginHost*>& getHosts()
    {
        static std::map<const te::Engine*, PluginHost*> hosts;
        return hosts;
    }
} // namespace

// A helper running, the instances opened in it by their IDs, and whether it has gone. Its helper goes first as it goes,
// so that its threads tell nothing of instances no longer there.
struct PluginHost::Running
{
    std::mutex lock;
    std::map<std::int64_t, SandboxedInstance*> instances;
    std::atomic<bool> lost { false };
    std::unique_ptr<sandbox::Helper> helper;

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

PluginHost::PluginHost (te::Engine& e)
    : engine (e),
      program (getDefaultHelperProgram()),
      messageHangMilliseconds (sandbox::Helper::messageHangMilliseconds),
      audioHangMilliseconds (sandbox::Helper::audioHangMilliseconds)
{
    getHosts().try_emplace (&engine, this); // the first an Engine has (the tests make a second with the app's services)
}

PluginHost::~PluginHost()
{
    setOn (false);

    if (const auto found = getHosts().find (&engine); found != getHosts().end() && found->second == this)
        getHosts().erase (found);

    // Instances that outlive it tell it nothing more.
    for (const auto& [key, weak] : helpers)
    {
        if (const auto running = weak.lock())
        {
            const std::lock_guard held (running->lock);

            for (const auto& [id, instance] : running->instances)
                instance->onLost = nullptr;
        }
    }
}

PluginHost* PluginHost::find (const te::Engine& engine)
{
    const auto found = getHosts().find (&engine);
    return found != getHosts().end() ? found->second : nullptr;
}

void PluginHost::setHelperProgram (const juce::File& helperProgram)
{
    program = helperProgram;
}

void PluginHost::setEditorMaker (SandboxedInstance::EditorMaker maker)
{
    editorMaker = std::move (maker);
}

void PluginHost::setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds)
{
    messageHangMilliseconds = messageThreadMilliseconds;
    audioHangMilliseconds = audioThreadMilliseconds;
}

void PluginHost::setOn (bool shouldBeOn)
{
    if (shouldBeOn == on)
        return;

    on = shouldBeOn;
    auto& plugins = engine.getPluginManager();

    if (on)
    {
        tracktions = plugins.createPluginInstance;
        plugins.createPluginInstance = [this] (const juce::PluginDescription& description, double rate, int blockSize, juce::String& error)
        { return createInstance (description, rate, blockSize, error); };
    }
    else
    {
        plugins.createPluginInstance = tracktions;
        tracktions = nullptr;
    }
}

juce::String PluginHost::getHelperKey (const juce::PluginDescription& description)
{
    return description.pluginFormatName + ":" + description.fileOrIdentifier;
}

int PluginHost::getNumHelpers()
{
    return (int) std::count_if (helpers.begin(), helpers.end(),
                                [] (const auto& entry)
                                {
                                    const auto running = entry.second.lock();
                                    return running != nullptr && ! running->lost.load();
                                });
}

void PluginHost::setCrashedWhileLoading (const juce::StringArray& identities)
{
    crashedWhileLoading = identities;
}

bool PluginHost::hasCrashedWhileLoading (const juce::PluginDescription& description) const
{
    return crashedWhileLoading.contains (description.createIdentifierString());
}

void PluginHost::forgetCrashWhileLoading (const juce::PluginDescription& description)
{
    if (crashedWhileLoading.contains (description.createIdentifierString()))
    {
        crashedWhileLoading.removeString (description.createIdentifierString());
        sendChangeMessage();
    }
}

std::shared_ptr<PluginHost::Running> PluginHost::getHelper (const juce::String& key, juce::String& error)
{
    // The plug-in's helper, if it runs; one started for it otherwise (or again, after one that went).
    if (auto running = helpers[key].lock(); running != nullptr && ! running->lost.load())
        return running;

    auto running = std::make_shared<Running>();
    running->helper = std::make_unique<sandbox::Helper> (program);
    running->helper->setHangLimits (messageHangMilliseconds, audioHangMilliseconds);
    auto* raw = running.get();

    running->helper->onNotice = [raw] (sandbox::MessageReader& notice)
    {
        const auto id = sandbox::RemoteInstance::readNoticeInstance (notice);
        const std::lock_guard held (raw->lock);

        if (const auto found = raw->instances.find (id); found != raw->instances.end())
            found->second->getRemote().handleNotice (notice.getType(), notice);
    };

    running->helper->onLost = [raw]
    {
        raw->lost = true;
        const std::lock_guard held (raw->lock);

        for (const auto& [id, instance] : raw->instances)
            instance->helperLost();
    };

    if (! running->helper->start (startMilliseconds))
    {
        error = program.existsAsFile() ? "Phazite's plug-in host didn't start."
                                       : "Phazite's plug-in host (" + program.getFileName() + ") is missing.";
        return nullptr;
    }

    helpers[key] = running;
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
        crashedWhileLoading.addIfNotAlreadyThere (description.createIdentifierString());
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

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description, double sampleRate,
                                                                       int blockSize, juce::String& error)
{
    if (hasCrashedWhileLoading (description))
    {
        error = "It crashed as it loaded before, and stays off until it is reloaded.";
        return nullptr;
    }

    auto running = getHelper (getHelperKey (description), error);

    if (running == nullptr)
        return nullptr;

    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = std::max (smallestChannel, blockSize);
    limits.maxChanges = engine::DeviceRunner::maximumChanges; // what Phazite's engine moves in a block
    auto remote = open (*running, description, {}, sampleRate > 0.0 ? sampleRate : 44100.0, limits, error);

    if (remote == nullptr)
        return nullptr;

    const auto id = remote->getID();
    auto instance = std::make_unique<SandboxedInstance> (std::move (remote), description, running, running->unlist (id), editorMaker);
    track (*running, id, *instance);
    return instance;
}

bool PluginHost::reload (SandboxedInstance& instance, juce::String& error)
{
    const auto& description = instance.getDescription();
    auto running = getHelper (getHelperKey (description), error);

    if (running == nullptr)
        return false;

    // As it was: at its rate with its channel's limits, from the state it last had (the values it tells as it opens are
    // those), and its program, where the state left another.
    const auto limits = instance.getRemote().getChannel().getLimits();
    const auto programNow = instance.getRemote().getProgram();
    auto remote = open (*running, description, instance.getLastState(),
                        instance.getPreparedRate() > 0.0 ? instance.getPreparedRate() : 44100.0, limits, error);

    if (remote == nullptr)
        return false;

    if (! remote->getInfo().programs.empty() && remote->getProgram() != programNow)
        remote->setProgram (programNow, stateMilliseconds);

    const auto id = remote->getID();
    instance.replaceRemote (std::move (remote), running, running->unlist (id));
    track (*running, id, instance);
    sendChangeMessage();
    return true;
}

SandboxedInstance* findSandboxedInstance (te::Edit& edit, DeviceID device)
{
    if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (te::findPluginForID (edit, toTracktion (device)).get()))
        return dynamic_cast<SandboxedInstance*> (plugin->getAudioPluginInstance());

    return nullptr;
}

bool hasCrashed (const te::Plugin& device)
{
    const auto* plugin = dynamic_cast<const te::ExternalPlugin*> (&device);

    if (plugin == nullptr)
        return false;

    if (const auto* sandboxed = dynamic_cast<const SandboxedInstance*> (plugin->getAudioPluginInstance()))
        return sandboxed->hasCrashed();

    const auto* host = PluginHost::find (device.engine);
    return plugin->getAudioPluginInstance() == nullptr && host != nullptr && host->hasCrashedWhileLoading (plugin->desc);
}

bool reload (te::Plugin& device, juce::String& error)
{
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (&device);
    auto* host = plugin != nullptr ? PluginHost::find (device.engine) : nullptr;

    if (host == nullptr || ! hasCrashed (device))
        return false;

    // One whose helper went opens in a new one, the same instance; one that crashed as it loaded loads again.
    if (auto* sandboxed = dynamic_cast<SandboxedInstance*> (plugin->getAudioPluginInstance()))
    {
        if (! host->reload (*sandboxed, error))
            return false;
    }
    else
    {
        host->forgetCrashWhileLoading (plugin->desc);
        plugin->forceFullReinitialise();

        if (plugin->getAudioPluginInstance() == nullptr)
        {
            error = plugin->getLoadError();
            return false;
        }
    }

    // Its window made again, and the song given to the engine again, with it.
    device.engine.getUIBehaviour().recreatePluginWindowContentAsync (device);

    if (auto* session = findSession (device.edit))
        session->update();

    return true;
}
} // namespace phazite

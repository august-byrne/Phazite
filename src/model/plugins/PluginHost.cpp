// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginHost.h"
#include "sandbox/client/Helper.h"

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr int startMilliseconds = 20000; // how long a helper may take to start and answer
    constexpr int loadMilliseconds = 60000;  // and a plug-in to load in it (a sample library can take long)
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
};

PluginHost::PluginHost (te::Engine& e)
    : engine (e),
      program (getDefaultHelperProgram())
{
}

PluginHost::~PluginHost()
{
    setOn (false);
}

void PluginHost::setHelperProgram (const juce::File& helperProgram)
{
    program = helperProgram;
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

std::shared_ptr<PluginHost::Running> PluginHost::getHelper (const juce::String& key, juce::String& error)
{
    // The plug-in's helper, if it runs; one started for it otherwise (or again, after one that went).
    if (auto running = helpers[key].lock(); running != nullptr && ! running->lost.load())
        return running;

    auto running = std::make_shared<Running>();
    running->helper = std::make_unique<sandbox::Helper> (program);
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

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description, double sampleRate,
                                                                       int blockSize, juce::String& error)
{
    auto running = getHelper (getHelperKey (description), error);

    if (running == nullptr)
        return nullptr;

    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = std::max (smallestChannel, blockSize);
    auto remote = sandbox::RemoteInstance::open (*running->helper, toXml (description), {}, sampleRate > 0.0 ? sampleRate : 44100.0, limits,
                                                 error, loadMilliseconds);

    if (remote == nullptr)
        return nullptr;

    const auto id = remote->getID();
    auto* raw = running.get();
    auto instance = std::make_unique<SandboxedInstance> (std::move (remote), description, running,
                                                         [raw, id]
                                                         {
                                                             const std::lock_guard held (raw->lock);
                                                             raw->instances.erase (id);
                                                         });

    const std::lock_guard held (running->lock);
    running->instances[id] = instance.get();

    if (running->lost.load())
        instance->helperLost();

    return instance;
}

SandboxedInstance* findSandboxedInstance (te::Edit& edit, DeviceID device)
{
    if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (te::findPluginForID (edit, toTracktion (device)).get()))
        return dynamic_cast<SandboxedInstance*> (plugin->getAudioPluginInstance());

    return nullptr;
}
} // namespace phazite

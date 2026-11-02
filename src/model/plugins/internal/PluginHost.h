// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-ins Phazite hosts in helper processes (docs/tracktion-migration.md, T3; decided from step 0's measurement,
// docs/design.md, section 19): one helper for each plug-in, every instance of it there with an audio thread of its own,
// started as its first instance opens and quitting as its last goes; a helper that goes takes only its own plug-in's
// instances with it. Each instance is JUCE's plug-in instance that Tracktion's te::ExternalPlugin wraps
// (SandboxedInstance.h), made through Tracktion's own way of making plug-ins (te::PluginManager::createPluginInstance),
// which the host takes over while it is on (the setting while T3 lasts). The helpers' notices go to their instances by
// their IDs, and a helper's loss to every instance in it. Message thread, but for the helpers' own threads.
#pragma once

#include "SandboxedInstance.h"
#include "model/base/internal/Tracktion.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace phazite
{
class PluginHost final
{
public:
    explicit PluginHost (te::Engine&);
    ~PluginHost();

    // The helper program it starts: PhaziteHost next to the app, unless given another (the tests' helper).
    void setHelperProgram (const juce::File&);
    const juce::File& getHelperProgram() const noexcept { return program; }

    // Whether Tracktion makes plug-ins through it: those it makes from now on.
    void setOn (bool);
    bool isOn() const noexcept { return on; }

    // A plug-in opened in its helper as a plug-in instance Tracktion can play, or nullptr with why.
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&, double sampleRate, int blockSize,
                                                               juce::String& error);

    // Which plug-in's helper an instance goes to: its file's (one helper for every plug-in in a file).
    static juce::String getHelperKey (const juce::PluginDescription&);

    // How many helpers run now (not those that have gone, which their plug-ins' instances still hold).
    int getNumHelpers();

private:
    struct Running;

    te::Engine& engine;
    juce::File program;
    bool on = false;
    std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::PluginDescription&, double, int, juce::String&)> tracktions;
    std::map<juce::String, std::weak_ptr<Running>> helpers; // each kept alive by the instances in it

    std::shared_ptr<Running> getHelper (const juce::String& key, juce::String& error);
};

class EngineContext;
PluginHost& getPluginHost (EngineContext&); // the app's (model/engine/EngineContext.cpp)

// A device's plug-in instance in its helper, if the device is a plug-in hosted so; nullptr otherwise.
SandboxedInstance* findSandboxedInstance (te::Edit&, DeviceID);
} // namespace phazite

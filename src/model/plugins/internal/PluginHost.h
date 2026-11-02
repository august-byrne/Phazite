// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-ins Phazite hosts in helper processes (docs/tracktion-migration.md, T3; decided from step 0's measurement,
// docs/design.md, section 19): one helper for each plug-in, every instance of it there with an audio thread of its own,
// started as its first instance opens and quitting as its last goes; a helper that goes takes only its own plug-in's
// instances with it. Each instance is JUCE's plug-in instance that Tracktion's te::ExternalPlugin wraps
// (SandboxedInstance.h), made through Tracktion's own way of making plug-ins (te::PluginManager::createPluginInstance),
// which the host takes over for as long as it lives: Phazite hosts plug-ins so only. The helpers' notices go to their
// instances by their IDs, and a helper's loss to every instance in it. A helper that hangs is ended
// (sandbox/client/Helper.h), and its instances are crashed as a crashed helper's are, until each is reloaded. A plug-in
// that crashes its helper as it loads is kept off from then on, at the next start too (the app keeps the list in its
// settings), until it is reloaded by hand, so that it can't take time from every project that holds it. It tells its
// listeners (a change message) as an instance's helper goes, as one is reloaded, and as that list changes. Message
// thread, but for the helpers' own threads.
#pragma once

#include "SandboxedInstance.h"
#include "model/base/internal/Tracktion.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace phazite
{
class PluginHost final : public juce::ChangeBroadcaster
{
public:
    explicit PluginHost (te::Engine&);
    ~PluginHost() override;

    // The helper program it starts: PhaziteHost next to the app, unless given another (the tests' helper).
    void setHelperProgram (const juce::File&);
    const juce::File& getHelperProgram() const noexcept { return program; }

    // What makes the editors of the instances it makes from now on: a window of the app's (app/SandboxedEditor.h), which
    // the app sets as it starts. Without one, a plug-in shows no editor.
    void setEditorMaker (SandboxedInstance::EditorMaker);

    // How long a helper's message thread may not answer, and an audio thread keep a block, before it is ended (the
    // tests' are shorter): for the helpers started from now on.
    void setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds);

    // A plug-in opened in its helper as a plug-in instance Tracktion can play, or nullptr with why.
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&, double sampleRate, int blockSize,
                                                               juce::String& error);

    // An instance whose helper went, opened again in a new one from the state it last had; false, with why, if it
    // didn't open.
    bool reload (SandboxedInstance&, juce::String& error);

    // The plug-ins that crashed their helpers as they loaded, by their identities, which aren't loaded until a reload by
    // hand forgets one.
    juce::StringArray getCrashedWhileLoading() const { return crashedWhileLoading; }
    void setCrashedWhileLoading (const juce::StringArray&);
    bool hasCrashedWhileLoading (const juce::PluginDescription&) const;
    void forgetCrashWhileLoading (const juce::PluginDescription&);

    // Which plug-in's helper an instance goes to: its file's (one helper for every plug-in in a file).
    static juce::String getHelperKey (const juce::PluginDescription&);

    // How many helpers run now (not those that have gone, which their plug-ins' instances still hold).
    int getNumHelpers();

    // The host of an Engine's plug-ins: one for each Engine, which every engine context of it shares (the app has one; the
    // tests' services and sessions make more), made as the first asks and gone with the last.
    static std::shared_ptr<PluginHost> getFor (te::Engine&);

private:
    struct Running;

    te::Engine& engine;
    juce::File program;
    SandboxedInstance::EditorMaker editorMaker;
    int messageHangMilliseconds, audioHangMilliseconds;
    std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::PluginDescription&, double, int, juce::String&)> tracktions;
    std::map<juce::String, std::weak_ptr<Running>> helpers; // each kept alive by the instances in it
    juce::StringArray crashedWhileLoading;

    std::shared_ptr<Running> getHelper (const juce::String& key, juce::String& error);
    std::unique_ptr<sandbox::RemoteInstance> open (Running&, const juce::PluginDescription&, const juce::MemoryBlock& state,
                                                   double sampleRate, const sandbox::BlockChannel::Limits&, juce::String& error);
    void track (Running&, std::int64_t id, SandboxedInstance&); // told of the helper's notices and its loss
};

class EngineContext;
PluginHost& getPluginHost (EngineContext&); // the app's (model/engine/EngineContext.cpp)

// A device's plug-in instance in its helper, if the device is a plug-in hosted so; nullptr otherwise.
SandboxedInstance* findSandboxedInstance (te::Edit&, DeviceID);

// Whether a device is a plug-in whose helper went (a crash, or a hang it was ended for), or one that crashed its helper as
// it loaded and stays off until it is reloaded; and its reload: in a new helper, from the state it last had, its window
// made again and the engine given the song again; false, with why, if it didn't load.
bool hasCrashed (const te::Plugin&);
bool reload (te::Plugin&, juce::String& error);
} // namespace phazite

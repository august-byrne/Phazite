// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-ins Phazite hosts in helper processes (docs/design.md, section 19): one helper for each plug-in, every instance
// of it there with an audio thread of its own, started as its first instance opens and quitting as its last goes; a
// helper that goes takes only its own plug-in's instances with it. Each instance is a SandboxedInstance (SandboxedInstance.h),
// which a song's PluginInstances (PluginInstances.h) makes. The helpers' notices go to their instances by their IDs, and a
// helper's loss to every instance in it. A helper that hangs is ended (sandbox/client/Helper.h), and its instances are
// crashed as a crashed helper's are, until each is reloaded. A plug-in that crashes its helper as it loads is kept off from
// then on, at the next start too (the app keeps the list in its settings), until it is reloaded by hand, so that it can't
// take time from every project that holds it. With the plug-ins scanning found (the app keeps the list in its settings),
// and the formats they are scanned in (scanned in a helper too, each with a time limit: model/engine/internal/PluginScanner.h).
// It tells its listeners (a change message) as an instance's helper goes, as one is reloaded, and as that list changes.
// Message thread, but for the helpers' own threads.
#pragma once

#include "SandboxedInstance.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace phazite
{
class PluginHost final : public juce::ChangeBroadcaster,
                         private juce::ChangeListener // the plug-ins scanning found
{
public:
    PluginHost();
    ~PluginHost() override;

    // The plug-ins scanning found, and the formats they are found in (JUCE's, which scanning searches with). Phazite's process
    // loads none of them itself.
    juce::KnownPluginList& getKnownPlugins() noexcept { return knownPlugins; }
    const juce::KnownPluginList& getKnownPlugins() const noexcept { return knownPlugins; }
    juce::AudioPluginFormatManager& getFormats() noexcept { return formats; }

    // Stops a scan under way (the plug-in list's window closing), through what the scanner sets
    // (model/engine/internal/PluginScanner.h).
    void cancelScan();
    void setScanCanceller (std::function<void()> cancel) { scanCanceller = std::move (cancel); }

    // The helper program it starts: PhaziteHost next to the app, unless given another (the tests' helper).
    void setHelperProgram (const juce::File&);
    const juce::File& getHelperProgram() const noexcept { return program; }

    // How long a helper's message thread may not answer, and an audio thread keep a block, before it is ended (the
    // tests' are shorter): for the helpers started from now on.
    void setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds);

    // A plug-in opened in its helper with a state (none for its own defaults), or nullptr with why.
    std::unique_ptr<SandboxedInstance> createInstance (const juce::PluginDescription&, const juce::MemoryBlock& state, double sampleRate,
                                                       int blockSize, juce::String& error);

    // An instance whose helper went, opened again in a new one from the state it last had; false, with why, if it
    // didn't open.
    bool reload (SandboxedInstance&, juce::String& error);

    // Whether plug-ins load as projects open (docs/performance-audit.md, R2): while this is off, the plug-ins of a project
    // that opens stay as they were saved, unloaded and silent, so that one that crashes as it loads can't take Phazite down
    // again.
    void setLoadsPlugins (bool load) noexcept { loads = load; }
    bool loadsPlugins() const noexcept { return loads; }

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

private:
    struct Running;

    juce::KnownPluginList knownPlugins;
    juce::AudioPluginFormatManager formats;
    std::function<void()> scanCanceller;
    juce::File program;
    int messageHangMilliseconds, audioHangMilliseconds;
    bool loads = true;
    std::map<juce::String, std::weak_ptr<Running>> helpers; // each kept alive by the instances in it
    juce::StringArray crashedWhileLoading;

    std::shared_ptr<Running> getHelper (const juce::String& key, juce::String& error);
    std::unique_ptr<sandbox::RemoteInstance> open (Running&, const juce::PluginDescription&, const juce::MemoryBlock& state,
                                                   double sampleRate, const sandbox::BlockChannel::Limits&, juce::String& error);
    void track (Running&, std::int64_t id, SandboxedInstance&); // told of the helper's notices and its loss
    void changeListenerCallback (juce::ChangeBroadcaster*) override { sendChangeMessage(); }
};
} // namespace phazite

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
// Message thread, but for the helpers' own threads, and open(), which its own threads run for the plug-ins of a project that
// opens (PluginInstances.h), several at once.
#pragma once

#include "SandboxedInstance.h"
#include "model/base/BackgroundJobs.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <deque>
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

    // What a scan left out of a file it scanned, said rather than left out silently (a list cut short: the scanner notes it
    // from its thread), and taken by the plug-in list's window, which shows it (app/MainWindow.cpp). Any thread.
    void noteScanProblem (const juce::String&);
    juce::StringArray takeScanProblems();

    // The helper program it starts: PhaziteHost next to the app, unless given another (the tests' helper).
    void setHelperProgram (const juce::File&);
    const juce::File& getHelperProgram() const noexcept { return program; }

    // How long a helper's message thread may not answer, and an audio thread keep a block, before it is ended (the
    // tests' are shorter): for the helpers started from now on.
    void setHangLimits (int messageThreadMilliseconds, int audioThreadMilliseconds);

    // How long a plug-in may take to load in its helper (a minute: a sample library can take long), from as its request
    // is sent; and how long a helper that goes has to quit before it is ended (a second), for the helpers started from now
    // on. The tests' are other.
    static constexpr int defaultLoadMilliseconds = 60000;
    void setLoadMilliseconds (int) noexcept;
    void setQuitMilliseconds (int) noexcept;

    // A plug-in opened in its helper with a state (none for its own defaults) and, where that leaves it on another, a program
    // (-1 for the one the state leaves), then the state again (so that the program never overrides the settings), or nullptr
    // with why.
    std::unique_ptr<SandboxedInstance> createInstance (const juce::PluginDescription&, const juce::MemoryBlock& state, int program,
                                                       double sampleRate, int blockSize, juce::String& error);

    // The same in two halves, so that plug-ins open in the background, beside each other (known bug 69): opened in its helper,
    // which is started for it if it doesn't run (on any thread, which waits for as long as that takes: a job of the host's,
    // runInBackground()), unless abandoned says it isn't wanted any more (asked before each step: one opened for nothing is
    // closed at once); then made an instance of, on the message thread (nullptr, with why, for one that didn't open). Its
    // helper is kept before its instance, which goes first and calls into it as it goes. An instance whose helper went as it
    // loaded, while the helper held other instances or loads, may have been taken down by one of them: it is tried once
    // more in a fresh helper before it counts as one that crashes as it loads.
    struct Opened
    {
        juce::PluginDescription description;
        juce::MemoryBlock state;
        std::shared_ptr<void> running; // its helper's
        std::unique_ptr<sandbox::RemoteInstance> remote;
        juce::String error;
    };

    Opened open (const juce::PluginDescription&, const juce::MemoryBlock& state, int program, double sampleRate, int blockSize,
                 const std::function<bool()>& abandoned = {});
    std::unique_ptr<SandboxedInstance> makeInstance (Opened&);

    // Runs a job on the host's opening threads, one at a time for each helper (a helper does one thing at a time, and a
    // plug-in's load limit starts as its request is sent), the helpers beside each other; given what says the host goes.
    void runInBackground (const juce::String& helperKey, std::function<void (const BackgroundJobs::ShouldStop&)>);

    // How many plug-ins are being opened in a plug-in's helper now, for the tests.
    int getNumLoading (const juce::PluginDescription&);

    // An instance whose helper went, opened again in a new one, as open() opens a plug-in (in the background: known bug 166,
    // the message thread waited for the helper to start and the plug-in to open), then given what open() opened in place of
    // the remote it had, at the rate it was opened at (the instance's prepared rate): false, with why, if it didn't open.
    // What it is opened from: its plug-in, the state given, its program, and its channel for its prepared rate and block.
    struct Reopening
    {
        juce::PluginDescription description;
        juce::MemoryBlock state;
        int program = -1;
        double sampleRate = 0.0;
        int blockSize = 0;
    };

    static Reopening describeReopening (SandboxedInstance&, const juce::MemoryBlock& state);
    bool replaceRemote (SandboxedInstance&, Opened&, double sampleRate, juce::String& error);

    // Whether plug-ins load as projects open (docs/archive/performance-audit.md, R2): while this is off, the plug-ins of a project
    // that opens stay as they were saved, unloaded and silent, so that one that crashes as it loads can't take Phazite down
    // again.
    void setLoadsPlugins (bool load) noexcept { loads = load; }
    bool loadsPlugins() const noexcept { return loads; }

    // The plug-ins that crashed their helpers as they loaded, by their identities, which aren't loaded until a reload by
    // hand forgets one.
    juce::StringArray getCrashedWhileLoading() const;
    void setCrashedWhileLoading (const juce::StringArray&);
    bool hasCrashedWhileLoading (const juce::PluginDescription&) const;
    void forgetCrashWhileLoading (const juce::PluginDescription&);

    // Which plug-in's helper an instance goes to: its file's (one helper for every plug-in in a file).
    static juce::String getHelperKey (const juce::PluginDescription&);

    // How many helpers run now (not those that have gone, which their plug-ins' instances still hold, nor those that end).
    int getNumHelpers();

    // A helper goes once its last instance has closed, without anything waiting for it: asked to quit, and ended if it doesn't
    // soon, on a thread of the host's, beside the others that go (known bug 69: closing a project waited for each in turn).
    // How many are still ending; and that wait, for the tests.
    int getNumHelpersEnding() const;
    void waitForHelpersEnded() const;

private:
    struct Running;

    juce::KnownPluginList knownPlugins;
    juce::AudioPluginFormatManager formats;
    std::function<void()> scanCanceller;
    std::mutex scanProblemsLock;
    juce::StringArray scanProblems;
    juce::File program;
    int messageHangMilliseconds, audioHangMilliseconds, loadMilliseconds, quitMilliseconds;
    bool loads = true;

    // What the opening threads share with the message thread: the helpers, each kept alive by the instances in it (and
    // what is being opened in it), the plug-ins kept off, the jobs waiting for each helper, and whether the host goes (no
    // helper starts then).
    mutable std::mutex lock;
    std::map<juce::String, std::weak_ptr<Running>> helpers;
    juce::StringArray crashedWhileLoading;
    std::map<juce::String, std::deque<std::function<void (const BackgroundJobs::ShouldStop&)>>> waitingJobs;
    bool stopping = false;

    std::shared_ptr<BackgroundJobs> endings;  // the helpers that go, each ended there (shared with the helpers)
    std::unique_ptr<BackgroundJobs> openings; // the plug-ins opened in the background, beside each other

    std::shared_ptr<Running> getHelper (const juce::String& key, juce::String& error, bool again = false);
    void runJobs (const juce::String& helperKey, const BackgroundJobs::ShouldStop&); // a helper's, as they come
    void keepOffAfterCrash (const juce::PluginDescription&, juce::String& error);    // a plug-in that crashed as it loaded
    std::unique_ptr<sandbox::RemoteInstance> open (Running&, const juce::PluginDescription&, const juce::MemoryBlock& state,
                                                   double sampleRate, const sandbox::BlockChannel::Limits&, juce::String& error);
    void track (Running&, std::int64_t id, SandboxedInstance&); // told of the helper's notices and its loss
    void changeListenerCallback (juce::ChangeBroadcaster*) override { sendChangeMessage(); }
};
} // namespace phazite

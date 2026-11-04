// SPDX-License-Identifier: AGPL-3.0-or-later
// The helper program that hosts plug-ins for Phazite: started by Phazite with the name of a pipe to connect to, it
// shows no window of its own and runs until Phazite asks it to quit or goes.
#include "host/core/Host.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::host
{
class HostApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Phazite Plug-in Host"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        endQuietlyOnCrashes();
        host = std::make_unique<Host>();

        // Started by anything else than Phazite: nothing to do.
        if (! host->connect (commandLine))
            quit();
    }

    void shutdown() override { host = nullptr; }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<Host> host;
};
} // namespace phazite::host

START_JUCE_APPLICATION (phazite::host::HostApplication)

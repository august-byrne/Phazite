// SPDX-License-Identifier: AGPL-3.0-or-later
// The helper program's side of Phazite (docs/tracktion-migration.md, T3): the pipe Phazite made, which it connects to,
// Phazite's requests, each handled on the helper's message thread (where plug-ins are made, as their formats ask) and
// answered with the request's number, the instances it hosts with their editors (Editor.h), and what they change
// themselves, told to Phazite thirty times a second at most (a key Phazite takes over an editor at once). It quits when
// Phazite asks or the pipe goes (Phazite closed or crashed).
#pragma once

#include "Editor.h"
#include "Instance.h"
#include "sandbox/core/Messages.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <memory>

namespace phazite::host
{
// The plug-in formats the program hosts: the app's (src/host/app/Formats.cpp), or the tests' helper's, with their
// stand-ins (tests/host/TestFormats.cpp).
void addPluginFormats (juce::AudioPluginFormatManager&);

class Host final : private juce::Timer
{
public:
    Host();
    ~Host() override;

    // Connects to the pipe named by the program's command line ("--PhaziteSandbox:<pipe>"); false if there is none.
    bool connect (const juce::String& commandLine);

private:
    class Pipe;

    juce::AudioPluginFormatManager formats;
    std::unique_ptr<Pipe> pipe;
    std::map<std::int64_t, std::unique_ptr<Instance>> instances;
    std::map<std::int64_t, std::unique_ptr<Editor>> editors; // by their instances' IDs: each goes before its instance

    void received (const juce::MemoryBlock&);
    void handle (const juce::MemoryBlock&); // on the message thread
    void send (sandbox::MessageWriter&);
    void timerCallback() override;

    Instance* findInstance (std::int64_t id) const;
    void openInstance (sandbox::MessageReader&, sandbox::MessageWriter& answer);
    void prepareInstance (sandbox::MessageReader&, sandbox::MessageWriter& answer);
    void openEditor (std::int64_t id, sandbox::MessageWriter& answer);
    void placeEditor (sandbox::MessageReader&);
    void setEditorKeys (sandbox::MessageReader&);
};

// The helper process ends at once when a plug-in crashes it, rather than with the operating system's dialog. Its CPU
// time so far, in seconds, and its memory: what of it is in memory and no other process's, in bytes.
void endQuietlyOnCrashes();
double getProcessCpuSeconds();
std::int64_t getProcessMemoryBytes();

// A channel's limits as a request carries them.
sandbox::BlockChannel::Limits readLimits (sandbox::MessageReader&);
} // namespace phazite::host

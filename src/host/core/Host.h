// SPDX-License-Identifier: AGPL-3.0-or-later
// The helper program's side of Phazite (docs/tracktion-migration.md, T3): the pipe Phazite made, which it connects to,
// Phazite's requests, each handled on the helper's message thread (where plug-ins are made, as their formats ask) and
// answered with the request's number, and the instances it hosts. It quits when Phazite asks or the pipe goes (Phazite
// closed or crashed).
#pragma once

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

class Host final
{
public:
    Host();
    ~Host();

    // Connects to the pipe named by the program's command line ("--PhaziteSandbox:<pipe>"); false if there is none.
    bool connect (const juce::String& commandLine);

private:
    class Pipe;

    juce::AudioPluginFormatManager formats;
    std::unique_ptr<Pipe> pipe;
    std::map<std::int64_t, std::unique_ptr<Instance>> instances;

    void received (const juce::MemoryBlock&);
    void handle (const juce::MemoryBlock&); // on the message thread
    void reply (sandbox::MessageWriter&);

    void openInstance (sandbox::MessageReader&, sandbox::MessageWriter& answer);
    std::unique_ptr<juce::AudioPluginInstance> makePlugin (const juce::String& format, const juce::String& file, const juce::String& name,
                                                           double sampleRate, int blockSize, juce::String& error);
};

// The helper process's CPU time so far, in seconds, and its memory: what of it is in memory and no other process's, in bytes.
double getProcessCpuSeconds();
std::int64_t getProcessMemoryBytes();
} // namespace phazite::host

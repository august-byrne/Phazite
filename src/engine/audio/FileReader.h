// SPDX-License-Identifier: AGPL-3.0-or-later
// The thread that reads the audio files Phazite's engine plays (AudioFile.h), ahead of the audio thread, which never waits
// for the disk. It reads the files of the plans it is given, as long as any plan plays them.
#pragma once

#include "AudioFile.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <mutex>
#include <vector>

namespace phazite::engine
{
class FileReader : private juce::Thread
{
public:
    FileReader();
    ~FileReader() override;

    // Message thread: a file a plan plays, read from now on until nothing holds it any more.
    void add (const std::shared_ptr<AudioFile>&);

private:
    std::mutex mutex;
    std::vector<std::weak_ptr<AudioFile>> files;
    juce::WaitableEvent added;

    void run() override;
};
} // namespace phazite::engine

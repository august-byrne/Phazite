// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FileReader.h"

#include <algorithm>

namespace phazite::engine
{
FileReader::FileReader()
    : juce::Thread ("Phazite audio files")
{
    startThread (juce::Thread::Priority::high);
}

FileReader::~FileReader()
{
    // Woken, and waited for however long the chunk it reads takes: never ended by force, which can leave a lock it held
    // held for ever.
    signalThreadShouldExit();
    added.signal();
    stopThread (-1);
}

void FileReader::add (const std::shared_ptr<AudioFile>& file)
{
    {
        const std::lock_guard lock (mutex);
        const auto known = std::any_of (files.begin(), files.end(), [&file] (const auto& each) { return each.lock() == file; });

        if (! known)
            files.push_back (file);
    }

    added.signal();
}

void FileReader::run()
{
    while (! threadShouldExit())
    {
        std::vector<std::shared_ptr<AudioFile>> reading;

        {
            const std::lock_guard lock (mutex);
            std::erase_if (files, [] (const auto& file) { return file.expired(); });

            for (const auto& file : files)
                if (auto held = file.lock())
                    reading.push_back (std::move (held));
        }

        // A chunk of each file at a time, round them all, until none has anything to read; then a look every 20 ms (where
        // a longer file plays moves on), or at once when a file comes.
        auto readAny = false;

        for (const auto& file : reading)
            readAny = file->readNext() || readAny;

        reading.clear(); // the last hold on a file a plan has let go of goes here, off the audio thread

        if (! readAny)
            added.wait (20);
    }
}
} // namespace phazite::engine

// SPDX-License-Identifier: AGPL-3.0-or-later
// The hits (transients) in the audio files the song's clips play (docs/design.md, section 7): what the audio clip editor
// shows under a clip's warp markers, what a marker is pulled from, and what Quantize Audio moves to the grid
// (AudioClipModel.h), as Ableton Live finds a sample's transients as it loads. Found once for each file, by the sampler's
// onset detection (devices/sampler/SampleAnalysis.h), on a thread of their own at a low priority, and kept while the
// project context lives: by the file's path, size and last change, so that a file written again is looked at again.
#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace phazite
{
class Transients : private juce::Thread
{
public:
    Transients();
    ~Transients() override;

    // The hits in a file, in seconds of the file, in time order, once they are found; nothing until then, and asking starts
    // finding them. Message thread.
    std::optional<std::vector<double>> find (const juce::File&);

    // The same, found on the calling thread at once if they aren't yet (Quantize Audio's, which can't wait for them).
    std::vector<double> findNow (const juce::File&);

    // How they are found: a file's hits, in seconds, in as much of it as longestSeconds; none for a file that can't be read.
    static std::vector<double> analyse (const juce::File&);
    static constexpr double longestSeconds = 10.0 * 60.0;

private:
    std::mutex lock;
    std::map<juce::String, std::vector<double>> found; // by getKey()
    std::vector<juce::File> waiting;
    juce::WaitableEvent wake;

    static juce::String getKey (const juce::File&);
    void run() override;

    JUCE_DECLARE_NON_COPYABLE (Transients)
};
} // namespace phazite

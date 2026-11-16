// SPDX-License-Identifier: AGPL-3.0-or-later
// The hits (transients) in the audio files the song's clips play (docs/design.md, section 7): what the audio clip editor
// shows under a clip's warp markers, what a marker is pulled from, and what Quantize Audio moves to the grid
// (AudioClipModel.h), as Ableton Live finds a sample's transients as it loads. Found once for each file, by the sampler's
// onset detection (devices/sampler/SampleAnalysis.h), on a thread of their own at a low priority, and kept while the
// project context lives: by the file's path, size and last change, so that a file written again is looked at again.
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
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

    // The hits found already in a file, without asking for them; and hits found elsewhere (by Quantize Audio's work, which
    // reads with analyse() on its own thread, never touching this one, as it may outlive it), kept as found here. Message
    // thread.
    std::optional<std::vector<double>> lookUp (const juce::File&);
    void keep (const juce::File&, const std::vector<double>& hits);

    // How they are found, a file's hits, in seconds, in as much of it as longestSeconds; none for a file that can't be read;
    // nothing once shouldStop says so, which it asks between seconds of the file as it reads them, and in the onsets'
    // detection, setting progress from 0 to 1. What it keeps of the sound, mixed to one channel, is at most longestSeconds at keptRate (a file of a higher rate is
    // kept at a share of its rate), however long or fast the file says it is, held at once and no more; framesKept, if given,
    // hears how many frames it kept.
    static std::optional<std::vector<double>> analyse (const juce::File&, const std::function<bool()>& shouldStop = nullptr,
                                                       const std::function<void (float)>& progress = nullptr,
                                                       juce::int64* framesKept = nullptr);
    static constexpr double longestSeconds = 10.0 * 60.0;
    static constexpr double keptRate = 96000.0;

    // How many files the thread has analysed: each once, however often it is asked for meanwhile.
    int getNumAnalysed() const noexcept { return analysed.load(); }

private:
    std::mutex lock;
    std::map<juce::String, std::vector<double>> found; // by getKey()
    std::vector<juce::File> waiting;
    juce::File analysing; // the one the thread is finding them in now, which isn't queued again meanwhile
    juce::WaitableEvent wake;
    std::atomic<int> analysed { 0 };

    static juce::String getKey (const juce::File&);
    void run() override;

    JUCE_DECLARE_NON_COPYABLE (Transients)
};
} // namespace phazite

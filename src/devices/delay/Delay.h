// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's delay (roadmap step 2.10): echoes of the sound, in time with the song (a note value at its tempo) or a time
// in milliseconds, that come back with less of themselves each time (the feedback), each darker and thinner than the one
// before through a low and a high cut, as tape echoes are. Ping-pong bounces them between the left and the right. A
// change of the time glides to the new one, as a tape's would, rather than clicking. It puts out the echoes alone: its
// mix, which its thin plug-in applies, blends them with the sound.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"

#include <array>
#include <atomic>

namespace phazite::devices
{
class Delay final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteDelay";
    static constexpr double maximumSeconds = 5.0; // the longest echo: a bar at 48 BPM

    enum Parameter
    {
        sync, // the time is a note value at the song's tempo (on) or milliseconds (off)
        time, // the note value
        milliseconds,
        feedback,
        pingPong,
        lowCut,
        highCut,
        mix // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    static const DeviceInfo info;

    // How long the echo takes for the parameters' values at a tempo (in beats per minute, a beat being a quarter note):
    // the note value's length, or the milliseconds; at most maximumSeconds.
    static double getDelaySeconds (std::span<const float> parameters, double tempo);

    // The tempo it last played at, in beats per minute (devices/core/LiveValue.h), for its editor's note values.
    const LiveValue& getShownTempo() const noexcept { return shownTempo; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

private:
    double sampleRate = 44100.0;
    float timeSmoothing = 0.001f; // of the way to a new time, per sample
    float delaySamples = 1.0f;
    bool isFirstBlock = true;
    std::array<dsp::DelayLine, 2> lines;
    std::array<dsp::Filter, 2> lowCuts {}, highCuts {};
    std::atomic<double> tailSeconds { 2.0 };
    LiveValue shownTempo { 120.0f };
};
} // namespace phazite::devices

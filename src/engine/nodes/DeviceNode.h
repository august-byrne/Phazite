// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices in Phazite's engine (docs/tracktion-migration.md, T2): its runner (DeviceRunner.h), with
// its parameters' values set by hand and its switch, which the message thread sets one at a time without building
// anything, and which the audio thread reads as each block starts; the parameters its automation moves (the plan's), but
// those written by hand now; and the values it played, which controls show. Kept by the device's ID from one plan to the
// next, so that what it plays sounds on through an edit.
#pragma once

#include "DeviceRunner.h"

#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace phazite::engine
{
// A parameter of a device that its automation moves, by its index among the device's: the plan's.
struct ParameterCurve
{
    int parameter = 0;
    std::shared_ptr<const Curve> curve;
};

class DeviceNode
{
public:
    DeviceNode (const devices::DeviceInfo&, std::unique_ptr<devices::Processor>);

    DeviceRunner& getRunner() noexcept { return runner; }
    const DeviceRunner& getRunner() const noexcept { return runner; }
    const devices::DeviceInfo& getDeviceInfo() const noexcept { return runner.getDeviceInfo(); }

    // Message thread: a parameter's value set by hand (kept to its range), by its index; and the device's switch.
    void setValue (int parameter, float value) noexcept;
    float getValue (int parameter) const noexcept;
    void setOn (bool) noexcept;
    bool isOn() const noexcept { return on.load (std::memory_order_relaxed); }

    // Message thread: whether a parameter is written by hand now (automation's touch, latch and write modes), so that it
    // plays its value set by hand rather than its automation.
    void setWriting (int parameter, bool) noexcept;
    bool isWriting (int parameter) const noexcept;

    // Any thread: a parameter's value as the device played it last, and without its modulation.
    float getPlayedValue (int parameter) const noexcept;
    float getPlayedBaseValue (int parameter) const noexcept;

    // Audio thread: plays a block through the device, if it is on, its parameters moved by their automation (the plan's)
    // as the block says. One switched back on starts from silence, rather than from where it was when it was switched off.
    void process (juce::AudioBuffer<float>&, int startSample, int numSamples, DeviceRunner::Block,
                  std::span<const ParameterCurve> automation = {}) noexcept;

private:
    DeviceRunner runner;
    std::unique_ptr<std::atomic<float>[]> values, played, playedBase;
    std::unique_ptr<std::atomic<bool>[]> writing;
    std::vector<float> blockValues;                 // as a block starts, for the runner
    std::vector<DeviceRunner::Automated> automated; // the audio thread's, for each block: as long as there are parameters
    std::atomic<bool> on { true };
    bool wasOn = true; // the audio thread's
    static_assert (std::atomic<float>::is_always_lock_free);

    DeviceNode (const DeviceNode&) = delete;
    DeviceNode& operator= (const DeviceNode&) = delete;
};
} // namespace phazite::engine

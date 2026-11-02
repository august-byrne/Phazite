// SPDX-License-Identifier: AGPL-3.0-or-later
// One of Phazite's own devices in Phazite's engine (docs/tracktion-migration.md, T2): its runner (DeviceRunner.h), with
// its parameters' values set by hand and its switch, which the message thread sets one at a time without building
// anything, and which the audio thread reads as each block starts. Kept by the device's ID from one plan to the next, so
// that what it plays sounds on through an edit.
#pragma once

#include "DeviceRunner.h"

#include <atomic>
#include <memory>
#include <vector>

namespace phazite::engine
{
class DeviceNode
{
public:
    DeviceNode (const devices::DeviceInfo&, std::unique_ptr<devices::Processor>);

    DeviceRunner& getRunner() noexcept { return runner; }
    const devices::DeviceInfo& getDeviceInfo() const noexcept { return runner.getDeviceInfo(); }

    // Message thread: a parameter's value set by hand (kept to its range), by its index; and the device's switch.
    void setValue (int parameter, float value) noexcept;
    float getValue (int parameter) const noexcept;
    void setOn (bool) noexcept;
    bool isOn() const noexcept { return on.load (std::memory_order_relaxed); }

    // Audio thread: plays a block through the device, if it is on. One switched back on starts from silence, rather than
    // from where it was when it was switched off (docs/known-bugs.md, bug 5).
    void process (juce::AudioBuffer<float>&, int startSample, int numSamples, DeviceRunner::Block) noexcept;

private:
    DeviceRunner runner;
    std::unique_ptr<std::atomic<float>[]> values;
    std::vector<float> blockValues; // as a block starts, for the runner
    std::atomic<bool> on { true };
    bool wasOn = true; // the audio thread's
    static_assert (std::atomic<float>::is_always_lock_free);

    DeviceNode (const DeviceNode&) = delete;
    DeviceNode& operator= (const DeviceNode&) = delete;
};
} // namespace phazite::engine

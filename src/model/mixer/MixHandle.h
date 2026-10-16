// SPDX-License-Identifier: AGPL-3.0-or-later
// An effect's mix (Mixer.h: hasMix(), getMix(), setMix()) as a knob changes it: a drag is one undo step, "Change Mix",
// from its first move to its last (a Gesture, model/commands/internal/Undo.h), and a change outside a drag, such as a key
// press, is one of its own. It forgets a device that is deleted. The model hands handles out (MixerModel.h; its
// Tracktion side is internal/MixHandle.h).
#pragma once

#include <memory>

namespace phazite
{
class MixHandle
{
public:
    MixHandle(); // of nothing
    MixHandle (MixHandle&&) noexcept;
    MixHandle& operator= (MixHandle&&) noexcept;
    ~MixHandle(); // ends a gesture in progress, keeping its change

    bool isValid() const;
    float getValue() const; // 1 without a device

    void beginGesture();
    void setValue (float mix);
    void endGesture();

    struct Impl;
    explicit MixHandle (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;
};
} // namespace phazite

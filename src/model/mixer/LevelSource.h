// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's level as a meter reads it: polled from a timer on the message thread, never sent as change messages
// (docs/design.md, section 24, rule 6). It follows what it measures, which can come and go (a plain folder that
// becomes a bus, playback that starts again): refresh() before reading. Each read gives the loudest level a channel
// reached since the last read, in decibels. The model hands sources out (MixerModel.h; its Tracktion side is
// internal/LevelSource.h).
#pragma once

#include <memory>

namespace phazite
{
class LevelSource
{
public:
    static constexpr float silenceDb = -100.0f;

    LevelSource(); // of nothing
    LevelSource (LevelSource&&) noexcept;
    LevelSource& operator= (LevelSource&&) noexcept;
    ~LevelSource();

    void refresh(); // finds what it measures again, if that changed

    bool isMeasuring() const;             // there is something to measure now
    int getNumChannels() const;           // the channels its sound used last, 1 for mono, 0 for none
    float getAndClearLevel (int channel); // the loudest level since the last read, in decibels; silenceDb for none

    struct Impl;
    explicit LevelSource (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;
};
} // namespace phazite

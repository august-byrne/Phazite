// SPDX-License-Identifier: AGPL-3.0-or-later
// The song as Phazite's own engine plays it (docs/tracktion-migration.md, T2): compiled into plans (engine/core/Plan.h),
// with what each plan points at and sounds on from one to the next (each track's strip and channel, each of Phazite's
// devices) kept by the song's IDs, and made anew for another sample rate or block size. Values set by hand (a device's
// parameter or switch, a channel's volume, pan, mute and solo) reach what plays at once, without a new plan. A playback
// session has one for its project (PlaybackSession.h), and a render one of its own. Message thread.
#pragma once

#include "engine/core/Plan.h"
#include "model/base/internal/Tracktion.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>

namespace phazite
{
class SongPlayer
{
public:
    explicit SongPlayer (te::Edit&);

    // What the engine plays of the song now, at a rate and in blocks of up to so many samples: the tempo map; the loop and
    // the metronome, live (a render plays the song once, from its start, without the metronome); and its tracks of Phazite's
    // devices (plug-ins, containers, folders as buses, returns and sends come with T2's later steps) into the master.
    std::unique_ptr<engine::Plan> compile (double sampleRate, int maximumBlockSize, bool isRender);

    // Takes a change of the song that is a value set by hand into what plays at once; false for any other change, which
    // needs a new plan.
    bool takeValue (const juce::ValueTree& tree, const juce::Identifier& property);

    // What plays a device (by its ID), and a track's strip (by its ID; 0 for the master's), for what shows them (meters,
    // editors' displays); nullptr for none.
    engine::DeviceNode* findDevice (std::int64_t id) const;
    engine::StripState* findStrip (std::int64_t id) const;

private:
    te::Edit& edit;
    double preparedRate = 0.0;
    int preparedBlockSize = 0;

    std::map<std::int64_t, std::shared_ptr<engine::DeviceNode>> devices;
    std::map<std::int64_t, std::shared_ptr<engine::StripState>> strips; // the master's under 0
    std::map<std::int64_t, std::shared_ptr<engine::Delay>> delays;      // each track's into the master
    std::set<std::int64_t> used;                                        // the devices the plan being compiled plays
    bool lowLatency = false;                                            // as the plan being compiled sees it

    std::shared_ptr<engine::StripState> getStrip (std::int64_t id);
    std::shared_ptr<engine::DeviceNode> getDevice (const juce::ValueTree& deviceState);
    void addDevices (engine::Plan::Strip&, const juce::ValueTree& channelState);
    void setValues (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void loadSounds (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void setChannel (const juce::ValueTree& channelState, bool isMaster);
    void updateAudibility();
};
} // namespace phazite

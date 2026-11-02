// SPDX-License-Identifier: AGPL-3.0-or-later
// A project as Phazite's own engine plays it (docs/tracktion-migration.md, T2): the song compiled into what the engine
// plays (engine/core/Plan.h), given again as the song changes, at most once a turn of the message loop; and the transport
// the model's functions use while Phazite's engine plays the project (model/transport/internal/Playback.h), in the song's
// beats. ProjectContext makes one for each Edit it holds, which joins the app's PlaybackHost.
#pragma once

#include "engine/core/Plan.h"
#include "model/base/Beats.h"
#include "model/base/internal/Tracktion.h"

#include <memory>

namespace phazite
{
class PlaybackHost;

class PlaybackSession : private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    PlaybackSession (te::Edit&, PlaybackHost&);
    ~PlaybackSession() override;

    te::Edit& getEdit() const noexcept { return edit; }

    // Whether Phazite's engine plays this project now: the host is on, this is the project it plays, and Tracktion isn't
    // recording (until T2 step 9 records through Phazite's engine).
    bool isPlayingProject() const;

    // The transport, through the host's engine. The playhead is in the song's beats.
    bool isPlaying() const;
    void play();
    void stop();
    void locate (BeatPosition);
    BeatPosition getPlayhead() const;

    // Gives the engine the song as it is now, if this is the project it plays.
    void update();

private:
    te::Edit& edit;
    PlaybackHost& host;
    juce::ValueTree editState; // the song is its PHAZITE element, which a bare Edit gets when something is first written

    void changed (const juce::ValueTree& tree, const juce::ValueTree& child = {});
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { changed (tree); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { changed (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { changed (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { changed (parent); }
    void handleAsyncUpdate() override { update(); }

    JUCE_DECLARE_NON_COPYABLE (PlaybackSession)
};

// What Phazite's engine plays of a song, at a sample rate: its tempo map, its loop, and the metronome, which clicks live
// but never in a render.
struct PlanOptions
{
    double sampleRate = 44100.0;
    bool isRender = false;
};

std::unique_ptr<engine::Plan> compilePlan (const te::Edit&, PlanOptions);

// The session through which Phazite's engine plays an Edit now (isPlayingProject()), or nullptr while Tracktion's does.
PlaybackSession* findPlayingSession (const te::Edit&);
} // namespace phazite

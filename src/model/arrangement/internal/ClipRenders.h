// SPDX-License-Identifier: AGPL-3.0-or-later
// The renders that audio clips play stretched, pitched or reversed (AudioClips.h; roadmap step 2.18): made with Phazite's
// stretcher (devices/stretch/Stretch.h) into the song's cache folder (Song::getCacheFolder()), the project's Cache folder, as 32-bit WAV files
// at the source's rate. A render is never changed once made: other settings make another file, so undo plays the old
// one again at once.
#pragma once

#include "devices/stretch/Stretch.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
// Renders a file with some settings into another, on the calling thread: into a file beside it first, which becomes the
// render once it is whole. Returns false if the source can't be read or the render written, or shouldStop stopped it;
// true if the render is there, even if someone else made it meanwhile. framesWritten, if given, counts the frames written
// so far, for another thread to show how far it has come.
bool renderClipAudio (const juce::File& source, const stretch::Settings&, const juce::File& render,
                      const std::function<bool()>& shouldStop = {}, std::atomic<juce::int64>* framesWritten = nullptr);

// Renders, on the calling thread, every clip's render that isn't there yet, and has each new one played. What
// the project's ClipRenders does in the background; exports and freezes call it first, as a render that isn't there plays
// silence. Returns false if shouldStop stopped it or a render failed.
bool renderMissingClipAudio (const Song&, const std::function<bool()>& shouldStop = {});

// Deletes the renders in the song's cache folder that no clip plays, and any left half-made. When a project closes
// (model/mixer/internal/Freeze.h: purgeCache()).
void purgeClipRenders (const Song&);

//==============================================================================
// Makes the renders a song's clips need, in the background, while the project context holds it: each one that
// isn't there yet, a short while after the clips last changed (a drag of a clip's tempo makes no render for each value
// it passes), two at a time. When one is ready, the engine plays it, so that the clip sounds.
// One that no clip needs any more stops. One that fails (its source can't be read, or the cache folder written) is
// remembered and not tried again until its source changes (its date or its size), or, for one whose folder couldn't be
// written, until the folder can be (both looked at every two seconds while any failed: checkFailedAgain()), or the clip
// needs another render (other settings, or another cache folder) or needs it again after no clip did: its clips stay
// silent and say so (hasFailed(), getFailedClips()).
class ClipRenders : private juce::ValueTree::Listener,
                    private juce::Timer,
                    private juce::AsyncUpdater
{
public:
    explicit ClipRenders (const Song&);
    ~ClipRenders() override;

    // Whether any clip still waits for its render: those that failed wait for nothing.
    bool isRendering() const;

    // Whether a render failed and won't be tried again until what it is made from changes; and whether any did.
    bool hasFailed (const juce::File& render) const;
    bool hasFailures() const noexcept { return ! failed.empty(); }

    // The clips whose renders failed, as last found: again as the clips change and as failures are added or cleared, not
    // each time this is asked (the playlist asks while it shows any).
    const std::vector<ClipID>& getFailedClips() const noexcept { return failedClips; }

    // What its timer does every two seconds while any failed: tries again each whose source changed, or whose folder can
    // be written now. Called by a test, at once.
    void checkFailedAgain();

    // How many renders it has started, all told: for the tests that one isn't tried again.
    int getRendersStarted() const noexcept { return rendersStarted; }

    // How far the renders being made have come, since none was (for the status bar's bar): how many are being made or wait
    // their turn (clips that play the same render count once), and how much of all their frames is done, from 0 to 1.
    struct Progress
    {
        int renders = 0;
        double done = 0.0;
    };

    Progress getProgress() const;

    // How far one render has come, from 0 to 1, while it is being made or waits its turn; nothing otherwise.
    std::optional<double> getProgressOf (const juce::File& render) const;

private:
    // A render being made, as its job and the keeper share it: the switch that stops it, and its frames, written and all.
    struct Render
    {
        std::atomic<bool> stopped { false };
        std::atomic<juce::int64> written { 0 };
        juce::int64 length = 0;
    };

    static_assert (std::atomic<juce::int64>::is_always_lock_free);

    const Song& song;
    juce::ValueTree state; // the song's
    BackgroundJobs jobs;

    // The renders being made: never the jobs themselves, which go as they end, between any two calls here.
    std::map<juce::File, std::shared_ptr<Render>> running;

    // The frames of the renders made and being made since none was, those stopped left out, for getProgress().
    juce::int64 framesMade = 0, framesAll = 0;

    // The renders that failed, each with its source as it was then: whether it was there, its date and its size.
    struct Source
    {
        bool exists = false;
        juce::Time modified;
        juce::int64 size = 0;

        static Source of (const juce::File&);
        bool operator== (const Source&) const = default;
    };

    // And whether its folder couldn't be written as it failed.
    struct Failure
    {
        Source source;
        bool folderShut = false;
    };

    std::map<juce::File, Failure> failed;
    std::vector<ClipID> failedClips;
    int rendersStarted = 0;

    // A job that ended, by the jobs, for the message thread to have played: whether it failed (not stopped, and no render),
    // its source as it was when it began, and whether the folder of its render couldn't be written as it failed.
    struct Finished
    {
        juce::File render;
        bool failed = false;
        Source source;
        bool folderShut = false;
    };

    juce::CriticalSection finishedLock;
    std::vector<Finished> finished;

    void update();
    void findFailedClips();
    void jobFinished (Finished); // on its thread

    void timerCallback() override;
    void handleAsyncUpdate() override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;

    JUCE_DECLARE_NON_COPYABLE (ClipRenders)
};
} // namespace phazite

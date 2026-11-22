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
#include "model/base/internal/SongValues.h"
#include "model/engine/AudioFiles.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace phazite
{
// Renders a file with some settings into another, on the calling thread: into a file beside it first, which becomes the
// render once it is whole. Returns false if the source can't be read or the render written, or shouldStop stopped it;
// true if the render is there, even if someone else made it meanwhile. framesWritten, if given, counts the frames written
// so far, for another thread to show how far it has come.
bool renderClipAudio (const juce::File& source, const stretch::Settings&, const juce::File& render,
                      const std::function<bool()>& shouldStop = {}, std::atomic<juce::int64>* framesWritten = nullptr);

// A clip's render that isn't there yet, with what it is made from, and how many frames its source has (as the song's record
// of files says: getAudioFileInfo()).
struct ClipRenderToMake
{
    juce::File render, source;
    stretch::Settings settings;
    juce::int64 sourceLength = 0;
};

// The clips' renders that aren't there yet, each once, read from the song on the message thread: their sources asked about
// now, as the player made next asks about each file it doesn't know yet (getRenderFile()), so that each is named as that
// player will name it (a source written again within the second, under another).
std::vector<ClipRenderToMake> findMissingClipRenders (const Song&);

// Makes them, on the calling thread (any but the audio thread: nothing of the song is read), one after another, until
// shouldStop (asked between the stretcher's blocks) says stop, telling how far it has come, from 0 to 1 by their frames, as
// it asks. Returns those it made, or that someone else made meanwhile.
juce::Array<juce::File> makeClipRenders (const std::vector<ClipRenderToMake>&, const std::function<bool()>& shouldStop = {},
                                         const std::function<void (double)>& progress = {});

// For the tests: what is called on the thread that makes them (makeClipRenders()) each time the stretcher asks whether to
// stop, with what says whether to; none if not set. Set while none are made.
void setClipRenderStopSeam (std::function<void (const std::function<bool()>& shouldStop)>);

// Has renders newly made played: Phazite's engine plays them from its next plan. Message thread.
void announceClipRenders (const Song&, const juce::Array<juce::File>& made);

// All three on the calling thread, the message thread: every clip's render that isn't there yet made, and each new one
// played. What the project's ClipRenders does in the background; the tests call it before they render a song whose renders
// they need (a render, OfflineRender.h, makes them on its own thread, as each job's turn comes). Returns false if shouldStop
// stopped it or a render failed.
bool renderMissingClipAudio (const Song&, const std::function<bool()>& shouldStop = {});

// How many renders renderClipAudio() has made since the program started, on the message thread and on other threads: for
// the tests that a render makes them off the message thread.
struct ClipRendersMade
{
    int onMessageThread = 0, elsewhere = 0;
};

ClipRendersMade countClipRendersMade();

// Deletes the renders in the song's cache folder that no clip plays, and any left half-made. When a project closes
// (model/mixer/internal/Freeze.h: purgeCache()).
void purgeClipRenders (const Song&);

//==============================================================================
// Makes the renders a song's clips need, in the background, while the project context holds it: each one that
// isn't there yet, a short while after the clips last changed (a drag of a clip's tempo makes no render for each value
// it passes), or a source was found written again in its place (its render is named for what is on the disk:
// getRenderFile()), two at a time. When one is ready, the engine plays it, so that the clip sounds.
// One that no clip needs any more stops. One that fails (its source can't be read, or the cache folder written) is
// remembered and not tried again until its source changes (its date or its size, which names another render), or, for one
// whose folder couldn't be written, until the folder can be (both looked at every two seconds while any failed:
// checkFailedAgain()), or the clip needs another render (other settings, or another cache folder) or needs it again after
// no clip did: its clips stay silent and say so (hasFailed(), getFailedClips()).
class ClipRenders : private juce::ValueTree::Listener,
                    private SongValues::Listener,
                    private FileIdentityListener,
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

    // What its timer does every two seconds while any failed: tries again each whose source changed; and has the folders of
    // those whose folders couldn't be written looked at in the background, trying again, as that check ends, each whose
    // folder can be written now. Called by a test, at once.
    void checkFailedAgain();
    bool isCheckingFolders() const noexcept { return checkingFolders; }

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

    // The renders that failed, each with whether its folder couldn't be written as it failed. One whose source changed is
    // another render (getRenderFile()).
    struct Failure
    {
        bool folderShut = false;
    };

    std::map<juce::File, Failure> failed;
    std::vector<ClipID> failedClips;
    std::set<ClipID> failedTakenOut; // listed as failed, taken out since the list was found: listed again as they come back
    int rendersStarted = 0;

    // A job that ended, by the jobs, for the message thread to have played: whether it failed (not stopped, and no render),
    // and whether the folder of its render couldn't be written as it failed.
    struct Finished
    {
        juce::File render;
        bool failed = false;
        bool folderShut = false;
    };

    juce::CriticalSection finishedLock;
    std::vector<Finished> finished;
    std::vector<juce::File> foldersChecked; // by the check of folders, those that can be written (under finishedLock)
    bool folderCheckEnded = false;          // likewise
    bool checkingFolders = false;           // a check of folders is under way

    void update();
    void findFailedClips();
    void rendersMayChange();     // the clips, or what they play, changed: update() in a while
    void jobFinished (Finished); // on its thread

    void timerCallback() override;
    void handleAsyncUpdate() override;
    void fileIdentityChanged (const juce::File&) override; // a source may be written again: another render to make

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void clipsChanged (const std::vector<SongValues::ClipChange>&, const std::vector<TrackID>&) override; // SongValues.h
    void tempoMapChanged() override;                                                                      // SongValues.h

    // The chord lane (SongValues.h: chordLaneChanged()): nothing, as no clip's render is made of chords or keys; the tree
    // callbacks above leave what is in the song's CHORDLANE alone (isHeardAsKept(), as every listener of the song's root does)
    // and, unlike the other listeners, the CHORDLANE's own coming and going too (isChordLanePlaceholder()).
    static bool isChordLanePlaceholder (const juce::ValueTree& parent, const juce::ValueTree& child);

    // The sections (SongValues.h: sectionsChanged()): nothing, as no clip's render is made of them; the tree callbacks above
    // leave what is in the song's SECTIONS alone (isHeardAsKept()) and, as the chord lane's, the SECTIONS's own coming and
    // going too (isSectionsPlaceholder()), so that a section edit makes the renders look again at nothing.
    static bool isSectionsPlaceholder (const juce::ValueTree& parent, const juce::ValueTree& child);

    // Not the curves (SongValues.h: curvesChanged()): no render holds automation, and the tree's callbacks above leave the
    // curves' elements alone (isHeardAsKept(), through isHeardAsCurves()). Nor the MIDI links (midiLinksChanged()): no render
    // is made of them, and the tree's callbacks above leave what is in the song's MIDILINKS alone (isHeardAsMidiLinks()).

    JUCE_DECLARE_NON_COPYABLE (ClipRenders)
};
} // namespace phazite

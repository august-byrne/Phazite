// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing's insides (model/mixer/FreezeModel.h), for the model, the app and tests. A frozen track (Schema.h: trackFrozen)
// plays its frozen file in place of its clips and its frozen devices, those before the first that must hear another track
// live; the devices after them, its fader and its sends play on as they are. Phazite's engine renders the file
// (model/playback/internal/OfflineRender.h) into the song's cache folder, its project's Cache folder, so that a frozen
// track opens frozen. Freezing and unfreezing aren't undo steps.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/WholeState.h"
#include "model/mixer/FreezeModel.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <set>
#include <vector>

namespace phazite
{
// A track's frozen devices, whose sound its frozen file holds, in their order: its devices and containers up to the first
// that listens to a sidechain (or holds one that does), which must hear the other track's sound live. None for a folder or
// a return, which aren't frozen.
std::vector<DeviceID> getFrozenDevices (const Song&, TrackID);

// Where a track's frozen file is: in the song's cache folder, "Frozen", its ID and ".wav", a WAV file of 32-bit floats as
// Phazite's engine renders it, from the song's start.
juce::File getFrozenFile (const Song&, TrackID);

// Freezes a track with a file rendered from it, before its fader, through its frozen devices, which becomes its frozen file:
// the song's track is frozen (Schema.h: trackFrozen), and what plays the song plays the file. Not an undo step, but where
// it is a part of one that freezes the track as it does something else (Bounce in Place), through that undo manager.
void freezeFromFile (Song&, TrackID, const juce::File& rendered, juce::UndoManager* = nullptr);

// Plays a frozen track live again: the song's track isn't frozen. Not an undo step. Its frozen file stays until the cache
// is purged.
void unfreeze (Song&, TrackID);

// Whether a device's sound is in its track's frozen file: the track is frozen, and the device is one of its frozen devices,
// or in one of its frozen containers.
bool isFrozenDevice (const Song&, DeviceID);

// The tracks that play their frozen files, top to bottom.
std::vector<TrackID> getFrozenTracks (const Song&);

//==============================================================================
// As a project opens: the song's frozen tracks whose frozen files aren't in its cache folder any more play live, without
// an undo step.
void forgetMissingFrozenFiles (Song&);

// Deletes from the song's cache folder what nothing in the song uses: renders of clips that have changed since
// (model/arrangement/internal/ClipRenders.h), and the frozen files of tracks that aren't frozen, or are gone. When a
// project closes (when it opens, its clips haven't yet asked for the renders they use).
void purgeCache (Song&);

//==============================================================================
// Keeps a song's frozen tracks honest while the project context holds it: a frozen track whose file no longer holds what
// the song would play is unfrozen (by undo or redo too), so that what plays is always what is shown. What a file holds:
// the track's clips with their notes, its frozen devices with what they are set to, its modulators and macros, which can
// move them, their automation, and the song's tempos, as the file is in seconds. It compares that with a copy taken when
// the track was frozen (or came back frozen, by undo), or the project opened, so that notifications that change nothing
// leave it frozen, and so do changes of what plays after the file: the track's fader, its sends, its name.
//
// Every change is compared at the next turn of the message loop, or as the song is stored or a plan of it compiled, whichever
// comes first (Song::PendingWriter): a change the song's tree tells (a frozen device's value, its automation), as a tree's
// callback reads none of the song's clips (model/base/internal/SongValues.h: during an undo the tree is back before the clips
// are); and the clips, their notes and the tempo map, heard from the song's values (SongValues::Listener). A frozen track
// whose clip changed plays live from then, and no save, version or plan takes it frozen meanwhile. A track frozen keeps a
// copy of what it holds as it is frozen, which freezeFromFile() has this take at once (Song::writePending()), and else the
// later moment takes; one that came back frozen (by an undo or a redo, or moved: JUCE takes a track out and puts it in
// again) is compared with what its file was kept with as it was frozen, if this saw it frozen before. None of its tree
// callbacks reads the song's clips or their notes (SongValues.h).
class FreezeKeeper : private juce::ValueTree::Listener,
                     private SongValues::Listener,
                     private Song::PendingWriter,
                     private juce::AsyncUpdater
{
public:
    explicit FreezeKeeper (Song&);
    ~FreezeKeeper() override;

    // The song made another branch's in place by apply (model/versions/internal/VersionHistory.h: finishSwitch()), which this
    // doesn't watch, and which says whether it was. A track frozen there stays frozen only if its file holds what it plays
    // there, as far as this knows: what the file held as the track was last frozen (on any branch: a track's file is its
    // own, getFrozenFile(), whichever froze it), or as the project opened. Every other plays live.
    bool keepThrough (const std::function<bool()>& apply);

    // What a frozen track's file holds, as the song held it then: a copy of its parts' elements, the song's values
    // (model/base/internal/WholeState.h), its clips and their notes among them, and the song's tempos (TempoMapValue.h), which
    // put its notes and clips where they are in seconds (its time signatures put nothing there).
    struct Holds
    {
        juce::ValueTree parts;
        ValuesKept values;
        immer::flex_vector<TempoEntry> tempos;
    };

private:
    Song& song;
    juce::ValueTree state;            // the song's
    std::map<TrackID, Holds> frozen;  // what each frozen track's file holds
    std::map<TrackID, Holds> files;   // and each track's file, kept as it is unfrozen, as the file stays
    std::vector<ClipID> notesChanged; // the clips whose notes changed, compared at the next turn of the message loop
    std::set<TrackID> clipTracks;     // the frozen tracks whose clips changed, the same
    std::set<TrackID> toTake;         // the tracks frozen since, whose holdings the later moment takes (noteFrozenTracks())
    bool checking = false;

    // What the tree told (check()), for the next turn: the frozen tracks taken again (after an undo or a redo), these tracks
    // compared, every frozen track compared.
    struct Waiting
    {
        bool takeTracks = false, all = false;
        std::set<TrackID> tracks;
    };

    Waiting waiting;

    std::vector<TrackID> takeFrozenTracks (bool fromFiles = false);
    void noteFrozenTracks (bool cameIn); // the same from a tree's callback, which reads nothing of the song's clips
    void compare (TrackID);
    void check (const juce::ValueTree& changed, bool cameIn);
    void compareMarked(); // what waits: the clips', and what the tree told during an undo or a redo
    void forgetMarked();

    // The notes of a frozen track's clip, its clips, and the tempo map (every frozen track's), heard as the song's values
    // change: marked, and compared once the step is whole (a listener of the values reads nothing of the song as it is told:
    // SongValues.h). The chord lane (chordLaneChanged()): nothing, as no frozen track's file holds anything of chords or keys.
    // The tree's callbacks below leave what the areas tell alone (isHeardAsKept()).
    void clipNotesChanged (const std::vector<ClipID>&) override;
    void clipsChanged (const std::vector<SongValues::ClipChange>&, const std::vector<TrackID>& reordered) override;
    void tempoMapChanged() override;

    void handleAsyncUpdate() override { compareMarked(); }
    void writePending() override; // what is marked compared now, but during an undo or a redo (Song.h)

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (! isHeardAsKept (parent, child))
            check (parent, true);
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        if (! isHeardAsKept (parent, child))
            check (parent, false);
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override
    {
        if (! isHeardAsKept (parent))
            check (parent, false);
    }

    JUCE_DECLARE_NON_COPYABLE (FreezeKeeper)
};

//==============================================================================
// What a TrackRender runs while it lives, which its progress window shows: a freeze's or a bounce's (Freeze.cpp), or a
// version's bounce (model/versions/VersionsModel.h).
struct TrackRender::Impl
{
    virtual ~Impl() = default;
    virtual float getProgress() const = 0;
    virtual juce::String getTitle() const = 0;
    virtual std::optional<RenderWait> getWait() const { return {}; } // a render's (OfflineRender::findWait())
    virtual juce::String getGoOnName() const { return {}; }          // a second way out of its window but Cancel, if any
    virtual void goOn() {}
    virtual void cancel() {}                      // most cancel as they go (TrackRender::cancel())
    virtual bool isHold() const { return false; } // a wait the window holds its input for (TrackRender::isHold())
};
} // namespace phazite

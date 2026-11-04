// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing's insides (model/mixer/FreezeModel.h), for the model, the app and tests. A frozen track (Schema.h: trackFrozen)
// plays its frozen file in place of its clips and its frozen devices, those before the first that must hear another track
// live; the devices after them, its fader and its sends play on as they are. Phazite's engine renders the file
// (model/playback/internal/OfflineRender.h) into the song's cache folder, its project's Cache folder, so that a frozen
// track opens frozen. Freezing and unfreezing aren't undo steps.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <map>
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
// the song's track is frozen (Schema.h: trackFrozen), and what plays the song plays the file. Not an undo step.
void freezeFromFile (Song&, TrackID, const juce::File& rendered);

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
class FreezeKeeper : private juce::ValueTree::Listener
{
public:
    explicit FreezeKeeper (Song&);
    ~FreezeKeeper() override;

private:
    Song& song;
    juce::ValueTree state;                     // the song's
    std::map<TrackID, juce::ValueTree> frozen; // what each frozen track's file holds
    bool checking = false;

    void takeFrozenTracks();
    void compare (TrackID);
    void check (const juce::ValueTree& changed);

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override { check (parent); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override { check (parent); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { check (parent); }

    JUCE_DECLARE_NON_COPYABLE (FreezeKeeper)
};
} // namespace phazite

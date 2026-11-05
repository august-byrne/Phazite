// SPDX-License-Identifier: AGPL-3.0-or-later
// What differs between two songs (docs/versions.md: Differences): by track, the master and the song itself, in Phazite's
// own words, leaving out what is no change (model/base/internal/Schema.h: isUncountedProperty(), isUncountedElement()).
// Elements are matched by their keys (schema::getElementKey()): items by their IDs, wherever they moved. Plain functions of
// trees, for any thread.
#pragma once

#include "SongPieces.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <set>
#include <vector>

namespace phazite
{
// The values of the plug-ins' parameters then and now, if known (SongPieces.h): a plug-in whose knobs moved is changed,
// named by them ("Serum: Cutoff 40% → 55%"), as its state says nothing to anyone but the plug-in.
struct PluginValues
{
    const PluginParameterValues* before = nullptr;
    const PluginParameterValues* after = nullptr;
};

// Whether two elements hold the same, counted as a project's changes are: the same properties, and the same children by
// their keys, those with an order in the same order.
bool holdTheSame (const juce::ValueTree& a, const juce::ValueTree& b);

// What changed of one thing: a track (a folder or a return too), the master, or the song's own (its tempo, its sections,
// its chords, the order of its tracks).
struct SongChange
{
    enum class Where
    {
        track,
        master,
        song
    };

    enum class Kind
    {
        added,
        removed,
        changed
    };

    Where where = Where::song;
    Kind kind = Kind::changed;
    std::uint64_t trackID = 0; // the track's, or the master's
    juce::String name;         // as the song shows it: "Bass", "Track 3", "Master"; "Song" for the song's own
    juce::StringArray what;    // "volume", "12 notes", "Limiter", "+ Reverb", "2 clips added"

    bool operator== (const SongChange&) const = default;
};

// The changes from one song to another: its tracks in the song's order, then the tracks it doesn't have any more, the
// master, and the song's own. Nothing for a song that is the same.
std::vector<SongChange> findChanges (const juce::ValueTree& songBefore, const juce::ValueTree& songAfter, const PluginValues& = {});

// The parts of findChanges(), for what works out the changes of a few things at a time: one track's or the master's own
// (from its element then to now; invalid then for one added, now for one removed), without its automation; the song's own;
// and the devices whose automation changed (Schema.h: CURVE, by curveDevice), which each track's line names.
SongChange findTrackChange (const juce::ValueTree& before, const juce::ValueTree& after, const juce::ValueTree& songBefore,
                            const juce::ValueTree& songAfter, const PluginValues& = {});
SongChange findSongChange (const juce::ValueTree& songBefore, const juce::ValueTree& songAfter);
std::set<std::uint64_t> findAutomatedDevicesChanged (const juce::ValueTree& songBefore, const juce::ValueTree& songAfter);

// The IDs a track's (or the master's) element holds of what automation names: its devices', its containers', its channel's
// fader and VCA, its sends', its modulators' and its macros'. Not its tracks', which are lines of their own.
std::set<std::uint64_t> getAutomatableIDs (const juce::ValueTree& track);

// Every track's element in a song, folders' tracks after their folder, as the playlist shows them top to bottom, and the
// name it shows (Tracks.h: getTrackName()).
std::vector<juce::ValueTree> getTrackElements (const juce::ValueTree& song);
juce::String getTrackNameIn (const juce::ValueTree& song, const juce::ValueTree& track);

// A version's name while it has none of its own: what its changes are about, "Bass, Drums, + Strings".
juce::String summarise (const std::vector<SongChange>&);
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the
// conversion of its elements are ModulationValue.h's. It holds IDs (each modulator's and macro's). Message thread, but for
// whole state's parts, which read only the values they are given.
//
// An area under two kinds of owner, each named by its ID: a channel (a track, the master), and a container on a channel, the
// devices' area's (DevicesArea.h); so nothing of it follows an owner as it moves. A container that comes to another channel
// does so by a version's pass (ApplySong.cpp), which gives its macros their place. A container's macros keep their channel
// beside their container (ModulatorPlace), so that what is told of them names it without reading anything.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
class Song;

// What the values' listeners are told of modulation (Listener::modulationChanged()): once for each change of its value, each
// modulator and macro whose value differs between the two (immer's diff, which skips what the two share; one whose value is
// another alike, none), with where it was and is, the parts of it that changed and its value after; and each owner whose list
// holds those it held before in another order. It listens to modulation's area from its making to its end.
struct SongValues::ModulationTeller final : ValueArea<SongModulation>::Listener
{
    ModulationTeller (ValueArea<SongModulation>&, juce::ListenerList<SongValues::Listener>&);
    ~ModulationTeller() override;

    ValueArea<SongModulation>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongModulation& before, const SongModulation& after) override;

    JUCE_DECLARE_NON_COPYABLE (ModulationTeller)
};

// Modulation's part of the keeper (SongValues.h), outside the history or refused in a step, nothing while the history undoes
// or redoes. The one channel whose element the song's tree holds is the master (no track is in it: the tracks' safety net
// reads a TRACK's modulators, takeChannelModulatorsIn(), below):
//   (a) a CONTAINER added by itself to the master outside the history, with MACRO elements in it (the devices' safety net,
//       DevicesArea.h, which takes the container in after this; a container's makers make its macros as values, which its
//       inserter sets: Mixer.h, insertDevice()): each read as the song holds it (readHeldModulator()), set, taken out of the
//       container, and counted (countModulationAdopted());
//   (b) a MODULATOR or a MACRO added by itself to the master: refused in a step (a JUCE assertion: the step can't take it out
//       again, as its Undo, taking the tree's change back first, would find it gone; JUCE workaround W62
//       (docs/juce-workarounds.md)), and read in and taken out outside the history.
// A container that leaves the song (removed, ungrouped) leaves its macros to the step that took it out
// (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()), and a track's removal its channel's (eraseTrackHoldings()).
// And the adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::ModulationKeeper
{
    ModulationKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for every
    // element, whoever's it is): nothing, at once, but for what is modulation's (a lone container, modulator or macro on a
    // channel of the song's).
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // Every MODULATOR and MACRO element out of the tree the loader made consistent, once its reader read the song's into the area
    // as the song holds them (readHeldModulationIn(): ModulationValue.h, readHeldModulation(), each of which the loader left as
    // the song holds it: asserted; SongValues.cpp: adopt()).
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;

    JUCE_DECLARE_NON_COPYABLE (ModulationKeeper)
};

// Modulation's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song leads to
// an owner of modulators or macros (the master, a channel's container, from the song's element down: whole state writes each
// TRACK itself, WholeState.cpp: writeTracksIn(), asking each area of each); and a copy's TRACK, MASTER or CONTAINER given its
// modulators and macros from the values kept (a channel's own by its track's ID, the master's under 0, a container's by its
// ID), in their list's order, after its other children (a track's sends after them: the sends' area writes them after
// modulation, SendsArea.h). Nothing for another element.
bool leadsToModulation (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeModulationIn (juce::ValueTree& element, const SongState&);

// Modulation's reader of a song's tree (SongState.h: readSongTree(), a line of it): every modulator and macro of its tracks,
// its master and their containers into the state, as the song holds them; no tree changed. Any thread.
void readHeldModulationIn (const juce::ValueTree& song, SongState&);

// The MODULATOR and MACRO elements of a channel's element (a TRACK or the MASTER that comes into the song with them: the
// tracks' safety net, TracksArea.h) and of each CONTAINER on it, each read as the song holds one (ModulationValue.h:
// readHeldModulator(), at its owner's place) after the others of its owner; one of an ID the area holds already left out. How
// many it read.
int takeChannelModulatorsIn (SongModulation&, const juce::ValueTree& channel);

// Modulation's part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a TRACK's,
// the MASTER's or a CONTAINER's modulators and macros, the same in the same order, each the same value (its place too); true
// for another element.
bool holdSameModulationOf (const juce::ValueTree& element, const SongState&, const juce::ValueTree& otherElement, const SongState&);
} // namespace phazite

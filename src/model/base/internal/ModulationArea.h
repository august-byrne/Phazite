// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the
// conversion of its elements are ModulationValue.h's. It holds IDs (each modulator's and macro's). Message thread, but for
// whole state's parts, which read only the values they are given.
//
// An area under two kinds of owner: a channel (a TRACK, the MASTER), an element that stays in the song's tree, and a
// container on a channel, the devices' area's (DevicesArea.h). So its keeper follows a channel as it comes and goes (as the
// clips' keeper follows a track), with its containers' macros by their places; a container that comes to another channel
// does so by a version's pass (ApplySong.cpp), which gives its macros their place. A container's macros keep their channel
// beside their container (ModulatorPlace), so that what is told of them names it without reading anything.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <vector>

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

// Modulation's part of the keeper (SongValues.h), in the mode of the moment, nothing while the history undoes or redoes:
//   (a) a TRACK (alone, a folder with its tracks, the track list) or the MASTER taken out of the song: its channel's modulators
//       and the macros of each container on it (found by their places: the song's tree holds no container, the devices' area
//       does) remembered with its element, and erased; the same element back: given back, their place's track the element's
//       ID then, each macro keeping its container;
//   (b) a TRACK or the MASTER that comes into the song with MODULATOR or MACRO elements in it, its CONTAINERs' too (a safety
//       net: a container's makers make its macros as values, Mixer.h: MadeDevices; a test's): each read as the song holds it
//       (readHeldModulator()), set, taken out of the tree, and counted (countModulationAdopted());
//   (c) a MODULATOR or a MACRO added by itself to a channel of the song's: refused in a step (a JUCE assertion: the step can't
//       take it out again, as its Undo, taking the tree's change back first, would find it gone; JUCE workaround W62
//       (docs/juce-workarounds.md)), and read in and taken out outside the history.
// A container that leaves the song alone (removed, ungrouped) leaves its macros to the step that took it out
// (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()).
// And the adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::ModulationKeeper
{
    ModulationKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree, or taken out of one (the keeper's callbacks, but while the history
    // undoes or redoes, for every element, whoever's it is): nothing, at once, but for what is modulation's (a track, what
    // holds tracks, the master, a container, a modulator or a macro, of the song's).
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);
    void childRemoved (juce::ValueTree& parent, juce::ValueTree& child);

    // Every modulator and macro of the tree the loader made consistent into the area, outside the history, as the song holds
    // them (ModulationValue.h: readHeldModulation(), each of which the loader left as the song holds it: asserted), and every
    // MODULATOR and MACRO element out of the tree (SongValues.cpp: adopt()).
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    // An owner taken out of the song (a TRACK, the MASTER, a CONTAINER) with what it held as it went, in its list's order: given
    // back if the same element comes back, let go of once nothing else holds the element (the history let go of the step that
    // could bring it back) or it is back in the song by an undo, which brought them back itself.
    struct Remembered
    {
        juce::ValueTree owner {};
        std::vector<ModulatorValue> modulators {};
    };

    std::vector<Remembered> remembered;
    size_t sweepAt = 64;

    void remember (const juce::ValueTree& owner, SongModulation& next);
    bool giveBack (const juce::ValueTree& owner, const ModulatorPlace& place, SongModulation& next);
    void sweep();

    JUCE_DECLARE_NON_COPYABLE (ModulationKeeper)
};

// Modulation's part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song leads to
// an owner of modulators or macros (the track list, a track, a folder's tracks, the master, a channel's container, from the
// song's element down); and a copy's TRACK, MASTER or CONTAINER given its modulators and macros from the values kept (a
// channel's own by its track's ID, the master's under 0, a container's by its ID), in their list's order, after its other
// children (a track's sends after them: the sends' area writes them after modulation, SendsArea.h). Nothing for another
// element.
bool leadsToModulation (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeModulationIn (juce::ValueTree& element, const ValuesKept&);

// Modulation's part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a TRACK's,
// the MASTER's or a CONTAINER's modulators and macros, the same in the same order, each the same value (its place too); true
// for another element.
bool holdSameModulationOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
} // namespace phazite

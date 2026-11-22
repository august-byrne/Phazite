// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation's area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows), as far as it is
// built: the song's modulators and macros are still the song's tree's (Schema.h: MODULATOR and MACRO under a TRACK, the
// MASTER or a CONTAINER, with their MODULATES and a modulator's VALUES), and their values and the conversion are
// ModulationValue.h's. Here: modulation's part of the keeper, which tells the values' listeners what the tree tells of them
// (Listener::modulationChanged()), and what the unsaved check compares of them with the saved copy while the trees hold them.
// Message thread, but for the comparisons' readers, which read only the trees they are given.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SongValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace phazite
{
class Song;

// Modulation's part of the keeper (SongValues.h), while the song's tree holds modulation: for each of the tree's
// notifications under the song's modulators and macros (isHeardAsModulation()), and for each TRACK, CONTAINER, MASTER or
// track list that comes into the song or leaves it with modulators or macros in it, whatever the history does (an undo's and
// a redo's, a write's outside the history), the values' listeners told at once, in one call, each modulator and macro it
// changed (Listener::modulationChanged()): what changed worked out from the song's modulation as the listeners were last
// told it (told), against the elements the notification names read now, as the song's readers read them
// (ModulationValue.h: readModulatorValue(), readModulatorPlace()). A property, or a VALUES or a MODULATES changed, added,
// taken out or moved, of one modulator: that one read again, and its parts compared; a modulator or a macro added to its
// owner, taken out, moved among its owner's others, given another ID or come to be read or not (its type), and a channel or a
// container given another ID: the owner's children's IDs compared with its list (none of the others read), each that came
// read; and what leaves the song with its owners: each modulator of them that the listeners knew, taken out, without a read.
// A call that changes nothing tells nothing. Nothing of the song's values is read, nor anything of the song's tree but the
// elements the notification names, their owners and their children.
struct SongValues::ModulationKeeper
{
    ModulationKeeper (SongValues&, Song&);

    // The keeper's callbacks, each for every element, whoever's it is, in an undo or a redo too: nothing, at once, but for
    // what is modulation's.
    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childAdded (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childOrderChanged (const juce::ValueTree& parent);

    SongValues& values;
    SongModulation told; // the song's modulation as the listeners were last told it (at the start: as the song holds it)

    // What one notification changed, told in one call as it ends.
    struct Heard
    {
        std::vector<ModulationChange> changes;
        std::vector<ModulatorPlace> reordered;
    };

    void reread (const juce::ValueTree& modulator, Heard&);
    void resync (const juce::ValueTree& owner, Heard&);
    void resyncAllIn (const juce::ValueTree& element, Heard&);
    void forgetAllIn (const juce::ValueTree& element, Heard&);
    void tell (Heard&) const;

    JUCE_DECLARE_NON_COPYABLE (ModulationKeeper)
};

// What the unsaved check compares of modulation with the saved copy (model/project/Project.cpp) while the song's tree and the
// saved copy's hold it: a modulator's or a macro's element in a song's tree (PHAZITE), by its ID (the first that
// readModulationValue() reads; an invalid tree for none); and an owner's modulators' and macros' IDs in its element's order
// (a channel's, or a container's on it: ModulatorPlace), those readModulationValue() reads, none for an owner the tree
// hasn't. Reading only the trees given, no value whole.
juce::ValueTree findModulatorElement (const juce::ValueTree& song, ModulatorID);
std::vector<ModulatorID> readOwnerOrder (const juce::ValueTree& song, const ModulatorPlace& owner);
} // namespace phazite

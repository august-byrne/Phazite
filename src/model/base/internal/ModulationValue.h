// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's modulation as a value (roadmap step S5.20, modulation's area): each modulator and macro (Schema.h: MODULATOR,
// MACRO), by its ID, with what it modulates (MODULATES) and a modulator's values set by hand (its VALUES); each owner's in
// their order (a track's or the master's own, a container's macros); in Phazite's own types, with the helpers that alone
// write it, and the conversion between the elements and the value, the one place that reads and writes those elements by
// name. The song's tree is still the storage (modulation's first piece): the area's readers read the tree through this
// conversion (model/modulation/internal/Modulation.h: getModulationValue(), getModulatorValue()), and the song holds no
// area of it yet (SongValues.h).
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values). It includes the devices'
// modulators (devices/modulation/Modulators.h: a modulator's kind and parameters), which every layer may, and the parameter
// references (model/devices/ParameterRef.h), as Selection.h does. immer is the model's alone (tools/check-architecture.sh:
// immer-outside-model).
#pragma once

#include "devices/modulation/Modulators.h"
#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/devices/ParameterRef.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
// Where a modulator or a macro plays: its channel, a track's (a track's, a folder's or a return's, by its ID) or the
// master's (by a flag, never its ID, which a version applied sets anew: the MASTER is one by its type); and for a container's
// macro, its container too, on that channel. The owner whose list holds it is the container where it has one, else the
// channel (SongModulation: containers, channels).
struct ModulatorPlace
{
    TrackID track {};      // the TRACK it is on, or its container is on; none for the master's
    bool onMaster = false; // the MASTER's, or a container's on the master
    DeviceID container {}; // a container's macro's: the CONTAINER; none for a channel's own

    bool operator== (const ModulatorPlace&) const = default;
};

// What a modulator or a macro modulates (Schema.h: MODULATES): a parameter, by its device's ID and its own (ParameterRef.h),
// and how much, -1 to 1.
struct ModulationEntry
{
    ParameterRef parameter {};
    float amount = 0.0f;

    // Written out, not defaulted: a defaulted one would compare the amount with a floating-point ==, which GCC's
    // -Wfloat-equal refuses.
    bool operator== (const ModulationEntry&) const noexcept;
};

// A modulator or a macro (Schema.h: MODULATOR, MACRO) as the song holds it. A modulator has a kind (devices/modulation/
// Modulators.h), which a macro hasn't.
struct ModulatorValue
{
    ModulatorID id {};                             // modulatorID
    ModulatorPlace place {};                       // where it plays
    std::optional<devices::ModulatorKind> kind {}; // modulatorType: none for a macro

    // A modulator's: whether it is switched off (deviceOff); its parameters' values set by hand (VALUES), each by its index
    // among its kind's (devices::getModulatorParameters()), a choice by its number, as the song holds them (none at its
    // default, as Phazite's writers keep them; a song read as Phazite's code always read it may hold one at its default:
    // readModulatorValue()); and a step sequencer's steps (modulatorSteps), -1 to 1, without the zeros at the end.
    bool off = false;
    immer::map<std::uint32_t, float> values {};
    immer::flex_vector<float> steps {};

    // A macro's: its name (macroName) and its value, 0 to 1 (macroValue).
    juce::String name {};
    float value = 0.0f;

    // What it modulates, in the order its modulations were made (MODULATES).
    immer::flex_vector<ModulationEntry> modulations {};

    bool isMacro() const noexcept { return ! kind.has_value(); }

    // A modulator's parameter's value set by hand, by its index among its kind's: the value held, else the parameter's
    // default; 0 for a macro or an index its kind hasn't.
    float getParameterValue (int index) const;

    // What it modulates of a parameter, or nullptr: a pointer into this value, valid while it lives.
    const ModulationEntry* findModulation (const ParameterRef&) const;

    // The same lists and values without a look inside (ValueArea.h), the rest alike; and alike, compared.
    bool isSameAs (const ModulatorValue&) const noexcept;
    bool operator== (const ModulatorValue&) const;
};

// Every modulator and macro of the song: by its ID; and each owner's in its order, modulators and macros together, as
// they were made (the order a file holds them in): a channel's own by its track's ID (the master's under 0), a container's
// macros by its container's ID. An owner without any has no list. The helpers below are the only writers of the three, and
// keep them in step: holdsItsLists() says whether they are.
struct SongModulation
{
    immer::map<std::uint64_t, ModulatorValue> modulators {};
    immer::map<std::uint64_t, immer::flex_vector<ModulatorID>> channels {};
    immer::map<std::uint64_t, immer::flex_vector<ModulatorID>> containers {};

    // One by its ID, or nullptr: a pointer into this value, valid while it lives.
    const ModulatorValue* find (ModulatorID id) const { return modulators.find (id.getRaw()); }

    // An owner's list (the container's of a place that has one, else its channel's), or nullptr for one without any.
    const immer::flex_vector<ModulatorID>* findList (const ModulatorPlace& owner) const;

    // The same three without a look inside (ValueArea.h); and alike, compared.
    bool isSameAs (const SongModulation&) const noexcept;
    bool operator== (const SongModulation&) const;
};

// A modulator or a macro added, at a place in its owner's list (from 0, or -1 for after the others there), or changed: one
// whose owner is another leaves its old owner's list for the new one's (at the place given, or at its end); one whose owner
// stays keeps its place (a container's macro whose container moved to another channel too). An owner left without any loses
// its list.
SongModulation withModulator (const SongModulation&, ModulatorValue, int index = -1);

// One taken out, of its owner's list too (nothing for one that isn't there).
SongModulation withoutModulator (const SongModulation&, ModulatorID);

// An owner's list in another order: the same IDs (else nothing changes).
SongModulation withOwnerOrder (const SongModulation&, const ModulatorPlace& owner, immer::flex_vector<ModulatorID> order);

// Whether the three agree: each modulator and macro once in one list, the list its place names, and each list holding
// only those there, none of them empty. For the tests, and the assertions where it is cheap.
bool holdsItsLists (const SongModulation&);

//==============================================================================
// The conversion: the one place that reads and writes MODULATOR, MACRO and MODULATES elements and a MODULATOR's VALUES by
// name.

// A MODULATOR's kind as its type names it (Schema.h: modulatorType, one of preset::modulatorTypeNames); none for a type
// Phazite doesn't know (which the loader takes the modulator away for), and for any other element.
std::optional<devices::ModulatorKind> readModulatorKind (const juce::ValueTree& modulator);

// The place of a MODULATOR's or a MACRO's element in a song's tree, as its parent and its container's parent say (a track's
// ID as readID() reads one): what the conversion of a song's tree reads each with.
ModulatorPlace readModulatorPlace (const juce::ValueTree& element);

// A modulator or a macro read from its element at a place, as Phazite's code always read it (model/modulation/Modulation.cpp
// and its readers before the value): its ID as readID() reads one; a modulator's switch, every value its VALUES hold of a
// parameter its kind has (a choice by its name or its number, any other by its number: SavedValues.h, readSavedValue()),
// its steps each within -1 to 1 (0 for what is no finite number), as many as it has; a macro's name and value (0 for none);
// and every MODULATES, its device's ID as readID() reads one, its amount 0 for none. Numbers as the song reads one
// (SongText.h: readValueNumber()). None for an element that is neither a MACRO nor a MODULATOR of a kind Phazite plays,
// which every reader of the song left out. For a song whose tree is the storage, and for tests. Counted
// (countModulatorsRead()).
std::optional<ModulatorValue> readModulatorValue (const juce::ValueTree& element, const ModulatorPlace&);

// The same as the song holds it: what enters modulation's area (the adoption, the keeper, a version's modulators and a
// merge's choice). Its ID one the song can count on from (more than 0 and below schema::maxID), else none; a modulator's
// values each by its parameter's description (a choice by a name it has or its number, a number brought within its range
// and to its steps: devices::constrain(); none at its default, and none of what is no finite number); its steps within -1
// to 1, to a ten-thousandth (0 for what is no finite number), at most devices::Modulator::maxSteps, without the zeros at
// the end; a macro's value by its description (SongText.h: readDescribed(), 0 to 1, 0 for none); its modulations each of a
// parameter it names (a device's ID and a parameter's), its amount by its description and to a ten-thousandth, one of each
// parameter (the first stays). What else the element holds (a property or a child Phazite doesn't know) is no part of it.
// Counted (countModulatorsRead()).
std::optional<ModulatorValue> readHeldModulator (const juce::ValueTree& element, const ModulatorPlace&);

// A modulator's or a macro's element, new, as Phazite's writers write one (model/modulation/Modulation.cpp,
// model/devices/Parameters.cpp, model/mixer/Containers.cpp, model/presets/Presets.cpp): its ID always; a modulator's type
// always, its switch only while off, its VALUES only while it holds a value (each by its parameter's ID, a choice by its
// name: SavedValues.h, toSavedValue()), its steps only while it has one, each to a ten-thousandth, separated by spaces,
// without the zeros at the end; a macro's name always and its value only while not 0; then each modulation, in its order,
// its device's ID as text, its parameter and its amount to a ten-thousandth. Its VALUES before its MODULATES.
juce::ValueTree writeModulatorElement (const ModulatorValue&);

// Every modulator and macro of a song's tree (PHAZITE), read as readModulatorValue() reads one: each TRACK's (in the track
// list, folders' tracks too, in the song's order of its tracks) and the MASTER's, each channel's own and then those of each of
// its CONTAINERs, each owner's in its element's order; of two with one ID (which the loader leaves none of), the first. What
// reads the song's modulation while the tree is its storage (Modulation.h: getModulationValue()), and the tests.
SongModulation readModulationValue (const juce::ValueTree& song);

// What modulates a parameter in a song's tree (PHAZITE): each modulator's and macro's modulation of it as
// readModulatorValue() reads one (its ID, and the amount), in the order readModulationValue() reads the owners in, without
// reading any modulator whole (none counted: countModulatorsRead()). What a knob asks at each tick while the tree is the
// storage (Modulation.h: findModulationsOf()).
std::vector<std::pair<ModulatorID, float>> readModulationsOf (const juce::ValueTree& song, const ParameterRef&);

// A modulator or a macro with one of its own properties (Schema.h: modulatorID, modulatorType, deviceOff and modulatorSteps
// of a modulator; modulatorID, macroName and macroValue of a macro) given a value as readHeldModulator() reads one, or taken
// away (a void value: as if the element hadn't it). A modulator's type to another kind keeps the values its VALUES would
// hold by their names, those of parameters the new kind has. What a merge's clash on such a property sets; false, changing
// nothing, for a property it hasn't, and for a type Phazite doesn't know (or none), which no modulator holds.
bool setModulatorProperty (ModulatorValue&, const juce::Identifier& property, const juce::var& value);

// The IDs the song's modulation holds: the largest, below schema::maxID (IDsHeld.h: countHeldID()), 0 for none; and each
// modulator's and macro's (forEachIDHeld()), as what names one names it (a MIDI link of a macro's value or of a modulator's
// parameter).
std::uint64_t findLargestIDIn (const SongModulation&);
void forEachIDHeld (const SongModulation&, const std::function<void (std::uint64_t)>& each);

// Each ID the song's modulation names: each modulation's device's (a device's, a chain's, a fader's or a send's), once for
// each modulation, in no order. What finds what names an item that is gone.
void forEachIDNamed (const SongModulation&, const std::function<void (std::uint64_t)>& each);

// How many modulators and macros were read into values (readModulatorValue(), readHeldModulator()) since Phazite started, on
// any thread: for the test that a knob's paint and a modulator's slot read only the modulators they show
// (tests/PerformanceGateTests.cpp). Allocations can't tell it: immer takes its nodes from lists of freed ones.
std::uint64_t countModulatorsRead() noexcept;
} // namespace phazite

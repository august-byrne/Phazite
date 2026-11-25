// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's devices as values (its first piece): each device, container and chain
// (Schema.h: DEVICE, CONTAINER, CHAIN) by its ID as a DeviceValue, with where it is (DevicePlace), Phazite's own device's
// values set by hand (its VALUES) by the parameter's index and its sounds (its SOUNDS) by their index among its type's; every
// one of a song (SongDevices) with the lists that order them, and the helpers that alone write it; and the conversion between
// those elements and the values, the one place that reads and writes them by name. The values are the storage: the song holds
// them in its devices' area (SongValues.h: devices), which its readers read by reference (model/mixer/internal/Mixer.h:
// getDevicesValue(), findDevice()); the conversion reads what enters the area (a file's devices as the song is made of
// its state, a version's, a merge's choice), writes a whole copy's
// elements (DevicesArea.h: writeDeviceWhole()), and reads a copy's elements (a preset's, a version's compared).
//
// What the value covers, and what it doesn't. A DEVICE's deviceID, deviceType, deviceOff, deviceSidechain, pluginFormat to
// pluginInstrument and deviceMix (a plug-in's), and its VALUES and SOUNDS children (Phazite's own device's); a CONTAINER's
// deviceID, deviceOff, deviceMix, containerName, containerSplit and containerInstrument; a CHAIN's chainID, chainName,
// channelVolume, channelPan, chainCrossover and its key and velocity ranges; and the lists: a channel's devices and
// containers, a container's chains, a chain's devices, each in its element's order. Not in it, and left alone by the
// conversion (a written element holds none of them): a plug-in's VALUE children, its pluginState, pluginProgram,
// pluginFormatID and pluginVersion, and a chain's chainMute and chainSolo, which are written in the undo history and outside
// it alike and go to a keyed store of their own (a later piece); a device's window place (deviceWindowX, deviceWindowY), view
// state written outside the history alone, which goes to a value outside the history; a container's macros (MACRO), which are
// modulation's (ModulationValue.h); and what Phazite doesn't know.
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values). It includes the devices'
// descriptions (devices/core/DeviceInfo.h), which every layer may, and the containers' plain values (model/mixer/
// ContainerValues.h, a header of nothing but plain types, which includes nothing); its .cpp finds a type's description
// through model/devices/internal/Devices.h, a header as plain (it includes the devices' descriptions alone). immer is the
// model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/mixer/ContainerValues.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace phazite
{
class Song;

// Where a device, a container or a chain is: its channel, a track's by its ID (a track's, a folder's made a bus or a
// return's) or the master's by a flag, never its ID (which a version applied sets anew: the MASTER is one by its type); and
// for what is in a container, the container, and for a device in a chain, the chain. The list that holds it: a chain's for a
// device in one, a container's for a chain, else its channel's (SongDevices: chainDevices, chains, channels).
struct DevicePlace
{
    TrackID track {};      // the TRACK it is on, or its container is on; none for the master's
    bool onMaster = false; // the MASTER's, or in a container on the master
    DeviceID container {}; // a chain's container, and a device's in a chain
    DeviceID chain {};     // a device's in a chain

    bool operator== (const DevicePlace&) const = default;
};

// Which plug-in a device is (Schema.h: pluginFormat, pluginName, pluginMaker, pluginIdentifier, pluginUid, pluginInstrument),
// in Phazite's own types, no juce::PluginDescription (the plug-ins' module stays out of the model's base: Mixer.h's
// getPluginIdentity() makes one).
struct PluginIdentity
{
    juce::String format {}, name {}, maker {}, identifier {};
    int uid = 0;
    bool instrument = false;

    bool operator== (const PluginIdentity&) const = default;
};

// A device, a container or a chain as the song holds it: one type for the three, as they share the song's IDs and what names
// one (a curve, a lane, a modulation) names any of them by a DeviceID. Each field is its form's alone; the others keep their
// defaults.
struct DeviceValue
{
    enum class Form : std::uint8_t
    {
        own,       // one of Phazite's own devices, or a type a test registered (model/devices/internal/Devices.h)
        plugin,    // a plug-in (Schema.h: deviceIsPlugin)
        container, // a CONTAINER
        chain      // a container's CHAIN
    };

    DeviceID id {};        // deviceID, chainID
    DevicePlace place {};  // where it is
    Form form = Form::own; // what it is

    // A device's and a container's: whether it is switched off (deviceOff).
    bool off = false;

    // A device's (its own or a plug-in): the track its sidechain listens to (deviceSidechain), none for none.
    TrackID sidechain {};

    // Phazite's own device's: its type ID (deviceType); its parameters' values set by hand (VALUES), each by its index among
    // its description's parameters, a choice by its number (written by its name: SavedValues.h), as the song holds them (none
    // at its default, as Phazite's writers keep them; a song read as Phazite's code always read it may hold one at its
    // default: readDeviceValue()); and its sounds (SOUNDS), each by its index among its type's (getSoundPropertiesOf()),
    // none for none (an empty one is none).
    juce::String type {};
    immer::map<std::uint32_t, float> values {};
    immer::map<std::uint16_t, juce::String> sounds {};

    // A plug-in's: which it is.
    PluginIdentity plugin {};

    // A plug-in's and a container's: its mix (deviceMix), 0 to 1, 1 where its element has none.
    float mix = 1.0f;

    // A container's and a chain's: its name (containerName, chainName).
    juce::String name {};

    // A container's: what part of the sound each chain plays (containerSplit), and whether it is an instrument container
    // (containerInstrument).
    ContainerSplit split = ContainerSplit::layers;
    bool instrument = false;

    // A chain's: its volume in decibels and its pan (channelVolume, channelPan), 0 where its element has none; its crossover
    // in hertz (chainCrossover), none for its place's default (Containers.h: getDefaultCrossover()); and the notes it plays
    // (chainLowKey to chainHighVelocity).
    float volume = 0.0f;
    float pan = 0.0f;
    std::optional<float> crossover {};
    NoteRange keys = allKeys;
    NoteRange velocities = allVelocities;

    bool isPlugin() const noexcept { return form == Form::plugin; }

    // Whether it turns notes into sound: one of Phazite's own instruments (by its description), a plug-in that is one, an
    // instrument container.
    bool isInstrument() const;

    // One of Phazite's own device's parameters' value set by hand, by its index among its description's: the value held,
    // else the parameter's default; 0 for another form, a type Phazite doesn't know or an index its type hasn't.
    float getParameterValue (int index) const;

    // The same maps without a look inside (ValueArea.h), the rest alike; and alike, compared (the floats exactly), each map
    // by identity first, then by the entries where the two differ alone (countDeviceEntriesCompared()).
    bool isSameAs (const DeviceValue&) const noexcept;
    bool operator== (const DeviceValue&) const;
};

// Every device, container and chain of the song by its ID, and the lists that order them: a channel's devices and containers
// by its track's ID (the master's under 0), in their order (an instrument first); a container's chains by its ID; a chain's
// devices by its ID. An owner without any has no list (a container without chains, which no edit makes and a file may hold,
// a chain without devices). The helpers below are the only writers of the four, and keep them in step: holdsItsLists() says
// whether they are.
struct SongDevices
{
    immer::map<std::uint64_t, DeviceValue> devices {};
    immer::map<std::uint64_t, immer::flex_vector<DeviceID>> channels {};
    immer::map<std::uint64_t, immer::flex_vector<DeviceID>> chains {};
    immer::map<std::uint64_t, immer::flex_vector<DeviceID>> chainDevices {};

    // One by its ID, or nullptr: a pointer into this value, valid while it lives: keep the value in a local of its own, never
    // a temporary's.
    const DeviceValue* find (DeviceID id) const { return devices.find (id.getRaw()); }

    // The list that holds what is at a place (a chain's for a place with a chain, a container's for one with a container,
    // else its channel's), or nullptr for none.
    const immer::flex_vector<DeviceID>* findList (const DevicePlace&) const;

    // The same four without a look inside (ValueArea.h); and alike, compared.
    bool isSameAs (const SongDevices&) const noexcept;
    bool operator== (const SongDevices&) const;
};

// A device, a container or a chain added at a place in its list (from 0, or -1 for after the others there), or changed: one
// whose list is another leaves its old list for the new one's (at the place given, or at its end); one whose list stays keeps
// its place there. A list left empty goes. What it holds goes with it: a container's chains and their devices given its
// channel, a chain's devices its channel and container (holdsItsLists()).
SongDevices withDevice (const SongDevices&, DeviceValue, int index = -1);

// One taken out, of its list too, with what it holds: a container's chains and their devices, a chain's devices (nothing for
// one that isn't there).
SongDevices withoutDevice (const SongDevices&, DeviceID);

// A list in another order: the same IDs (else nothing changes). The list of what is at the place given (SongDevices::findList()).
SongDevices withListOrder (const SongDevices&, const DevicePlace& owner, immer::flex_vector<DeviceID> order);

// Whether the four agree: each item under its own ID, once in one list, the list its place names; each list holding only
// those there, none of them empty, each a list of an owner the value holds (a container's chains of a container, a chain's
// devices of a chain); a chain's place its container's channel with its container, a device's in a chain its chain's with its
// chain; no container in a container (nor a chain or a container in a chain); a device's form one of a device's. For the
// tests, and the assertions where it is cheap.
bool holdsItsLists (const SongDevices&);

//==============================================================================
// The conversion: the one place that reads and writes DEVICE, CONTAINER and CHAIN elements' own properties and a DEVICE's
// VALUES and SOUNDS by name (what else the elements hold, and whose it is: the top of this header). Numbers as the song reads
// one (SongText.h: readValueNumber(), readDescribed()), never a cast.

// The place of a DEVICE's, a CONTAINER's or a CHAIN's element in a song's tree, as its parents say (a track's ID as readID()
// reads one): its channel (its TRACK, or the MASTER), its container, its chain.
DevicePlace readDevicePlace (const juce::ValueTree& element);

// What an element is: a DEVICE of a plug-in (its type deviceIsPlugin) or else one of Phazite's own (whatever its type, known
// or not), a CONTAINER or a CHAIN; none for any other element.
std::optional<DeviceValue::Form> readDeviceForm (const juce::ValueTree& element);

// A device, a container or a chain read from its element at a place, as Phazite's code always read it (model/mixer/,
// model/devices/ before the value): its ID as readID() reads one; a DEVICE's switch and sidechain, and Phazite's own device's
// type, every value its VALUES hold of a parameter its description has (a choice by its name or its number, any other by its
// number: SavedValues.h, readSavedValue(), whatever it is, at its default too) and every sound its SOUNDS hold of a property its
// type has (getSoundPropertiesOf(), but an empty one); a plug-in's identity and mix (1 for none or what is no finite number); a
// CONTAINER's switch, mix, name, split (layers for none and any other word) and whether it is an instrument container; a CHAIN's
// name, its volume and pan (0 for none), its crossover (none for none) and its ranges (each end its number as an int truncates
// it, the range's end for none), each number as it is, in its range or not. None for an element that is none of the three, and
// for a DEVICE of a type Phazite doesn't know (nor a plug-in), which every reader of the song left out of what plays and the
// loader takes away. Called by the song's check (tests/SongCheck.cpp) and the tests alone, against readHeldDevice(). Counted
// (countDevicesReadAsValues()).
std::optional<DeviceValue> readDeviceValue (const juce::ValueTree& element, const DevicePlace&);

// The same as the song holds it: what enters the devices' area (a file's as the song is made of its state, a
// version's devices and a merge's choice). Its ID one the song can count on from (more than 0 and below schema::maxID), else
// none; Phazite's own device's values each by its parameter's description (SavedValues.h: readHeldSavedValue(): a choice by
// a name it has, or by its number where it is held as a number, any other number brought within its range and to its steps,
// none at its default, none of what is no finite number nor of a choice written as text that is none of its names); a mix, a
// volume, a pan and a crossover each by its description, within its range (SongText.h: readDescribed()), the same as none
// for what is no finite number; a range's ends each its description's, within its range, made whole. Everything else as
// readDeviceValue() reads it; what else the element holds is no part of it. Counted (countDevicesReadAsValues()).
std::optional<DeviceValue> readHeldDevice (const juce::ValueTree& element, const DevicePlace&);

// What readHeldDevice() reads of an element but Phazite's own device's values set by hand and sounds, which it leaves empty:
// its own properties as the song holds them, read without a map made, and not counted (countDevicesReadAsValues()): what the
// versions' pass reads of a version's item on a channel it doesn't take (model/versions/ApplySong.cpp), and whether an
// element is one the song holds (none for what readHeldDevice() reads none of).
std::optional<DeviceValue> readHeldDeviceFields (const juce::ValueTree& element, const DevicePlace&);

// A device's, a container's or a chain's element, new, as Phazite's writers write one (model/mixer/Mixer.cpp,
// Containers.cpp, model/devices/Parameters.cpp, model/presets/Presets.cpp), without what the value doesn't cover and without
// the items in it (a container's CHAINs and a chain's DEVICEs come from the lists): its ID always; a DEVICE's type always
// (deviceIsPlugin for a plug-in), its switch only while off and its sidechain only while it listens; a plug-in's format,
// name, maker, identifier and unique ID always and its instrument only while it is one, its mix only below 1; Phazite's own
// device's SOUNDS, then its VALUES, each only while it holds one, each sound by its property's name, each value by its
// parameter's ID (a choice by its name: SavedValues.h, toSavedValue()); a CONTAINER's name always, its switch only while off,
// its mix only below 1, its split only while not layers and its instrument only while it is one; a CHAIN's name always, its
// volume and pan only while not 0, its crossover only while it has one, and each end of its ranges only while not the
// range's end. Each number the float it is (SongText.h: writeValue() writes it in the fewest digits that read back as it).
juce::ValueTree writeDeviceElement (const DeviceValue&);

// A DEVICE's, a CONTAINER's or a CHAIN's element of a song's tree made to hold what the song holds of it, as Phazite writes
// that (writeDeviceElement() of what readHeldDevice() reads of it), each property compared as text (SongText.h:
// isSameValue()): of its own properties, each the value covers (the top of this header) set where it holds another text
// than the writer writes (a number's finer digits, "0.30000001" for a mix of 0.3; a switch as "yes"), added where the
// writer writes one it hasn't (a plug-in's or a chain's name, written empty), taken away where the writer writes none (one
// at its default: off="0", mix="1", instrument="0", a range's end); its VALUES and its SOUNDS replaced by those the writer
// writes, where they hold anything else (a value at its default, a value written otherwise, a sound of a property its type
// hasn't or an empty one, a VALUES or a SOUNDS left empty, a property or a child Phazite doesn't know in either), its
// the two in the order they are in; a plug-in's SOUNDS taken away, which it never plays. What the value doesn't cover stays as
// it is: the store's (a plug-in's state, program, format ID and version, its VALUE children; a chain's mute and solo), a
// window's place, the element's other children and a property Phazite doesn't know. Its numbers are within their ranges
// already (Project.h: makeConsistent(), keepNumbers()). Nothing for an element that holds nothing else, nor for one
// readHeldDevice() reads none of. Whether it changed. Allocates nothing for an element that needs no change (every
// element Phazite's writers wrote): its properties, its VALUES and its SOUNDS are looked through as they are, and only where
// its VALUES or its SOUNDS need writing again is it read whole (counted: countDevicesReadAsValues()). The loader's, for
// every device, container and chain (makeConsistent()), so that each of a song that went through it reads the same as
// readDeviceValue() reads it and as readHeldDevice() does, and is the element writeDeviceElement() writes of that.
bool keepDeviceAsHeld (juce::ValueTree element);

// Every device, container and chain of a song's tree (PHAZITE): each TRACK's (in the track list, a folder's before the tracks
// in it) and the MASTER's, each channel's DEVICEs and CONTAINERs in its element's order, each CONTAINER's CHAINs and each
// CHAIN's DEVICEs in theirs; of two with one ID (which the loader leaves none of), the first, and none of a type Phazite
// doesn't know; each read as readHeldDeviceFields() reads it, without its values set by hand and its sounds (not counted:
// countDevicesReadAsValues()): what the loader's check of the sidechains' loops reads of a file's tree (Sidechain.h:
// forgetSidechainLoopsIn()), never a device's values whole.
SongDevices readDevicesFields (const juce::ValueTree& song);

// A device, a container or a chain with one of its own properties given a value as readHeldDevice() reads one, or taken
// away (a void value: as if the element hadn't it): of its element (DEVICE, CONTAINER, CHAIN: a property the value covers of
// its form), or of a DEVICE's VALUES (a parameter's ID: Phazite's own device's) or SOUNDS (a property its type has). A type of
// Phazite's own changed to another keeps the values and sounds of the same names the new type has. What a merge's clash on
// such a property sets; false, changing nothing, for a property the value doesn't cover (the store's, a window's place, one
// Phazite doesn't know), an element the value's form hasn't, and a type Phazite doesn't know, which no device holds.
bool setDeviceProperty (DeviceValue&, const juce::Identifier& element, const juce::Identifier& property, const juce::var& value);

//==============================================================================
// One field of one element read as readDeviceValue() reads it, without reading the rest (none counted:
// countDevicesReadAsValues()): what reads one field of a copy's element (a preset's, a version's compared, the store's). Each of
// an element that isn't one it is on reads as one without it. (The conversion's readers of the other fields are its own, in
// DevicesValue.cpp.)
DeviceID readDeviceElementID (const juce::ValueTree& element); // a DEVICE's, a CONTAINER's (deviceID), a CHAIN's (chainID)
juce::String readDeviceType (const juce::ValueTree& device);   // deviceType, as written (deviceIsPlugin for a plug-in)
PluginIdentity readPluginIdentity (const juce::ValueTree& device);
bool readIsInstrument (const juce::ValueTree& element);              // DeviceValue::isInstrument()
juce::String readDeviceElementName (const juce::ValueTree& element); // a CONTAINER's (containerName), a CHAIN's (chainName)

// Phazite's own device's value set by hand of one of its parameters (its VALUES' property of the parameter's ID, as
// readSavedValue() reads it), the parameter's default for none, its name made a text, or taken as the caller holds it; and
// one of a SOUNDS' properties, empty for none (what the sounds' loaders take: model/devices/internal/DrumKit.h,
// SamplerSound.h, Wavetables.h).
float readDeviceParameterValue (const juce::ValueTree& device, const devices::ParameterInfo&);
float readDeviceParameterValue (const juce::ValueTree& device, const devices::ParameterInfo&, const juce::Identifier& id);
juce::String readSoundsProperty (const juce::ValueTree& sounds, const juce::Identifier& property);

// The IDs of what an owner's element lists, in its order (readDeviceElementID()): a TRACK's or the MASTER's DEVICEs and
// CONTAINERs, a CONTAINER's CHAINs, a CHAIN's DEVICEs; none for another element.
std::vector<DeviceID> readDeviceList (const juce::ValueTree& owner);

// Whether a song holds a device or a container with this ID, and with chainsToo a container's chain too (an invalid ID: no),
// without a value made of its devices and without an allocation: what the selection asks of the devices it names and of an
// automation point's owner (Selection.h), and what the readers outside the devices' own code ask of a device by its ID
// (Mixer.h: hasDevice(), which counts a chain). A look in the devices' area.
bool holdsDevice (const Song&, DeviceID, bool chainsToo = false);

//==============================================================================
// The properties of one of Phazite's own devices' SOUNDS (Schema.h), in their order, by which a device's value holds its
// sounds: a drum sampler's pads', each pad's layers' factory sound then file; a synth's oscillators', each one's table then
// file; a sampler's sound, file, slices and slices taken away. None for any other type. Made once for each type, for any
// thread. And a property's index among a type's, -1 for one it hasn't.
const std::vector<juce::Identifier>& getSoundPropertiesOf (std::string_view typeID);
int findSoundIndex (std::string_view typeID, const juce::Identifier& property);

// One of Phazite's own device's sounds by its property's name (SOUNDS), as its value holds it: empty for none, for a property
// its type hasn't, and for another form. A look in its sounds' map, nothing allocated but the text given back.
juce::String getDeviceSound (const DeviceValue&, const juce::Identifier& property);

//==============================================================================
// The IDs the song's devices hold and name (IDsHeld.h): the largest a device, a container or a chain holds, below
// schema::maxID, 0 for none (the song's count of IDs stays past it); each one's own (what a curve, a lane, a modulation, a
// MIDI link and a drum pad's track name, the last by the sampler's: Schema.h, trackPadDevice); and each track a device's
// sidechain listens to (deviceSidechain), what goes with the track.
std::uint64_t findLargestIDIn (const SongDevices&);
void forEachIDHeld (const SongDevices&, const std::function<void (std::uint64_t)>& each);
void forEachIDNamed (const SongDevices&, const std::function<void (std::uint64_t)>& each);

// How many devices, containers and chains were read into values whole (readDeviceValue(), readHeldDevice(),
// keepDeviceAsHeld()) since Phazite started, on any thread: for the test that a control's tick and a strip's slots read no
// device whole (tests/PerformanceGateTests.cpp). Not the plan's count of the devices whose values it reads again
// (SongPlayer::countDevicesRead()), which counts another reading. Allocations can't tell it: immer takes its nodes from lists
// of freed ones.
std::uint64_t countDevicesReadAsValues() noexcept;

// How many entries of devices' values set by hand and sounds were compared (DeviceValue::operator==(), each a pair of entries
// of one index) since Phazite started, on any thread: for the test that a step of a knob's drag compares a few, however many
// values its device holds (tests/PerformanceGateTests.cpp). Two maps that share their root compare none.
std::uint64_t countDeviceEntriesCompared() noexcept;
} // namespace phazite

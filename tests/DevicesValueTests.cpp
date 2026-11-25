// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's devices as values (model/base/internal/DevicesValue.h): a device,
// a container and a chain as a DeviceValue and the song's as SongDevices, their helpers, and the conversion between the
// elements and the values: the golden project's devices, damaged ones, a drum sampler with every field and sound set and
// every factory preset's device, each read the same by the lenient reader and as the song holds it once the song has
// opened, written as Phazite writes it and read back the same, the song's text the same written twice; and the devices'
// readers by ID, which read the elements through the conversion, keeping their results for the odd inputs a file can hold.
#include "GoldenFiles.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongState.h"
#include "model/base/internal/SongText.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/Devices.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
namespace devices = phazite::devices;
using phazite::DeviceID;
using phazite::DevicePlace;
using phazite::DeviceValue;
using phazite::SongDevices;
using phazite::TrackID;
using Form = DeviceValue::Form;

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

juce::String writeText (const juce::ValueTree& song)
{
    juce::MemoryOutputStream out;
    phazite::writeProjectText (out, song);
    return out.toString();
}

juce::ValueTree readGolden()
{
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    return juce::ValueTree::fromXml (*xml);
}

// Each DEVICE, CONTAINER and CHAIN of a song's tree, wherever it is (clips and automation aside).
void forEachDeviceElement (const juce::ValueTree& tree, const std::function<void (const juce::ValueTree&)>& visit)
{
    for (const auto& child : tree)
    {
        if (child.hasType (schema::CLIP) || child.hasType (schema::AUTOMATION))
            continue;

        if (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::CHAIN))
            visit (child);

        forEachDeviceElement (child, visit);
    }
}

// The properties the value covers of an element of its form (DevicesValue.h, at its top).
std::vector<const juce::Identifier*> getCoveredProperties (const juce::ValueTree& element)
{
    if (element.hasType (schema::CHAIN))
        return { &schema::chainID.getName(),      &schema::chainName.getName(),        &schema::channelVolume.getName(),
                 &schema::channelPan.getName(),   &schema::chainCrossover.getName(),   &schema::chainLowKey.getName(),
                 &schema::chainHighKey.getName(), &schema::chainLowVelocity.getName(), &schema::chainHighVelocity.getName() };

    if (element.hasType (schema::CONTAINER))
        return { &schema::deviceID.getName(),      &schema::deviceOff.getName(),      &schema::deviceMix.getName(),
                 &schema::containerName.getName(), &schema::containerSplit.getName(), &schema::containerInstrument.getName() };

    if (phazite::readDeviceForm (element) == Form::plugin)
        return { &schema::deviceID.getName(),         &schema::deviceType.getName(),       &schema::deviceOff.getName(),
                 &schema::deviceSidechain.getName(),  &schema::pluginFormat.getName(),     &schema::pluginName.getName(),
                 &schema::pluginMaker.getName(),      &schema::pluginIdentifier.getName(), &schema::pluginUid.getName(),
                 &schema::pluginInstrument.getName(), &schema::deviceMix.getName() };

    return { &schema::deviceID.getName(), &schema::deviceType.getName(), &schema::deviceOff.getName(), &schema::deviceSidechain.getName() };
}

// An element of a song that went through the loader read both ways the same, its value written as Phazite writes it and
// read back the same, and the written element the element: property for property of those the value covers, each the
// written one holds held as the same text and none other held, and the children the value covers (its SOUNDS and its
// VALUES) the written ones, each property for property as the same text, nothing else in them. What the
// value doesn't cover (the store's, a window's place, a property Phazite doesn't know, a plug-in's VALUEs) aside. Returns the
// value.
DeviceValue checkAsHeld (const juce::ValueTree& element)
{
    INFO (element.toXmlString().toStdString());
    const auto place = phazite::readDevicePlace (element);
    const auto lenient = phazite::readDeviceValue (element, place);
    const auto held = phazite::readHeldDevice (element, place);
    REQUIRE (lenient.has_value());
    REQUIRE (held.has_value());
    CHECK (*lenient == *held);
    CHECK (lenient->isSameAs (*lenient));

    const auto written = phazite::writeDeviceElement (*held);
    CHECK (phazite::readHeldDevice (written, place) == held);
    CHECK (phazite::readDeviceValue (written, place) == held);

    for (const auto* property : getCoveredProperties (element))
    {
        INFO (property->toString());
        REQUIRE (element.hasProperty (*property) == written.hasProperty (*property));

        if (written.hasProperty (*property))
            CHECK (phazite::isSameValue (element[*property], written[*property]));
    }

    // Every property the written one holds is one the value covers.
    for (int i = 0; i < written.getNumProperties(); ++i)
    {
        const auto name = written.getPropertyName (i);
        const auto covered = getCoveredProperties (element);
        CHECK (std::any_of (covered.begin(), covered.end(), [&name] (const juce::Identifier* each) { return *each == name; }));
    }

    // Its SOUNDS and its VALUES each the written one of its type (in the order they are in: Phazite's writers write either
    // first, and the loader keeps it).
    auto numChildren = 0;

    for (const auto& child : element)
        if (child.hasType (schema::SOUNDS) || child.hasType (schema::VALUES))
            ++numChildren;

    REQUIRE (numChildren == written.getNumChildren());

    for (int i = 0; i < written.getNumChildren(); ++i)
    {
        const auto kept = written.getChild (i);
        const auto child = element.getChildWithName (kept.getType());
        REQUIRE (child.isValid());
        CHECK (child.getNumProperties() == kept.getNumProperties());
        CHECK (child.getNumChildren() == 0);

        for (int p = 0; p < kept.getNumProperties(); ++p)
        {
            const auto name = kept.getPropertyName (p);
            REQUIRE (child.hasProperty (name));
            CHECK (phazite::isSameValue (child[name], kept[name]));
        }
    }

    // A loaded element is left as it is.
    auto copy = element.createCopy();
    CHECK_FALSE (phazite::keepDeviceAsHeld (copy));
    return *held;
}

// Every device of a song as it opened: each as checkAsHeld() checks it, the song's devices whole holding their lists, the
// same as the readers by ID read them, and the song's text the same written whole twice. How many there are.
int checkSongsDevices (const phazite::Song& song)
{
    phazite::test::checkSong (song);
    const auto whole = phazite::test::copyWholeSong (song);
    auto count = 0;

    forEachDeviceElement (whole,
                          [&count] (const juce::ValueTree& element)
                          {
                              checkAsHeld (element);
                              ++count;
                          });

    const auto all = phazite::getDevicesValue (song);
    CHECK (phazite::holdsItsLists (all));
    CHECK ((int) all.devices.size() == count);

    for (const auto& [raw, value] : all.devices)
    {
        const auto id = DeviceID::fromRaw (raw);
        REQUIRE (phazite::findDevice (song, id) != nullptr);
        CHECK (*phazite::findDevice (song, id) == value);
        CHECK (phazite::hasDevice (song, id));
        CHECK (phazite::isPlugin (song, id) == value.isPlugin());

        if (value.form != Form::chain)
            CHECK (phazite::isInstrument (song, id) == value.isInstrument());
    }

    // Each channel's list as getDevices() reads it, the master's under 0.
    for (const auto& [key, list] : all.channels)
    {
        const auto channel = key == 0 ? TrackID() : TrackID::fromRaw (key);
        CHECK (phazite::getDevices (song, channel) == std::vector<DeviceID> (list.begin(), list.end()));
    }

    // Saved and opened again: the same text.
    const auto again = phazite::makeSong (whole.createCopy()); // which takes what its values hold out of the tree it opens
    REQUIRE (again != nullptr);
    const auto differences = phazite::test::compareLines (writeText (whole), writeText (phazite::test::copyWholeSong (*again)), 10);
    CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n").toStdString());
    CHECK (phazite::getDevicesValue (*again) == all);
    return count;
}

DeviceValue makeOwn (std::uint64_t id, DevicePlace place, const char* type = devices::Equalizer::typeID)
{
    DeviceValue value;
    value.id = DeviceID::fromRaw (id);
    value.place = place;
    value.type = type;
    return value;
}

DeviceValue makeOfForm (std::uint64_t id, DevicePlace place, Form form)
{
    auto value = makeOwn (id, place);
    value.form = form;
    value.type = {};
    return value;
}

DevicePlace onTrack (std::uint64_t track)
{
    return { .track = TrackID::fromRaw (track) };
}

std::vector<DeviceID> listOf (const immer::flex_vector<DeviceID>* list)
{
    return list != nullptr ? std::vector<DeviceID> (list->begin(), list->end()) : std::vector<DeviceID>();
}

std::vector<DeviceID> ids (std::initializer_list<std::uint64_t> raw)
{
    std::vector<DeviceID> made;

    for (const auto each : raw)
        made.push_back (DeviceID::fromRaw (each));

    return made;
}

// A value of a parameter of one of Phazite's own devices that isn't its default: a choice's next, a number a quarter of its
// range on (or back), within its steps.
float makeOffDefault (const devices::ParameterInfo& info)
{
    for (const auto candidate :
         { info.defaultValue + 1.0f, info.defaultValue - 1.0f, info.defaultValue + 0.25f * (info.maximum - info.minimum),
           info.defaultValue - 0.25f * (info.maximum - info.minimum), info.maximum, info.minimum })
        if (const auto value = devices::constrain (info, candidate); ! phazite::isDefaultValue (info, value))
            return value;

    FAIL ("no value off the default of " << std::string (info.id));
    return info.defaultValue;
}
} // namespace

TEST_CASE ("Devices value: the golden project's devices read the same both ways, written as Phazite writes them, and its text the "
           "same written twice")
{
    const auto file = readGolden();
    auto inFile = 0;

    // As the file holds them, which Phazite wrote: read the same both ways before the song opens too.
    forEachDeviceElement (file,
                          [&inFile] (const juce::ValueTree& element)
                          {
                              const auto place = phazite::readDevicePlace (element);
                              CHECK (phazite::readDeviceValue (element, place) == phazite::readHeldDevice (element, place));
                              ++inFile;
                          });

    // Read as the song holds them (SongState.h: readSongTree(), of a copy the loader made consistent), before the song is
    // made of the file's tree.
    auto consistent = file.createCopy();
    phazite::makeConsistent (consistent);
    const auto all = phazite::readSongTree (consistent).devices;
    const auto song = phazite::makeSong (file);
    REQUIRE (song != nullptr);
    CHECK (checkSongsDevices (*song) == inFile);
    CHECK (inFile > 20);

    // What the file holds: a container's chains in their order, a chain's devices, the master's devices.
    CHECK (all == phazite::getDevicesValue (*song));
    const auto* layers = all.find (DeviceID::fromRaw (37));
    REQUIRE (layers != nullptr);
    CHECK (layers->form == Form::container);
    CHECK (layers->instrument);
    CHECK (layers->name == "Layers");
    CHECK (listOf (all.chains.find (37)) == ids ({ 38, 48, 51 }));
    const auto* chain = all.find (DeviceID::fromRaw (38));
    REQUIRE (chain != nullptr);
    CHECK (chain->keys == phazite::NoteRange { 24, 59 });
    CHECK (chain->place.container == DeviceID::fromRaw (37));
    const auto* sampler = all.find (DeviceID::fromRaw (49));
    REQUIRE (sampler != nullptr);
    CHECK (sampler->place.chain == DeviceID::fromRaw (48));
    CHECK (sampler->sounds.size() == 3);
    CHECK (sampler->values.size() == 3);
    const auto* drums = all.find (DeviceID::fromRaw (90));
    REQUIRE (drums != nullptr);
    CHECK (drums->sounds.size() == 3);
    CHECK (drums->sounds[(std::uint16_t) phazite::findSoundIndex (devices::DrumSampler::typeID, schema::getPadSound (36))] == "808 Kick");
    const auto* bands = all.find (DeviceID::fromRaw (53));
    REQUIRE (bands != nullptr);
    CHECK (juce::exactlyEqual (bands->mix, 0.5f));
    CHECK (bands->split == phazite::ContainerSplit::bands);
    const auto* crossed = all.find (DeviceID::fromRaw (63));
    REQUIRE (crossed != nullptr);
    REQUIRE (crossed->crossover.has_value());
    CHECK (juce::exactlyEqual (*crossed->crossover, 2000.0f));
    const auto* compressor = all.find (DeviceID::fromRaw (87));
    REQUIRE (compressor != nullptr);
    CHECK (compressor->sidechain == TrackID::fromRaw (3));
    const auto* plugin = all.find (DeviceID::fromRaw (112));
    REQUIRE (plugin != nullptr);
    CHECK (plugin->isPlugin());
    CHECK (plugin->plugin.identifier == "PhaziteTestEffect");
    const auto* reverb = all.find (DeviceID::fromRaw (36));
    REQUIRE (reverb != nullptr);
    CHECK (reverb->place.onMaster);
    CHECK (listOf (all.findList (reverb->place)).front() == reverb->id);
}

TEST_CASE ("Devices value: damaged devices open as the song holds them, the loader's one rule its held reader's, the same at every "
           "opening")
{
    // A song with every kind the model makes, then devices another program wrote into it: values at their defaults, of
    // parameters the type hasn't, a choice as its number's text, numbers past their ranges or none; sounds of properties the
    // type hasn't, empty ones and an empty SOUNDS; a plug-in with VALUES and SOUNDS; a device of a type Phazite hasn't; a
    // container without chains and one with an unknown split; a chain's ranges, volume and crossover past their ends; and a
    // chain with an instrument in a container of effects.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::test::setOwnInstrument (*song, tracks[0], devices::Synth::typeID);
    const auto reverbReturn = phazite::test::addReturnWithOwnEffect (*song, tracks[0], devices::Reverb::typeID);
    const auto container = phazite::addContainer (*song, tracks[1]);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (*song, container).front();
    phazite::test::addOwnDeviceToChain (*song, chain, devices::Equalizer::typeID);

    auto file = phazite::test::copyWholeSongWithDevice (
        *song, tracks[1],
        parse (R"(<DEVICE id="900001" type="phaziteReverb" off="1"><VALUES mix="0.25" size="2" unknown="3" decay="nan"/>)"
               R"(<SOUNDS sample="808 Bass"/></DEVICE>)"));
    const auto addTo = [&file] (TrackID channel, const juce::String& xml)
    {
        auto parent = file.getChildWithName (schema::MASTER);

        if (channel.isValid())
        {
            const std::function<juce::ValueTree (const juce::ValueTree&)> find = [&find, channel] (const juce::ValueTree& tree)
            {
                for (const auto& child : tree)
                {
                    if (child.hasType (schema::TRACK) && phazite::readID (child[schema::trackID]) == channel.getRaw())
                        return child;

                    if (const auto found = find (child); found.isValid())
                        return found;
                }

                return juce::ValueTree();
            };

            parent = find (file);
        }

        REQUIRE (parent.isValid());
        parent.appendChild (parse (xml), nullptr);
    };

    addTo (tracks[1], R"(<DEVICE id="900002" type="phaziteCompressor"><VALUES mode="1" threshold="-24" character="FET"/></DEVICE>)");
    addTo (tracks[1], R"(<DEVICE id="900003" type="phaziteSampler"><SOUNDS sample="" sampleFile="Audio/Hit.wav" pad36Sound="x"/>)"
                      R"(<VALUES/></DEVICE>)");
    addTo (tracks[1], R"(<DEVICE id="900004" type="phaziteSynth"><SOUNDS/></DEVICE>)");
    addTo (tracks[1], R"(<DEVICE id="900005" type="plugin" format="VST3" name="Gone" manufacturer="Nobody" identifier="gone.vst3" uid="7")"
                      R"( mix="2"><VALUES mix="0.5"/><SOUNDS sample="808 Bass"/><VALUE parameter="3" value="0.5"/></DEVICE>)");
    addTo (tracks[1], R"(<DEVICE id="900006" type="phaziteNothing"><VALUES mix="0.5"/></DEVICE>)");
    addTo ({}, R"(<CONTAINER id="900007" name="Empty"/>)");
    addTo ({}, R"(<CONTAINER id="900008" name="Odd" split="diagonal" mix="1"><CHAIN id="900009" name="One" volume="30" pan="-4")"
               R"( crossover="1e9" lowKey="-5" highKey="200" lowVelocity="0" highVelocity="127"><DEVICE id="900010" type="phaziteSynth"/>)"
               R"(<DEVICE id="900011" type="phaziteDelay"/></CHAIN></CONTAINER>)");
    addTo (reverbReturn, R"(<DEVICE id="900012" type="phaziteEq" mix="0.5"/>)");

    const auto opened = phazite::makeSong (file);
    REQUIRE (opened != nullptr);
    const auto count = checkSongsDevices (*opened);
    CHECK (count == (int) phazite::getDevicesValue (*song).devices.size() + 11);

    // The loader's rule, named: a value at its default goes (the reverb's mix), and one of a parameter the type hasn't, and one
    // that is no number; a choice as its number's text goes; a sound of a property the type hasn't goes, and an empty one; an
    // empty VALUES or SOUNDS goes; a plug-in's VALUES and SOUNDS go, its VALUE stays (the store's).
    const auto reverb = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900001));
    REQUIRE (reverb.isValid());
    CHECK_FALSE (reverb.getChildWithName (schema::VALUES).hasProperty ("mix"));
    CHECK_FALSE (reverb.getChildWithName (schema::VALUES).hasProperty ("unknown"));
    CHECK_FALSE (reverb.getChildWithName (schema::VALUES).hasProperty ("decay"));
    CHECK (reverb.getChildWithName (schema::VALUES).hasProperty ("size"));
    CHECK_FALSE (reverb.getChildWithName (schema::SOUNDS).isValid()); // a reverb has no sounds
    const auto compressor = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900002));
    CHECK_FALSE (compressor.getChildWithName (schema::VALUES).hasProperty ("mode"));
    CHECK_FALSE (compressor.getChildWithName (schema::VALUES).hasProperty ("threshold"));
    CHECK (compressor.getChildWithName (schema::VALUES)["character"].toString() == "FET");
    const auto sampler = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900003));
    CHECK_FALSE (sampler.getChildWithName (schema::VALUES).isValid());
    CHECK (sampler.getChildWithName (schema::SOUNDS).getNumProperties() == 1);
    CHECK (sampler.getChildWithName (schema::SOUNDS)[schema::samplerSampleFile].toString() == "Audio/Hit.wav");
    CHECK_FALSE (phazite::test::copyDevice (*opened, DeviceID::fromRaw (900004)).getChildWithName (schema::SOUNDS).isValid());
    const auto plugin = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900005));
    CHECK_FALSE (plugin.getChildWithName (schema::VALUES).isValid());
    CHECK_FALSE (plugin.getChildWithName (schema::SOUNDS).isValid());
    CHECK (plugin.getChildWithName (schema::VALUE).isValid());

    // The readers by ID, for what the loader leaves: a plug-in that isn't installed, its mix within its range; a device of a
    // type Phazite hasn't, gone; a container without chains, and one whose split is no word of its; a chain's numbers within
    // their ranges; an instrument in a container of effects' chain; a device on the master, a return and a folder.
    const auto gone = DeviceID::fromRaw (900005);
    CHECK (phazite::isPlugin (*opened, gone));
    CHECK (phazite::opensWindow (*opened, gone));
    CHECK (phazite::getDeviceName (*opened, gone) == "Gone");
    CHECK (phazite::hasMix (*opened, gone));
    CHECK (juce::exactlyEqual (phazite::getMix (*opened, gone), 1.0f));
    CHECK_FALSE (phazite::isInstrument (*opened, gone));
    CHECK (phazite::getPluginIdentity (phazite::test::copyDevice (*opened, gone)).uniqueId == 7);
    CHECK_FALSE (phazite::hasDevice (*opened, DeviceID::fromRaw (900006)));
    CHECK (phazite::findDevice (*opened, DeviceID::fromRaw (900006)) == nullptr);

    const auto empty = DeviceID::fromRaw (900007);
    CHECK (phazite::isContainer (*opened, empty));
    CHECK (phazite::getChains (*opened, empty).empty());
    CHECK (phazite::getContainerDevices (*opened, empty).empty());
    CHECK_FALSE (phazite::canUngroupContainer (*opened, empty));
    CHECK (phazite::getDeviceChannel (*opened, empty) == phazite::getMasterID (*opened));
    CHECK (phazite::findDevice (*opened, empty)->place.onMaster);

    const auto odd = DeviceID::fromRaw (900008), oddChain = DeviceID::fromRaw (900009);
    CHECK (phazite::getContainerSplit (*opened, odd) == phazite::ContainerSplit::layers);
    CHECK (juce::exactlyEqual (phazite::getMix (*opened, odd), 1.0f));
    CHECK (juce::exactlyEqual (phazite::getChainVolume (*opened, oddChain), 6.0f));
    CHECK (juce::exactlyEqual (phazite::getChainPan (*opened, oddChain), -1.0f));
    CHECK (phazite::getChainKeys (*opened, oddChain) == phazite::NoteRange { 0, 127 });
    CHECK (phazite::getChainVelocities (*opened, oddChain) == phazite::NoteRange { 1, 127 });
    CHECK (phazite::getChainDevices (*opened, oddChain) == ids ({ 900010, 900011 }));
    CHECK (phazite::getChainInstrument (*opened, oddChain) == DeviceID::fromRaw (900010));
    CHECK (phazite::isInstrument (*opened, DeviceID::fromRaw (900010)));
    CHECK (phazite::findChainOf (*opened, DeviceID::fromRaw (900011)) == oddChain);
    CHECK (phazite::findContainerOf (*opened, DeviceID::fromRaw (900011)) == odd);
    const auto* oddValue = phazite::findDevice (*opened, oddChain);
    REQUIRE (oddValue != nullptr);
    CHECK (oddValue->place == DevicePlace { .onMaster = true, .container = odd });

    const auto* onReturn = phazite::findDevice (*opened, DeviceID::fromRaw (900012));
    REQUIRE (onReturn != nullptr);
    CHECK (onReturn->place == onTrack (reverbReturn.getRaw()));
    CHECK (juce::exactlyEqual (onReturn->mix, 1.0f)); // an own device's mix is its VALUES' "mix"
    CHECK (phazite::getDeviceChannel (*opened, onReturn->id) == reverbReturn);

    // A device on a folder made a bus.
    const auto folder = phazite::insertFolder (*opened, TrackID(), nullptr);
    const auto onFolder = phazite::test::addOwnDevice (*opened, folder, devices::Equalizer::typeID);
    CHECK (phazite::findDevice (*opened, onFolder)->place == onTrack (folder.getRaw()));
    CHECK (phazite::getDevices (*opened, folder) == std::vector<DeviceID> { onFolder });
    checkSongsDevices (*opened);
}

TEST_CASE ("Devices value: explicit defaults and long numbers another program wrote open as Phazite writes them, the same at every "
           "opening")
{
    // Each of the three elements with its own properties at their defaults written out (off="0", mix="1",
    // instrument="0", a range's ends, a range's end past it, brought to it), and a mix, a chain's volume, pan and crossover
    // in more digits than their floats hold: the loader's rule writes each covered property as Phazite's writer does (none
    // at its default, each number in the fewest digits that read back as its float), and leaves what the value doesn't
    // cover and the store does (a plug-in's state, a chain's mute) and a window's place as they were; a property Phazite
    // doesn't know on a DEVICE, a CONTAINER or a CHAIN is dropped as the song opens (the song holds its devices as values).
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    auto file = phazite::test::copyWholeSongWithDevice (
        *song, track,
        parse (R"(<DEVICE id="900101" type="plugin" format="VST3" name="P" manufacturer="M" identifier="p.vst3" uid="3" off="0")"
               R"( instrument="0" sidechain="0" mix="0.30000001" state="AAAA" windowX="10" windowY="20" unknown="kept"/>)"));
    auto master = file.getChildWithName (schema::MASTER);
    REQUIRE (master.isValid());
    master.appendChild (parse (R"(<DEVICE id="900102" type="phaziteEq" off="0" sidechain="0" unknown="kept"/>)"), nullptr);
    master.appendChild (parse (R"(<CONTAINER id="900103" name="Layers" off="0" mix="1" instrument="0" unknown="kept">)"
                               R"(<CHAIN id="900104" name="A" volume="0" pan="0" lowKey="-5" highKey="127" lowVelocity="1")"
                               R"( highVelocity="200" mute="1"><DEVICE id="900105" type="phaziteDelay"/></CHAIN></CONTAINER>)"),
                        nullptr);
    master.appendChild (parse (R"(<CONTAINER id="900106" name="Bands" split="bands" mix="0.30000001">)"
                               R"(<CHAIN id="900107" name="Low"/><CHAIN id="900108" name="High" volume="-3.10000001")"
                               R"( pan="0.30000001" crossover="1234.5600001"/></CONTAINER>)"),
                        nullptr);

    const auto opened = phazite::makeSong (file);
    REQUIRE (opened != nullptr);
    checkSongsDevices (*opened);

    // Each as Phazite writes it: the defaults gone, the numbers in their floats' fewest digits.
    const auto plugin = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900101));
    REQUIRE (plugin.isValid());

    for (const auto* gone : { "off", "instrument", "sidechain" })
        CHECK_FALSE (plugin.hasProperty (gone));

    CHECK (phazite::writeValue (plugin[schema::deviceMix]) == "0.3");
    CHECK (plugin[schema::pluginState].toString() == "AAAA"); // the store's
    CHECK ((int) plugin[schema::deviceWindowX] == 10);        // a window's place
    CHECK_FALSE (plugin.hasProperty ("unknown"));             // Phazite doesn't know it: dropped

    const auto own = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900102));
    CHECK_FALSE (own.hasProperty (schema::deviceOff));
    CHECK_FALSE (own.hasProperty (schema::deviceSidechain));
    CHECK_FALSE (own.hasProperty ("unknown"));

    const auto layers = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900103));

    for (const auto* gone : { "off", "mix", "instrument" })
        CHECK_FALSE (layers.hasProperty (gone));

    CHECK_FALSE (layers.hasProperty ("unknown"));
    const auto chainA = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900104));

    for (const auto* gone : { "volume", "pan", "lowKey", "highKey", "lowVelocity", "highVelocity" })
        CHECK_FALSE (chainA.hasProperty (gone));

    CHECK (chainA.hasProperty (schema::chainMute)); // the store's

    const auto bands = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900106));
    CHECK (phazite::writeValue (bands[schema::deviceMix]) == "0.3");
    const auto high = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900108));
    CHECK (phazite::writeValue (high[schema::channelVolume]) == "-3.1");
    CHECK (phazite::writeValue (high[schema::channelPan]) == "0.3");
    CHECK (phazite::writeValue (high[schema::chainCrossover]) == "1234.56");

    // Saved, opened, saved again: the same text twice.
    const auto first = writeText (phazite::test::copyWholeSong (*opened));
    const auto again = phazite::makeSong (phazite::test::copyWholeSong (*opened));
    REQUIRE (again != nullptr);
    CHECK (writeText (phazite::test::copyWholeSong (*again)) == first);
}

TEST_CASE ("Devices value: a drum sampler with every field and every sound set reads the same both ways, and what reading it costs")
{
    using devices::DrumSampler;
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    phazite::test::OwnValues values;

    for (const auto& info : DrumSampler::info.parameters)
        values.emplace_back (juce::String (info.id.data(), info.id.size()), makeOffDefault (info));

    REQUIRE (values.size() == 3329);
    const auto drums = phazite::test::setOwnInstrument (*song, track, DrumSampler::typeID, values);
    const auto& properties = phazite::getSoundPropertiesOf (DrumSampler::typeID);
    REQUIRE (properties.size() == 1024);
    CHECK (phazite::getSoundProperties (DrumSampler::info).size() == 1024);

    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
    {
        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
        {
            phazite::test::writeDeviceSoundByHand (*song, drums, schema::getPadSound (pad, layer), "808 Kick");
            phazite::test::writeDeviceSoundByHand (*song, drums, schema::getPadSoundFile (pad, layer),
                                                   "Audio/Pad " + juce::String (pad) + " " + juce::String (layer) + ".wav");
        }
    }

    const auto opened = phazite::makeSong (phazite::test::copyWholeSong (*song));
    REQUIRE (opened != nullptr);
    checkSongsDevices (*opened);
    const auto element = phazite::test::copyDevice (*opened, drums);
    REQUIRE (element.isValid());
    const auto held = checkAsHeld (element);
    CHECK (held.values.size() == 3329);
    CHECK (held.sounds.size() == 1024);

    for (int i = 0; i < (int) DrumSampler::info.parameters.size(); i += 97)
        CHECK (juce::exactlyEqual (held.getParameterValue (i),
                                   phazite::readDeviceParameterValue (element, DrumSampler::info.parameters[(size_t) i])));

    // What reading it costs: whole, as the song holds it, against the old reading of one parameter, and of every one.
    const auto place = phazite::readDevicePlace (element);
    std::optional<DeviceValue> readWhole;
    auto sum = 0.0f;
    const auto whole = phazite::test::countAllocations ([&] { readWhole = phazite::readHeldDevice (element, place); });
    const auto one =
        phazite::test::countAllocations ([&] { sum += phazite::readDeviceParameterValue (element, DrumSampler::info.parameters[1000]); });
    const auto every = phazite::test::countAllocations (
        [&]
        {
            for (const auto& info : DrumSampler::info.parameters)
                sum += phazite::readDeviceParameterValue (element, info);
        });
    CHECK (std::isfinite (sum));
    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 10; ++i)
        CHECK (phazite::readHeldDevice (element, place).has_value());
    const auto wholeMs = (juce::Time::getMillisecondCounterHiRes() - start) / 10.0;

    if (! whole.has_value() || ! one.has_value() || ! every.has_value())
        return;

    CHECK (readWhole == held);
    MESSAGE ("A drum sampler of 3,329 values and 1,024 sounds read whole: " << *whole << " allocations, " << wholeMs
                                                                            << " ms; one parameter read alone: " << *one
                                                                            << "; every parameter read alone: " << *every);
    // Measured 2026-11-22: about three allocations for each of its 4,353 entries (immer's nodes as the maps grow), against
    // about one for each parameter read alone (the parameter's name made each time): more than reading its parameters one at a
    // time, which the plan's compile weighs against the reading of what changed (immer::diff) when it reads values.
    CHECK (*whole <= 3 * (3329 + 1024));
}

TEST_CASE ("Devices value: every factory preset's device reads the same both ways once its song opens")
{
    const auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 1);
    const auto& plugins = phazite::getPluginHost (phazite::test::getEngineContext());
    auto files = phazite::test::getFactoryPresetsFolder().findChildFiles (juce::File::findFiles, true,
                                                                          juce::String ("*") + phazite::presetExtension);
    REQUIRE (files.size() > 20);
    auto added = 0;

    for (const auto& file : files)
    {
        const auto track = phazite::insertPlainTrack (*song, tracks.front());

        if (phazite::addDeviceFromPreset (*song, plugins, track, phazite::readPreset (file), nullptr).isValid())
            ++added;
    }

    CHECK (added == files.size());
    const auto opened = phazite::makeSong (phazite::test::copyWholeSong (*song));
    REQUIRE (opened != nullptr);
    CHECK (checkSongsDevices (*opened) >= added);

    // Before the loader too: the presets' devices hold nothing a value would read otherwise.
    forEachDeviceElement (phazite::test::copyWholeSong (*song),
                          [] (const juce::ValueTree& element)
                          {
                              const auto place = phazite::readDevicePlace (element);
                              CHECK (phazite::readDeviceValue (element, place) == phazite::readHeldDevice (element, place));
                          });
}

TEST_CASE ("Devices value: the helpers keep the lists, a container's chains and their devices going with it")
{
    const auto container = makeOfForm (10, onTrack (1), Form::container);
    auto chainA = makeOfForm (11, { .track = TrackID::fromRaw (1), .container = DeviceID::fromRaw (10) }, Form::chain);
    auto chainB = makeOfForm (12, chainA.place, Form::chain);
    const auto inA = makeOwn (13, { .track = TrackID::fromRaw (1), .container = DeviceID::fromRaw (10), .chain = DeviceID::fromRaw (11) });

    SongDevices devices;
    devices = phazite::withDevice (devices, makeOwn (1, onTrack (1)));
    devices = phazite::withDevice (devices, makeOwn (2, onTrack (1)));
    devices = phazite::withDevice (devices, makeOwn (3, onTrack (1)), 0);
    devices = phazite::withDevice (devices, container);
    devices = phazite::withDevice (devices, chainA);
    devices = phazite::withDevice (devices, chainB);
    devices = phazite::withDevice (devices, inA);
    devices = phazite::withDevice (devices, makeOwn (20, { .onMaster = true }));
    CHECK (phazite::holdsItsLists (devices));
    CHECK (listOf (devices.channels.find (1)) == ids ({ 3, 1, 2, 10 }));
    CHECK (listOf (devices.channels.find (0)) == ids ({ 20 }));
    CHECK (listOf (devices.chains.find (10)) == ids ({ 11, 12 }));
    CHECK (listOf (devices.chainDevices.find (11)) == ids ({ 13 }));
    CHECK (listOf (devices.findList (inA.place)) == ids ({ 13 }));

    // Changed in place keeps its place; moved to another chain leaves the first's list, which goes empty.
    auto moved = inA;
    moved.off = true;
    devices = phazite::withDevice (devices, moved);
    CHECK (listOf (devices.chainDevices.find (11)) == ids ({ 13 }));
    moved.place.chain = DeviceID::fromRaw (12);
    devices = phazite::withDevice (devices, moved);
    CHECK (devices.chainDevices.find (11) == nullptr);
    CHECK (listOf (devices.chainDevices.find (12)) == ids ({ 13 }));
    CHECK (phazite::holdsItsLists (devices));

    // Reordered with the same IDs only.
    const auto order = ids ({ 1, 2, 3, 10 });
    const auto reordered = phazite::withListOrder (devices, onTrack (1), immer::flex_vector<DeviceID> (order.begin(), order.end()));
    CHECK (listOf (reordered.channels.find (1)) == ids ({ 1, 2, 3, 10 }));
    const auto refused = ids ({ 1, 2, 3 });
    CHECK (phazite::withListOrder (devices, onTrack (1), immer::flex_vector<DeviceID> (refused.begin(), refused.end())) == devices);

    // A container moved to another channel takes its chains and their devices along, and a chain moved to another
    // container its devices: their places follow it, their lists stay its.
    auto elsewhere = container;
    elsewhere.place = onTrack (2);
    const auto carried = phazite::withDevice (devices, elsewhere);
    CHECK (phazite::holdsItsLists (carried));
    CHECK (listOf (carried.channels.find (2)) == ids ({ 10 }));
    CHECK (listOf (carried.chains.find (10)) == ids ({ 11, 12 }));
    CHECK (carried.find (DeviceID::fromRaw (11))->place
           == DevicePlace { .track = TrackID::fromRaw (2), .container = DeviceID::fromRaw (10) });
    CHECK (carried.find (DeviceID::fromRaw (13))->place
           == DevicePlace { .track = TrackID::fromRaw (2), .container = DeviceID::fromRaw (10), .chain = DeviceID::fromRaw (12) });
    auto onMaster = container;
    onMaster.place = { .onMaster = true };
    const auto toTheMaster = phazite::withDevice (devices, onMaster);
    CHECK (phazite::holdsItsLists (toTheMaster));
    CHECK (toTheMaster.find (DeviceID::fromRaw (13))->place.onMaster);

    const auto other = makeOfForm (14, onTrack (2), Form::container);
    auto withOther = phazite::withDevice (devices, other);
    auto chainMoved = *withOther.find (DeviceID::fromRaw (12));
    chainMoved.place = { .track = TrackID::fromRaw (2), .container = DeviceID::fromRaw (14) };
    withOther = phazite::withDevice (withOther, chainMoved);
    CHECK (phazite::holdsItsLists (withOther));
    CHECK (listOf (withOther.chains.find (14)) == ids ({ 12 }));
    CHECK (withOther.find (DeviceID::fromRaw (13))->place
           == DevicePlace { .track = TrackID::fromRaw (2), .container = DeviceID::fromRaw (14), .chain = DeviceID::fromRaw (12) });

    // A container taken out takes its chains and their devices.
    const auto without = phazite::withoutDevice (devices, DeviceID::fromRaw (10));
    CHECK (phazite::holdsItsLists (without));
    CHECK (without.devices.size() == 4);
    CHECK (without.chains.empty());
    CHECK (without.chainDevices.empty());
    CHECK (listOf (without.channels.find (1)) == ids ({ 3, 1, 2 }));
    CHECK (phazite::withoutDevice (without, DeviceID::fromRaw (99)) == without);

    // What doesn't hold together: a chain's device whose chain isn't there, a container in a chain, a list of none.
    auto broken = devices;
    broken.devices = broken.devices.set (13, makeOwn (13, { .track = TrackID::fromRaw (1), .chain = DeviceID::fromRaw (11) }));
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = devices;
    broken.devices =
        broken.devices.set (10, makeOfForm (10, { .track = TrackID::fromRaw (1), .container = DeviceID::fromRaw (9) }, Form::container));
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = devices;
    broken.channels = broken.channels.set (7, {});
    CHECK_FALSE (phazite::holdsItsLists (broken));

    // The IDs held and named.
    auto withSidechain = makeOwn (30, onTrack (1));
    withSidechain.sidechain = TrackID::fromRaw (77);
    devices = phazite::withDevice (devices, withSidechain);
    CHECK (phazite::findLargestIDIn (devices) == 30);
    std::vector<std::uint64_t> held, named;
    phazite::forEachIDHeld (devices, [&held] (std::uint64_t id) { held.push_back (id); });
    phazite::forEachIDNamed (devices, [&named] (std::uint64_t id) { named.push_back (id); });
    CHECK (held.size() == devices.devices.size());
    CHECK (named == std::vector<std::uint64_t> { 77 });
}

TEST_CASE ("Devices value: a merge's choice sets a property as the song would hold it, and refuses what the value doesn't cover")
{
    auto reverb = makeOwn (1, onTrack (1), devices::Reverb::typeID);
    CHECK (phazite::setDeviceProperty (reverb, schema::VALUES, "size", 0.9f));
    CHECK (juce::exactlyEqual (reverb.getParameterValue (devices::findParameter (devices::Reverb::info, "size")), 0.9f));
    CHECK (phazite::setDeviceProperty (reverb, schema::VALUES, "mix", 0.25f)); // its default: none held
    CHECK (reverb.values.size() == 1);
    CHECK_FALSE (phazite::setDeviceProperty (reverb, schema::VALUES, "nothing", 1.0f));
    CHECK_FALSE (phazite::setDeviceProperty (reverb, schema::SOUNDS, schema::samplerSample, "808 Bass"));
    CHECK (phazite::setDeviceProperty (reverb, schema::DEVICE, schema::deviceOff, true));
    CHECK (reverb.off);
    CHECK_FALSE (phazite::setDeviceProperty (reverb, schema::DEVICE, schema::pluginState, "AAAA")); // the store's
    CHECK_FALSE (phazite::setDeviceProperty (reverb, schema::DEVICE, schema::deviceWindowX, 4));    // a window's place
    CHECK_FALSE (phazite::setDeviceProperty (reverb, schema::CHAIN, schema::chainName, "x"));
    CHECK_FALSE (phazite::setDeviceProperty (reverb, schema::DEVICE, schema::deviceType, "phaziteNothing"));
    CHECK (phazite::setDeviceProperty (reverb, schema::DEVICE, schema::deviceType, devices::Equalizer::typeID));
    CHECK (reverb.type == devices::Equalizer::typeID);

    auto sampler = makeOwn (2, onTrack (1), devices::Sampler::typeID);
    CHECK (phazite::setDeviceProperty (sampler, schema::SOUNDS, schema::samplerSampleFile, "Audio/A.wav"));
    CHECK (sampler.sounds.size() == 1);
    CHECK (phazite::setDeviceProperty (sampler, schema::SOUNDS, schema::samplerSampleFile, juce::var()));
    CHECK (sampler.sounds.empty());

    auto chain = makeOfForm (3, { .track = TrackID::fromRaw (1), .container = DeviceID::fromRaw (4) }, Form::chain);
    CHECK (phazite::setDeviceProperty (chain, schema::CHAIN, schema::channelVolume, 40.0));
    CHECK (juce::exactlyEqual (chain.volume, 6.0f));
    CHECK (phazite::setDeviceProperty (chain, schema::CHAIN, schema::chainHighKey, 300));
    CHECK (chain.keys.high == 127);
    CHECK (phazite::setDeviceProperty (chain, schema::CHAIN, schema::chainCrossover, juce::var()));
    CHECK_FALSE (chain.crossover.has_value());
    CHECK_FALSE (phazite::setDeviceProperty (chain, schema::CHAIN, schema::chainMute, true)); // the store's

    auto container = makeOfForm (4, onTrack (1), Form::container);
    CHECK (phazite::setDeviceProperty (container, schema::CONTAINER, schema::containerSplit, schema::splitMidSide));
    CHECK (container.split == phazite::ContainerSplit::midSide);
    CHECK (phazite::setDeviceProperty (container, schema::CONTAINER, schema::deviceMix, -1.0));
    CHECK (juce::exactlyEqual (container.mix, 0.0f));
}

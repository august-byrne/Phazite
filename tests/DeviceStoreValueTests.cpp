// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song stores of its devices in its undo history and outside it alike, as a keyed value
// (model/base/internal/DeviceStoreValue.h), with the value outside the history (OutsideValue.h, DevicesOutside.h), and the
// store's readers and writers by ID (model/plugins/internal/DeviceStore.h). The value's conversion: the golden project's plug-ins
// and chains, and damaged ones, read the same both ways once the song has opened, and written back as the file holds them; its
// changes over every map; and the plug-in sequences the keyed area must give (design J.1: a value a plug-in's window set, a
// plug-in's held value, a state stored after a preset, a chain soloed, an answer for a device gone), each on a ValueArea of the
// store in a song's history. And on real songs: a plug-in's values through Undo, a chain's switches through Undo, a window's
// place.
//   phazite_tests.exe --test-case="Device store*"
#include "GoldenFiles.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/OutsideValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/ValueArea.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::ChangeDirection;
using phazite::DeviceID;
using phazite::DeviceStore;
using phazite::PluginBytes;
using phazite::PluginValueEntry;
using phazite::PluginValueKey;

static_assert (phazite::KeyedAreaValue<DeviceStore>);
static_assert (phazite::detail::GivesKeyedChanges<DeviceStore>);

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

PluginValueKey keyOf (std::uint64_t device, const juce::String& parameter)
{
    PluginValueKey key;
    key.device = DeviceID::fromRaw (device);
    key.parameter = parameter;
    return key;
}

PluginBytes bytesOf (const juce::String& text)
{
    return PluginBytes { .bytes = std::make_shared<const juce::MemoryBlock> (text.toRawUTF8(), text.getNumBytesAsUTF8()) };
}

// A store's contents as text, each map by its keys in order: "values 1/0=0.5 1/2=0.25(held) | states 1=AB | programs 1=2 |
// formats | versions | mutes 5 | solos".
juce::String describe (const DeviceStore& store)
{
    std::map<juce::String, juce::String> values;

    for (const auto& [key, entry] : store.pluginValues)
        values[juce::String ((juce::int64) key.device.getRaw()) + "/" + key.parameter] =
            juce::String (entry.value) + (entry.isSongs ? "" : "(held)");

    const auto byID = [] (const auto& map, const auto& write)
    {
        std::map<std::uint64_t, juce::String> sorted;

        for (const auto& [id, entry] : map)
            sorted[id] = write (entry);

        juce::StringArray words;

        for (const auto& [id, written] : sorted)
            words.add (juce::String ((juce::int64) id) + (written.isEmpty() ? "" : "=" + written));

        return words.joinIntoString (" ");
    };

    juce::StringArray words { "values" };

    for (const auto& [key, value] : values)
        words.add (key + "=" + value);

    words.add ("| states " + byID (store.states, [] (const PluginBytes& b) { return b.get().toString(); }));
    words.add ("| programs " + byID (store.programs, [] (int program) { return juce::String (program); }));
    words.add ("| formats " + byID (store.formatIDs, [] (const juce::String& text) { return text; }));
    words.add ("| versions " + byID (store.versions, [] (const juce::String& text) { return text; }));
    words.add ("| mutes " + byID (store.chainMutes, [] (bool) { return juce::String(); }));
    words.add ("| solos " + byID (store.chainSolos, [] (bool) { return juce::String(); }));
    return words.joinIntoString (" ").replace ("  ", " ").trim();
}

// The store's changes a test makes, each to one entry: given (a value) or taken away (none).
DeviceStore withValue (DeviceStore store, std::uint64_t device, const juce::String& parameter, std::optional<PluginValueEntry> entry)
{
    store.pluginValues = entry.has_value() ? store.pluginValues.set (keyOf (device, parameter), *entry)
                                           : store.pluginValues.erase (keyOf (device, parameter));
    return store;
}

DeviceStore withState (DeviceStore store, std::uint64_t device, std::optional<juce::String> text)
{
    store.states = text.has_value() ? store.states.set (device, bytesOf (*text)) : store.states.erase (device);
    return store;
}

DeviceStore withProgram (DeviceStore store, std::uint64_t device, std::optional<int> program)
{
    store.programs = program.has_value() ? store.programs.set (device, *program) : store.programs.erase (device);
    return store;
}

DeviceStore withSwitch (DeviceStore store, std::uint64_t chain, bool solo, bool on)
{
    auto& switches = solo ? store.chainSolos : store.chainMutes;
    switches = on ? switches.set (chain, true) : switches.erase (chain);
    return store;
}

// A song's history with a store's area in it, as ValueAreaTests' keyed cases have their test value: the changes a test makes,
// in the history (set()) or outside it.
struct StoreSong
{
    StoreSong() { song->getUndoManager().clearUndoHistory(); }

    juce::String now() const { return describe (area.get()); }

    void set (DeviceStore next) { area.set (std::move (next)); }
    void setOutside (DeviceStore next) { area.setOutsideHistory (std::move (next)); }

    void inStep (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (*song, name);
        change();
    }

    void undo() { phazite::undo (*song); }
    void redo() { phazite::redo (*song); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ValueArea<DeviceStore> area { song->getUndoManager(), DeviceStore() };
};

//==============================================================================
// Each plug-in DEVICE and each CHAIN of a song's tree.
void forEachStoreElement (const juce::ValueTree& tree, const std::function<void (const juce::ValueTree&)>& visit)
{
    for (const auto& child : tree)
    {
        if (child.hasType (schema::CLIP) || child.hasType (schema::AUTOMATION))
            continue;

        if (child.hasType (schema::CHAIN) || phazite::readDeviceForm (child) == phazite::DeviceValue::Form::plugin)
            visit (child);

        forEachStoreElement (child, visit);
    }
}

const std::vector<const juce::Identifier*>& getStoreProperties()
{
    static const std::vector<const juce::Identifier*> properties { &schema::pluginState.getName(),    &schema::pluginProgram.getName(),
                                                                   &schema::pluginFormatID.getName(), &schema::pluginVersion.getName(),
                                                                   &schema::chainMute.getName(),      &schema::chainSolo.getName() };
    return properties;
}

// A plug-in's VALUE children as (parameter, value) in their order.
std::vector<std::pair<juce::String, juce::var>> readValueChildren (const juce::ValueTree& element)
{
    std::vector<std::pair<juce::String, juce::var>> values;

    for (const auto& child : element)
        if (child.hasType (schema::VALUE))
            values.emplace_back (child[schema::valueParameter].toString(), child[schema::valueSetByHand]);

    return values;
}

// An element of a song that went through the loader read both ways the same, and written back from the store into the same
// element without the store's properties and VALUEs: the same properties as the same text, the same VALUEs, in their
// parameters' order (sortedInFile: whether the file holds them so, which the written one then is too). The properties the
// store holds none of where the file held one (a state that isn't base 64, an empty format ID) named in dropped: since the
// store's switch the loader takes them away as the song opens (DeviceStoreValue.h: keepStoreAsHeld()), so a song's element
// holds none of them either.
void checkRoundTrip (const juce::ValueTree& element, const std::vector<const juce::Identifier*>& dropped = {})
{
    INFO (element.toXmlString().toStdString());
    const auto id = phazite::readDeviceElementID (element);
    REQUIRE (id.isValid());

    DeviceStore lenient, held;
    phazite::readStoreOf (element, lenient);
    phazite::readHeldStoreOf (element, held);
    CHECK (lenient == held);
    CHECK (describe (lenient) == describe (held));

    auto written = element.createCopy();

    for (const auto* property : getStoreProperties())
        written.removeProperty (*property, nullptr);

    for (int i = written.getNumChildren(); --i >= 0;)
        if (written.getChild (i).hasType (schema::VALUE))
            written.removeChild (i, nullptr);

    phazite::writeStoreInto (written, held, id);

    for (const auto* property : getStoreProperties())
    {
        INFO (property->toString());

        // A chain's switch written off ("0"), which the loader leaves and which reads as off: none in the store, nor written.
        const auto isSwitchOff = (*property == schema::chainMute.getName() || *property == schema::chainSolo.getName())
                                 && element.hasProperty (*property) && ! (bool) element[*property];

        if (isSwitchOff)
        {
            CHECK (element.hasProperty (*property));
            CHECK_FALSE (written.hasProperty (*property));
            continue;
        }

        if (std::find (dropped.begin(), dropped.end(), property) != dropped.end())
        {
            CHECK_FALSE (element.hasProperty (*property));
            CHECK_FALSE (written.hasProperty (*property));
            continue;
        }

        REQUIRE (element.hasProperty (*property) == written.hasProperty (*property));

        if (written.hasProperty (*property))
            CHECK (phazite::isSameValue (element[*property], written[*property]));
    }

    // Read back from what was written: the same store.
    DeviceStore again;
    phazite::readHeldStoreOf (written, again);
    CHECK (again == held);

    // The same VALUEs, each as the same text, in their parameters' order.
    auto inFile = readValueChildren (element);
    const auto inWritten = readValueChildren (written);
    std::stable_sort (inFile.begin(), inFile.end(),
                      [] (const auto& a, const auto& b) { return phazite::isPluginParameterBefore (a.first, b.first); });
    REQUIRE (inFile.size() == inWritten.size());

    for (size_t i = 0; i < inFile.size(); ++i)
    {
        CHECK (inFile[i].first == inWritten[i].first);
        CHECK (phazite::isSameValue (inFile[i].second, inWritten[i].second));
    }

    // And nothing else of the element changed.
    for (int i = 0; i < element.getNumProperties(); ++i)
    {
        const auto name = element.getPropertyName (i);

        if (std::none_of (getStoreProperties().begin(), getStoreProperties().end(), [&name] (const auto* p) { return *p == name; }))
            CHECK (phazite::isSameValue (element[name], written[name]));
    }
}

juce::ValueTree findByID (const juce::ValueTree& tree, std::uint64_t id)
{
    for (const auto& child : tree)
    {
        if ((child.hasType (schema::DEVICE) || child.hasType (schema::CHAIN)) && phazite::readDeviceElementID (child).getRaw() == id)
            return child;

        if (const auto found = findByID (child, id); found.isValid())
            return found;
    }

    return {};
}
} // namespace

//==============================================================================
TEST_CASE ("Device store: the golden project's plug-ins and chains read the same both ways, and are written back as the file holds "
           "them")
{
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto song = phazite::makeSong (juce::ValueTree::fromXml (*xml));
    REQUIRE (song != nullptr);
    const auto whole = phazite::test::copyWholeSong (*song);

    auto plugins = 0, chains = 0, values = 0;
    DeviceStore all;

    forEachStoreElement (whole,
                         [&] (const juce::ValueTree& element)
                         {
                             checkRoundTrip (element);
                             phazite::readHeldStoreOf (element, all);
                             ++(element.hasType (schema::CHAIN) ? chains : plugins);
                             values += (int) readValueChildren (element).size();
                         });

    CHECK (plugins == 2);
    CHECK (chains >= 8);
    CHECK (values == 1);

    // What the file holds: the effect's state, program, format ID (none: not a VST3), version and its one value; the synth's
    // state and version; a soloed chain and a muted one.
    REQUIRE (all.states.size() == 2);
    CHECK (all.states.find (112)->get() == *phazite::readBytes ("AAAAPwAAgD4="));
    CHECK (all.states.find (115)->get() == *phazite::readBytes ("AACAPg=="));
    auto withoutStates = all;
    withoutStates.states = {};
    CHECK (describe (withoutStates)
           == "values 112/0=0.25 | states | programs 112=0 | formats | versions 112=1 115=1 | mutes 48 | solos 38");

    // The readers by ID read what the conversion reads.
    const auto effect = DeviceID::fromRaw (112);
    CHECK (phazite::getPluginState (*song, effect) == *phazite::readBytes ("AAAAPwAAgD4="));
    CHECK (phazite::getPluginProgram (*song, effect) == 0);
    CHECK (phazite::getPluginVersion (*song, effect) == "1");
    CHECK (phazite::getPluginFormatID (*song, effect).isEmpty());
    CHECK (phazite::findPluginValue (*song, effect, "0").value_or (-1.0f) == doctest::Approx (0.25f));
    CHECK_FALSE (phazite::findPluginValue (*song, effect, "1").has_value());
    REQUIRE (phazite::getPluginValues (*song, effect).size() == 1);
    CHECK (phazite::getPluginValues (*song, effect)[0].parameter == "0");
    CHECK_FALSE (phazite::getPluginProgram (*song, DeviceID::fromRaw (115)).has_value());
    CHECK (phazite::isChainSoloed (*song, DeviceID::fromRaw (38)));
    CHECK (phazite::isChainMuted (*song, DeviceID::fromRaw (48)));
    CHECK_FALSE (phazite::isChainMuted (*song, DeviceID::fromRaw (38)));
    CHECK (phazite::getWindowPlace (*song, DeviceID::fromRaw (89)) == std::optional (phazite::WindowPlace { .x = 120, .y = 80 }));
    CHECK_FALSE (phazite::getWindowPlace (*song, DeviceID::fromRaw (88)).has_value());
}

TEST_CASE ("Device store: damaged plug-ins and chains open as the store holds them, the same both ways, written back in their "
           "parameters' order")
{
    // A song with a container of two chains, then plug-ins another program wrote into it: VALUEs out of their parameters'
    // order (numbers as numbers: 2 before 10; a9 before a10), one past its range, one that is no number, two of one
    // parameter, a program below 0; one whose state isn't base 64 and whose format ID is empty; and the chains' switches
    // written otherwise ("0", "yes").
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 1);
    const auto container = phazite::addContainer (*song, tracks[0]);
    REQUIRE (container.isValid());
    REQUIRE (phazite::addChain (*song, container).isValid());
    const auto chains = phazite::getChains (*song, container);
    REQUIRE (chains.size() == 2);

    auto file = phazite::test::copyWholeSongWithDevice (
        *song, tracks[0],
        parse (R"(<DEVICE id="900001" type="plugin" format="VST3" name="Gone" manufacturer="Nobody" identifier="gone.vst3" uid="7")"
               R"( program="-2" state="AAAAPwAAgD4=" version="1.2" formatID="0123456789ABCDEF0123456789ABCDEF">)"
               R"(<VALUE parameter="10" value="0.5"/><VALUE parameter="2" value="2"/><VALUE parameter="0" value="nan"/>)"
               R"(<VALUE parameter="2" value="0.75"/><VALUE parameter="b" value="0.125"/><VALUE parameter="a10" value="0.25"/>)"
               R"(<VALUE parameter="a9" value="0.375"/></DEVICE>)"));
    auto odd = parse (R"(<DEVICE id="900002" type="plugin" format="VST3" name="Odd" manufacturer="Nobody" identifier="odd.vst3")"
                      R"( uid="8" state="not base 64!" formatID=""/>)");
    findByID (file, chains[0].getRaw()).appendChild (odd, nullptr); // in the first chain
    findByID (file, chains[0].getRaw()).setProperty (schema::chainMute, "0", nullptr);
    findByID (file, chains[0].getRaw()).setProperty (schema::chainSolo, "yes", nullptr);
    findByID (file, chains[1].getRaw()).setProperty (schema::chainMute, "1", nullptr);

    const auto opened = phazite::makeSong (file);
    REQUIRE (opened != nullptr);
    const auto whole = phazite::test::copyWholeSong (*opened);
    auto count = 0;

    forEachStoreElement (whole,
                         [&count] (const juce::ValueTree& element)
                         {
                             if (phazite::readDeviceElementID (element).getRaw() == 900002)
                                 checkRoundTrip (element, { &schema::pluginState.getName(), &schema::pluginFormatID.getName() });
                             else
                                 checkRoundTrip (element);

                             ++count;
                         });

    CHECK (count == 4);

    // As the loader leaves them, which the store holds: of two of one parameter the first, a value within its range, one that is
    // no number none at all or 0 as the loader takes it; the program as the file holds it (what reads it takes it as 0); a
    // chain's switch read as it reads.
    DeviceStore store;
    phazite::readHeldStoreOf (findByID (whole, 900001), store);
    const auto gone = phazite::test::copyDevice (*opened, DeviceID::fromRaw (900001));
    CHECK (*store.programs.find (900001) == -2);
    CHECK (phazite::getPluginProgram (*opened, DeviceID::fromRaw (900001)) == -2);
    CHECK (store.pluginValues.find (keyOf (900001, "2")) != nullptr);
    CHECK (store.pluginValues.find (keyOf (900001, "2"))->value <= 1.0f);

    // Written: in their parameters' order.
    auto written = phazite::writeDeviceElement (*phazite::readHeldDevice (gone, {}));
    phazite::writeStoreInto (written, store, DeviceID::fromRaw (900001));
    juce::StringArray order;

    for (const auto& [parameter, value] : readValueChildren (written))
        order.add (parameter);

    CHECK (order.joinIntoString (" ").endsWith ("2 10 a9 a10 b"));

    // A state that isn't base 64 and an empty format ID, which the loader takes away as the song opens: none in the store,
    // so a song written from the store has neither.
    DeviceStore oddStore;
    phazite::readHeldStoreOf (findByID (whole, 900002), oddStore);
    CHECK (oddStore.states.empty());
    CHECK (oddStore.formatIDs.empty());
    CHECK (phazite::getPluginState (*opened, DeviceID::fromRaw (900002)).isEmpty());
}

TEST_CASE ("Device store: a plug-in's parameters are written in an order that is strict and total, numbers as numbers, a minus "
           "sign as text")
{
    // isPluginParameterBefore(), which writeStoreInto() sorts a plug-in's VALUEs by: over parameters as files hold them (a
    // VST3's numbers, others' names), with numbers that read the same ("02", "2") and a minus sign, which is text ("-2"
    // before "-10"). Strict (none before itself), total (of two different ones, exactly one before the other: none the
    // same both ways), and each before what those after it are before.
    const std::vector<juce::String> parameters { "0", "1", "2", "02", "10", "-2", "-10", "a9", "a10", "b", "B", "", "1.5", "x" };

    for (const auto& a : parameters)
    {
        INFO (a.toStdString());
        CHECK_FALSE (phazite::isPluginParameterBefore (a, a));

        for (const auto& b : parameters)
        {
            if (a == b)
                continue;

            INFO (b.toStdString());
            CHECK (phazite::isPluginParameterBefore (a, b) != phazite::isPluginParameterBefore (b, a));

            for (const auto& c : parameters)
                if (phazite::isPluginParameterBefore (a, b) && phazite::isPluginParameterBefore (b, c))
                    CHECK (phazite::isPluginParameterBefore (a, c));
        }
    }

    CHECK (phazite::isPluginParameterBefore ("2", "10"));
    CHECK (phazite::isPluginParameterBefore ("-2", "-10"));
    CHECK (phazite::isPluginParameterBefore ("a9", "a10"));
}

TEST_CASE ("Device store: the changes between two stores over every map, moved back and forward, onto a store that gained others")
{
    DeviceStore before;
    before = withValue (before, 1, "0", phazite::makeSongsPluginValue (0.5f));
    before = withValue (before, 1, "1", phazite::makeSongsPluginValue (0.25f));
    before = withValue (before, 2, "0", phazite::makeHeldPluginValue (1.0f));
    before = withState (before, 1, "one");
    before = withState (before, 2, "two");
    before = withProgram (before, 1, 3);
    before = withProgram (before, 2, 4);
    before.formatIDs = before.formatIDs.set (1, "A").set (2, "B");
    before.versions = before.versions.set (1, "1.0").set (2, "2.0");
    before = withSwitch (before, 5, false, true);
    before = withSwitch (before, 6, false, true);
    before = withSwitch (before, 5, true, true);
    before = withSwitch (before, 6, true, true);

    // In every map an entry added, one changed and one taken away.
    auto after = before;
    after = withValue (after, 1, "2", phazite::makeSongsPluginValue (0.75f));
    after = withValue (after, 1, "0", phazite::makeSongsPluginValue (0.125f));
    after = withValue (after, 2, "0", std::nullopt);
    after = withState (after, 3, "three");
    after = withState (after, 1, "uno");
    after = withState (after, 2, std::nullopt);
    after = withProgram (after, 3, 0);
    after = withProgram (after, 1, 5);
    after = withProgram (after, 2, std::nullopt);
    after.formatIDs = after.formatIDs.set (3, "C").set (1, "AA").erase (2);
    after.versions = after.versions.set (3, "3.0").set (1, "1.1").erase (2);
    after = withSwitch (after, 7, false, true);
    after = withSwitch (after, 6, false, false);
    after = withSwitch (after, 7, true, true);
    after = withSwitch (after, 5, true, false);

    const auto changes = phazite::findChanges (before, after);
    CHECK (changes.pluginValues.size() == 3);
    CHECK (changes.states.size() == 3);
    CHECK (changes.programs.size() == 3);
    CHECK (changes.formatIDs.size() == 3);
    CHECK (changes.versions.size() == 3);
    CHECK (changes.chainMutes.size() == 2);
    CHECK (changes.chainSolos.size() == 2);

    CHECK (describe (phazite::withChanges (before, changes, ChangeDirection::forward)) == describe (after));
    CHECK (describe (phazite::withChanges (after, changes, ChangeDirection::back)) == describe (before));
    CHECK (phazite::withChanges (before, changes, ChangeDirection::forward) == after);
    CHECK (phazite::withChanges (after, changes, ChangeDirection::back) == before);

    // The very store where each entry is so already.
    CHECK (phazite::withChanges (after, changes, ChangeDirection::forward).isSameAs (after));

    // Onto a store that gained others meanwhile (in every map): those stay, the changes' entries move.
    const auto gain = [] (DeviceStore store)
    {
        store = withValue (store, 9, "9", phazite::makeSongsPluginValue (0.875f));
        store = withState (store, 9, "nine");
        store = withProgram (store, 9, 9);
        store.formatIDs = store.formatIDs.set (9, "I");
        store.versions = store.versions.set (9, "9.0");
        store = withSwitch (store, 9, false, true);
        return withSwitch (store, 9, true, true);
    };

    CHECK (describe (phazite::withChanges (gain (after), changes, ChangeDirection::back)) == describe (gain (before)));
    CHECK (describe (phazite::withChanges (gain (before), changes, ChangeDirection::forward)) == describe (gain (after)));

    // A plug-in's held value is no part of the song's store: alike stores but for one, equal; the same entry but for whose it
    // is, a change.
    CHECK (withValue (before, 4, "0", phazite::makeHeldPluginValue (0.5f)) == before);
    CHECK_FALSE (withValue (before, 4, "0", phazite::makeSongsPluginValue (0.5f)) == before);
    CHECK (phazite::findChanges (withValue (before, 2, "0", phazite::makeSongsPluginValue (1.0f)), before).pluginValues.size() == 1);

    // A held value is never written.
    juce::ValueTree device (schema::DEVICE, { { schema::deviceID, 2 }, { schema::deviceType, schema::deviceIsPlugin } });
    phazite::writeStoreInto (device, before, DeviceID::fromRaw (2));
    CHECK_FALSE (device.getChildWithName (schema::VALUE).isValid());
    CHECK (device[schema::pluginProgram].toString() == "4");
}

//==============================================================================
// The plug-in sequences of design J.1 on the keyed area: what the tree gives today, each named.
TEST_CASE ("Device store, J.1 a: a value the plug-in's window set outside the history survives the Undo and Redo of a step that "
           "changed another")
{
    StoreSong store;
    store.setOutside (withValue (store.area.get(), 1, "P", phazite::makeSongsPluginValue (0.25f))); // as the song opened

    // "Turn Device Off" stands in, a step that changes nothing of P: here another parameter of the plug-in, so that the step
    // has an action of the store's.
    store.inStep ("Turn Device Off", [&] { store.set (withValue (store.area.get(), 1, "Q", phazite::makeSongsPluginValue (0.5f))); });
    store.setOutside (withValue (store.area.get(), 1, "P", phazite::makeSongsPluginValue (0.8f)));
    CHECK (store.now() == "values 1/P=0.8 1/Q=0.5 | states | programs | formats | versions | mutes | solos");

    // The tree: the step's Undo puts back what it changed of its own properties, never P's VALUE: P stays at 0.8.
    store.undo();
    CHECK (store.now() == "values 1/P=0.8 | states | programs | formats | versions | mutes | solos");
    store.redo();
    CHECK (store.now() == "values 1/P=0.8 1/Q=0.5 | states | programs | formats | versions | mutes | solos");

    // The same with another step begun between the step and the window's write (the step no longer the history's current one).
    store.inStep ("Another", [&] { store.set (withProgram (store.area.get(), 2, 1)); });
    store.setOutside (withValue (store.area.get(), 1, "P", phazite::makeSongsPluginValue (0.9f)));
    store.undo();
    store.undo();
    CHECK (store.now() == "values 1/P=0.9 | states | programs | formats | versions | mutes | solos");
    store.redo();
    store.redo();
    CHECK (store.now() == "values 1/P=0.9 1/Q=0.5 | states | programs 2=1 | formats | versions | mutes | solos");
}

TEST_CASE ("Device store, J.1 b: a parameter's first value set by hand, its Undo gives back what the plug-in held, not the song's")
{
    // The parameters' code writes what the plug-in holds as an entry not the song's, outside the history, before the step's set
    // (the tree: the VALUE appended holding the plug-in's value, then set, both in the step; its Undo sets the VALUE back to
    // the plug-in's value, which the plug-in takes, and takes it away).
    StoreSong store;
    store.inStep ("Turn Knob",
                  [&]
                  {
                      store.setOutside (withValue (store.area.get(), 1, "P", phazite::makeHeldPluginValue (1.0f)));
                      store.set (withValue (store.area.get(), 1, "P", phazite::makeSongsPluginValue (0.4f)));
                  });

    CHECK (store.now() == "values 1/P=0.4 | states | programs | formats | versions | mutes | solos");

    // Undo: the held entry, the value to give the plug-in; the song holds none of P (equal to a store without it, and none
    // written).
    store.undo();
    CHECK (store.now() == "values 1/P=1(held) | states | programs | formats | versions | mutes | solos");
    CHECK (store.area.get() == DeviceStore());
    juce::ValueTree device (schema::DEVICE, { { schema::deviceID, 1 }, { schema::deviceType, schema::deviceIsPlugin } });
    phazite::writeStoreInto (device, store.area.get(), DeviceID::fromRaw (1));
    CHECK (device.getNumChildren() == 0);

    // Redo: the step's value, the song's.
    store.redo();
    CHECK (store.now() == "values 1/P=0.4 | states | programs | formats | versions | mutes | solos");
}

TEST_CASE ("Device store, J.1 c: a state stored outside the history after a preset's state set in a step is lost by the preset's "
           "Undo, as the tree loses it")
{
    // The keyed area: the step changed the state's entry, so its Undo puts back the entry's value before it, whatever was
    // stored into it since; its Redo the step's value after.
    StoreSong store;
    store.setOutside (withState (store.area.get(), 1, "S0"));
    store.inStep ("Load Preset", [&] { store.set (withState (store.area.get(), 1, "S1")); });
    store.setOutside (withState (store.area.get(), 1, "S2")); // a save's store, as the plug-in gave it
    store.undo();
    CHECK (store.now() == "values | states 1=S0 | programs | formats | versions | mutes | solos");
    store.redo();
    CHECK (store.now() == "values | states 1=S1 | programs | formats | versions | mutes | solos");

    // The tree today, on a real song: the same.
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1)[0];
    const auto plugin = phazite::test::addPluginDevice (*song, track, phazite::test::TestPlugin::getDescription());
    const auto stateOf = [&song, plugin] { return phazite::getPluginState (*song, plugin).toString(); };
    phazite::test::storePluginState (*song, plugin, juce::MemoryBlock ("S0", 2));
    song->getUndoManager().clearUndoHistory();

    {
        const phazite::UndoStep step (*song, "Load Preset");
        phazite::test::setPluginStateInStep (*song, plugin, juce::MemoryBlock ("S1", 2));
    }

    phazite::test::storePluginState (*song, plugin, juce::MemoryBlock ("S2", 2));
    CHECK (stateOf() == "S2");
    phazite::undo (*song);
    CHECK (stateOf() == "S0");
    phazite::redo (*song);
    CHECK (stateOf() == "S1");
}

TEST_CASE ("Device store, J.1 d: a chain soloed outside the history, then taken away in a step with its entries, is soloed again "
           "after the Undo")
{
    // The chain's entries go in the step that takes it away, and the chain left alone has its switch taken off in it too.
    StoreSong store;
    store.setOutside (withSwitch (store.area.get(), 5, true, true));
    store.setOutside (withSwitch (store.area.get(), 6, false, true));
    store.inStep ("Remove Chain",
                  [&]
                  {
                      store.set (withSwitch (store.area.get(), 5, true, false));
                      store.set (withSwitch (store.area.get(), 6, false, false));
                  });

    CHECK (store.now() == "values | states | programs | formats | versions | mutes | solos");
    store.undo();
    CHECK (store.now() == "values | states | programs | formats | versions | mutes 6 | solos 5");
    store.redo();
    CHECK (store.now() == "values | states | programs | formats | versions | mutes | solos");
    store.undo();
    CHECK (store.now() == "values | states | programs | formats | versions | mutes 6 | solos 5");
}

TEST_CASE ("Device store, J.1 e: an answer written by ID for a device a step took away is kept, written into no song that lacks "
           "the device, and there after the Undo")
{
    // The removal keeps the device's entries (H.2: entries of items the song doesn't hold are kept): the step changes another
    // entry of the store (a chain's switch), none of the device's. The answer comes after it, outside the history.
    StoreSong store;
    store.setOutside (withState (store.area.get(), 7, "S0"));
    store.setOutside (withSwitch (store.area.get(), 5, false, true));
    store.inStep ("Remove Device", [&] { store.set (withSwitch (store.area.get(), 5, false, false)); });
    store.setOutside (withState (store.area.get(), 7, "S2"));
    CHECK (store.now() == "values | states 7=S2 | programs | formats | versions | mutes | solos");

    // A song without the device: writing each item it holds writes nothing of the device's.
    juce::ValueTree other (schema::DEVICE, { { schema::deviceID, 8 }, { schema::deviceType, schema::deviceIsPlugin } });
    phazite::writeStoreInto (other, store.area.get(), DeviceID::fromRaw (8));
    CHECK_FALSE (other.hasProperty (schema::pluginState));

    // The Undo of the removal: the answer's state, as the tree gives it today (the answer was written into the element the
    // removal's action holds, which the Undo brings back).
    store.undo();
    CHECK (store.now() == "values | states 7=S2 | programs | formats | versions | mutes 5 | solos");
    store.redo();
    CHECK (store.now() == "values | states 7=S2 | programs | formats | versions | mutes | solos");

    // Were the removal to take the device's entries in its step, the answer written after it would be lost by its Undo (the
    // step changed that entry): the Undo would put back the state before the removal, where the tree gives the answer's. So
    // the store's switch keeps the entries of a device taken away.
    StoreSong taking;
    taking.setOutside (withState (taking.area.get(), 7, "S0"));
    taking.inStep ("Remove Device", [&] { taking.set (withState (taking.area.get(), 7, std::nullopt)); });
    taking.setOutside (withState (taking.area.get(), 7, "S2"));
    taking.undo();
    CHECK (taking.now() == "values | states 7=S0 | programs | formats | versions | mutes | solos");
}

//==============================================================================
TEST_CASE ("Device store: a value outside the history knows no undo manager; set, it tells its listeners once, and nothing for the "
           "same value")
{
    struct Told final : phazite::OutsideValue<phazite::DevicesOutside>::Listener
    {
        int count = 0;
        phazite::DevicesOutside before, after;

        void outsideValueChanged (const phazite::DevicesOutside& was, const phazite::DevicesOutside& now) override
        {
            ++count;
            before = was;
            after = now;
        }
    };

    phazite::OutsideValue<phazite::DevicesOutside> windows;
    Told told;
    windows.addListener (&told);
    const auto device = DeviceID::fromRaw (3);
    CHECK_FALSE (windows.get().findWindow (device).has_value());

    windows.set (windows.get().withWindow (device, phazite::WindowPlace { .x = 10, .y = 20 }));
    CHECK (told.count == 1);
    CHECK (windows.get().findWindow (device) == std::optional (phazite::WindowPlace { .x = 10, .y = 20 }));
    CHECK_FALSE (told.before.findWindow (device).has_value());
    CHECK (told.after.isSameAs (windows.get()));

    windows.set (windows.get()); // the same value
    CHECK (told.count == 1);
    windows.set (windows.get().withWindow (device, std::nullopt));
    CHECK (told.count == 2);
    CHECK (windows.get() == phazite::DevicesOutside());
    windows.removeListener (&told);
}

//==============================================================================
// On real songs, through the functions by ID: what each piece of the store gives through Undo.
TEST_CASE ("Device store: a plug-in's value set in its own window reaches no undo step and survives the Undo of an unrelated step")
{
    const phazite::test::TestPlugin testPlugin; // known while the song is
    phazite::test::TestSong song;
    auto& open = *phazite::findSong (song.context);
    const auto plugin = phazite::test::TestPlugin::add (open, song.firstTrack);
    song.waitForPlugins();
    open.getUndoManager().clearUndoHistory();
    const phazite::ParameterRef gain { plugin, phazite::test::TestPlugin::gainID };

    // A value of the song's for its gain, set by a control; then a step that changes nothing of it.
    {
        const phazite::UndoStep step (open, "Turn Knob");
        phazite::setValueSetByHand (open, gain, 0.5f);
    }

    {
        const phazite::UndoStep step (open, "Turn Device Off");
        phazite::setDeviceOn (open, plugin, false);
    }

    // The gain turned in the plug-in's own window: the song's value takes it in, with no step.
    phazite::test::TestPlugin::setGain (open, plugin, 0.8f);
    CHECK (phazite::getHistorySteps (open).done == juce::StringArray { "Turn Knob", "Turn Device Off" });
    CHECK (phazite::findPluginValue (open, plugin, gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));

    // Undo of the unrelated step: the value stays, and the plug-in keeps it.
    phazite::undo (open);
    CHECK (phazite::isDeviceOn (open, plugin));
    CHECK (phazite::findPluginValue (open, plugin, gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));
    CHECK (phazite::test::TestPlugin::getGain (open, plugin) == doctest::Approx (0.8f));
    phazite::redo (open);
    CHECK (phazite::findPluginValue (open, plugin, gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));
    CHECK (phazite::test::TestPlugin::getGain (open, plugin) == doctest::Approx (0.8f));
}

TEST_CASE ("Device store: a plug-in's first value set by hand, undone, gives the plug-in back the value it held")
{
    const phazite::test::TestPlugin testPlugin;
    phazite::test::TestSong song;
    auto& open = *phazite::findSong (song.context);
    const auto plugin = phazite::test::TestPlugin::add (open, song.firstTrack);
    song.waitForPlugins();
    open.getUndoManager().clearUndoHistory();
    const phazite::ParameterRef gain { plugin, phazite::test::TestPlugin::gainID };
    auto* instance = phazite::findPluginInstance (open, plugin);
    REQUIRE (instance != nullptr);
    const auto index = instance->findParameter (gain.parameter);
    REQUIRE (index >= 0);
    REQUIRE (instance->getParameterValue (index) == doctest::Approx (1.0f)); // its gain as it starts
    CHECK_FALSE (phazite::findPluginValue (open, plugin, gain.parameter).has_value());

    {
        const phazite::UndoStep step (open, "Turn Knob");
        phazite::setValueSetByHand (open, gain, 0.3f);
    }

    CHECK (phazite::findPluginValue (open, plugin, gain.parameter).value_or (-1.0f) == doctest::Approx (0.3f));
    CHECK (instance->getParameterValue (index) == doctest::Approx (0.3f));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return std::abs (phazite::test::TestPlugin::getGain (open, plugin) - 0.3f) < 1.0e-4f; }));

    // Undo: the song sets none of it, and the plug-in has its own value again, in its instance and in its helper.
    phazite::undo (open);
    CHECK_FALSE (phazite::findPluginValue (open, plugin, gain.parameter).has_value());
    CHECK (instance->getParameterValue (index) == doctest::Approx (1.0f));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return std::abs (phazite::test::TestPlugin::getGain (open, plugin) - 1.0f) < 1.0e-4f; }));

    // Redo: the value set again.
    phazite::redo (open);
    CHECK (phazite::findPluginValue (open, plugin, gain.parameter).value_or (-1.0f) == doctest::Approx (0.3f));
    CHECK (instance->getParameterValue (index) == doctest::Approx (0.3f));
}

TEST_CASE ("Device store: a chain's mute and solo are no undo steps, and survive the Undo of an unrelated step and come back with the "
           "Undo of the chain's removal")
{
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1)[0];
    const auto container = phazite::addContainer (*song, track);
    REQUIRE (container.isValid());
    REQUIRE (phazite::addChain (*song, container).isValid());
    REQUIRE (phazite::addChain (*song, container).isValid());
    const auto chains = phazite::getChains (*song, container);
    REQUIRE (chains.size() == 3);
    song->getUndoManager().clearUndoHistory();

    phazite::setChainMuted (*song, chains[1], true);
    phazite::setChainSoloed (*song, chains[2], true);
    CHECK_FALSE (song->getUndoManager().canUndo());
    CHECK (phazite::isChainMuted (*song, chains[1]));
    CHECK (phazite::isChainSoloed (*song, chains[2]));
    CHECK_FALSE (phazite::isChainHeard (*song, chains[0])); // another is soloed
    CHECK (phazite::isChainHeard (*song, chains[2]));

    // An unrelated step, undone and done again.
    {
        const phazite::UndoStep step (*song, "Rename Chain");
        phazite::setChainName (*song, chains[0], "Low");
    }

    phazite::undo (*song);
    CHECK (phazite::isChainMuted (*song, chains[1]));
    CHECK (phazite::isChainSoloed (*song, chains[2]));
    phazite::redo (*song);
    CHECK (phazite::isChainMuted (*song, chains[1]));
    CHECK (phazite::isChainSoloed (*song, chains[2]));

    // The soloed chain taken away; its Undo brings it back soloed.
    {
        const phazite::UndoStep step (*song, "Remove Chain");
        phazite::removeChain (*song, chains[2]);
    }

    CHECK (phazite::getChains (*song, container).size() == 2);
    phazite::undo (*song);
    REQUIRE (phazite::getChains (*song, container).size() == 3);
    CHECK (phazite::isChainSoloed (*song, chains[2]));
    CHECK (phazite::isChainMuted (*song, chains[1]));

    // Two taken away in turn: the chain left alone is neither muted nor soloed, in the second step, whose Undo gives it its
    // mute back.
    {
        const phazite::UndoStep step (*song, "Remove Chain");
        phazite::removeChain (*song, chains[2]);
    }

    {
        const phazite::UndoStep step (*song, "Remove Chain");
        phazite::removeChain (*song, chains[0]);
    }

    REQUIRE (phazite::getChains (*song, container) == std::vector { chains[1] });
    CHECK_FALSE (phazite::isChainMuted (*song, chains[1]));
    phazite::undo (*song);
    CHECK (phazite::isChainMuted (*song, chains[1]));
    phazite::undo (*song);
    CHECK (phazite::isChainSoloed (*song, chains[2]));
    phazite::redo (*song);
    phazite::redo (*song);
    CHECK_FALSE (phazite::isChainMuted (*song, chains[1]));
}

TEST_CASE ("Device store: a window's place written is no undo step and no unsaved change, and survives an Undo")
{
    phazite::test::TestSong song;
    auto& open = *phazite::findSong (song.context);
    const auto equalizer = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    REQUIRE (song.getProject().save().wasOk());
    open.getUndoManager().clearUndoHistory();
    REQUIRE_FALSE (song.getProject().hasUnsavedChanges());

    phazite::setWindowPlace (open, equalizer, { .x = 40, .y = 60 });
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (open.getUndoManager().canUndo());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
    CHECK (phazite::getWindowPlace (open, equalizer) == std::optional (phazite::WindowPlace { .x = 40, .y = 60 }));

    // A step, then the window moved again, then the step undone and done again: the place stays where it was moved last.
    song.rename (song.firstTrack, "Lead");
    phazite::setWindowPlace (open, equalizer, { .x = 70, .y = 90 });
    phazite::undo (open);
    CHECK (phazite::getWindowPlace (open, equalizer) == std::optional (phazite::WindowPlace { .x = 70, .y = 90 }));
    phazite::redo (open);
    CHECK (phazite::getWindowPlace (open, equalizer) == std::optional (phazite::WindowPlace { .x = 70, .y = 90 }));

    // Nothing for a device the song hasn't.
    phazite::setWindowPlace (open, DeviceID::fromRaw (999999), { .x = 1, .y = 2 });
    CHECK_FALSE (phazite::getWindowPlace (open, DeviceID::fromRaw (999999)).has_value());
}

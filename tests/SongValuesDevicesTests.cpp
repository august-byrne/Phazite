// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's devices, containers and chains change hears it from one place (model/base/internal/SongValues.h:
// Listener::devicesChanged()), by their IDs (model/base/internal/DevicesArea.h). For each edit of devices made through the model,
// its Undo and its Redo, the listener is told every device that differs before and after, compared as values (getDevicesValue(),
// independent of what tells it), with where it was and is, at least the parts that differ, its value before as findDevice() read
// it before and after as findDevice() reads it after; one told that doesn't differ was taken out and came back; every owner whose
// devices came in another order; and nothing of another area. What a step told in all is checked, never how many calls. Another
// area's change, and what the devices' value doesn't cover (a plug-in's values, state and program, a chain's mute and solo, a
// window's place), tell it nothing. And what plays makes the plans it made per edit, and takes a value, a sound, a container's
// mix and a chain's volume at once without one; the unsaved check, MIDI learn and a frozen track follow them too.
//   phazite_tests.exe --test-case="Song values' devices*"
#include "EnginePlayback.h"
#include "PerformanceCounts.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/SwapModel.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/midi/MidiLinkModel.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace
{
namespace dev = phazite::devices;
using Change = phazite::SongValues::DeviceChange;
using phazite::DeviceID;
using phazite::DevicePlace;
using phazite::DeviceValue;
using phazite::SongDevices;
using phazite::TrackID;

// A knob of the equalizer whose values are plain numbers (its third band's gain, in decibels), as a knob's drag sets one.
constexpr int gainIndex = dev::Equalizer::getParameterIndex (2, dev::Equalizer::gain);

// What a device's changes told, in all: whether it was there as the first told it and is as the last told it, where it was
// then and is now, the parts of all of them, its value before the first and after the last.
struct Entry
{
    bool wasThere = false, isThere = false;
    DevicePlace before {}, after {};
    std::uint16_t parts = 0;
    std::optional<DeviceValue> was {}, now {};
};

// An owner's list, as a key: 0 a channel's (the master's under 0), 1 a container's, 2 a chain's; and its key there.
using ListKey = std::pair<int, std::uint64_t>;

ListKey getListKey (const DevicePlace& owner)
{
    if (owner.chain.isValid())
        return { 2, owner.chain.getRaw() };

    if (owner.container.isValid())
        return { 1, owner.container.getRaw() };

    return { 0, owner.onMaster ? 0 : owner.track.getRaw() };
}

// How many calls the values' listener heard, of each area, and what it was told of the devices, each by its ID.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }
    void curvesChanged (const std::vector<phazite::ParameterRef>&) override { ++curves; }

    void modulationChanged (const std::vector<phazite::SongValues::ModulationChange>&, const std::vector<phazite::ModulatorPlace>&) override
    {
        ++modulation;
    }

    void sendsChanged (const std::vector<phazite::SongValues::SendChange>&, const std::vector<TrackID>&, const phazite::SongSends&) override
    {
        ++sends;
    }

    void devicesChanged (const std::vector<Change>& changes, const std::vector<DevicePlace>& owners, const SongDevices& now) override
    {
        ++calls;
        CHECK_FALSE ((changes.empty() && owners.empty()));
        CHECK (phazite::holdsItsLists (now));

        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            CHECK (std::none_of (changes.begin(), changes.begin() + (std::ptrdiff_t) i,
                                 [&change] (const Change& other) { return other.id == change.id; })); // each once in a call
            CHECK ((change.wasThere || change.isThere));
            CHECK ((change.was != nullptr) == change.wasThere);
            CHECK ((change.now != nullptr) == change.isThere);
            CHECK (change.parts != 0);

            if (change.was != nullptr)
            {
                CHECK (change.was->id == change.id);
                CHECK (change.was->place == change.before);
                CHECK (change.was->form == change.form);
            }

            if (change.now != nullptr)
            {
                CHECK (change.now->id == change.id);
                CHECK (change.now->place == change.after);
                CHECK (change.now->form == change.form);
                CHECK (now.find (change.id) == change.now); // the value after handed whole, the change's in it
            }
            else
            {
                CHECK (now.find (change.id) == nullptr);
            }

            auto [found, first] = told.try_emplace (change.id.getRaw());
            auto& entry = found->second;

            if (first)
            {
                entry.wasThere = change.wasThere;
                entry.before = change.before;
                entry.was = change.was != nullptr ? std::optional (*change.was) : std::nullopt;
            }

            entry.isThere = change.isThere;
            entry.after = change.after;
            entry.parts = (std::uint16_t) (entry.parts | change.parts);
            entry.now = change.now != nullptr ? std::optional (*change.now) : std::nullopt;
        }

        for (const auto& owner : owners)
            reordered.insert (getListKey (owner));
    }

    int notes = 0, clips = 0, tempo = 0, curves = 0, modulation = 0, sends = 0, calls = 0;
    std::map<std::uint64_t, Entry> told {};
    std::set<ListKey> reordered {};
};

// The parts in which an item differs between two values of the song's devices, worked out here (independent of the keeper):
// none where it is the same, `made` where it comes or goes.
std::uint16_t findDifferingParts (const DeviceValue* was, const DeviceValue* is)
{
    if (was == nullptr || is == nullptr)
        return was == is ? 0 : (std::uint16_t) Change::made;

    std::uint16_t parts = 0;
    const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

    if (was->place != is->place)
        add (Change::place);

    if (was->off != is->off || was->sidechain != is->sidechain || was->type != is->type || ! (was->plugin == is->plugin)
        || was->name != is->name || was->split != is->split || was->instrument != is->instrument)
        add (Change::settings);

    if (! juce::exactlyEqual (was->mix, is->mix))
        add (Change::mix);

    auto sameValues = was->values.size() == is->values.size();

    for (const auto& [index, value] : was->values)
        if (const auto* other = is->values.find (index); other == nullptr || ! juce::exactlyEqual (value, *other))
            sameValues = false;

    if (! sameValues)
        add (Change::values);

    if (! (was->sounds == is->sounds))
        add (Change::sounds);

    if (! juce::exactlyEqual (was->volume, is->volume) || ! juce::exactlyEqual (was->pan, is->pan))
        add (Change::chainFader);

    const auto sameCrossover = was->crossover.has_value() == is->crossover.has_value()
                               && (! was->crossover.has_value() || juce::exactlyEqual (*was->crossover, *is->crossover));

    if (! sameCrossover || was->keys != is->keys || was->velocities != is->velocities)
        add (Change::chainRange);

    return parts;
}

// Each ID of either value.
std::vector<std::uint64_t> getAllIDs (const SongDevices& before, const SongDevices& after)
{
    std::vector<std::uint64_t> ids;

    for (const auto* devices : { &before, &after })
        for (const auto& [id, value] : devices->devices)
            if (std::find (ids.begin(), ids.end(), id) == ids.end())
                ids.push_back (id);

    return ids;
}

// The owners that hold those they held before in another order: each list in both values, the IDs in both, compared.
std::vector<ListKey> findReordered (const SongDevices& before, const SongDevices& after)
{
    std::vector<ListKey> owners;
    const auto inLists = [&owners] (const auto& was, const auto& is, int kind)
    {
        for (const auto& [key, list] : was)
        {
            const auto* other = is.find (key);

            if (other == nullptr)
                continue;

            std::vector<DeviceID> keptBefore, keptAfter;

            for (const auto id : list)
                if (std::find (other->begin(), other->end(), id) != other->end())
                    keptBefore.push_back (id);

            for (const auto id : *other)
                if (std::find (list.begin(), list.end(), id) != list.end())
                    keptAfter.push_back (id);

            if (keptBefore != keptAfter)
                owners.push_back ({ kind, key });
        }
    };

    inLists (before.channels, after.channels, 0);
    inLists (before.chains, after.chains, 1);
    inLists (before.chainDevices, after.chainDevices, 2);
    return owners;
}

// A song in a project of its own, with its versions, the values' listener recording what it is told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    // What a change told, how far the project's count of changes went up meanwhile, and the song's devices before and after,
    // as findDevice() reads them (getDevicesValue()).
    struct Heard
    {
        Told told {};
        juce::int64 project = 0;
        SongDevices before {}, after {};
    };

    Heard hear (const std::function<void()>& change)
    {
        told = {};
        const auto projectBefore = s.getProject().getChangeCount();
        auto before = phazite::getDevicesValue (song);
        change();
        Heard heard;
        heard.told = told;
        heard.project = s.getProject().getChangeCount() - projectBefore;
        heard.before = std::move (before);
        heard.after = phazite::getDevicesValue (song);
        return heard;
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
};

// What a change and its Undo and Redo told, each against the song's devices before and after: every item that differs told,
// as it was before (its value as findDevice() read it) and is after (there or not, where, its value as findDevice() reads
// it), with at least the parts that differ; one told that doesn't differ as it is after too (one taken out and back, as a
// track JUCE moves); every owner whose items came in another order told; and nothing of another area unless othersToo.
void checkHeard (const juce::String& name, const std::vector<Watched::Heard>& heard, bool othersToo = false)
{
    INFO (name.toStdString());
    const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
    REQUIRE (heard.size() <= moments.size());

    for (size_t i = 0; i < heard.size(); ++i)
    {
        INFO (moments[i]);
        const auto& each = heard[i];
        const auto ids = getAllIDs (each.before, each.after);
        auto differs = false;

        for (const auto id : ids)
        {
            INFO ("item " << id);
            const auto* was = each.before.find (DeviceID::fromRaw (id));
            const auto* is = each.after.find (DeviceID::fromRaw (id));
            const auto parts = findDifferingParts (was, is);
            const auto found = each.told.told.find (id);

            if (parts == 0 && found == each.told.told.end())
                continue;

            differs = differs || parts != 0;
            REQUIRE (found != each.told.told.end());
            const auto& entry = found->second;
            // Its parts, or taken out and put in again (another element of its ID, at its new place: a container grouped).
            CHECK (((entry.parts & parts) == parts || (entry.parts & Change::made) != 0));
            CHECK (entry.isThere == (is != nullptr));
            CHECK (entry.wasThere == (was != nullptr));

            // One told that doesn't differ was taken out and came back, as it was, or changed and changed back (a trial put
            // back).
            if (parts == 0)
                CHECK (((entry.parts & Change::made) != 0 || (entry.was.has_value() && entry.now.has_value() && *entry.was == *entry.now)));

            if (was != nullptr && entry.wasThere)
            {
                CHECK (entry.before == was->place);
                REQUIRE (entry.was.has_value());
                CHECK (*entry.was == *was);
            }

            if (is != nullptr)
            {
                CHECK (entry.after == is->place);
                REQUIRE (entry.now.has_value());
                CHECK (*entry.now == *is);
            }
        }

        // Nothing told of an item the song had neither before nor after.
        for (const auto& [id, entry] : each.told.told)
            CHECK (std::find (ids.begin(), ids.end(), id) != ids.end());

        for (const auto& owner : findReordered (each.before, each.after))
        {
            differs = true;
            CHECK_MESSAGE (each.told.reordered.contains (owner),
                           "owner " << owner.first << ":" << (juce::int64) owner.second << " not told reordered");
        }

        if (differs)
            CHECK (each.told.calls > 0);

        if (! othersToo)
        {
            CHECK (each.told.clips == 0);
            CHECK (each.told.notes == 0);
            CHECK (each.told.tempo == 0);
            CHECK (each.told.curves == 0);
            CHECK (each.told.modulation == 0);
            CHECK (each.told.sends == 0);
        }
    }
}

// Whether any change told of an item is of these parts.
bool toldAny (const Watched::Heard& heard, DeviceID id, std::uint16_t parts)
{
    const auto found = heard.told.told.find (id.getRaw());
    return found != heard.told.told.end() && (found->second.parts & parts) != 0;
}

bool toldOf (const Watched::Heard& heard, DeviceID id)
{
    return heard.told.told.contains (id.getRaw());
}

juce::String getParameterID (const dev::DeviceInfo& info, int index)
{
    const auto& id = info.parameters[(size_t) index].id;
    return juce::String (id.data(), id.size());
}

void setValue (phazite::Song& song, DeviceID device, const juce::String& parameter, float value)
{
    phazite::setValueSetByHand (song, phazite::ParameterRef { device, parameter }, value);
}
} // namespace

TEST_CASE ("Song values' devices: each edit of a device through the model tells what changed of it, its Undo and its Redo too, "
           "and nothing of another area")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    DeviceID eq, delay, synth;

    const auto added = w.hearEach ("Add Effect", [&] { eq = phazite::test::addOwnDevice (song, track, dev::Equalizer::typeID); });
    checkHeard ("Add Effect", added);
    REQUIRE (eq.isValid());
    CHECK (toldAny (added[0], eq, Change::made));
    CHECK_FALSE (added[0].told.told.at (eq.getRaw()).wasThere);
    CHECK (added[0].told.told.at (eq.getRaw()).after.track == track);
    CHECK_FALSE (added[1].told.told.at (eq.getRaw()).isThere);

    checkHeard ("Add Delay", w.hearEach ("Add Delay", [&] { delay = phazite::test::addOwnDevice (song, track, dev::Delay::typeID); }));
    checkHeard ("Set Instrument",
                w.hearEach ("Set Instrument", [&] { synth = phazite::test::setOwnInstrument (song, track, dev::Synth::typeID); }));

    // A knob from its default, to another value, and back to its default: its VALUES appearing and going.
    const auto gain = getParameterID (dev::Equalizer::info, gainIndex);
    const auto first = w.hearEach ("Turn Knob", [&] { setValue (song, eq, gain, 3.0f); });
    checkHeard ("Turn Knob", first);
    CHECK (toldAny (first[0], eq, Change::values));
    CHECK (toldAny (first[0], eq, Change::valuesAppeared));
    CHECK_FALSE (toldAny (first[0], eq, (std::uint16_t) (Change::made | Change::place | Change::settings)));
    const auto again = w.hearEach ("Turn Knob Again", [&] { setValue (song, eq, gain, 6.0f); });
    checkHeard ("Turn Knob Again", again);
    CHECK (toldAny (again[0], eq, Change::values));
    CHECK_FALSE (toldAny (again[0], eq, Change::valuesAppeared));
    const auto back = w.hearEach ("Knob to Default",
                                  [&] { setValue (song, eq, gain, dev::Equalizer::info.parameters[(size_t) gainIndex].defaultValue); });
    checkHeard ("Knob to Default", back);
    CHECK (toldAny (back[0], eq, Change::valuesAppeared));

    // A value that changes the device's latency, in a gesture and out.
    const auto phase = getParameterID (dev::Equalizer::info, dev::Equalizer::phaseParameter);
    checkHeard ("Linear Phase", w.hearEach ("Linear Phase", [&] { setValue (song, eq, phase, 1.0f); }));
    w.hear (
        [&]
        {
            phazite::Gesture gesture (song, "Drag Phase");
            setValue (song, eq, phase, 0.0f);
            setValue (song, eq, phase, 1.0f);
            setValue (song, eq, phase, 0.0f);
            gesture.commit();
        });
    CHECK (phazite::findDevice (song, eq)->values.empty());

    // Switched off, moved among its channel's, into a container's chain, within it and to its container's other chain.
    const auto off = w.hearEach ("Turn Off", [&] { phazite::setDeviceOn (song, delay, false); });
    checkHeard ("Turn Off", off);
    CHECK (toldAny (off[0], delay, Change::settings));
    checkHeard ("Move Effect", w.hearEach ("Move Effect", [&] { phazite::moveDevice (song, delay, 0); }));
    DeviceID container;
    checkHeard ("Add Container", w.hearEach ("Add Container", [&] { container = phazite::addContainer (song, track); }), true);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (song, container).front();
    DeviceID inChain;
    const auto intoChain =
        w.hearEach ("Add into Chain", [&] { inChain = phazite::test::addOwnDeviceToChain (song, chain, dev::Delay::typeID); });
    checkHeard ("Add into Chain", intoChain);
    CHECK (intoChain[0].told.told.at (inChain.getRaw()).after.chain == chain);
    CHECK (intoChain[0].told.told.at (inChain.getRaw()).after.container == container);

    // Another into the same chain, and the first moved past it within its chain: the chain told reordered.
    checkHeard ("Add into Chain Again",
                w.hearEach ("Add into Chain Again",
                            [&] { REQUIRE (phazite::test::addOwnDeviceToChain (song, chain, dev::Equalizer::typeID).isValid()); }));
    const auto inItsChain = w.hearEach ("Move in Chain", [&] { phazite::moveInChain (song, inChain, 1); });
    checkHeard ("Move in Chain", inItsChain);
    CHECK (inItsChain[0].told.reordered.contains ({ 2, chain.getRaw() }));
    CHECK (inItsChain[1].told.reordered.contains ({ 2, chain.getRaw() }));

    const auto other = w.s.addTrack ("Other");

    // The container's mix, split and name; its chains added, duplicated, removed, their volume, pan, crossover and keys.
    const auto mix = w.hearEach ("Container Mix", [&] { phazite::setMix (song, container, 0.5f); });
    checkHeard ("Container Mix", mix);
    CHECK (toldAny (mix[0], container, Change::mix));
    checkHeard ("Rename Container", w.hearEach ("Rename Container", [&] { phazite::setContainerName (song, container, "Wet"); }));
    DeviceID second;
    checkHeard ("Add Chain", w.hearEach ("Add Chain", [&] { second = phazite::addChain (song, container); }));
    REQUIRE (second.isValid());
    const auto toSecond = w.hearEach ("Move to Chain", [&] { REQUIRE (phazite::moveToChain (song, inChain, second).isValid()); });
    checkHeard ("Move to Chain", toSecond);
    CHECK (toSecond[0].told.told.at (inChain.getRaw()).after.chain == second);
    checkHeard ("Split", w.hearEach ("Split", [&] { phazite::setContainerSplit (song, container, phazite::ContainerSplit::bands); }));
    const auto volume = w.hearEach ("Chain Volume", [&] { phazite::setChainVolume (song, second, -6.0f); });
    checkHeard ("Chain Volume", volume);
    CHECK (toldAny (volume[0], second, Change::chainFader));
    checkHeard ("Chain Pan", w.hearEach ("Chain Pan", [&] { phazite::setChainPan (song, second, 0.5f); }));
    const auto crossover = w.hearEach ("Chain Crossover", [&] { phazite::setChainCrossover (song, second, 800.0f); });
    checkHeard ("Chain Crossover", crossover);
    CHECK (toldAny (crossover[0], second, Change::chainRange));
    checkHeard ("Duplicate Chain", w.hearEach ("Duplicate Chain", [&] { phazite::duplicateChain (song, second); }));
    checkHeard ("Remove Chain", w.hearEach ("Remove Chain", [&] { phazite::removeChain (song, second); }));
    checkHeard ("Move Chain",
                w.hearEach ("Move Chain", [&] { phazite::moveChain (song, phazite::getChains (song, container).back(), 0); }));

    const auto instruments = phazite::addInstrumentContainer (w.s.context, other);
    REQUIRE (instruments.isValid());
    const auto keysChain = phazite::getChains (song, instruments).front();
    checkHeard ("Chain Keys", w.hearEach ("Chain Keys", [&] { phazite::setChainKeys (song, keysChain, { 12, 60 }); }));
    checkHeard ("Chain Velocities", w.hearEach ("Chain Velocities", [&] { phazite::setChainVelocities (song, keysChain, { 20, 100 }); }));

    // Grouped into a container and taken apart.
    DeviceID grouped;
    checkHeard ("Group", w.hearEach ("Group", [&] { grouped = phazite::groupIntoContainer (song, { eq }); }), true);
    REQUIRE (grouped.isValid());
    checkHeard ("Ungroup", w.hearEach ("Ungroup", [&] { phazite::ungroupContainer (song, grouped); }), true);

    // Removed, and an instrument replaced.
    checkHeard ("Remove Effect", w.hearEach ("Remove Effect", [&] { phazite::removeDevice (song, eq); }));
    const auto replaced = w.hearEach ("Replace Instrument", [&] { phazite::test::setOwnInstrument (song, track, dev::Sampler::typeID); });
    checkHeard ("Replace Instrument", replaced);

    // A set equal to what the song holds: nothing told.
    const auto same = w.hear ([&] { phazite::setDeviceOn (song, delay, false); });
    CHECK (same.told.calls == 0);
}

TEST_CASE ("Song values' devices: a sample loaded, replaced and taken out, a pad's sound, Swap's trial and a preset loaded tell "
           "the device's sounds and values")
{
    Watched w;
    auto& song = w.song;
    const auto drumsTrack = w.s.firstTrack;
    const auto drums = w.s.setInstrument (drumsTrack, dev::DrumSampler::typeID);
    const auto kick = phazite::test::writeSound (w.s.getSoundsFolder().getChildFile ("Kick.wav"), 0.1);
    const auto snare = phazite::test::writeSound (w.s.getSoundsFolder().getChildFile ("Snare.wav"), 0.2);

    const auto loaded = w.hearEach ("Pad Sound", [&] { REQUIRE (phazite::setPadSound (song, drums, 36, kick)); });
    checkHeard ("Pad Sound", loaded);
    CHECK (toldAny (loaded[0], drums, Change::sounds));
    CHECK (toldAny (loaded[0], drums, Change::soundsAppeared));
    const auto replacedSound = w.hearEach ("Replace Pad Sound", [&] { REQUIRE (phazite::setPadSound (song, drums, 36, snare)); });
    checkHeard ("Replace Pad Sound", replacedSound);
    CHECK (toldAny (replacedSound[0], drums, Change::sounds));
    CHECK_FALSE (toldAny (replacedSound[0], drums, Change::soundsAppeared));
    checkHeard ("Clear Pad", w.hearEach ("Clear Pad", [&] { phazite::clearPadSound (song, drums, 36); }));

    // A pad's field among the sampler's thousands of values.
    const auto padField = getParameterID (dev::DrumSampler::info, dev::DrumSampler::getParameterIndex (40, (dev::DrumSampler::Field) 1));
    checkHeard ("Pad Field", w.hearEach ("Pad Field", [&] { setValue (song, drums, padField, 0.3f); }));

    // Swap's trial: opened, tried twice, kept; then tried and put back.
    {
        auto first = w.hear (
            [&]
            {
                phazite::Swap swap (w.s.context, drums, 38);
                REQUIRE (swap.isOpen());
                REQUIRE (swap.tryFile (kick));
                REQUIRE (swap.tryFile (snare));
                swap.keep();
            });
        checkHeard ("Swap kept", { first });
        CHECK (toldAny (first, drums, Change::sounds));
        phazite::test::waitForAudioWork (w.s.context);

        const auto before = phazite::findDevice (song, drums);
        auto putBack = w.hear (
            [&]
            {
                phazite::Swap swap (w.s.context, drums, 38);
                REQUIRE (swap.tryFile (kick));
                swap.putBack();
            });
        checkHeard ("Swap put back", { putBack });
        CHECK (*phazite::findDevice (song, drums) == *before);
    }

    // A sampler's sample, then a preset saved and loaded into another sampler.
    const auto keys = w.s.addTrack ("Keys");
    const auto sampler = w.s.setInstrument (keys, dev::Sampler::typeID);
    checkHeard ("Sample", w.hearEach ("Sample", [&] { REQUIRE (phazite::setSampleFile (song, sampler, kick)); }));
    checkHeard ("Factory Sample", w.hearEach ("Factory Sample", [&] { phazite::setFactorySample (song, sampler, 1); }));
    {
        const phazite::UndoStep step (song, "Turn");
        setValue (song, sampler, "fine", 10.0f);
    }

    const auto preset = w.s.getSoundsFolder().getChildFile ("Sampler.phazitepreset");
    REQUIRE (phazite::saveDevicePreset (w.s.context, sampler, preset));
    const auto keysTwo = w.s.addTrack ("Keys Two");
    const auto otherSampler = w.s.setInstrument (keysTwo, dev::Sampler::typeID);
    const auto presetLoaded = w.hear ([&] { REQUIRE (phazite::loadPresetInto (w.s.context, otherSampler, preset)); });
    phazite::test::waitForAudioWork (w.s.context);
    CHECK (toldOf (presetLoaded, otherSampler));
}

TEST_CASE ("Song values' devices: a sidechain set and cleared, a compressor's mode switched, a plug-in added and its mix")
{
    Watched w;
    auto& song = w.song;
    const auto bass = w.s.firstTrack;
    const auto kick = w.s.addTrack ("Kick");
    const auto compressor = w.s.addEffect (bass, dev::Compressor::typeID);

    const auto set = w.hearEach ("Sidechain", [&] { phazite::setSidechainSource (song, compressor, kick); });
    checkHeard ("Sidechain", set);
    CHECK (toldAny (set[0], compressor, Change::settings));
    const auto mode = getParameterID (dev::Compressor::info, dev::Compressor::mode);
    const auto switched = w.hearEach ("Mode", [&] { setValue (song, compressor, mode, (float) (int) dev::Compressor::Mode::transient); });
    checkHeard ("Mode", switched);
    CHECK (toldAny (switched[0], compressor, Change::sidechainSwitched));
    checkHeard ("Clear Sidechain", w.hearEach ("Clear Sidechain", [&] { phazite::setSidechainSource (song, compressor, {}); }));

    phazite::test::TestPlugin plugin;
    DeviceID added;
    checkHeard ("Add Plug-in", w.hearEach ("Add Plug-in", [&] { added = phazite::test::TestPlugin::add (song, bass); }));
    REQUIRE (added.isValid());
    const auto mix = w.hearEach ("Plug-in Mix", [&] { phazite::setMix (song, added, 0.5f); });
    checkHeard ("Plug-in Mix", mix);
    CHECK (toldAny (mix[0], added, Change::mix));
}

TEST_CASE ("Song values' devices: what takes devices along tells them: a track with its devices deleted, duplicated, moved into a "
           "folder, a folder made a bus and ungrouped, a version applied")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto other = w.s.addTrack ("Other");
    DeviceID synth, eq, container, onOther;

    {
        const phazite::UndoStep step (song, "Devices");
        synth = phazite::test::setOwnInstrument (song, track, dev::Synth::typeID);
        eq = phazite::test::addOwnDevice (song, track, dev::Equalizer::typeID);
        container = phazite::addContainer (song, track);
        phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), dev::Delay::typeID);
        onOther = phazite::test::addOwnDevice (song, other, dev::Equalizer::typeID);
    }

    const auto duplicated =
        w.hearEach ("Duplicate Track", [&] { REQUIRE (phazite::duplicateTracks (song, { track }, nullptr).size() == 1); });
    checkHeard ("Duplicate Track", duplicated, true);
    TrackID folder;
    checkHeard ("Insert Folder", w.hearEach ("Insert Folder", [&] { folder = phazite::insertFolder (song, other, nullptr); }), true);
    const auto moved = w.hearEach ("Move into Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    checkHeard ("Move into Folder", moved, true);
    checkHeard ("Make Bus", w.hearEach ("Make Bus", [&] { phazite::makeBus (song, folder); }), true);
    checkHeard ("Bus Effect", w.hearEach ("Bus Effect", [&] { phazite::test::addOwnDevice (song, folder, dev::Compressor::typeID); }));
    checkHeard ("Ungroup", w.hearEach ("Ungroup", [&] { phazite::ungroupFolder (song, folder); }), true);

    const auto deleted = w.hearEach ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    checkHeard ("Delete Track", deleted, true);

    for (const auto id : { synth, eq, container })
    {
        CHECK (deleted[0].told.told.at (id.getRaw()).wasThere);
        CHECK_FALSE (deleted[0].told.told.at (id.getRaw()).isThere);
        CHECK (deleted[1].told.told.at (id.getRaw()).isThere);
    }

    // A version applied: the song as it was at the start of this test, each way, and one track's line.
    const auto then = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Change");
        phazite::setDeviceOn (song, onOther, false);
        phazite::test::addOwnDevice (song, other, dev::Delay::typeID);
    }

    const auto now = phazite::test::copyWholeSong (song);
    checkHeard ("Make Song Like", w.hearEach ("Make Song Like", [&] { phazite::makeSongLike (song, then); }), true);
    checkHeard ("Make Song Like Again", { w.hear ([&] { phazite::makeSongTheSame (song, now); }) }, true);
    checkHeard ("Make Track Like", w.hearEach ("Make Track Like", [&] { phazite::makeTrackLike (song, other, then); }), true);
}

TEST_CASE ("Song values' devices: another area's change and what the devices' value doesn't cover tell nothing of the devices, "
           "nor their Undo and Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto synth = w.s.setInstrument (track, dev::Synth::typeID);
    const auto container = phazite::addContainer (song, track);
    const auto chain = phazite::getChains (song, container).front();
    phazite::test::TestPlugin plugin;
    DeviceID added;

    {
        const phazite::UndoStep step (song, "Plug-in");
        added = phazite::test::TestPlugin::add (song, track);
    }

    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    const auto reverb = w.s.addReturn (track, dev::Delay::typeID);

    const std::vector<std::pair<const char*, std::function<void()>>> edits {
        { "a note", [&] { w.s.addNote (clip, 60, 0.0, 1.0); } },
        { "a curve",
          [&]
          {
              w.s.addAutomation ({ synth, "cutoff" },
                                 { { phazite::BeatPosition::fromBeats (0.0), 0.2f }, { phazite::BeatPosition::fromBeats (2.0), 0.8f } });
          } },
        { "a send's level", [&] { phazite::setSendLevel (song, phazite::getSends (song, track).front(), -12.0f); } },
        { "a modulator", [&] { w.s.addModulator (track, phazite::ModulatorType::lfo); } },
        { "a plug-in's value",
          [&] { phazite::setValueSetByHand (song, phazite::ParameterRef { added, phazite::test::TestPlugin::gainID }, 0.3f); } },
        { "a plug-in's state", [&] { phazite::test::setPluginStateInStep (song, added, juce::MemoryBlock ("abc", 3)); } },
        { "a chain muted", [&] { phazite::setChainMuted (song, chain, true); } },
        { "a chain soloed", [&] { phazite::setChainSoloed (song, chain, true); } },
        { "a window moved", [&] { phazite::setWindowPlace (song, synth, phazite::WindowPlace { .x = 100, .y = 120 }); } },
    };

    juce::ignoreUnused (reverb);

    for (const auto& [what, edit] : edits)
    {
        INFO (what);

        for (const auto& heard : w.hearEach (what, edit))
            CHECK (heard.told.calls == 0);
    }
}

TEST_CASE ("Song values' devices: a knob turned gives the engine no plan, its first value set by hand and a device switched one, "
           "as before")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto eq = s.addEffect (track, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (s.context, track);
    const auto chain = phazite::getChains (s.context, container).front().id;
    const auto drums = s.setInstrument (s.addTrack ("Drums"), dev::DrumSampler::typeID);
    const auto kick = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"), 0.1);
    REQUIRE (phazite::setPadSound (song, drums, 36, kick));
    phazite::test::PlanCounter plans (s.context);
    plans.take();
    const auto gain = getParameterID (dev::Equalizer::info, gainIndex);
    const auto phase = getParameterID (dev::Equalizer::info, dev::Equalizer::phaseParameter);

    struct Edit
    {
        const char* what = "";
        std::function<void()> make {};
        int plans = 0;
    };

    const auto inStep = [&song] (const char* name, const std::function<void()>& change)
    {
        return [&song, name, change]
        {
            const phazite::UndoStep step (song, name);
            change();
        };
    };

    // In the order they are made, each on the song as the ones before left it.
    const std::vector<Edit> edits {
        { "its first value set by hand", inStep ("Turn", [&] { setValue (song, eq, gain, 3.0f); }), 1 },
        { "another", inStep ("Turn", [&] { setValue (song, eq, gain, 6.0f); }), 0 },
        { "its Undo", [&] { phazite::undo (song); }, 0 },
        { "its Redo", [&] { phazite::redo (song); }, 0 },
        { "back to its default",
          inStep ("Turn", [&] { setValue (song, eq, gain, dev::Equalizer::info.parameters[(size_t) gainIndex].defaultValue); }), 1 },
        { "a value that changes its latency", inStep ("Phase", [&] { setValue (song, eq, phase, 1.0f); }), 1 },
        { "switched off", [&] { phazite::setDeviceOn (s.context, eq, false); }, 1 },
        { "its Undo", [&] { phazite::undo (song); }, 1 },
        { "the container's mix", inStep ("Mix", [&] { phazite::setMix (song, container, 0.5f); }), 0 },
        { "the container renamed", [&] { phazite::renameContainer (s.context, container, "Wet"); }, 0 },
        { "its chain's volume", inStep ("Volume", [&] { phazite::setChainVolume (song, chain, -6.0f); }), 0 },
        { "its chain's pan", inStep ("Pan", [&] { phazite::setChainPan (song, chain, 0.25f); }), 0 },
        { "a chain added", [&] { phazite::addChain (s.context, container); }, 1 },
        { "a pad's sound set in place",
          inStep ("Pad", [&] { phazite::setDeviceSound (song, drums, phazite::schema::getPadSound (36, 0), "909 Clap"); }), 0 },
        { "the effect removed", [&] { phazite::removeDevice (s.context, eq); }, 1 },
        { "its Undo", [&] { phazite::undo (song); }, 1 },
        { "a knob turned beside its latency's value", inStep ("Turn", [&] { setValue (song, eq, gain, 6.0f); }), 0 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
    }

    // A value that changes its latency in a gesture (its others held, so that its values set by hand never go): the plan
    // waits for the gesture's end, one for the drag.
    {
        phazite::Gesture gesture (song, "Drag Phase");
        setValue (song, eq, phase, 0.0f);
        setValue (song, eq, phase, 1.0f);
        setValue (song, eq, phase, 0.0f);
        CHECK (plans.take() == 0);
        gesture.commit();
    }

    CHECK (plans.take() == 1);
}

TEST_CASE ("Song values' devices: a knob, a chain's volume and a container's mix set while the song plays play in the next block, "
           "from the values the song's values tell, and their Undo too")
{
    phazite::test::TestSong s;
    s.setTempo (120.0);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    s.addNote (s.addMidiClip (track, 0.0, 16.0), 60, 0.0, 16.0);
    const auto eq = s.addEffect (track, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (s.context, track);
    const auto chain = phazite::getChains (s.context, container).front().id;
    auto& song = *phazite::findSong (s.context);
    phazite::test::EnginePlayback playback;
    auto& player = phazite::getSession (s.context)->getPlayer();
    s.run ("transport.playStop");
    playback.play (0.1);
    phazite::test::PlanCounter plans (s.context);
    plans.take();

    const auto gain = getParameterID (dev::Equalizer::info, gainIndex);

    {
        const phazite::UndoStep step (song, "Turn");
        setValue (song, eq, gain, 4.0f);
    }

    {
        const phazite::UndoStep step (song, "Chain Volume");
        phazite::setChainVolume (song, chain, -12.0f);
    }

    playback.play (0.05);
    auto* node = player.findDevice ((std::int64_t) eq.getRaw());
    auto* chainState = player.findChain ((std::int64_t) chain.getRaw());
    REQUIRE (node != nullptr);
    REQUIRE (chainState != nullptr);
    CHECK (node->getValue (gainIndex) == doctest::Approx (4.0f));
    CHECK (chainState->getFader().getVolume() == doctest::Approx (-12.0f));
    CHECK (plans.take() == 1); // the knob's first value set by hand alone

    phazite::undo (song);
    phazite::undo (song);
    playback.play (0.05);
    CHECK (node->getValue (gainIndex) == doctest::Approx (dev::Equalizer::info.parameters[(size_t) gainIndex].defaultValue));
    CHECK (chainState->getFader().getVolume() == doctest::Approx (0.0f));
    s.run ("transport.playStop");
}

TEST_CASE ("Song values' devices: the unsaved check compares the devices it was told of with the saved copy, as the whole "
           "comparison does: changed and changed back is saved again, past mostToCompare of them too")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    const auto eq = s.addEffect (track, dev::Equalizer::typeID);
    const auto delay = s.addEffect (track, dev::Delay::typeID);
    const auto container = phazite::addContainer (s.context, track);
    const auto chain = phazite::getChains (s.context, container).front().id;
    const auto gain = getParameterID (dev::Equalizer::info, gainIndex);
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // What the quick check says, against the whole comparison, after the edits.
    const auto check = [&project] (const juce::String& name, bool unsaved, const std::function<void()>& edits)
    {
        INFO (name.toStdString());
        edits();
        CHECK (project.isWholeAsSaved() == ! unsaved);
        CHECK (project.hasUnsavedChanges() == unsaved);

        if (unsaved)
            REQUIRE (project.save().wasOk());
    };

    const auto turn = [&song, &eq, &gain] (float value)
    {
        const phazite::UndoStep step (song, "Turn");
        setValue (song, eq, gain, value);
    };

    check ("a knob turned and its Undo", false,
           [&]
           {
               turn (3.0f);
               phazite::undo (song);
           });
    check ("an effect moved past another and back", false,
           [&]
           {
               phazite::moveEffect (s.context, delay, -1);
               phazite::moveEffect (s.context, delay, 1);
           });
    check ("an effect removed and its removal undone", false,
           [&]
           {
               phazite::removeDevice (s.context, eq);
               phazite::undo (song);
           });
    check ("a chain's volume and its Undo", false,
           [&]
           {
               {
                   const phazite::UndoStep step (song, "Volume");
                   phazite::setChainVolume (song, chain, -6.0f);
               }

               phazite::undo (song);
           });
    check ("a knob turned", true, [&] { turn (4.0f); });
    check ("an effect switched off", true, [&] { phazite::setDeviceOn (s.context, delay, false); });
    check ("an effect moved", true, [&] { phazite::moveEffect (s.context, delay, -1); });
    check ("a chain added", true, [&] { phazite::addChain (s.context, container); });
    check ("an effect removed", true, [&] { phazite::removeDevice (s.context, eq); });

    // More devices than are compared one by one: the tracks compared whole.
    std::vector<DeviceID> many;

    {
        const phazite::UndoStep step (song, "Add Effects");

        for (size_t i = 0; i < phazite::Project::mostToCompare + 6; ++i)
            many.push_back (phazite::test::addOwnDevice (song, phazite::insertTrack (song, track, nullptr), dev::Equalizer::typeID));
    }

    REQUIRE (project.save().wasOk());
    const auto turnAll = [&song, &many, &gain] (float value)
    {
        const phazite::UndoStep step (song, "Turn Them");

        for (const auto id : many)
            setValue (song, id, gain, value);
    };

    check ("many turned and turned back", false,
           [&]
           {
               turnAll (5.0f);
               turnAll (dev::Equalizer::info.parameters[(size_t) gainIndex].defaultValue);
           });
    check ("many turned", true, [&] { turnAll (5.0f); });
}

TEST_CASE ("Song values' devices: learning a device's or a chain's parameter ends as it goes, in that call, by an undo too, and "
           "as its track goes, and stays while another goes")
{
    // A device's, a plug-in's or a chain's parameter ends as the song's values tell its device or chain taken out
    // (SongValues.h: devicesChanged(); MidiLinks.h: MidiLinker), at once.
    phazite::test::TestSong s;
    auto& context = s.context;
    auto& song = *phazite::findSong (context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto eq = s.addEffect (track, dev::Equalizer::typeID);
    const auto delay = s.addEffect (track, dev::Delay::typeID);
    const auto onOther = s.addEffect (other, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (context, track);
    const auto second = phazite::addChain (context, container);
    REQUIRE (second.isValid());
    const auto gain = dev::Equalizer::info.parameters[(size_t) gainIndex].id;

    // Another device goes: still learning; the device goes: learning ends at once, and its Undo brings it back, not the
    // learning.
    const auto knob = phazite::getDeviceHandle (context, eq).getParameter (gain);
    REQUIRE (knob.exists());
    phazite::startMidiLearn (knob);
    REQUIRE (phazite::isLearningMidi (knob));
    phazite::removeDevice (context, delay);
    CHECK (phazite::isLearningMidi (knob));
    phazite::removeDevice (context, eq);
    CHECK_FALSE (phazite::isLearningMidi (context));
    phazite::undo (song);
    REQUIRE (phazite::hasDevice (song, eq));
    CHECK_FALSE (phazite::isLearningMidi (context));

    // An undo that takes the device away again ends it too.
    const auto again = phazite::getDeviceHandle (context, eq).getParameter (gain);
    phazite::startMidiLearn (again);
    REQUIRE (phazite::isLearningMidi (again));
    phazite::redo (song);
    CHECK_FALSE (phazite::hasDevice (song, eq));
    CHECK_FALSE (phazite::isLearningMidi (context));

    // A chain's volume, as the chain goes.
    const auto volume = phazite::getChainVolume (context, second);
    REQUIRE (volume.exists());
    phazite::startMidiLearn (volume);
    REQUIRE (phazite::isLearningMidi (volume));
    phazite::removeChain (context, second);
    CHECK_FALSE (phazite::isLearningMidi (context));

    // A device's, as its track goes.
    const auto onOtherKnob = phazite::getDeviceHandle (context, onOther).getParameter (gain);
    phazite::startMidiLearn (onOtherKnob);
    REQUIRE (phazite::isLearningMidi (onOtherKnob));

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { other }, nullptr);
    }

    CHECK_FALSE (phazite::isLearningMidi (context));
}

TEST_CASE ("Song values' devices: devices on the master, added, turned while playing, switched off, a container with a chain, "
           "removed, and the Undo and Redo of each, told on the master, planned, and heard by the versions and the views as "
           "the master's")
{
    // The master's place is a flag (DevicePlace::onMaster), never its ID; what follows the devices names the master's channel
    // by its ID (the views: SongChanges::deviceChannels), its line (the versions) and its strip (what plays).
    struct View : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { heard.push_back (changes); }

        std::vector<phazite::SongChanges> heard;
    };

    Watched w;
    auto& song = w.song;
    auto& context = w.s.context;
    w.s.setTempo (120.0);
    const auto track = w.s.firstTrack;
    w.s.setInstrument (track, dev::Synth::typeID);
    w.s.addNote (w.s.addMidiClip (track, 0.0, 16.0), 60, 0.0, 16.0);
    const auto master = phazite::getMasterID (song);
    View view;
    context.addListener (&view);
    phazite::test::EnginePlayback playback;
    auto& player = phazite::getSession (context)->getPlayer();
    w.s.run ("transport.playStop");
    playback.play (0.1);
    phazite::test::PlanCounter plans (context);

    // A version kept as the song is: the changes from it on are the edit's alone.
    const auto keepVersion = [&context]
    {
        REQUIRE (phazite::test::waitForVersionHistory (context));
        phazite::setVersionDraft (context, {}, {});
        std::optional<juce::Result> result;
        REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
        REQUIRE (result->wasOk());
        REQUIRE (phazite::test::waitForVersionHistory (context));
    };

    // An edit as a step, its Undo and its Redo, each: the item told on the master; the plans it made; the views' devices
    // group, the master's channel named alone (none for a parameter's value, which no view shows as a change); and the changes
    // since the version kept, the master's line alone (none after the Undo). A plan made first lets go of the plans the
    // blocks played before it replaced, so that what is counted from there is the edit's alone (PerformanceCounts.h).
    const auto edit = [&] (const juce::String& name, const std::function<void()>& change, const std::function<DeviceID()>& item,
                           int plansMade, bool shown)
    {
        keepVersion();
        plans.makePlanNow();
        plans.take();
        view.heard.clear();
        const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
        std::vector<Watched::Heard> heard;

        for (size_t i = 0; i < moments.size(); ++i)
        {
            INFO (name.toStdString() << ", " << moments[i]);
            heard.push_back (w.hear (
                [&]
                {
                    if (i == 0)
                    {
                        const phazite::UndoStep step (song, name);
                        change();
                    }
                    else if (i == 1)
                    {
                        phazite::undo (song);
                    }
                    else
                    {
                        phazite::redo (song);
                    }
                }));

            const auto& told = heard.back().told.told;
            const auto found = told.find (item().getRaw());
            REQUIRE (found != told.end());
            const auto& entry = found->second;

            if (entry.wasThere)
                CHECK ((entry.before.onMaster && ! entry.before.track.isValid()));

            if (entry.isThere)
                CHECK ((entry.after.onMaster && ! entry.after.track.isValid()));

            CHECK (plans.take() == plansMade); // the song's changes delivered first, the views' among them

            auto devices = false, any = false;
            std::vector<TrackID> named;

            for (const auto& changes : std::exchange (view.heard, {}))
            {
                devices = devices || changes.devices;
                any = any || changes.deviceChannels.any;
                named.insert (named.end(), changes.deviceChannels.named.begin(), changes.deviceChannels.named.end());
            }

            CHECK (devices == shown);
            CHECK_FALSE (any);
            CHECK (named.empty() == ! shown);
            CHECK (std::all_of (named.begin(), named.end(), [master] (TrackID id) { return id == master; }));

            const auto lines = phazite::getChangesSinceLastVersion (phazite::test::withChangesIn (context));

            if (i == 1)
            {
                CHECK (lines.empty());
            }
            else
            {
                REQUIRE (lines.size() == 1);
                CHECK (lines.front().isMaster);
            }
        }

        checkHeard (name, heard, true); // a container comes with its macros
    };

    // An effect added, its knob turned while the song plays (in the next block, with no plan), switched off; a container with
    // a chain; the effect removed.
    DeviceID eq;
    edit ("Add to Master", [&] { eq = phazite::test::addOwnDevice (song, master, dev::Equalizer::typeID); }, [&] { return eq; }, 1, true);
    REQUIRE (eq.isValid());
    const auto gain = getParameterID (dev::Equalizer::info, gainIndex);

    {
        const phazite::UndoStep step (song, "Turn Knob");
        setValue (song, eq, gain, 2.0f); // its first value set by hand, which makes a plan
    }

    plans.take();
    playback.play (0.05);
    edit ("Turn Knob", [&] { setValue (song, eq, gain, 4.0f); }, [&] { return eq; }, 0, false);
    playback.play (0.05);
    auto* node = player.findDevice ((std::int64_t) eq.getRaw());
    REQUIRE (node != nullptr);
    CHECK (node->getValue (gainIndex) == doctest::Approx (4.0f)); // the Redo's
    plans.makePlanNow();
    plans.take();
    phazite::undo (song);
    CHECK (plans.take() == 0);
    playback.play (0.05);
    CHECK (node->getValue (gainIndex) == doctest::Approx (2.0f));
    phazite::redo (song);

    edit ("Turn Off", [&] { phazite::setDeviceOn (song, eq, false); }, [&] { return eq; }, 1, true);
    DeviceID container;
    edit ("Add Container", [&] { container = phazite::addContainer (song, master); }, [&] { return container; }, 1, true);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (song, container).front();
    CHECK (phazite::findDevice (song, chain)->place.onMaster);
    edit ("Remove Effect", [&] { phazite::removeDevice (song, eq); }, [&] { return eq; }, 1, true);

    w.s.run ("transport.playStop");
    context.removeListener (&view);
}

TEST_CASE ("Song values' devices: an Undo that brings back a sampler's only sound loads it once, though it makes a plan")
{
    // A device's first sound and its last make a plan (as the device's SOUNDS coming and going did), and are loaded at once
    // as every sound set in place is (SongPlayer::takeDevice()): the plan that follows doesn't load them a second time.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto sampler = s.setInstrument (s.firstTrack, dev::Sampler::typeID);
    const auto kick = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"), 0.1);
    REQUIRE (phazite::setSampleFile (song, sampler, kick));
    phazite::setFactorySample (song, sampler, 0); // its sample taken away: no sound left
    REQUIRE (phazite::findDevice (song, sampler)->sounds.empty());
    phazite::test::PlanCounter plans (s.context);
    plans.take();

    const auto loads = phazite::SongPlayer::countSoundLoads();
    phazite::undo (song);
    REQUIRE_FALSE (phazite::findDevice (song, sampler)->sounds.empty());
    CHECK (plans.take() == 1);
    CHECK (phazite::SongPlayer::countSoundLoads() - loads == 1);

    // Its Redo takes it away again: once, at once, and not a second time by the plan that follows.
    const auto again = phazite::SongPlayer::countSoundLoads();
    phazite::redo (song);
    CHECK (plans.take() == 1);
    CHECK (phazite::SongPlayer::countSoundLoads() - again == 1);
}

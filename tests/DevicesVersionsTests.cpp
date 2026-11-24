// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions write the song's devices through the devices' setters and the store's functions (roadmap step S5.20, the
// devices' versions pass; model/versions/ApplySong.cpp: makeDevicesLike(), and a merge's choice: model/mixer/DeviceClashes.cpp):
// a version applied to the whole song, in the history (one step, its Undo and its Redo) and outside it, and the song as
// copied, after a song changed since in every way a device can (one added, removed, its knob turned, its sound replaced,
// moved within its chain, grouped into a container, a container taken apart, a chain added and removed, a device moved into
// a container's chain, an instrument replaced), each pass leaving the song's devices the version's, a second writing
// nothing; plug-ins kept as the same instances, given back their states, one removed since brought back, one added since
// taken away; one track's line put back, a device moved by a version into a container on another track coming home as the
// same instance, or sent home from the line put back as it is now; a folder gone since brought back with its tracks and
// their devices, curves, modulators and sends; a track added since taken away while the version holds one of its devices on
// another track; what the version couldn't read (a track, a device) left as the song has it; and each kind of a merge's
// clash on a device chosen each way, what the store holds of one through its own chooser (known bug 239's case of a
// plug-in's value both lines set taken away for what was before), and the paths refused.
//   phazite_tests.exe --source-file=*DevicesVersionsTests*
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::DeviceID;
using phazite::DevicePlace;
using phazite::DeviceValue;
using phazite::TrackID;
using phazite::WindowPlace;
using phazite::test::TestPlugin;

// A step of the test's own, around what isn't a change of its own (a pass, a setter, a choice).
std::function<void()> inStep (phazite::Song& song, const juce::String& name, std::function<void()> change)
{
    return [&song, name, change = std::move (change)]
    {
        const phazite::UndoStep step (song, name);
        change();
    };
}

// A song's devices as a whole copy of it holds them, each read as the song holds one (DevicesValue.h: readHeldDevice()): each
// track's (a folder's before the tracks in it) and the master's, each owner before what it holds.
phazite::SongDevices readHeldDevices (const juce::ValueTree& whole)
{
    phazite::SongDevices devices;
    const std::function<void (const juce::ValueTree&)> readIn = [&devices, &readIn] (const juce::ValueTree& owner)
    {
        for (const auto& child : owner)
        {
            if (! (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::CHAIN)))
                continue;

            if (const auto value = phazite::readHeldDevice (child, phazite::readDevicePlace (child)))
            {
                devices = phazite::withDevice (devices, *value);
                readIn (child);
            }
        }
    };

    for (const auto& track : phazite::getTrackElements (whole))
        readIn (track);

    readIn (whole.getChildWithName (schema::MASTER));
    return devices;
}

// Whether the song holds the devices a whole copy holds, and the whole copy's song as text in what a version compares.
void checkHoldsAsIn (const phazite::Song& song, const juce::ValueTree& whole)
{
    CHECK (phazite::getDevicesValue (song) == readHeldDevices (whole));
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), whole));
    phazite::test::checkSong (song);
}

// The make of a version applied as Bring Back makes it: the song made like it, and what names what it hasn't forgotten.
void bringBack (phazite::Song& song, const juce::ValueTree& version)
{
    phazite::makeSongLike (song, version);
    phazite::removeBrokenReferences (song);
}

void restoreLine (phazite::Song& song, TrackID track, const juce::ValueTree& version)
{
    REQUIRE (phazite::makeTrackLike (song, track, version));
    phazite::removeBrokenReferences (song);
}

// A device moved by hand through the devices' setter to a place (what no edit but a version does: Mixer.h, setDevice()).
void moveTo (phazite::Song& song, DeviceID device, const DevicePlace& place)
{
    const auto* found = phazite::findDevice (song, device);
    REQUIRE (found != nullptr);
    auto moved = *found;
    moved.place = place;
    REQUIRE (phazite::setDevice (song, moved));
}

DevicePlace placeOf (const phazite::Song& song, DeviceID device)
{
    const auto place = phazite::findDevicePlace (song, device);
    REQUIRE (place.has_value());
    return *place;
}

// A whole copy's element of a track (or the master) with an element in it put in place of another, as a version's piece that
// couldn't be read leaves one (SongPieces.h: a stand-in, schema::versions::missing): its type and its key alone.
juce::ValueTree makeStandIn (const juce::ValueTree& element)
{
    juce::ValueTree standIn (element.getType());

    for (const auto& key : schema::getKeyProperties (element.getType()))
        if (element.hasProperty (key))
            standIn.setProperty (key, element[key], nullptr);

    standIn.setProperty (schema::versions::missing, true, nullptr);
    return standIn;
}

void putStandInFor (juce::ValueTree element)
{
    auto parent = element.getParent();
    REQUIRE (parent.isValid());
    const auto index = parent.indexOf (element);
    parent.removeChild (index, nullptr);
    parent.addChild (makeStandIn (element), index, nullptr);
}

juce::ValueTree findItemIn (const juce::ValueTree& whole, DeviceID id)
{
    for (const auto& child : whole)
    {
        if ((child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::CHAIN))
            && phazite::readID (child[schema::deviceID]) == id.getRaw())
            return child;

        if (const auto found = findItemIn (child, id); found.isValid())
            return found;
    }

    return {};
}

juce::String stepOf (const juce::Identifier& type, DeviceID id)
{
    return type.toString() + ":" + juce::String ((juce::int64) id.getRaw());
}
} // namespace

TEST_CASE ("The versions make the song's devices a version's through the devices' setters: the whole song in the history and "
           "outside it, a second time writing nothing, the song as copied, each moved device the same element")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    const auto third = s.addTrack ("Third");
    DeviceID synth, eq, delay, compressor, tables, apart, apartEq, apartDelay, container, chainA, chainB, a, b, c, loose;

    {
        const phazite::UndoStep step (song, "Devices");
        synth = phazite::test::setOwnInstrument (song, first, "phaziteSynth");
        eq = phazite::test::addOwnDevice (song, first, "phaziteEq");
        delay = phazite::test::addOwnDevice (song, first, "phaziteDelay");
        compressor = phazite::test::addOwnDevice (song, first, "phaziteCompressor");

        // The second track's synth with a table, and its container of two effects in one chain.
        tables = phazite::test::setOwnInstrument (song, second, "phaziteSynth");
        phazite::setDeviceSound (song, tables, schema::getOscillatorWavetable (0), "Pulse Width");
        apart = phazite::addContainer (song, second);
        REQUIRE (apart.isValid());
        const auto apartChain = phazite::getChains (song, apart).front();
        apartEq = phazite::test::addOwnDeviceToChain (song, apartChain, "phaziteEq");
        apartDelay = phazite::test::addOwnDeviceToChain (song, apartChain, "phaziteDelay");

        // The third track's container of two chains, and an effect on the track itself.
        container = phazite::addContainer (song, third);
        REQUIRE (container.isValid());
        chainA = phazite::getChains (song, container).front();
        chainB = phazite::addChain (song, container);
        REQUIRE (chainB.isValid());
        a = phazite::test::addOwnDeviceToChain (song, chainA, "phaziteEq");
        b = phazite::test::addOwnDeviceToChain (song, chainA, "phaziteSaturator");
        c = phazite::test::addOwnDeviceToChain (song, chainB, "phaziteReverb");
        loose = phazite::test::addOwnDevice (song, third, "phaziteCompressor");
        phazite::setValueSetByHand (song, { .device = eq, .parameter = "band4Gain" }, 3.0f);
    }

    const auto version = phazite::test::copyWholeSong (song);

    // A window's place set since, outside the history, which a version applied in the history leaves as it is: a device that
    // comes back to where the version has it as the same element keeps it; one made anew would come without it.
    phazite::setWindowPlace (song, eq, { .x = 10, .y = 20 });
    phazite::setWindowPlace (song, apartEq, { .x = 30, .y = 40 });
    phazite::setWindowPlace (song, loose, { .x = 50, .y = 60 });

    // The song since: a device added, one removed, a knob turned, a sound replaced, a device moved within its chain, two
    // grouped into a container, a container taken apart, a chain added and one removed, a device moved into a container's
    // chain, an instrument replaced.
    DeviceID added, grouped, addedChain, fm;

    {
        const phazite::UndoStep step (song, "Since");
        added = phazite::test::addOwnDevice (song, first, "phaziteReverb");
        phazite::removeDevice (song, compressor);
        phazite::setValueSetByHand (song, { .device = eq, .parameter = "band4Gain" }, -6.0f);
        phazite::setDeviceSound (song, tables, schema::getOscillatorWavetable (0), "Vowels");
        phazite::moveInChain (song, b, 0);
        grouped = phazite::groupIntoContainer (song, { eq, delay });
        REQUIRE (grouped.isValid());
        phazite::ungroupContainer (song, apart);
        addedChain = phazite::addChain (song, container);
        REQUIRE (addedChain.isValid());
        phazite::removeChain (song, chainB);
        moveTo (song, loose, { .track = third, .container = container, .chain = chainA });
        fm = phazite::test::setOwnInstrument (song, first, "phaziteFM");
    }

    const auto since = phazite::test::copyWholeSong (song);
    REQUIRE_FALSE (phazite::holdTheSame (since, version));
    phazite::test::checkSong (song);
    const WindowPlace eqWindow { .x = 10, .y = 20 }, apartWindow { .x = 30, .y = 40 }, looseWindow { .x = 50, .y = 60 };

    {
        phazite::test::UndoCheck undo (s.context);

        // In the history, as Bring Back applies it: one step, which UndoCheck undoes and redoes.
        undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
        checkHoldsAsIn (song, version);
        CHECK (phazite::isContainer (song, apart));
        CHECK (phazite::getChainDevices (song, phazite::getChains (song, apart).front()) == std::vector { apartEq, apartDelay });
        CHECK (phazite::getChainDevices (song, chainA) == std::vector { a, b });
        CHECK (phazite::getChainDevices (song, chainB) == std::vector { c });
        CHECK_FALSE (phazite::hasDevice (song, grouped));
        CHECK_FALSE (phazite::hasDevice (song, addedChain));
        CHECK_FALSE (phazite::hasDevice (song, added));
        CHECK_FALSE (phazite::hasDevice (song, fm));
        CHECK (placeOf (song, loose) == DevicePlace { .track = third });

        // Each moved as the same element, its window's place with it.
        CHECK (phazite::getWindowPlace (song, eq) == std::optional (eqWindow));
        CHECK (phazite::getWindowPlace (song, apartEq) == std::optional (apartWindow));
        CHECK (phazite::getWindowPlace (song, loose) == std::optional (looseWindow));

        // A second time: nothing to write, no step.
        const auto steps = undo.getNumSteps();
        undo.check ("Again", inStep (song, "Again", [&] { bringBack (song, version); }));
        CHECK (undo.getNumSteps() == steps);

        // And back to the song since.
        undo.check ("Bring Back Again", inStep (song, "Bring Back Again", [&] { bringBack (song, since); }));
        checkHoldsAsIn (song, since);
        CHECK (phazite::getWindowPlace (song, eq) == std::optional (eqWindow));
        undo.walkBackAndForth();
    }

    // Outside the history, as switching to another branch does: everything the version's, the windows' places too.
    phazite::makeSongTheSame (song, version);
    checkHoldsAsIn (song, version);
    CHECK_FALSE (phazite::getWindowPlace (song, eq).has_value());
    CHECK_FALSE (phazite::getWindowPlace (song, loose).has_value());

    // The song as copied after an edit undone: nothing of its devices written (asserted in Debug builds: ApplySong.cpp), and a
    // window moved since put back.
    const auto copy = phazite::test::copyWholeSong (song);
    phazite::setDeviceOn (song, delay, false);
    phazite::undo (song);
    phazite::setWindowPlace (song, delay, { .x = 1, .y = 2 });
    phazite::makeSongAsCopied (song, copy);
    checkHoldsAsIn (song, copy);
    CHECK_FALSE (phazite::getWindowPlace (song, delay).has_value());
}

TEST_CASE ("A version applied keeps the plug-ins it holds as the same instances and gives back their states; one removed since "
           "comes back with its state, one added since goes")
{
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);
    const auto track = s.firstTrack;
    DeviceID kept, removed;

    {
        const phazite::UndoStep step (song, "Plug-ins");
        kept = TestPlugin::add (song, track);
        removed = TestPlugin::add (song, track);
    }

    s.waitForPlugins();
    session->update();
    auto* instance = phazite::findPluginInstance (song, kept);
    REQUIRE (instance != nullptr);
    const auto gain = TestPlugin::getGain (song, kept);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&song] { return phazite::getPluginsWithOlderStates (song).isEmpty() && phazite::getPluginsAnswering (song).isEmpty(); }));
    song.storeDeviceStates();
    const auto version = phazite::test::copyWholeSong (song);
    const auto keptState = phazite::getPluginState (song, kept);
    const auto removedState = phazite::getPluginState (song, removed);

    // Since: the kept one's gain turned in its own window (stored outside the history), then grouped into a container; one
    // removed, another added.
    TestPlugin::setGain (song, kept, gain * 0.25f);
    song.storeDeviceStates();
    REQUIRE_FALSE (phazite::getPluginState (song, kept) == keptState);
    DeviceID added, grouped;

    {
        const phazite::UndoStep step (song, "Since");
        phazite::removeDevice (song, removed);
        added = TestPlugin::add (song, track);
        grouped = phazite::groupIntoContainer (song, { kept });
        REQUIRE (grouped.isValid());
    }

    s.waitForPlugins();
    session->update();
    REQUIRE (phazite::findPluginInstance (song, kept) == instance);
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
    s.waitForPlugins();
    session->update();
    checkHoldsAsIn (song, version);

    // The kept one the same instance, out of the container, its state the version's, which it plays.
    CHECK (phazite::findPluginInstance (song, kept) == instance);
    CHECK (placeOf (song, kept) == DevicePlace { .track = track });
    CHECK (phazite::getPluginState (song, kept) == keptState);
    CHECK (
        phazite::test::dispatchMessagesUntil ([&song, kept, gain] { return std::abs (TestPlugin::getGain (song, kept) - gain) < 0.001f; }));

    // The one removed since back, with its state; the one added since gone.
    REQUIRE (phazite::findPluginInstance (song, removed) != nullptr);
    CHECK (phazite::getPluginState (song, removed) == removedState);
    CHECK_FALSE (phazite::hasDevice (song, added));
    CHECK_FALSE (phazite::hasDevice (song, grouped));

    // Undone: the kept one grouped again, still the same instance.
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    session->update();
    CHECK (phazite::findPluginInstance (song, kept) == instance);
    CHECK (phazite::findContainerOf (song, kept) == grouped);
    undo.stepBackOrForth ("Redo", [&] { phazite::redo (song); });
    session->update();
    CHECK (phazite::findPluginInstance (song, kept) == instance);

    // A second time: nothing to write.
    const auto steps = undo.getNumSteps();
    undo.check ("Again", inStep (song, "Again", [&] { bringBack (song, version); }));
    CHECK (undo.getNumSteps() == steps);
    undo.walkBackAndForth();
}

TEST_CASE ("One track's line put back gives it its devices as the version holds them, a device a version moved into a container "
           "on another track coming home as the same instance; another track's line sends it home as it is now")
{
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    DeviceID plugin, eq, container, chain, inChain;

    {
        const phazite::UndoStep step (song, "Devices");
        plugin = TestPlugin::add (song, first);
        eq = phazite::test::addOwnDevice (song, first, "phaziteEq");
        container = phazite::addContainer (song, second);
        REQUIRE (container.isValid());
        chain = phazite::getChains (song, container).front();
        inChain = phazite::test::addOwnDeviceToChain (song, chain, "phaziteEq");
    }

    s.waitForPlugins();
    session->update();
    auto* instance = phazite::findPluginInstance (song, plugin);
    REQUIRE (instance != nullptr);
    const auto version = phazite::test::copyWholeSong (song);

    // Since: the plug-in moved into the second track's container (as only a version moves one), and a knob turned on each.
    {
        const phazite::UndoStep step (song, "Since");
        moveTo (song, plugin, { .track = second, .container = container, .chain = chain });
        phazite::setValueSetByHand (song, { .device = eq, .parameter = "band4Gain" }, 6.0f);
        phazite::setValueSetByHand (song, { .device = inChain, .parameter = "band4Gain" }, -6.0f);
    }

    session->update();
    REQUIRE (phazite::findPluginInstance (song, plugin) == instance);
    const auto versionsDevices = readHeldDevices (version);
    REQUIRE (versionsDevices.find (eq) != nullptr);
    REQUIRE (versionsDevices.find (inChain) != nullptr);
    const auto eqThen = *versionsDevices.find (eq), inChainThen = *versionsDevices.find (inChain);
    const auto inChainNow = *phazite::findDevice (song, inChain), eqNow = *phazite::findDevice (song, eq);
    phazite::test::UndoCheck undo (s.context);

    // The first track's line: the plug-in comes home, the same instance; the first track's knob the version's, the second's
    // as it is now.
    undo.check ("Restore First", inStep (song, "Restore First", [&] { restoreLine (song, first, version); }));
    session->update();
    CHECK (placeOf (song, plugin) == DevicePlace { .track = first });
    CHECK (phazite::getChannelDeviceValues (song, first).front().id == plugin);
    CHECK (phazite::findPluginInstance (song, plugin) == instance);
    CHECK (*phazite::findDevice (song, eq) == eqThen);
    CHECK (*phazite::findDevice (song, inChain) == inChainNow);
    phazite::test::checkSong (song);

    // A second time: nothing to write.
    const auto steps = undo.getNumSteps();
    undo.check ("Again", inStep (song, "Again", [&] { restoreLine (song, first, version); }));
    CHECK (undo.getNumSteps() == steps);

    // Undone, and the second track's line put back instead: the plug-in, which the version holds on the first track, sent
    // there as it is now; the second track's knob the version's, the first's as it is now.
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    REQUIRE (phazite::findChainOf (song, plugin) == chain);
    undo.check ("Restore Second", inStep (song, "Restore Second", [&] { restoreLine (song, second, version); }));
    session->update();
    CHECK (placeOf (song, plugin) == DevicePlace { .track = first });
    CHECK (phazite::findPluginInstance (song, plugin) == instance);
    CHECK (*phazite::findDevice (song, inChain) == inChainThen);
    CHECK (*phazite::findDevice (song, eq) == eqNow);
    CHECK (phazite::getChainDevices (song, chain) == std::vector { inChain });
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A folder's line put back brings its tracks back with their devices, their curves, their modulators and their sends")
{
    // The three earlier passes (the curves', modulation's, the sends') each once left the tracks that come back with a folder
    // without theirs: the devices pass takes the line's tracks as they do (ApplySong.cpp: makeTrackLikeIn()).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto inside = s.firstTrack;
    const auto synth = s.setInstrument (inside, "phaziteSynth");
    const auto returnTrack = s.addReturn (inside, "phaziteReverb", -6.0f);
    const auto folder = s.addBus ("Folder", { inside });
    const auto onFolder = s.addEffect (folder, "phaziteEq");
    DeviceID container, inChain;
    phazite::ModulatorID lfo;

    {
        const phazite::UndoStep step (song, "More");
        container = phazite::addContainer (song, inside);
        REQUIRE (container.isValid());
        inChain = phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), "phaziteDelay");
        lfo = phazite::addModulator (song, inside, phazite::ModulatorType::lfo);
        phazite::setModulation (song, lfo, { .device = synth, .parameter = "cutoff" }, 0.5f);
        phazite::setValueSetByHand (song, { .device = synth, .parameter = "cutoff" }, 1000.0f);
        phazite::addPoint (song, { .device = synth, .parameter = "cutoff" }, { .beat = {}, .position = 0.5f, .curve = 0.0f });
        phazite::addPoint (song, { .device = onFolder, .parameter = "band4Gain" }, { .beat = {}, .position = 0.25f, .curve = 0.0f });
    }

    const auto sends = phazite::getSends (song, inside);
    REQUIRE (sends.size() == 1);
    const auto version = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Delete Folder", inStep (song, "Delete Folder", [&] { phazite::deleteTracks (song, { folder }, nullptr); }));
    REQUIRE_FALSE (phazite::hasDevice (song, synth));
    REQUIRE_FALSE (phazite::hasDevice (song, onFolder));

    undo.check ("Restore Folder", inStep (song, "Restore Folder", [&] { restoreLine (song, folder, version); }));
    checkHoldsAsIn (song, version);

    for (const auto device : { synth, onFolder, container, inChain })
        CHECK (phazite::hasDevice (song, device));

    CHECK (phazite::findCurve (song, { .device = synth, .parameter = "cutoff" }) != nullptr);
    CHECK (phazite::findCurve (song, { .device = onFolder, .parameter = "band4Gain" }) != nullptr);
    REQUIRE (phazite::getModulatorValue (song, lfo).has_value());
    CHECK (phazite::getModulatorValue (song, lfo)->findModulation ({ .device = synth, .parameter = "cutoff" }) != nullptr);
    CHECK (phazite::getSends (song, inside) == sends);
    CHECK (phazite::test::hasTrack (song, returnTrack));
    undo.walkBackAndForth();
}

TEST_CASE ("A track's line put back takes away a track added since, sending first a device the version holds on another track "
           "home as it is now")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto first = s.firstTrack;
    DeviceID eq, other;

    {
        const phazite::UndoStep step (song, "Devices");
        eq = phazite::test::addOwnDevice (song, first, "phaziteEq");
        other = phazite::test::addOwnDevice (song, first, "phaziteDelay");
    }

    const auto version = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (s.context);

    // Since: a track added, the equalizer moved onto it (as only a version moves one) and its knob turned there, and an
    // effect of its own; then the equalizer's window moved, outside the history.
    const auto added = s.addTrack ("Added");
    DeviceID own;

    {
        const phazite::UndoStep step (song, "Since");
        moveTo (song, eq, { .track = added });
        phazite::setValueSetByHand (song, { .device = eq, .parameter = "band4Gain" }, 6.0f);
        own = phazite::test::addOwnDevice (song, added, "phaziteReverb");
    }

    phazite::setWindowPlace (song, eq, { .x = 7, .y = 8 });
    const auto eqNow = *phazite::findDevice (song, eq);

    // The added track's line: the track goes, the equalizer sent home first, as it is now (its knob as turned), the same
    // element (its window's place kept); the track's own effect with the track.
    undo.check ("Restore Added", inStep (song, "Restore Added", [&] { restoreLine (song, added, version); }));
    CHECK_FALSE (phazite::test::hasTrack (song, added));
    CHECK_FALSE (phazite::hasDevice (song, own));
    REQUIRE (phazite::hasDevice (song, eq));
    auto home = eqNow;
    home.place = { .track = first };
    CHECK (*phazite::findDevice (song, eq) == home);
    CHECK (phazite::getWindowPlace (song, eq) == std::optional (WindowPlace { .x = 7, .y = 8 }));
    std::vector<DeviceID> onFirst;

    for (const auto& device : phazite::getChannelDeviceValues (song, first))
        onFirst.push_back (device.id);

    CHECK (onFirst == std::vector { eq, other });
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("What a version couldn't read of the devices is left as the song has it: a track's, a device's, a container's")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    DeviceID onFirst, unread, readable, container, inContainer;

    {
        const phazite::UndoStep step (song, "Devices");
        onFirst = phazite::test::addOwnDevice (song, first, "phaziteEq");
        unread = phazite::test::addOwnDevice (song, second, "phaziteEq");
        readable = phazite::test::addOwnDevice (song, second, "phaziteDelay");
        container = phazite::addContainer (song, second);
        REQUIRE (container.isValid());
        inContainer = phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), "phaziteReverb");
    }

    const auto readHere = phazite::test::copyWholeSong (song);

    // A version whose first track couldn't be read, and on the second track an equalizer and a container that couldn't.
    auto version = readHere.createCopy();
    putStandInFor (phazite::test::findTrackIn (version, first));
    putStandInFor (findItemIn (version, unread));
    putStandInFor (findItemIn (version, container));

    // Since: a knob turned on each, a device added to each track.
    DeviceID addedFirst, addedSecond;

    {
        const phazite::UndoStep step (song, "Since");

        phazite::setValueSetByHand (song, { .device = onFirst, .parameter = "band4Gain" }, 6.0f);
        phazite::setValueSetByHand (song, { .device = unread, .parameter = "band4Gain" }, 6.0f);
        phazite::setValueSetByHand (song, { .device = readable, .parameter = "feedback" }, 0.5f);
        phazite::setValueSetByHand (song, { .device = inContainer, .parameter = "size" }, 0.75f);
        addedFirst = phazite::test::addOwnDevice (song, first, "phaziteDelay");
        addedSecond = phazite::test::addOwnDevice (song, second, "phaziteCompressor");
    }

    const auto now = phazite::getDevicesValue (song);
    const auto then = readHeldDevices (readHere);
    phazite::test::UndoCheck undo (s.context);
    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));

    // What couldn't be read, as the song has it: the first track's devices, the unread equalizer, the container with what is
    // in it. What could, the version's: the delay's knob; the compressor added since gone.
    CHECK (*phazite::findDevice (song, onFirst) == *now.find (onFirst));
    CHECK (phazite::hasDevice (song, addedFirst));
    CHECK (*phazite::findDevice (song, unread) == *now.find (unread));
    CHECK (*phazite::findDevice (song, inContainer) == *now.find (inContainer));
    CHECK (*phazite::findDevice (song, readable) == *then.find (readable));
    CHECK_FALSE (phazite::hasDevice (song, addedSecond));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

namespace
{
// A merge's clashes of a song whose two lines changed what they parted from: base, the song as it was; theirs, the other
// line's changes made from there; ours, this line's, made from there too, which the song holds after (ModulationWritersTests'
// Merge, for the devices).
struct Merge
{
    phazite::Song& song;
    juce::ValueTree base {}, ours {}, theirs {};
    std::vector<phazite::MergeClash> clashes {};

    Merge (phazite::Song& s, const std::function<void()>& theirChanges, const std::function<void()>& ourChanges)
        : song (s)
    {
        base = phazite::test::copyWholeSong (song);

        {
            const phazite::UndoStep step (song, "Theirs");
            theirChanges();
        }

        theirs = phazite::test::copyWholeSong (song);
        phazite::undo (song);

        {
            const phazite::UndoStep step (song, "Ours");
            ourChanges();
        }

        ours = phazite::test::copyWholeSong (song);
        clashes = phazite::mergeSongs (base, ours, theirs, [this] { return song.makeID(); }).clashes;
    }

    // The clash whose path goes through a step and ends at another, on a property.
    const phazite::MergeClash* find (const juce::String& through, const juce::String& last, const juce::Identifier& property) const
    {
        for (const auto& clash : clashes)
            if (clash.path.contains (through) && clash.path[clash.path.size() - 1] == last && clash.property == property)
                return &clash;

        return nullptr;
    }

    // The clash chosen with this value through a chooser, checked, then undone.
    using Chooser = phazite::AreaChoice (*) (phazite::Song&, const phazite::MergeClash&, int, const phazite::MergeSides&);

    void choose (phazite::test::UndoCheck& undo, Chooser chooser, const phazite::MergeClash& clash, int value,
                 const std::function<void()>& check)
    {
        const auto before = phazite::test::copyWholeSong (song);
        auto choice = phazite::AreaChoice::notTheArea;
        undo.check ("Choose", inStep (song, "Choose",
                                      [&] { choice = chooser (song, clash, value, { .base = base, .before = ours, .theirs = theirs }); }));
        CHECK_MESSAGE (choice == phazite::AreaChoice::chosen, "choice " << (int) choice << ", value " << value);

        if (choice != phazite::AreaChoice::chosen)
            return;

        check();
        phazite::test::checkSong (song);

        if (! phazite::test::copyWholeSong (song).isEquivalentTo (before))
            phazite::undo (song);
    }
};
} // namespace

TEST_CASE ("A merge's clash on a device, a container or a chain is chosen through the devices' setter, each kind each way, on a "
           "track, in a container's chain and on the master; the paths it can't name refused")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto master = phazite::getMasterID (song);
    DeviceID synth, container, chain, inChain, onMaster;

    {
        const phazite::UndoStep step (song, "Devices");
        synth = phazite::test::setOwnInstrument (song, track, "phaziteSynth");
        container = phazite::addContainer (song, track);
        REQUIRE (container.isValid());
        chain = phazite::getChains (song, container).front();
        inChain = phazite::test::addOwnDeviceToChain (song, chain, "phaziteEq");
        onMaster = phazite::test::addOwnDevice (song, master, "phaziteEq");
    }

    // Each line: a synth's table and its cutoff, a container's name, a chain's volume, the knob of an equalizer in the chain
    // and of the master's.
    Merge merge (
        song,
        [&]
        {
            phazite::setDeviceSound (song, synth, schema::getOscillatorWavetable (0), "Vowels");
            phazite::setValueSetByHand (song, { .device = synth, .parameter = "cutoff" }, 1000.0f);
            phazite::setContainerName (song, container, "Theirs");
            phazite::setChainVolume (song, chain, -12.0f);
            phazite::setValueSetByHand (song, { .device = inChain, .parameter = "band4Gain" }, -6.0f);
            phazite::setValueSetByHand (song, { .device = onMaster, .parameter = "band4Gain" }, -3.0f);
        },
        [&]
        {
            phazite::setDeviceSound (song, synth, schema::getOscillatorWavetable (0), "Saw Build");
            phazite::setValueSetByHand (song, { .device = synth, .parameter = "cutoff" }, 4000.0f);
            phazite::setContainerName (song, container, "Ours");
            phazite::setChainVolume (song, chain, -3.0f);
            phazite::setValueSetByHand (song, { .device = inChain, .parameter = "band4Gain" }, 6.0f);
            phazite::setValueSetByHand (song, { .device = onMaster, .parameter = "band4Gain" }, 3.0f);
        });

    phazite::test::UndoCheck undo (s.context);
    const auto values = schema::VALUES.toString() + ":" + schema::VALUES.toString();
    const auto sounds = schema::SOUNDS.toString() + ":" + schema::SOUNDS.toString();
    const auto chooseEach = [&] (const phazite::MergeClash& clash, const std::function<void (int)>& check)
    {
        for (int value = 0; value < 3; ++value)
            merge.choose (undo, &phazite::chooseDeviceClash, clash, value, [&check, value] { check (value); });
    };

    const auto readValueOf = [&song] (DeviceID device, const char* parameter)
    {
        const auto* held = phazite::findDevice (song, device);
        REQUIRE (held != nullptr);
        const auto* info = phazite::getOwnDeviceInfo (song, device);
        REQUIRE (info != nullptr);
        return held->getParameterValue (phazite::devices::findParameter (*info, parameter));
    };

    // A sound (SOUNDS), each way.
    const auto* table = merge.find (stepOf (schema::DEVICE, synth), sounds, schema::getOscillatorWavetable (0));
    REQUIRE (table != nullptr);
    chooseEach (
        *table,
        [&] (int value)
        {
            const juce::String expected[] = { "", "Saw Build", "Vowels" };
            CHECK (phazite::test::copyDevice (song, synth).getChildWithName (schema::SOUNDS)[schema::getOscillatorWavetable (0)].toString()
                   == expected[value]);
        });

    // A value set by hand (VALUES), each way: what was before, its default.
    const auto* cutoff = merge.find (stepOf (schema::DEVICE, synth), values, "cutoff");
    REQUIRE (cutoff != nullptr);
    chooseEach (*cutoff,
                [&] (int value)
                {
                    if (value == 0)
                        CHECK_FALSE (phazite::test::copyDevice (song, synth).getChildWithName (schema::VALUES).hasProperty ("cutoff"));
                    else
                        CHECK (readValueOf (synth, "cutoff") == doctest::Approx (value == 1 ? 4000.0f : 1000.0f));
                });

    // A container's own property, and a chain's.
    const auto* name = merge.find (stepOf (schema::CONTAINER, container), stepOf (schema::CONTAINER, container), schema::containerName);
    REQUIRE (name != nullptr);
    const auto nameBefore = name->base.toString();
    REQUIRE (nameBefore.isNotEmpty());
    chooseEach (*name,
                [&] (int value)
                {
                    const juce::String expected[] = { nameBefore, "Ours", "Theirs" };
                    CHECK (phazite::findDevice (song, container)->name == expected[value]);
                });

    const auto* volume = merge.find (stepOf (schema::CHAIN, chain), stepOf (schema::CHAIN, chain), schema::channelVolume);
    REQUIRE (volume != nullptr);
    chooseEach (*volume,
                [&] (int value)
                {
                    const float expected[] = { 0.0f, -3.0f, -12.0f };
                    CHECK (phazite::getChainVolume (song, chain) == doctest::Approx (expected[value]));
                });

    // A device in a container's chain (CONTAINER, CHAIN, DEVICE), and the master's.
    for (const auto device : { inChain, onMaster })
    {
        const auto* gain = merge.find (stepOf (schema::DEVICE, device), values, "band4Gain");
        REQUIRE (gain != nullptr);
        const float ours = device == inChain ? 6.0f : 3.0f, theirs = device == inChain ? -6.0f : -3.0f;
        chooseEach (*gain,
                    [&] (int value)
                    {
                        if (value == 0)
                            CHECK_FALSE (phazite::test::copyDevice (song, device).getChildWithName (schema::VALUES).isValid());
                        else
                            CHECK (readValueOf (device, "band4Gain") == doctest::Approx (value == 1 ? ours : theirs));
                    });
    }

    // Refused, nothing written: a device the song hasn't, one at another place than the path's, its ID, a property its value
    // doesn't cover, the item whole, what is deeper in one.
    auto nowhere = *cutoff;
    nowhere.path.set (nowhere.path.indexOf (stepOf (schema::DEVICE, synth)), stepOf (schema::DEVICE, DeviceID::fromRaw (song.makeID())));
    auto elsewhere = *cutoff;
    elsewhere.path.insert (elsewhere.path.indexOf (stepOf (schema::DEVICE, synth)), stepOf (schema::CHAIN, chain));
    auto byID = *name;
    byID.property = schema::deviceID;
    auto unknown = *name;
    unknown.property = "nothing";
    auto whole = *name;
    whole.property = {};
    auto deeper = *cutoff;
    deeper.path.add ("PARAMETER:cutoff");

    for (const auto* refused : { &nowhere, &elsewhere, &byID, &unknown, &whole, &deeper })
    {
        const phazite::UndoStep step (song, "Refused");
        CHECK (phazite::chooseDeviceClash (song, *refused, 2, { .base = merge.base, .before = merge.ours, .theirs = merge.theirs })
               == phazite::AreaChoice::refused);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    // What isn't the devices': a path through none of them.
    auto notTheirs = *cutoff;
    notTheirs.path = juce::StringArray { "TRACKS:", "TRACK:" + juce::String ((juce::int64) track.getRaw()) };
    CHECK (phazite::chooseDeviceClash (song, notTheirs, 2, { .base = merge.base, .before = merge.ours, .theirs = merge.theirs })
           == phazite::AreaChoice::notTheArea);
    CHECK (phazite::chooseDeviceStoreClash (song, *cutoff, 2, { .base = merge.base, .before = merge.ours, .theirs = merge.theirs })
           == phazite::AreaChoice::notTheArea);
}

TEST_CASE ("A merge's clash on what the store holds of a plug-in or a chain is chosen through the store's functions, each way; a "
           "plug-in's value both lines set goes for what was before (known bug 239's shape)")
{
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    DeviceID plugin, container, chain;

    {
        const phazite::UndoStep step (song, "Devices");
        plugin = TestPlugin::add (song, track);
        container = phazite::addContainer (song, track);
        REQUIRE (container.isValid());
        chain = phazite::getChains (song, container).front();
        phazite::setPluginValueSetByHand (song, plugin, TestPlugin::toneID, 0.5f, 0.5f);
    }

    s.waitForPlugins();

    // A state the plug-in takes as its own (TestPlugin.cpp: its gain, its tone, its mix), each of another gain.
    const auto bytes = [] (int gain)
    {
        juce::MemoryOutputStream state;
        state.writeFloat ((float) gain / 4.0f);
        state.writeFloat (0.5f);
        state.writeFloat (1.0f);
        return state.getMemoryBlock();
    };

    // Each line: the plug-in's state and program, its tone (set before both), and its gain, which the line they parted from
    // set none of.
    Merge merge (
        song,
        [&]
        {
            phazite::setPluginStateInStep (song, plugin, bytes (2));
            phazite::setPluginProgramInStep (song, plugin, 2);
            phazite::setPluginValueSetByHand (song, plugin, TestPlugin::toneID, 0.25f, 0.5f);
            phazite::setPluginValueSetByHand (song, plugin, TestPlugin::gainID, 0.75f, 1.0f);
        },
        [&]
        {
            phazite::setPluginStateInStep (song, plugin, bytes (1));
            phazite::setPluginProgramInStep (song, plugin, 1);
            phazite::setPluginValueSetByHand (song, plugin, TestPlugin::toneID, 0.75f, 0.5f);
            phazite::setPluginValueSetByHand (song, plugin, TestPlugin::gainID, 0.25f, 1.0f);
        });

    phazite::test::UndoCheck undo (s.context);
    const auto deviceStep = stepOf (schema::DEVICE, plugin);
    const auto chooseEach = [&] (const phazite::MergeClash& clash, const std::function<void (int)>& check)
    {
        for (int value = 0; value < 3; ++value)
            merge.choose (undo, &phazite::chooseDeviceStoreClash, clash, value, [&check, value] { check (value); });
    };

    // Its state, and its program.
    const auto* state = merge.find (deviceStep, deviceStep, schema::pluginState);
    REQUIRE (state != nullptr);
    chooseEach (*state,
                [&] (int value)
                {
                    if (value == 0)
                        CHECK (phazite::getPluginState (song, plugin).isEmpty());
                    else
                        CHECK (phazite::getPluginState (song, plugin) == bytes (value == 1 ? 1 : 2));
                });

    const auto* program = merge.find (deviceStep, deviceStep, schema::pluginProgram);
    REQUIRE (program != nullptr);
    chooseEach (*program,
                [&] (int value)
                {
                    const std::optional<int> expected[] = { std::nullopt, 1, 2 };
                    CHECK (phazite::getPluginProgram (song, plugin) == expected[value]);
                });

    // A value both lines changed.
    const auto valueStep = [] (const char* parameter) { return schema::VALUE.toString() + ":" + parameter; };
    const auto* tone = merge.find (deviceStep, valueStep (TestPlugin::toneID), schema::valueSetByHand);
    REQUIRE (tone != nullptr);
    chooseEach (*tone,
                [&] (int value)
                {
                    const float expected[] = { 0.5f, 0.75f, 0.25f };
                    CHECK (phazite::findPluginValue (song, plugin, TestPlugin::toneID) == std::optional (expected[value]));
                });

    // A value both lines set of a parameter the line they parted from set none of: what was before, none (not a value of none,
    // which read as 0: known bug 239's shape).
    const auto* gain = merge.find (deviceStep, valueStep (TestPlugin::gainID), schema::valueSetByHand);
    REQUIRE (gain != nullptr);
    CHECK (gain->base.isVoid());
    chooseEach (*gain,
                [&] (int value)
                {
                    const std::optional<float> expected[] = { std::nullopt, 0.25f, 0.75f };
                    CHECK (phazite::findPluginValue (song, plugin, TestPlugin::gainID) == expected[value]);
                });

    // A chain's mute and the plug-in's version, which no step writes (a merge of songs another program made may clash on them).
    phazite::MergeClash mute;
    mute.path = juce::StringArray { "TRACKS:", "TRACK:" + juce::String ((juce::int64) track.getRaw()),
                                    stepOf (schema::CONTAINER, container), stepOf (schema::CHAIN, chain) };
    mute.property = schema::chainMute;
    mute.theirs = true;
    chooseEach (mute, [&] (int value) { CHECK (phazite::isChainMuted (song, chain) == (value == 2)); });

    phazite::MergeClash version;
    version.path = juce::StringArray { "TRACKS:", "TRACK:" + juce::String ((juce::int64) track.getRaw()), deviceStep };
    version.property = schema::pluginVersion;
    version.ours = "1.0";
    version.theirs = "2.0";
    chooseEach (version,
                [&] (int value)
                {
                    const juce::String expected[] = { "", "1.0", "2.0" };
                    CHECK (phazite::getPluginVersion (song, plugin) == expected[value]);
                });

    // Refused, nothing written: a VALUE's parameter, a value outside 0 to 1, a state that isn't base 64, a plug-in's store on
    // a chain, what the song hasn't.
    auto parameter = *tone;
    parameter.property = schema::valueParameter;
    auto outside = *tone;
    outside.theirs = 2.0;
    auto notBytes = *state;
    notBytes.theirs = "not base 64!";
    auto onChain = mute;
    onChain.property = schema::pluginState;
    onChain.theirs = state->theirs;
    auto nowhere = *tone;
    nowhere.path.set (nowhere.path.indexOf (deviceStep), stepOf (schema::DEVICE, DeviceID::fromRaw (song.makeID())));

    for (const auto* refused : { &parameter, &outside, &notBytes, &nowhere })
    {
        const phazite::UndoStep step (song, "Refused");
        CHECK (phazite::chooseDeviceStoreClash (song, *refused, 2, { .base = merge.base, .before = merge.ours, .theirs = merge.theirs })
               == phazite::AreaChoice::refused);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    // A plug-in's state on a chain isn't the store's: the devices' chooser refuses it.
    {
        const phazite::UndoStep step (song, "Refused");
        CHECK (phazite::chooseDeviceStoreClash (song, onChain, 2, { .base = merge.base, .before = merge.ours, .theirs = merge.theirs })
               == phazite::AreaChoice::notTheArea);
        CHECK (phazite::chooseDeviceClash (song, onChain, 2, { .base = merge.base, .before = merge.ours, .theirs = merge.theirs })
               == phazite::AreaChoice::refused);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }
}

namespace
{
// An element's name in a whole copy ("TRACK:12", "DEVICE:44"), as the versions name one.
juce::String nameOfItem (const juce::Identifier& type, std::uint64_t id)
{
    return type.toString() + ":" + juce::String ((juce::int64) id);
}

// A track's tracks, devices and containers, as a whole copy of the song holds them, in its element's order.
std::vector<juce::String> readTracksAndDevices (const juce::ValueTree& whole, TrackID track)
{
    std::vector<juce::String> names;

    for (const auto& child : phazite::test::findTrackIn (whole, track))
        if (child.hasType (schema::TRACK) || child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER))
            names.push_back (child.getType().toString() + ":" + schema::getElementKey (child));

    return names;
}

// The song as its project saved it, the version that saved it: nothing to save, and its whole copy the version's to the letter.
void checkAsSaved (phazite::Project& project, const phazite::Song& song, const juce::ValueTree& version)
{
    CHECK_FALSE (project.hasUnsavedChanges());
    CHECK (project.isWholeAsSaved());
    CHECK (phazite::test::copyWholeSong (song).toXmlString() == version.toXmlString());
    phazite::test::checkSong (song);
}

// What is written into a song's tree while it listens (a listener belongs to the tree object it was added to).
struct TreeWrites : public juce::ValueTree::Listener
{
    explicit TreeWrites (const juce::ValueTree& tree)
        : state (tree)
    {
        state.addListener (this);
    }

    ~TreeWrites() override { state.removeListener (this); }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++count; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++count; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++count; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { ++count; }

    juce::ValueTree state;
    int count = 0;
};

// What is written into a song's devices' store while it listens (SongValues.h: deviceStoreChanged()), which holds what the
// tree held of a plug-in's values set by hand until the store's switch: each entry told.
struct StoreWrites final : phazite::SongValues::Listener
{
    explicit StoreWrites (const phazite::Song& s)
        : song (s)
    {
        song.getValues()->addListener (this);
    }

    ~StoreWrites() override { song.getValues()->removeListener (this); }

    void deviceStoreChanged (const std::vector<phazite::SongValues::DeviceStoreChange>& entries, const phazite::DeviceStore&) override
    {
        count += (int) entries.size();
    }

    const phazite::Song& song;
    int count = 0;
};
} // namespace

TEST_CASE ("A version brought back puts a bus's tracks among its effects where it holds them, the project as saved: a track "
           "deleted since comes back before the effect")
{
    // A channel's devices are written before its tracks, always (DevicesArea.h: writeDevicesIn(), since the devices' switch:
    // before it a bus's effect added after its tracks stood after them in the file), so that a track that comes back, placed
    // among the tracks by the passes over elements, is where the version holds it.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    const auto bus = s.addBus ("Bus", { first, second });
    const auto effect = s.addEffect (bus, "phaziteEq");
    const std::vector<juce::String> held { nameOfItem (schema::DEVICE, effect.getRaw()), nameOfItem (schema::TRACK, first.getRaw()),
                                           nameOfItem (schema::TRACK, second.getRaw()) };
    REQUIRE (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
    REQUIRE (project.save().wasOk());
    const auto version = phazite::test::copyWholeSong (song);

    {
        phazite::test::UndoCheck undo (s.context);
        undo.check ("Delete Second", inStep (song, "Delete Second", [&] { phazite::deleteTracks (song, { second }, nullptr); }));
        REQUIRE_FALSE (project.isWholeAsSaved()); // not asked of hasUnsavedChanges(), whose mark holds till a save
        undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
        CHECK (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
        checkAsSaved (project, song, version);

        // A second time: nothing to write.
        const auto steps = undo.getNumSteps();
        undo.check ("Again", inStep (song, "Again", [&] { bringBack (song, version); }));
        CHECK (undo.getNumSteps() == steps);
        undo.walkBackAndForth();
    }

    // Outside the history, as switching to another branch does, from the song with the track deleted.
    phazite::undo (song);
    REQUIRE_FALSE (phazite::test::hasTrack (song, second));
    phazite::makeSongTheSame (song, version);
    CHECK (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
    checkAsSaved (project, song, version);
}

TEST_CASE ("A version brought back puts a bus's tracks among its effects where it holds them, the project as saved: an effect "
           "removed since comes back before a track")
{
    // A device put back into the bus's list is written before its tracks, as the version holds it (DevicesArea.h:
    // writeDevicesIn(), since the devices' switch).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto first = s.firstTrack;
    const auto bus = s.addBus ("Bus", { first });
    const auto effect = s.addEffect (bus, "phaziteEq");
    const auto second = s.addTrack ("Second");

    {
        const phazite::UndoStep step (song, "Into the Bus");
        phazite::moveTrackIntoFolder (song, second, bus);
    }

    const std::vector<juce::String> held { nameOfItem (schema::DEVICE, effect.getRaw()), nameOfItem (schema::TRACK, first.getRaw()),
                                           nameOfItem (schema::TRACK, second.getRaw()) };
    REQUIRE (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
    REQUIRE (project.save().wasOk());
    const auto version = phazite::test::copyWholeSong (song);

    {
        phazite::test::UndoCheck undo (s.context);
        undo.check ("Remove", inStep (song, "Remove", [&] { phazite::removeDevice (song, effect); }));
        REQUIRE_FALSE (project.isWholeAsSaved()); // not asked of hasUnsavedChanges(), whose mark holds till a save
        undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
        CHECK (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
        checkAsSaved (project, song, version);

        const auto steps = undo.getNumSteps();
        undo.check ("Again", inStep (song, "Again", [&] { bringBack (song, version); }));
        CHECK (undo.getNumSteps() == steps);
        undo.walkBackAndForth();
    }

    phazite::undo (song);
    REQUIRE_FALSE (phazite::hasDevice (song, effect));
    phazite::makeSongTheSame (song, version);
    CHECK (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
    checkAsSaved (project, song, version);
}

TEST_CASE ("A bus's line put back puts its tracks among its effects where the version holds them, and a track's line put back "
           "puts it among its bus's effects")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto first = s.firstTrack;
    const auto bus = s.addBus ("Bus", { first });
    const auto effect = s.addEffect (bus, "phaziteEq");
    const auto second = s.addTrack ("Second");

    {
        const phazite::UndoStep step (song, "Into the Bus");
        phazite::moveTrackIntoFolder (song, second, bus);
    }

    const std::vector<juce::String> held { nameOfItem (schema::DEVICE, effect.getRaw()), nameOfItem (schema::TRACK, first.getRaw()),
                                           nameOfItem (schema::TRACK, second.getRaw()) };
    REQUIRE (project.save().wasOk());
    const auto version = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (s.context);

    // The bus's line: its effect back before its second track.
    undo.check ("Remove", inStep (song, "Remove", [&] { phazite::removeDevice (song, effect); }));
    undo.check ("Restore Bus", inStep (song, "Restore Bus", [&] { restoreLine (song, bus, version); }));
    CHECK (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
    checkAsSaved (project, song, version);

    // The second track's line, the track deleted since: back after the effect.
    undo.check ("Delete Second", inStep (song, "Delete Second", [&] { phazite::deleteTracks (song, { second }, nullptr); }));
    undo.check ("Restore Second", inStep (song, "Restore Second", [&] { restoreLine (song, second, version); }));
    CHECK (readTracksAndDevices (phazite::test::copyWholeSong (song), bus) == held);
    checkAsSaved (project, song, version);

    const auto steps = undo.getNumSteps();
    undo.check ("Again", inStep (song, "Again", [&] { restoreLine (song, second, version); }));
    undo.check ("Again", inStep (song, "Again", [&] { restoreLine (song, bus, version); }));
    CHECK (undo.getNumSteps() == steps);
    undo.walkBackAndForth();
}

TEST_CASE ("A switch to a version puts a device back after the lanes of its track and of the master, where the version holds "
           "it before them: the lanes are view state, which neither the project nor the versions report")
{
    // The device's place against the lanes differs from the version's text (a device put into an empty list goes after its
    // owner's other children); the lanes are uncounted, so that no comparison reports it. Pinned here, so that it can't
    // become a change reported unseen.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    const auto master = phazite::getMasterID (song);
    const auto effect = s.addEffect (track, "phaziteEq");
    const auto onMaster = s.addEffect ({}, "phaziteEq");

    for (const auto channel : { track, master })
        phazite::showLane (song, channel,
                           { .device = phazite::getFader (song, channel), .parameter = phazite::getVolumeParameterID (song, channel) });

    const auto whole = phazite::test::copyWholeSong (song);

    for (const auto& channel : { phazite::test::findTrackIn (whole, track), whole.getChildWithName (schema::MASTER) })
    {
        const auto lanes = channel.getChildWithName (schema::LANES);
        REQUIRE (lanes.isValid());
        REQUIRE (channel.getChildWithName (schema::DEVICE).isValid());
        REQUIRE (channel.indexOf (channel.getChildWithName (schema::DEVICE)) < channel.indexOf (lanes));
    }

    REQUIRE (project.save().wasOk());
    const auto version = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Remove");
        phazite::removeDevice (song, effect);
        phazite::removeDevice (song, onMaster);
    }

    REQUIRE_FALSE (project.isWholeAsSaved()); // not asked of hasUnsavedChanges(), whose mark holds till a save
    phazite::makeSongTheSame (song, version);
    CHECK (phazite::hasDevice (song, effect));
    CHECK (phazite::hasDevice (song, onMaster));
    CHECK_FALSE (project.hasUnsavedChanges());
    CHECK (project.isWholeAsSaved());
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), version));
    phazite::test::checkSong (song);
}

TEST_CASE ("A device a version moves home is the same element: what is written of it outside the history after survives the "
           "Undo and the Redo of that step")
{
    // The tree's undo actions hold the element they moved: with a copy put in its place, the Undo would bring back another
    // element than the one written to since, and lose what was written to it outside the history.
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);
    const auto track = s.firstTrack;
    DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Plug-in");
        plugin = TestPlugin::add (song, track);
    }

    s.waitForPlugins();
    session->update();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&song] { return phazite::getPluginsWithOlderStates (song).isEmpty() && phazite::getPluginsAnswering (song).isEmpty(); }));
    song.storeDeviceStates();
    const auto version = phazite::test::copyWholeSong (song);
    const auto stateThen = phazite::getPluginState (song, plugin);
    DeviceID grouped;

    {
        const phazite::UndoStep step (song, "Group");
        grouped = phazite::groupIntoContainer (song, { plugin });
        REQUIRE (grouped.isValid());
    }

    {
        const phazite::UndoStep step (song, "Bring Back");
        bringBack (song, version);
    }

    REQUIRE (placeOf (song, plugin) == DevicePlace { .track = track });

    // Outside the history: its window moved, and a state stored after a knob turned in its window.
    const WindowPlace moved { .x = 70, .y = 80 };
    phazite::setWindowPlace (song, plugin, moved);
    TestPlugin::setGain (song, plugin, TestPlugin::getGain (song, plugin) * 0.5f);
    song.storeDeviceStates();
    const auto stored = phazite::getPluginState (song, plugin);
    REQUIRE_FALSE (stored == stateThen);

    phazite::undo (song);
    CHECK (phazite::findContainerOf (song, plugin) == grouped);
    CHECK (phazite::getWindowPlace (song, plugin) == std::optional (moved));
    CHECK (phazite::getPluginState (song, plugin) == stored);

    phazite::redo (song);
    CHECK (placeOf (song, plugin) == DevicePlace { .track = track });
    CHECK (phazite::getWindowPlace (song, plugin) == std::optional (moved));
    CHECK (phazite::getPluginState (song, plugin) == stored);
    phazite::test::checkSong (song);
}

TEST_CASE ("What the store holds of a plug-in of 200 values set by hand: a version that holds the same writes nothing and puts "
           "nothing in order; one a value apart puts them in order once; a second pass writes nothing, in the history and out")
{
    // The store's pass compares a plug-in's values before it writes (ApplySong.cpp: makeDeviceStoreLike()), and the store's
    // writer writes them once a call (DeviceStore.cpp: setStoreEntries(), countPluginValuePlacesBuilt()), not once for each
    // value: its cost grows with the values' count, not with its square. What it wrote: into the tree, and into the store
    // (which holds a plug-in's values since the store's switch).
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Plug-in");
        plugin = TestPlugin::add (song, track);
    }

    s.waitForPlugins();
    constexpr int numValues = 200;
    const auto parameterOf = [] (int i) { return juce::String (1000 + i); };

    {
        const phazite::UndoStep step (song, "Values");

        for (int i = 0; i < numValues; ++i)
            phazite::setPluginValueSetByHand (song, plugin, parameterOf (i), (float) i / (float) numValues, 0.0f);
    }

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&song] { return phazite::getPluginsWithOlderStates (song).isEmpty() && phazite::getPluginsAnswering (song).isEmpty(); }));
    REQUIRE (phazite::getPluginValues (song, plugin).size() == (size_t) numValues);
    const auto version = phazite::test::copyWholeSong (song);

    // Applied in the history, or outside it: what it wrote into the tree, how many undo actions, how many times the values'
    // places were listed, how many writes the store's pass counted.
    struct Applied
    {
        int written = 0, actions = 0;
        std::uint64_t placesBuilt = 0, storeWrites = 0;
    };

    const auto apply = [&song, &version] (bool inHistory)
    {
        Applied applied;
        const TreeWrites writes (song.getState());
        const StoreWrites stored (song);
        const auto places = phazite::countPluginValuePlacesBuilt();
        const auto storeWrites = phazite::countDeviceStorePassWrites();

        if (inHistory)
        {
            const phazite::UndoStep step (song, "Bring Back");
            bringBack (song, version);
            applied.actions = song.getUndoManager().getNumActionsInCurrentTransaction();
        }
        else
        {
            phazite::makeSongTheSame (song, version);
        }

        applied.written = writes.count + stored.count;
        applied.placesBuilt = phazite::countPluginValuePlacesBuilt() - places;
        applied.storeWrites = phazite::countDeviceStorePassWrites() - storeWrites;
        return applied;
    };

    const auto turn = [&song, plugin, &parameterOf]
    {
        const phazite::UndoStep step (song, "Turn");
        phazite::setPluginValueSetByHand (song, plugin, parameterOf (7), 0.9f, 0.0f);
    };

    for (const auto inHistory : { true, false })
    {
        INFO ((inHistory ? "in the history" : "outside it"));

        // The same: nothing written, nothing listed.
        const auto same = apply (inHistory);
        CHECK (same.written == 0);
        CHECK (same.actions == 0);
        CHECK (same.placesBuilt == 0);
        CHECK (same.storeWrites == 0);

        // A value apart: that value written, the values listed once.
        turn();
        const auto apart = apply (inHistory);
        CHECK (apart.written >= 1);
        CHECK (apart.placesBuilt == 1);
        CHECK (apart.storeWrites >= 1);
        CHECK (phazite::findPluginValue (song, plugin, parameterOf (7)) == std::optional ((float) 7 / (float) numValues));

        // Again: nothing.
        const auto again = apply (inHistory);
        CHECK (again.written == 0);
        CHECK (again.placesBuilt == 0);
        CHECK (again.storeWrites == 0);
        phazite::test::checkSong (song);
    }
}

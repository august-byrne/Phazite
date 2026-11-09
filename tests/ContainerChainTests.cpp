// SPDX-License-Identifier: AGPL-3.0-or-later
// A container's chains (src/model/mixer/ContainerModel.h; roadmap step 4.11): chains added, copied, moved, renamed and
// removed as named undo steps, their mute and solo as a track's; the split of a container of effects (layers, bands with
// their crossovers in order, the middle and side, the left and right); instrument containers, whose chains play the notes
// in their ranges; the chains' volumes, pans and crossovers as parameters that automation and macros move; what Phazite's
// engine plays of all that; and the presets that keep it.
#include "EnginePlayback.h"
#include "TestDevices.h"
#include "TestSong.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>

namespace
{
using phazite::ContainerSplit;
using phazite::DeviceID;
using phazite::NoteRange;
using phazite::ParameterRef;
using phazite::TrackID;
using phazite::test::EnginePlayback;
using phazite::test::TestSong;
using phazite::test::TestTone;

// The song as Phazite's engine renders it from its start, and the level of a side of it over a stretch, in dBFS.
juce::AudioBuffer<float> renderOwn (TestSong& song, double seconds = 1.0)
{
    phazite::test::dispatchPendingMessages();
    return phazite::renderWithOwnEngine (*phazite::findSong (song.context), (int) (seconds * 48000.0), 48000.0, 256);
}

float levelOf (const juce::AudioBuffer<float>& audio, int side, double from = 0.25, double to = 0.75)
{
    const auto start = (int) (from * 48000.0);
    return juce::Decibels::gainToDecibels (audio.getRMSLevel (side, start, (int) (to * 48000.0) - start));
}

constexpr float toneLevel = -18.05f; // a 440 Hz tone at a quarter of full scale, through the master's -3 dB

// A tone track, its tone through an empty container of effects with so many chains.
struct ToneContainer
{
    explicit ToneContainer (int chains)
    {
        song.setTempo (120.0);
        song.addAudioClip (song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 2.0, 440.0, 0.25f),
                           0.0);
        container = phazite::addContainer (song.context, song.firstTrack);
        REQUIRE (container.isValid());

        while ((int) phazite::getChains (song.context, container).size() < chains)
            REQUIRE (phazite::addChain (song.context, container).isValid());
    }

    DeviceID chain (int index) const { return phazite::getChains (song.context, container)[(size_t) index].id; }
    phazite::Song& model() const { return *phazite::findSong (song.context); }
    juce::String describeUndo() const { return phazite::describeUndo (&model()); }
    float level (int side = 0) { return levelOf (renderOwn (song), side); }

    TestSong song;
    DeviceID container;
};

// A test device (TestDevices.h) on a track (an instrument in place of its instrument), or into a chain (an instrument in
// place of the chain's), as a model function puts one there; its ID.
DeviceID addTestDevice (TestSong& song, TrackID track, const char* type)
{
    return phazite::test::addTestDevice (*phazite::findSong (song.context), track, type);
}

DeviceID addToChain (TestSong& song, DeviceID chain, const char* type)
{
    phazite::test::registerTestDevices();
    auto& model = *phazite::findSong (song.context);
    const auto id = phazite::insertIntoChain (model, chain, phazite::makeOwnDeviceState (model, type));
    REQUIRE (id.isValid());
    return id;
}

juce::StringArray namesOf (const std::vector<phazite::ChainInfo>& chains)
{
    juce::StringArray names;

    for (const auto& chain : chains)
        names.add (chain.name);

    return names;
}
} // namespace

TEST_CASE ("A container's chains are added, copied, moved, renamed and removed as named undo steps; one left alone is heard")
{
    ToneContainer tone (1);
    auto& context = tone.song.context;
    const auto container = tone.container;

    // One chain at first; another after it, "Chain 2", as one undo step.
    REQUIRE (namesOf (phazite::getChains (context, container)) == juce::StringArray { "Chain 1" });
    const auto first = tone.chain (0);
    const auto second = phazite::addChain (context, container);
    REQUIRE (second.isValid());
    CHECK (tone.describeUndo() == juce::String ("Undo Add Chain"));
    CHECK (namesOf (phazite::getChains (context, container)) == juce::StringArray { "Chain 1", "Chain 2" });
    CHECK (phazite::getChainOf (context, first) == DeviceID()); // a chain isn't a device in one
    CHECK (phazite::getContainerOfChain (context, second) == container);

    // An effect in it, then a copy of it after it, with a copy of its effect, named as it is.
    phazite::addEffectToChain (context, second, tone.song.findDeviceType ("phaziteReverb"));
    CHECK (tone.describeUndo() == juce::String ("Undo Add Reverb"));
    const auto reverb = phazite::getChain (context, second)->devices.at (0).id;
    CHECK (phazite::getChainOf (context, reverb) == second);
    CHECK (phazite::getContainerOf (context, reverb) == container);
    const auto copy = phazite::duplicateChain (context, second);
    REQUIRE (copy.isValid());
    CHECK (tone.describeUndo() == juce::String ("Undo Duplicate Chain"));
    auto chains = phazite::getChains (context, container);
    REQUIRE (chains.size() == 3);
    CHECK (chains[2].id == copy);
    CHECK (chains[2].name == juce::String ("Chain 2"));
    REQUIRE (chains[2].devices.size() == 1);
    CHECK (chains[2].devices[0].name == juce::String ("Reverb"));
    CHECK (chains[2].devices[0].id != reverb);
    const auto copiedReverb = chains[2].devices[0].id;

    // Renamed; a name of nothing but spaces leaves it as it is.
    phazite::renameChain (context, copy, "Wet");
    CHECK (tone.describeUndo() == juce::String ("Undo Rename Chain"));
    phazite::renameChain (context, copy, "   ");
    CHECK (phazite::getChain (context, copy)->name == juce::String ("Wet"));

    // Moved up, before the chain it copied; an undo puts it back.
    phazite::moveChain (context, copy, -1);
    CHECK (tone.describeUndo() == juce::String ("Undo Move Chain"));
    CHECK (namesOf (phazite::getChains (context, container)) == juce::StringArray { "Chain 1", "Wet", "Chain 2" });
    phazite::undo (tone.model());
    CHECK (namesOf (phazite::getChains (context, container)) == juce::StringArray { "Chain 1", "Chain 2", "Wet" });

    // Muted and soloed as a track is, without undo steps: a soloed chain silences the others that aren't.
    phazite::toggleChainSolo (context, second);
    phazite::toggleChainMute (context, first);
    CHECK (tone.describeUndo() == juce::String ("Undo Rename Chain"));
    CHECK (phazite::getChain (context, second)->soloed);
    CHECK (phazite::getChain (context, second)->heard);
    CHECK_FALSE (phazite::getChain (context, copy)->heard);
    CHECK (phazite::getChain (context, first)->muted);

    // Soloed alone, the others' solos go; again, its own does.
    phazite::toggleChainSolo (context, copy);
    phazite::toggleChainSoloAlone (context, copy);
    CHECK_FALSE (phazite::getChain (context, second)->soloed);
    CHECK (phazite::getChain (context, copy)->soloed);
    phazite::toggleChainSoloAlone (context, copy);
    CHECK_FALSE (phazite::getChain (context, copy)->soloed);

    // Removed with its devices; then the one left alone is heard, neither muted nor soloed; the last can't go.
    phazite::removeChain (context, copy);
    CHECK (tone.describeUndo() == juce::String ("Undo Remove Chain"));
    CHECK_FALSE (phazite::findDeviceState (tone.model(), copiedReverb).isValid());
    phazite::removeChain (context, second);
    chains = phazite::getChains (context, container);
    REQUIRE (chains.size() == 1);
    CHECK (chains[0].id == first);
    CHECK_FALSE (chains[0].muted);
    CHECK (chains[0].heard);
    CHECK_FALSE (phazite::canRemoveChain (context, first));
    phazite::removeChain (context, first);
    CHECK (phazite::getChains (context, container).size() == 1);

    // An undo brings the chain back with its devices.
    phazite::undo (tone.model());
    CHECK (namesOf (phazite::getChains (context, container)) == juce::StringArray { "Chain 1", "Chain 2" });
    CHECK (phazite::getChain (context, second)->devices.size() == 1);
}

TEST_CASE ("A container holds sixteen chains at most, and one split into its middle and side, or its left and right, two")
{
    ToneContainer tone (1);
    auto& context = tone.song.context;
    const auto container = tone.container;

    // Split into the middle and side: a second chain for the side, as one undo step.
    CHECK (phazite::canSplit (context, container, ContainerSplit::midSide));
    phazite::setContainerSplit (context, container, ContainerSplit::midSide);
    CHECK (tone.describeUndo() == juce::String ("Undo Split Container"));
    auto chains = phazite::getChains (context, container);
    REQUIRE (chains.size() == 2);
    CHECK (chains[0].part == juce::String ("Mid"));
    CHECK (chains[1].part == juce::String ("Side"));
    CHECK_FALSE (phazite::canAddChain (context, container));
    CHECK_FALSE (phazite::addChain (context, container).isValid());

    phazite::setContainerSplit (context, container, ContainerSplit::leftRight);
    chains = phazite::getChains (context, container);
    CHECK (chains[0].part == juce::String ("Left"));
    CHECK (chains[1].part == juce::String ("Right"));

    // Three chains are layers or bands, not two parts.
    phazite::setContainerSplit (context, container, ContainerSplit::layers);
    CHECK (phazite::getChains (context, container)[0].part.isEmpty());
    REQUIRE (phazite::addChain (context, container).isValid());
    CHECK_FALSE (phazite::canSplit (context, container, ContainerSplit::midSide));
    CHECK_FALSE (phazite::canSplit (context, container, ContainerSplit::leftRight));
    CHECK (phazite::canSplit (context, container, ContainerSplit::bands));

    while (phazite::canAddChain (context, container))
        REQUIRE (phazite::addChain (context, container).isValid());

    CHECK ((int) phazite::getChains (context, container).size() == phazite::maximumChains);

    // An instrument container's chains play notes, and split nothing.
    TestSong song;
    song.setInstrument (song.firstTrack, "phaziteSynth");
    const auto instruments = phazite::addInstrumentContainer (song.context, song.firstTrack);
    REQUIRE (instruments.isValid());
    CHECK_FALSE (phazite::canSplit (song.context, instruments, ContainerSplit::bands));
}

TEST_CASE ("A container split into bands keeps its chains' crossovers in order, each a parameter between its neighbours'")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    const auto container = tone.container;
    phazite::setContainerSplit (context, container, ContainerSplit::bands);

    // Two bands at 250 Hz: the first starts at the bottom, so has no crossover.
    auto chains = phazite::getChains (context, container);
    CHECK (chains[0].part == juce::String ("Below 250 Hz"));
    CHECK (chains[1].part == juce::String ("Above 250 Hz"));
    CHECK_FALSE (phazite::getChainCrossover (context, chains[0].id).isValid());
    auto crossover = phazite::getChainCrossover (context, chains[1].id);
    REQUIRE (crossover.isValid());
    CHECK (crossover.getValue() == doctest::Approx (250.0f));
    CHECK (crossover.getName() == juce::String ("Crossover"));

    // A third band starts above the second, where three bands' third does.
    const auto third = phazite::addChain (context, container);
    CHECK (phazite::getChainCrossover (context, third).getValue() == doctest::Approx (2500.0f));
    CHECK (phazite::getChains (context, container)[1].part == juce::String ("250 Hz to 2.50 kHz"));

    // A crossover stays between its neighbours', as one undo step a change.
    crossover.setValue (5000.0f);
    CHECK (crossover.getValue() == doctest::Approx (2500.0f));
    crossover.setValue (5.0f);
    CHECK (crossover.getValue() == doctest::Approx (20.0f));
    crossover.setValue (500.0f);
    CHECK (crossover.getValue() == doctest::Approx (500.0f));
    CHECK (tone.describeUndo() == juce::String ("Undo Change Chain 2 Crossover"));

    // A copy of a band's chain is a band between it and the next.
    const auto copy = phazite::duplicateChain (context, chains[1].id);
    const auto copied = phazite::getChainCrossover (context, copy).getValue();
    CHECK (copied > 500.0f);
    CHECK (copied < 2500.0f);

    // The chains stay in their bands' order; back to layers, they have no crossovers.
    CHECK_FALSE (phazite::canMoveChains (context, container));
    phazite::setContainerSplit (context, container, ContainerSplit::layers);
    CHECK (phazite::canMoveChains (context, container));
    CHECK_FALSE (phazite::getChainCrossover (context, chains[1].id).isValid());
}

TEST_CASE ("A container's chains' volumes, pans and crossovers are automated and modulated as a device's parameters")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    const auto container = tone.container;
    const auto second = tone.chain (1);
    phazite::setContainerSplit (context, container, ContainerSplit::bands);

    // Its track's automation targets, after the container: each chain's volume and pan, and a band's crossover.
    juce::StringArray names;

    for (const auto& target : phazite::getAutomationTargets (context, container))
        names.add (target.deviceName + ": " + target.name);

    CHECK (names
           == juce::StringArray { "Container: Chain 1 Volume", "Container: Chain 1 Pan", "Container: Chain 2 Volume",
                                  "Container: Chain 2 Pan", "Container: Chain 2 Crossover" });

    // The container's macros reach them first, then the track's modulators.
    const auto macros = phazite::getContainerMacros (context, container);
    const auto lfo = phazite::addModulator (context, tone.song.firstTrack, phazite::ModulatorType::lfo);
    auto volume = phazite::getChainVolume (context, second);
    const auto modulators = phazite::getModulatorsFor (volume);
    REQUIRE (modulators.size() == 9);
    CHECK (modulators[0].id == macros[0].id);
    CHECK (modulators[8].id == lfo);
    phazite::modulate (volume, macros[0].id, -0.5f);
    CHECK (phazite::describeUndo (&tone.model()) == juce::String ("Undo Modulate Chain 2 Volume"));
    CHECK (phazite::getModulations (volume).size() == 1);

    // Its parameters are on the container's track.
    CHECK (phazite::getAutomationTarget (context, { second, "pan" })->track == tone.song.firstTrack);
}

TEST_CASE ("Phazite's engine plays a container's chains side by side, which add up, each through its volume, mute and solo")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;

    // Two chains with nothing in them: twice the tone.
    CHECK (tone.level() == doctest::Approx (toneLevel + 6.02f).epsilon (0.002));

    // The second at -6 dB: one and a half times the tone.
    phazite::getChainVolume (context, tone.chain (1)).setValue (phazite::decibelsToFaderPosition (-6.0206f));
    CHECK (tone.level() == doctest::Approx (toneLevel + 3.52f).epsilon (0.002));

    // The second muted: the first alone. Soloed: the second alone.
    phazite::toggleChainMute (context, tone.chain (1));
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));
    phazite::toggleChainMute (context, tone.chain (1));
    phazite::toggleChainSoloAlone (context, tone.chain (1));
    CHECK (tone.level() == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));
    phazite::toggleChainSoloAlone (context, tone.chain (1));

    // Panned hard left, the second chain plays on the left only, 3 dB louder there, as a channel's pan does.
    phazite::getChainVolume (context, tone.chain (1)).setValue (phazite::decibelsToFaderPosition (0.0f));
    phazite::getChainPan (context, tone.chain (1)).setValue (-1.0f);
    const auto panned = renderOwn (tone.song);
    CHECK (levelOf (panned, 1) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (levelOf (panned, 0) == doctest::Approx (toneLevel + 20.0f * std::log10 (1.0f + std::sqrt (2.0f))).epsilon (0.002));
}

TEST_CASE ("Phazite's engine lines a container's chains up with the latest, so that they add up in step")
{
    ToneContainer tone (2);
    addToChain (tone.song, tone.chain (0), phazite::test::TestDelay::typeID); // the tone 64 samples late, upside down

    // In step, the first chain's tone, upside down, cancels the second's: nothing. Out of step, they would comb.
    CHECK (tone.level() < -80.0f);

    // The second muted: the first's alone, at the tone's level.
    phazite::toggleChainMute (tone.song.context, tone.chain (1));
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Phazite's engine splits a container's sound into bands that add up to it, each its chain's")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    phazite::setContainerSplit (context, tone.container, ContainerSplit::bands); // at 250 Hz, below the tone

    // Both bands: the tone as it came (through an all-pass).
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));

    // The low band alone: the tone, above it, 20 dB down; the high band alone: nearly all of it.
    phazite::toggleChainMute (context, tone.chain (1));
    const auto low = tone.level();
    CHECK (low < toneLevel - 18.0f);
    CHECK (low > toneLevel - 23.0f);
    phazite::toggleChainMute (context, tone.chain (1));
    phazite::toggleChainMute (context, tone.chain (0));
    const auto high = tone.level();
    CHECK (high < toneLevel);
    CHECK (high > toneLevel - 1.5f);

    // The crossover moved above the tone: the tone in the low band, which is muted.
    phazite::getChainCrossover (context, tone.chain (1)).setValue (1000.0f);
    CHECK (tone.level() < toneLevel - 25.0f);

    // Three bands add up too.
    phazite::toggleChainMute (context, tone.chain (0));
    REQUIRE (phazite::addChain (context, tone.container).isValid());
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Phazite's engine splits a container's sound into its middle and side, or its left and right, which add up to it")
{
    ToneContainer tone (1);
    auto& context = tone.song.context;
    phazite::setContainerSplit (context, tone.container, ContainerSplit::midSide);

    // The tone is the same on both sides: all of it in the middle, none in the side.
    CHECK (tone.level (0) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (tone.level (1) == doctest::Approx (toneLevel).epsilon (0.002));
    phazite::toggleChainMute (context, tone.chain (1));
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));
    phazite::toggleChainMute (context, tone.chain (1));
    phazite::toggleChainMute (context, tone.chain (0));
    CHECK (tone.level() < -90.0f);

    // Its left muted: the right alone.
    phazite::setContainerSplit (context, tone.container, ContainerSplit::leftRight);
    const auto audio = renderOwn (tone.song);
    CHECK (levelOf (audio, 0) < -90.0f);
    CHECK (levelOf (audio, 1) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Phazite's engine moves a container's chain's volume by its automation and by its container's macros")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    const ParameterRef volume { tone.chain (1), phazite::schema::chainVolumeParameter };

    // Its automation keeps the second chain silent: the first alone.
    tone.song.addAutomation (volume,
                             { { phazite::BeatPosition::fromBeats (0.0), 0.0f }, { phazite::BeatPosition::fromBeats (8.0), 0.0f } });
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));

    // Without, a macro turned all the way up turns it down all the way.
    phazite::deleteAutomation (phazite::getChainVolume (context, tone.chain (1)));
    CHECK (tone.level() == doctest::Approx (toneLevel + 6.02f).epsilon (0.002));
    const auto macro = phazite::getContainerMacros (context, tone.container)[0].id;
    tone.song.modulate (macro, volume, -1.0f);
    tone.song.setMacroValue (macro, 1.0f);
    CHECK (tone.level() == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("An instrument container plays each chain's instrument on the notes in its keys and velocities")
{
    TestSong song;
    song.setTempo (120.0);
    auto& context = song.context;
    const auto track = song.firstTrack;
    const auto tone = addTestDevice (song, track, TestTone::typeID);

    // From the instruments' menu: the track's instrument goes into its first chain, and it is the track's instrument.
    const auto container = phazite::addInstrumentContainer (context, track);
    REQUIRE (container.isValid());
    CHECK (phazite::describeUndo (phazite::findSong (context)) == juce::String ("Undo Add Instrument Container"));
    REQUIRE (phazite::getInstrument (context, track).has_value());
    CHECK (phazite::getInstrument (context, track)->id == container);
    CHECK (phazite::getInstrument (context, track)->isContainer);
    CHECK_FALSE (phazite::getInstrument (context, track)->hasMix); // nothing comes into it to mix with
    CHECK (phazite::isInstrumentContainer (context, container));
    const auto first = phazite::getChains (context, container)[0].id;
    CHECK (phazite::getChain (context, first)->hasInstrument);
    CHECK (phazite::getChain (context, first)->devices.at (0).id == tone);

    // Another chain, its own instrument chosen for it, then replaced, first in it.
    const auto second = phazite::addChain (context, container);
    CHECK_FALSE (phazite::getChain (context, second)->hasInstrument);
    phazite::chooseChainInstrument (context, second, song.findDeviceType ("phaziteSynth"));
    CHECK (phazite::describeUndo (phazite::findSong (context)) == juce::String ("Undo Add Synth"));
    CHECK (phazite::getChain (context, second)->hasInstrument);
    phazite::chooseChainInstrument (context, second, song.findDeviceType ("phaziteSynth"));
    CHECK (phazite::describeUndo (phazite::findSong (context)) == juce::String ("Undo Replace Instrument with Synth"));
    CHECK (phazite::getChain (context, second)->devices.size() == 1);
    addToChain (song, second, TestTone::typeID);
    REQUIRE (phazite::getChain (context, second)->devices.size() == 1);
    CHECK (phazite::getChain (context, second)->devices[0].name == juce::String ("Test Tone"));

    // By keys: the low notes the first chain's, the high the second's.
    phazite::setChainKeys (context, first, { 0, 59 });
    CHECK (phazite::describeUndo (phazite::findSong (context)) == juce::String ("Undo Change Key Range"));
    phazite::setChainKeys (context, second, { 60, 127 });
    CHECK (phazite::getChain (context, second)->keys == NoteRange { 60, 127 });
    const auto clip = song.addMidiClip (track, 0.0, 8.0);
    song.addNote (clip, 50, 0.0, 4.0, 64);  // the first chain's
    song.addNote (clip, 70, 2.0, 2.0, 127); // the second's, from a second in
    const auto gain = juce::Decibels::decibelsToGain (phazite::defaultMasterVolume);
    auto audio = renderOwn (song, 2.0);
    CHECK (audio.getSample (0, 24000) == doctest::Approx (gain * 64.0f / 127.0f).epsilon (0.001));
    CHECK (audio.getSample (0, 72000) == doctest::Approx (gain * (64.0f + 127.0f) / 127.0f).epsilon (0.001));

    // By velocity, every key in both: the soft note the first chain's, the hard one the second's, each ending only its own.
    phazite::setChainKeys (context, first, phazite::allKeys);
    phazite::setChainKeys (context, second, phazite::allKeys);
    phazite::setChainVelocities (context, first, { 1, 80 });
    CHECK (phazite::describeUndo (phazite::findSong (context)) == juce::String ("Undo Change Velocity Range"));
    phazite::setChainVelocities (context, second, { 81, 127 });
    audio = renderOwn (song, 2.0);
    CHECK (audio.getSample (0, 24000) == doctest::Approx (gain * 64.0f / 127.0f).epsilon (0.001));
    CHECK (audio.getSample (0, 72000) == doctest::Approx (gain * (64.0f + 127.0f) / 127.0f).epsilon (0.001));

    // A chain muted plays nothing of its notes.
    phazite::toggleChainMute (context, second);
    audio = renderOwn (song, 2.0);
    CHECK (audio.getSample (0, 72000) == doctest::Approx (gain * 64.0f / 127.0f).epsilon (0.001));
}

TEST_CASE ("A note ends in the instrument container's chain it started in, whatever that chain's keys are now")
{
    TestSong song;
    song.setTempo (120.0);
    auto& context = song.context;
    addTestDevice (song, song.firstTrack, TestTone::typeID);
    const auto container = phazite::addInstrumentContainer (context, song.firstTrack);
    const auto chain = phazite::getChains (context, container)[0].id;
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 8.0);
    song.addNote (clip, 60, 0.0, 2.0, 100); // a second long
    EnginePlayback playback;

    // Playing, the note holds; its key no longer the chain's, its end still reaches it.
    song.run ("transport.playStop");
    auto audio = playback.play (0.5);
    CHECK (audio.getSample (0, audio.getNumSamples() - 1) > 0.1f);
    phazite::setChainKeys (context, chain, { 70, 127 });
    audio = playback.play (1.0);
    CHECK (audio.getSample (0, audio.getNumSamples() - 1) == doctest::Approx (0.0f));
}

TEST_CASE ("A track's instrument and effects group into an instrument container in its place, which ungroups while it has one chain")
{
    TestSong song;
    auto& context = song.context;
    const auto track = song.firstTrack;
    const auto synth = song.setInstrument (track, "phaziteSynth");
    const auto eq = song.addEffect (track, "phaziteEq");
    const auto reverb = song.addEffect (track, "phaziteReverb");

    // The instrument with the EQ: an instrument container in the instrument's place, the reverb after it.
    const auto container = phazite::groupIntoContainer (context, { eq, synth });
    REQUIRE (container.isValid());
    CHECK (phazite::isInstrumentContainer (context, container));
    CHECK (phazite::getInstrument (context, track)->id == container);
    REQUIRE (phazite::getEffects (context, track).size() == 1);
    CHECK (phazite::getEffects (context, track)[0].id == reverb);
    const auto chain = phazite::getChains (context, container)[0];
    REQUIRE (chain.devices.size() == 2);
    CHECK (chain.devices[0].id == synth);
    CHECK (chain.devices[1].id == eq);

    // The EQ moves after the instrument, never before it; the instrument doesn't move.
    phazite::moveEffect (context, eq, -1);
    CHECK (phazite::getChain (context, chain.id)->devices[0].id == synth);

    // With two chains it stays a container; with one, it ungroups: the instrument the track's again, the EQ its first effect.
    const auto second = phazite::addChain (context, container);
    CHECK_FALSE (phazite::canUngroupContainer (context, container));
    phazite::removeChain (context, second);
    REQUIRE (phazite::canUngroupContainer (context, container));
    phazite::ungroupContainer (context, container);
    CHECK (phazite::getInstrument (context, track)->id == synth);
    REQUIRE (phazite::getEffects (context, track).size() == 2);
    CHECK (phazite::getEffects (context, track)[0].id == eq);
}

TEST_CASE ("An effect moves into another chain of its container, and nowhere else")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    phazite::addEffectToChain (context, tone.chain (0), tone.song.findDeviceType ("phaziteEq"));
    phazite::addEffectToChain (context, tone.chain (0), tone.song.findDeviceType ("phaziteReverb"));
    const auto devices = phazite::getChain (context, tone.chain (0))->devices;
    const auto eq = devices[0].id, reverb = devices[1].id;

    // Into the second chain, as one step; then before nothing new, nothing.
    phazite::moveEffectToChain (context, reverb, tone.chain (1), -1);
    CHECK (tone.describeUndo() == juce::String ("Undo Move Device"));
    CHECK (phazite::getChainOf (context, reverb) == tone.chain (1));
    CHECK (phazite::getChain (context, tone.chain (0))->devices.size() == 1);

    // Back, before the EQ.
    phazite::moveEffectToChain (context, reverb, tone.chain (0), 0);
    const auto back = phazite::getChain (context, tone.chain (0))->devices;
    REQUIRE (back.size() == 2);
    CHECK (back[0].id == reverb);
    CHECK (back[1].id == eq);

    // Not into another container's chain.
    const auto other = phazite::addContainer (context, tone.song.firstTrack);
    phazite::moveEffectToChain (context, eq, phazite::getChains (context, other)[0].id, -1);
    CHECK (phazite::getContainerOf (context, eq) == tone.container);
}

TEST_CASE ("A container's preset keeps its chains, its split and what its macros modulate in them; an instrument container's too")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    const auto container = tone.container;
    const auto first = tone.chain (0), second = tone.chain (1);
    phazite::setContainerSplit (context, container, ContainerSplit::bands);
    phazite::getChainCrossover (context, second).setValue (500.0f);
    phazite::getChainVolume (context, second).setValue (phazite::decibelsToFaderPosition (-3.0f));
    phazite::toggleChainMute (context, first);
    phazite::renameChain (context, second, "Top");
    phazite::addEffectToChain (context, second, tone.song.findDeviceType ("phaziteEq"));
    const auto eq = phazite::getChain (context, second)->devices[0].id;
    const auto macro = phazite::getContainerMacros (context, container)[0].id;
    tone.song.modulate (macro, { second, phazite::schema::chainVolumeParameter }, -0.5f);
    tone.song.modulate (macro, { eq, "band4Gain" }, 0.25f);

    // Saved and loaded on another track: new IDs, all else as it was.
    const auto file = tone.song.getSoundsFolder().getChildFile ("Bands.phazitepreset");
    REQUIRE (phazite::saveDevicePreset (context, container, file));
    const auto other = tone.song.addTrack ("Other");
    REQUIRE (phazite::loadPreset (context, file, other));
    const auto loaded = phazite::getEffects (context, other).back();
    REQUIRE (loaded.isContainer);
    CHECK (loaded.id != container);
    CHECK (phazite::getContainerSplit (context, loaded.id) == ContainerSplit::bands);
    const auto chains = phazite::getChains (context, loaded.id);
    REQUIRE (chains.size() == 2);
    CHECK (chains[0].muted);
    CHECK (chains[1].name == juce::String ("Top"));
    CHECK (chains[1].id != second);
    CHECK (phazite::getChainCrossover (context, chains[1].id).getValue() == doctest::Approx (500.0f));
    CHECK (phazite::faderPositionToDecibels (phazite::getChainVolume (context, chains[1].id).getValue()) == doctest::Approx (-3.0f));
    REQUIRE (chains[1].devices.size() == 1);
    CHECK (chains[1].devices[0].name == juce::String ("EQ"));

    // Its first macro modulates the new chain's volume and the new EQ's gain.
    const auto loadedMacro = phazite::getContainerMacros (context, loaded.id)[0].id;
    const auto modulations = phazite::getModulations (context, loadedMacro);
    REQUIRE (modulations.size() == 2);
    CHECK (modulations[0].parameter == ParameterRef { chains[1].id, phazite::schema::chainVolumeParameter });
    CHECK (modulations[0].amount == doctest::Approx (-0.5f));
    CHECK (modulations[1].parameter == ParameterRef { chains[1].devices[0].id, "band4Gain" });

    // An instrument container's preset goes on a track as its instrument, with its chains' instruments and ranges.
    const auto keys = tone.song.addTrack ("Keys");
    tone.song.setInstrument (keys, "phaziteSynth");
    const auto instruments = phazite::addInstrumentContainer (context, keys);
    const auto low = phazite::getChains (context, instruments)[0].id;
    phazite::setChainKeys (context, low, { 0, 59 });
    const auto instrumentsFile = tone.song.getSoundsFolder().getChildFile ("Split Keys.phazitepreset");
    REQUIRE (phazite::saveDevicePreset (context, instruments, instrumentsFile));
    CHECK (phazite::readPresetInfo (instrumentsFile)->isInstrument);
    const auto pads = tone.song.addTrack ("Pads");
    REQUIRE (phazite::loadPreset (context, instrumentsFile, pads));
    const auto made = phazite::getInstrument (context, pads);
    REQUIRE (made.has_value());
    REQUIRE (phazite::isInstrumentContainer (context, made->id));
    const auto madeChain = phazite::getChains (context, made->id).at (0);
    CHECK (madeChain.keys == NoteRange { 0, 59 });
    CHECK (madeChain.hasInstrument);
}

TEST_CASE ("A track's preset keeps its instrument container, and what its modulators modulate in its chains")
{
    TestSong song;
    auto& context = song.context;
    const auto track = song.firstTrack;
    song.setInstrument (track, "phaziteSynth");
    const auto container = phazite::addInstrumentContainer (context, track);
    const auto second = phazite::addChain (context, container);
    phazite::chooseChainInstrument (context, second, song.findDeviceType ("phaziteSynth"));
    const auto lfo = song.addModulator (track, phazite::ModulatorType::lfo);
    song.modulate (lfo, { second, phazite::schema::chainPanParameter }, 0.5f);

    const auto file = song.getSoundsFolder().getChildFile ("Layers.phazitepreset");
    REQUIRE (phazite::saveTrackPreset (context, track, file));
    REQUIRE (phazite::loadPreset (context, file, {}));
    const auto copy = phazite::getTrackIDs (*phazite::findSong (context)).back();
    REQUIRE (copy != track);
    const auto instrument = phazite::getInstrument (context, copy);
    REQUIRE (instrument.has_value());
    REQUIRE (phazite::isInstrumentContainer (context, instrument->id));
    const auto chains = phazite::getChains (context, instrument->id);
    REQUIRE (chains.size() == 2);
    CHECK (chains[1].hasInstrument);

    const auto modulators = phazite::getModulators (context, copy);
    REQUIRE (! modulators.empty());
    const auto modulations = phazite::getModulations (context, modulators.front().id);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].parameter == ParameterRef { chains[1].id, phazite::schema::chainPanParameter });
}

TEST_CASE ("A copy of a track has its containers' chains with IDs of their own, and their automation")
{
    ToneContainer tone (2);
    auto& context = tone.song.context;
    auto& model = tone.model();
    const ParameterRef volume { tone.chain (1), phazite::schema::chainVolumeParameter };
    tone.song.addAutomation (volume, { { phazite::BeatPosition::fromBeats (0.0), 0.5f } });

    const auto copies = phazite::duplicateTracks (model, { tone.song.firstTrack }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto container = phazite::getEffects (context, copies[0]).back().id;
    const auto chains = phazite::getChains (context, container);
    REQUIRE (chains.size() == 2);
    CHECK (chains[1].id != tone.chain (1));
    CHECK (phazite::getAutomationPoints (context, { chains[1].id, phazite::schema::chainVolumeParameter }).size() == 1);
}

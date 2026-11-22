// SPDX-License-Identifier: AGPL-3.0-or-later
// What points at a device, a fader, a send or a track follows the change that moves or removes it (roadmap step S1.12):
// sidechains, modulations, macros, automation and the tracks of a drum sampler's pads. A device moved in its strip, grouped
// into a container, moved between its chains and ungrouped keeps them all; a device, a return or a drum sampler removed
// takes what named it along, in the same undo step; a folder made a bus or plain again keeps its volume's automation and
// modulation; and automation moved in time keeps its shape, a plug-in's that isn't loaded too.
#include "EnginePlayback.h"
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "engine/automation/Curve.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/SwapModel.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <functional>
#include <string>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;
using phazite::BeatDuration;
using phazite::BeatPosition;
using phazite::DeviceID;
using phazite::ParameterRef;
using phazite::TrackID;

// How many elements of a type in the song name an item by a property: the MODULATES that modulate one of its parameters,
// say.
int countNaming (const phazite::Song& song, const juce::Identifier& type, const juce::Identifier& property, std::uint64_t id)
{
    auto count = 0;
    const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (type) && (std::uint64_t) (juce::int64) child[property] == id)
                ++count;

            visit (child);
        }
    };

    visit (song.getState());
    return count;
}

int countModulations (const phazite::Song& song, DeviceID device)
{
    return countNaming (song, schema::MODULATES, schema::modulatesDevice, device.getRaw());
}

// How many of the song's curves are of a device's parameters, as the song's automation holds them (Automation.h).
int countCurves (const phazite::Song& song, DeviceID device)
{
    const auto curves = phazite::getCurveKeys (song);
    return (int) std::count_if (curves.begin(), curves.end(), [device] (const ParameterRef& curve) { return curve.device == device; });
}

// A parameter's position at a beat as its curve in the song has it, as Phazite's engine plays it, whether or not its device
// knows the parameter (a plug-in that isn't loaded doesn't).
float positionAt (const phazite::Song& song, const ParameterRef& parameter, double beat)
{
    std::vector<phazite::engine::CurvePoint> points;

    for (const auto& point : phazite::getPoints (song, parameter))
        points.push_back ({ point.beat.inBeats(), point.position, point.curve });

    return phazite::engine::Curve (std::move (points)).getPositionAt (beat);
}

// A curve as a file holds it, with points (beat, value, bend), put into the song as it opens: for a plug-in that isn't
// loaded, whose parameters no control reaches.
void putCurve (phazite::Song& song, const ParameterRef& parameter, std::initializer_list<phazite::AutomationPoint> points)
{
    auto automation = song.getState().getOrCreateChildWithName (schema::AUTOMATION, nullptr);
    juce::ValueTree curve (schema::CURVE);
    curve.setProperty (schema::curveDevice, juce::String ((juce::int64) parameter.device.getRaw()), nullptr);
    curve.setProperty (schema::curveParameter, parameter.parameter, nullptr);

    for (const auto& point : points)
        curve.appendChild (juce::ValueTree (schema::POINT, { { schema::pointBeat, point.beat.inBeats() },
                                                             { schema::pointValue, point.position },
                                                             { schema::pointCurve, point.curve } }),
                           nullptr);

    automation.appendChild (curve, nullptr);
}

void change (phazite::Song& song, const juce::String& name, const std::function<void()>& edit)
{
    const phazite::UndoStep step (song, name);
    edit();
}
} // namespace

//==============================================================================
TEST_CASE ("A device moved in its strip, grouped into a container, moved between its chains and ungrouped keeps what points at it")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto key = s.addTrack ("Key");
    s.setInstrument (track, phazite::devices::Synth::typeID);
    const auto eq = s.addEffect (track, phazite::devices::Equalizer::typeID);
    const auto saturator = s.addEffect (track, phazite::devices::Saturator::typeID);
    const auto compressor = s.addEffect (track, phazite::devices::Compressor::typeID);
    change (song, "Sidechain", [&] { phazite::setSidechainSource (song, compressor, key); });
    REQUIRE (phazite::getSidechainSource (song, compressor) == key);

    // The saturator's drive: modulated by the track's LFO, automated, its lane shown.
    const ParameterRef drive { saturator, "drive" };
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    s.modulate (lfo, drive, 0.5f);
    s.addAutomation (drive, { { at (0.0), 0.0f }, { at (4.0), 12.0f } });
    phazite::showLane (song, track, drive);

    // A drum sampler with a pad on a track of its own.
    const auto drums = s.addTrack ("Drums");
    const auto sampler = s.setInstrument (drums, phazite::devices::DrumSampler::typeID);
    const auto pad = phazite::givePadItsOwnTrack (song, sampler, 36);
    REQUIRE (pad.isValid());

    const auto pointsAtDrive = [&] (size_t modulations = 1)
    {
        CHECK (phazite::getModulations (song, drive).size() == modulations);
        CHECK (phazite::canModulate (song, lfo, drive));
        CHECK (phazite::getPoints (song, drive).size() == 2);
        CHECK (phazite::getParameterTrack (song, drive) == track);
        const auto lanes = phazite::getLanes (song, track);
        CHECK (std::any_of (lanes.begin(), lanes.end(), [&] (const phazite::AutomationLane& lane) { return lane.parameter == drive; }));
    };

    UndoCheck undo (song);

    // In its strip: before the EQ. The compressor moved too keeps its sidechain.
    undo.check ("Move Device", [&] { phazite::moveDevice (song, saturator, 1); });
    CHECK (phazite::getEffects (song, track) == std::vector { saturator, eq, compressor });
    pointsAtDrive();
    undo.check ("Move Device", [&] { phazite::moveDevice (song, compressor, 0); });
    CHECK (phazite::getSidechainSource (song, compressor) == key);

    // Grouped into a container, then into its second chain, under its macro too.
    DeviceID container;
    undo.check ("Group", [&] { change (song, "Group", [&] { container = phazite::groupIntoContainer (song, { saturator }); }); });
    REQUIRE (container.isValid());
    CHECK (phazite::findContainerOf (song, saturator) == container);
    pointsAtDrive();

    const auto macro = phazite::getContainerMacros (song, container).front();
    undo.check ("Macro", [&] { change (song, "Macro", [&] { phazite::setModulation (song, macro, drive, 0.25f); }); });
    DeviceID second;
    undo.check ("Add Chain", [&] { change (song, "Add Chain", [&] { second = phazite::addChain (song, container); }); });
    undo.check ("Move to Chain", [&] { change (song, "Move", [&] { phazite::moveToChain (song, saturator, second, 0); }); });
    CHECK (phazite::findChainOf (song, saturator) == second);
    CHECK (phazite::canModulate (song, macro, drive));
    pointsAtDrive (2);

    // Out again: the macro goes with its container, the track's LFO stays.
    const auto first = phazite::getChains (song, container).front();
    undo.check ("Remove Chain", [&] { change (song, "Remove Chain", [&] { phazite::removeChain (song, first); }); });
    undo.check ("Ungroup", [&] { change (song, "Ungroup", [&] { phazite::ungroupContainer (song, container); }); });
    CHECK_FALSE (phazite::findContainerOf (song, saturator).isValid());
    pointsAtDrive();

    // A drum sampler grouped into an instrument container keeps its pad on its own track.
    DeviceID drumContainer;
    undo.check ("Group", [&] { change (song, "Group", [&] { drumContainer = phazite::groupIntoContainer (song, { sampler }); }); });
    REQUIRE (drumContainer.isValid());
    CHECK (phazite::findPadTrack (song, sampler, 36) == pad);
    CHECK (phazite::hasPadsOnOwnTracks (song, drums));

    checkIDs (song);
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("Removing a device takes away what modulated it, a container's chains' too, in the same undo step")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, phazite::devices::Synth::typeID);
    const auto saturator = s.addEffect (track, phazite::devices::Saturator::typeID);
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    s.modulate (lfo, { synth, "cutoff" }, 0.5f);
    s.modulate (lfo, { saturator, "drive" }, 0.5f);
    REQUIRE (countModulations (song, synth) == 1);
    UndoCheck undo (song);

    undo.check ("Remove Saturator", [&] { phazite::removeDevice (song, saturator); });
    CHECK (countModulations (song, saturator) == 0);
    CHECK (countModulations (song, synth) == 1);

    // Replaced, an instrument is removed too.
    undo.check ("Replace Instrument", [&] { s.setInstrument (track, phazite::devices::DrumSampler::typeID); });
    CHECK (countModulations (song, synth) == 0);
    checkIDs (song);

    // A container's chain's volume, modulated by the track's LFO: its chain removed, then the container ungrouped.
    DeviceID container;
    const auto eq = s.addEffect (track, phazite::devices::Equalizer::typeID);
    change (song, "Group", [&] { container = phazite::groupIntoContainer (song, { eq }); });
    DeviceID second;
    change (song, "Add Chain", [&] { second = phazite::addChain (song, container); });
    const auto first = phazite::getChains (song, container).front();
    s.modulate (lfo, { first, schema::chainVolumeParameter }, 0.5f);
    s.modulate (lfo, { second, schema::chainVolumeParameter }, 0.5f);
    REQUIRE (countModulations (song, second) == 1);
    undo.check ("Remove Chain", [&] { change (song, "Remove Chain", [&] { phazite::removeChain (song, second); }); });
    CHECK (countModulations (song, second) == 0);
    undo.check ("Ungroup", [&] { change (song, "Ungroup", [&] { phazite::ungroupContainer (song, container); }); });
    CHECK (countModulations (song, first) == 0);
    checkIDs (song);
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("Deleting a return takes away the sends to it, with what modulated them, in the same undo step")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto reverb = s.addReturn (track, "phaziteReverb");
    s.addSend (other, reverb, -12.0f);
    const auto keptReturn = s.addReturn (other, "phaziteDelay");
    REQUIRE (phazite::getSends (song, track).size() == 1);
    REQUIRE (phazite::getSends (song, other).size() == 2);
    const auto send = phazite::getSends (song, track).front();
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    s.modulate (lfo, { send, schema::sendLevelParameter }, 0.5f);
    UndoCheck undo (song);

    undo.check ("Delete Track", [&] { change (song, "Delete Track", [&] { phazite::deleteTracks (song, { reverb }, nullptr); }); });
    CHECK (phazite::getSends (song, track).empty());
    CHECK (phazite::getSends (song, other).size() == 1);
    CHECK (phazite::getSendTarget (song, phazite::getSends (song, other).front()) == keptReturn);
    CHECK (countModulations (song, send) == 0);
    CHECK (phazite::getAutomatableParameters (song, track).size() == 2); // the fader's volume and pan alone
    checkIDs (song);
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("A device on a drum sampler's track can't listen to one of its own pads' tracks, whose sound comes from it")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto drums = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto sampler = s.setInstrument (drums, phazite::devices::DrumSampler::typeID);
    const auto pad = phazite::givePadItsOwnTrack (song, sampler, 36);
    REQUIRE (pad.isValid());
    const auto compressor = s.addEffect (drums, phazite::devices::Compressor::typeID);

    const auto sources = phazite::getSidechainSources (song, compressor);
    CHECK (std::find (sources.begin(), sources.end(), pad) == sources.end());
    CHECK (std::find (sources.begin(), sources.end(), other) != sources.end());

    for (const auto& choice : phazite::describeSidechainChoices (song, compressor))
        if (choice.track == pad)
            CHECK (choice.whyNot.isNotEmpty());

    change (song, "Sidechain", [&] { phazite::setSidechainSource (song, compressor, pad); });
    CHECK_FALSE (phazite::getSidechainSource (song, compressor).isValid());

    // A device on the pad's track may listen to the sampler's: nothing of its sound comes from the pad's.
    const auto onPad = s.addEffect (pad, phazite::devices::Compressor::typeID);
    const auto padSources = phazite::getSidechainSources (song, onPad);
    CHECK (std::find (padSources.begin(), padSources.end(), drums) != padSources.end());
}

//==============================================================================
TEST_CASE (
    "A drum sampler removed or replaced takes its pads' tracks along in the same undo step; one that holds clips stays a plain track")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto drums = s.firstTrack;
    const auto sampler = s.setInstrument (drums, phazite::devices::DrumSampler::typeID);
    const auto kick = phazite::givePadItsOwnTrack (song, sampler, 36);
    const auto snare = phazite::givePadItsOwnTrack (song, sampler, 38);
    REQUIRE (kick.isValid());
    REQUIRE (snare.isValid());
    s.addMidiClip (snare, 0.0, 4.0);
    UndoCheck undo (song);

    const auto checkGone = [&]
    {
        CHECK_FALSE (phazite::findTrackState (song, kick).isValid());
        CHECK (phazite::findTrackState (song, snare).isValid());
        CHECK_FALSE (phazite::getPlayedPad (song, snare).has_value());
        checkIDs (song);
    };

    undo.check ("Remove Drum Sampler", [&] { phazite::removeDevice (song, sampler); });
    checkGone();
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::findPadTrack (song, sampler, 36) == kick);
    CHECK (phazite::findPadTrack (song, sampler, 38) == snare);

    undo.check ("Replace Instrument", [&] { s.setInstrument (drums, phazite::devices::Synth::typeID); });
    checkGone();
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });

    undo.check ("Delete Track", [&] { change (song, "Delete Track", [&] { phazite::deleteTracks (song, { drums }, nullptr); }); });
    checkGone();
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("A folder made plain and a bus again keeps its volume's automation and modulation; its pan's goes while it is plain")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto inside = s.addTrack ("Inside");
    const auto bus = s.addBus ("Bus", { inside });
    const auto volume = s.getVolumeParameter (bus);
    const auto pan = s.getPanParameter (bus);
    s.addAutomation (volume, { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
    s.addAutomation (pan, { { at (0.0), -0.5f }, { at (4.0), 0.5f } });
    const auto lfo = s.addModulator (bus, phazite::ModulatorType::lfo);
    s.modulate (lfo, volume, 0.25f);
    s.modulate (lfo, pan, 0.5f);
    phazite::showLane (song, bus, volume);
    UndoCheck undo (song);

    const auto keeps = [&] (const ParameterRef& now)
    {
        const auto points = phazite::getPoints (song, now);
        REQUIRE (points.size() == 2);
        CHECK (points[0].position == doctest::Approx (0.5f));
        CHECK (points[1].position == doctest::Approx (0.8f));
        CHECK (phazite::getModulations (song, now).size() == 1);
        const auto lanes = phazite::getLanes (song, bus);
        CHECK (std::any_of (lanes.begin(), lanes.end(), [&] (const phazite::AutomationLane& lane) { return lane.parameter == now; }));
        checkIDs (song);
    };

    undo.check ("Make Folder Plain", [&] { phazite::setMixedAsBus (song, bus, false); });
    const auto vca = s.getVolumeParameter (bus);
    REQUIRE (vca != volume);
    keeps (vca);
    CHECK (countModulations (song, volume.device) == 0); // the pan's too: a VCA has none

    undo.check ("Mix Folder as Bus", [&] { phazite::setMixedAsBus (song, bus, true); });
    const auto fader = s.getVolumeParameter (bus);
    REQUIRE (fader != vca);
    keeps (fader);
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("Time edits and clips moved with their automation take the automation of a plug-in that isn't loaded")
{
    TestSong s; // no stand-in alive: the host doesn't know the plug-in
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    DeviceID plugin;
    change (song, "Add Device", [&] { plugin = TestPlugin::add (song, track); });
    const ParameterRef gain { plugin, TestPlugin::gainID };
    putCurve (song, gain, { { at (0.0), 0.2f }, { at (5.0), 0.2f }, { at (6.0), 0.9f }, { at (7.0), 0.2f }, { at (16.0), 0.2f } });
    const auto clip = s.addMidiClip (track, 4.0, 4.0);
    REQUIRE (positionAt (song, gain, 6.0) == doctest::Approx (0.9f));

    change (song, "Insert Time", [&] { phazite::insertTime (song, at (2.0), BeatDuration::fromBeats (4.0)); });
    CHECK (positionAt (song, gain, 10.0) == doctest::Approx (0.9f));
    phazite::undo (song);
    CHECK (positionAt (song, gain, 6.0) == doctest::Approx (0.9f));

    change (song, "Remove Time", [&] { phazite::removeTime (song, { at (0.0), at (2.0) }); });
    CHECK (positionAt (song, gain, 4.0) == doctest::Approx (0.9f));
    phazite::undo (song);

    change (song, "Nudge", [&] { phazite::nudgeClips (s.context, { clip }, BeatDuration::fromBeats (4.0)); });
    CHECK (positionAt (song, gain, 10.0) == doctest::Approx (0.9f));
    CHECK (positionAt (song, gain, 6.0) == doctest::Approx (0.2f));
    phazite::undo (song);
    CHECK (positionAt (song, gain, 6.0) == doctest::Approx (0.9f));
}

//==============================================================================
TEST_CASE ("Automation moved with clips, pasted and moved in time keeps the shape of a bent line outside what changed")
{
    for (const auto bend : { 0.4f, -0.3f, 0.8f, -0.8f, 0.95f, -0.95f })
    {
        CAPTURE (bend);
        TestSong s;
        auto& song = *phazite::findSong (s.context);
        const auto track = s.firstTrack;
        const auto volume = s.getVolumeParameter (track);
        const auto clip = s.addMidiClip (track, 8.0, 4.0);
        s.addAutomation (volume, { { at (0.0), 0.1f, bend }, { at (32.0), 0.9f } });
        std::vector<float> before;

        for (auto beat = 0.0; beat <= 32.0; beat += 0.25)
            before.push_back (positionAt (song, volume, beat));

        const auto original = [&before] (double beat) { return before[(size_t) std::lround (beat * 4.0)]; };
        std::string step;
        const auto checkSame = [&] (double from, double to, double shift)
        {
            for (auto beat = from; beat <= to; beat += 0.25)
            {
                CAPTURE (step);
                CAPTURE (beat);
                CHECK (positionAt (song, volume, beat + shift) == doctest::Approx (original (beat)).epsilon (1.0e-4));
            }
        };

        // The clip nudged two beats later: what lies before it and after where it lands is as it was, and so is what it took.
        step = "Nudge";
        change (song, "Nudge", [&] { phazite::nudgeClips (s.context, { clip }, BeatDuration::fromBeats (2.0)); });
        checkSame (0.0, 7.75, 0.0);
        checkSame (14.25, 32.0, 0.0);
        checkSame (8.25, 11.75, 2.0);
        phazite::undo (song);
        checkSame (0.0, 32.0, 0.0);

        // Its four beats copied and pasted twelve beats later.
        const auto copies = phazite::copyAutomation (song, { { track, { at (8.0), at (12.0) } } });
        step = "Paste";
        change (song, "Paste", [&] { phazite::pasteAutomation (song, copies, BeatDuration::fromBeats (12.0)); });
        checkSame (0.0, 19.75, 0.0);
        checkSame (24.25, 32.0, 0.0);
        checkSame (8.25, 11.75, 12.0);
        phazite::undo (song);

        // Four beats of time inserted at beat 6, and taken out at beat 6.
        step = "Insert Time";
        change (song, "Insert Time", [&] { phazite::insertTime (song, at (6.0), BeatDuration::fromBeats (4.0)); });
        checkSame (0.0, 6.0, 0.0);
        checkSame (6.0, 28.0, 4.0);
        phazite::undo (song);

        step = "Remove Time";
        change (song, "Remove Time", [&] { phazite::removeTime (song, { at (6.0), at (10.0) }); });
        checkSame (0.0, 5.75, 0.0);
        checkSame (10.25, 32.0, -4.0);
        phazite::undo (song);

        // Beats 8 to 12 moved to beat 20: what lies outside both is as it was.
        step = "Move Time";
        change (song, "Move Time", [&] { phazite::moveTime (song, { at (8.0), at (12.0) }, at (20.0)); });
        checkSame (0.0, 7.75, 0.0);
        checkSame (20.25, 32.0, 0.0);
        checkSame (8.25, 11.75, 8.0);
        phazite::undo (song);
        checkSame (0.0, 32.0, 0.0);
    }
}

//==============================================================================
TEST_CASE ("A preset loaded while a track writes automation in Latch writes no automation for the values it sets")
{
    TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, phazite::devices::Synth::typeID);
    s.addMidiClip (track, 0.0, 16.0);

    // A preset of the synth with its cutoff turned down, the synth then set back.
    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Dark.phazitepreset");
    s.setParameter (synth, "cutoff", 400.0f);
    REQUIRE (phazite::saveDevicePreset (s.context, synth, file));
    s.setParameter (synth, "cutoff", 8000.0f);

    phazite::setAutomationMode (s.context, track, phazite::AutomationMode::latch);
    EnginePlayback playback;
    s.run ("transport.playStop");
    playback.play (0.5);
    REQUIRE (phazite::loadPresetInto (s.context, synth, file));
    playback.play (0.5);
    s.run ("transport.playStop");

    CHECK (countCurves (song, synth) == 0);
    CHECK (phazite::getValueSetByHand (song, { synth, "cutoff" }) == doctest::Approx (400.0f));
}

TEST_CASE ("Write mode's punch-in as playback starts is an undo step of its own, not a part of the change before")
{
    TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.addMidiClip (track, 0.0, 16.0);
    const auto volume = s.getVolumeParameter (track);
    s.addAutomation (volume, { { at (0.0), 0.5f }, { at (16.0), 0.5f } });
    s.setVolume (track, -20.0f); // set by hand, away from its lane
    const auto byHand = phazite::getValueSetByHand (song, volume);
    REQUIRE (byHand != doctest::Approx (0.5f));
    s.rename (track, "Named");

    phazite::setAutomationMode (s.context, track, phazite::AutomationMode::write);
    EnginePlayback playback;
    s.run ("transport.playStop");
    playback.play (0.5);
    s.run ("transport.playStop");

    // What the pass recorded undone, the track keeps its name, and its volume is set by hand where it was.
    while (phazite::describeUndo (&song) == "Undo Record Automation")
        phazite::undo (song);

    CHECK (phazite::getTrackName (song, track) == juce::String ("Named"));
    CHECK (phazite::getValueSetByHand (song, volume) == doctest::Approx (byHand));
}

//==============================================================================
TEST_CASE ("Points selected on a device's lane are neither found nor counted once the device is removed, and are again after Undo")
{
    // The selection counted the points of a curve whose device was gone, which getSelectedPoints() left out.
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto saturator = s.addEffect (s.firstTrack, phazite::devices::Saturator::typeID);
    const ParameterRef drive { saturator, "drive" };
    s.addAutomation (drive, { { at (0.0), 0.0f }, { at (4.0), 12.0f } });
    auto& selection = phazite::getSelection (s.context);
    phazite::selectPoints (selection, drive, { 0, 1 });
    REQUIRE (phazite::getSelectedPoints (selection).has_value());
    CHECK (selection.getNumObjectsSelected() == 2);

    phazite::removeDevice (song, saturator);
    CHECK_FALSE (phazite::getSelectedPoints (selection).has_value());
    CHECK (selection.getNumObjectsSelected() == 0);

    phazite::undo (song);
    CHECK (phazite::getSelectedPoints (selection).has_value());
    CHECK (selection.getNumObjectsSelected() == 2);
}

//==============================================================================
TEST_CASE ("A plain folder's volume ridden in Latch is written, though an effect dropped on the folder meanwhile makes it a bus")
{
    // The pass kept the plain folder's VCA, which the bus's fader replaced: as playback stopped, it found no parameter and
    // wrote nothing.
    TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto inside = s.addTrack ("Inside");
    s.addMidiClip (inside, 0.0, 16.0);
    const auto folder = s.addBus ("Bus", { inside });
    phazite::setMixedAsBus (song, folder, false);
    REQUIRE_FALSE (phazite::isBus (song, folder));
    const auto vca = s.getVolumeParameter (folder);
    phazite::setAutomationMode (s.context, folder, phazite::AutomationMode::latch);

    EnginePlayback playback;
    s.run ("transport.playStop");
    playback.play (0.5);
    phazite::getVolume (s.context, folder).setValue (0.3f);
    playback.play (0.5);
    s.addEffect (folder, phazite::devices::Equalizer::typeID);
    REQUIRE (phazite::isBus (song, folder));
    playback.play (0.5);
    s.run ("transport.playStop");

    const auto fader = s.getVolumeParameter (folder);
    REQUIRE (fader != vca);
    const auto points = phazite::getPoints (song, fader);
    CHECK (std::any_of (points.begin(), points.end(),
                        [] (const phazite::AutomationPoint& p) { return std::abs (p.position - 0.3f) < 0.001f; }));
}

TEST_CASE ("Undone, a folder's switch between a bus and a plain folder gives its volume's lane back its place and its height")
{
    // The lane was named after the new fader alone: undone, the curve showed as a lane of its own after the others.
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto inside = s.addTrack ("Inside");
    const auto bus = s.addBus ("Bus", { inside });
    const auto volume = s.getVolumeParameter (bus), pan = s.getPanParameter (bus);
    s.addAutomation (volume, { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
    s.addAutomation (pan, { { at (0.0), -0.5f }, { at (4.0), 0.5f } });
    phazite::showLane (song, bus, volume);
    phazite::showLane (song, bus, pan);
    phazite::setLaneHeight (song, bus, volume, 100);

    const auto checkLanes = [&] (const ParameterRef& first, bool withPan)
    {
        const auto lanes = phazite::getLanes (song, bus);
        REQUIRE (lanes.size() == (withPan ? 2u : 1u));
        CHECK (lanes[0].parameter == first);
        CHECK (lanes[0].height == 100);
    };

    checkLanes (volume, true);
    phazite::setMixedAsBus (song, bus, false);
    checkLanes (s.getVolumeParameter (bus), false);
    phazite::undo (song);
    checkLanes (volume, true);
    phazite::redo (song);
    checkLanes (s.getVolumeParameter (bus), false);
}

TEST_CASE ("Playing in Write mode keeps a Swap open while no punch-in changes a value")
{
    // Playback in Write began a step for any automated parameter, which kept the preset a Swap was trying and ended the Swap.
    TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, phazite::devices::Synth::typeID);
    s.addMidiClip (track, 0.0, 16.0);
    const auto volume = s.getVolumeParameter (track);
    const auto byHand = phazite::getValueSetByHand (song, volume);
    s.addAutomation (volume, { { at (0.0), byHand }, { at (16.0), byHand } });

    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Dark.phazitepreset");
    s.setParameter (synth, "cutoff", 400.0f);
    REQUIRE (phazite::saveDevicePreset (s.context, synth, file));
    s.setParameter (synth, "cutoff", 8000.0f);

    phazite::Swap swap (s.context, synth);
    REQUIRE (swap.tryFile (file));
    REQUIRE (swap.isOpen());
    phazite::setAutomationMode (s.context, track, phazite::AutomationMode::write);
    EnginePlayback playback;
    s.run ("transport.playStop");
    playback.play (0.25);
    CHECK (swap.isOpen());
    s.run ("transport.playStop");
}

TEST_CASE ("A Latch pass that already runs writes the value a preset sets, as the engine plays it")
{
    // A pass running while a preset loaded took none of its values: the lane it wrote ended where the hand left it, not
    // where the preset put it, which the engine played.
    TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, phazite::devices::Synth::typeID);
    s.addMidiClip (track, 0.0, 16.0);

    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Dark.phazitepreset");
    s.setParameter (synth, "cutoff", 400.0f);
    REQUIRE (phazite::saveDevicePreset (s.context, synth, file));
    s.setParameter (synth, "cutoff", 8000.0f);

    phazite::setAutomationMode (s.context, track, phazite::AutomationMode::latch);
    EnginePlayback playback;
    s.run ("transport.playStop");
    playback.play (0.25);
    phazite::setValueSetByHand (song, { synth, "cutoff" }, 2000.0f); // the pass begins
    playback.play (0.25);
    REQUIRE (phazite::loadPresetInto (s.context, synth, file));
    playback.play (0.25);
    s.run ("transport.playStop");

    const auto points = phazite::getPoints (song, { synth, "cutoff" });
    REQUIRE (points.size() >= 2);
    const auto at400 = phazite::toControlPosition (phazite::getParameter (s.context, { synth, "cutoff" }).getRange(), 400.0f);
    CHECK (std::any_of (points.begin(), points.end(),
                        [&at400] (const phazite::AutomationPoint& p) { return std::abs (p.position - at400) < 0.001f; }));
    CHECK (countCurves (song, synth) == 1); // no other parameter's
}

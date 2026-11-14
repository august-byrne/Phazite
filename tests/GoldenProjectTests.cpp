// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files of saved projects (tests/golden/projects): a new project and a project with one of each kind of track and
// every piece of Phazite's data are saved, reopened and saved again, and the second save is compared with a committed
// file. GoldenFiles.h says what the comparison leaves out and how to regenerate the files. The second project holds every
// name a saved song can have: its test fails for an element type, a property or a value that Schema.h declares, or that
// the song's rules name (schema::describeSongRules()), which the project doesn't hold, unless getNamesNotInTheProject()
// says why it can't.
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "SandboxedPlugins.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/nodes/Channel.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/chords/Progressions.h"
#include "model/chords/internal/Chords.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/devices/internal/Wavetables.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/internal/Notes.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContents.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/TempoModel.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "ui/pianoroll/DrumRows.h"

#include <map>
#include <optional>
#include <vector>

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

std::unique_ptr<phazite::Project> openProject (const juce::File& file)
{
    juce::String error;
    auto project = phazite::Project::open (file, false, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    phazite::test::settle();
    return project;
}

juce::String getAttributeNames (const juce::XmlElement& element)
{
    juce::StringArray names;

    for (const auto& attribute : element.getAttributeIterator())
        names.add (attribute.name.toString());

    return names.joinIntoString (" ");
}

juce::String getChildTags (const juce::XmlElement& element)
{
    juce::StringArray tags;

    for (const auto* child : element.getChildIterator())
        tags.add (child->getTagName());

    return tags.joinIntoString (" ");
}

//==============================================================================
// One of the device types the device lists offer, by its type, such as phazite::devices::Reverb::typeID.
phazite::DeviceType findType (const phazite::ProjectContext& context, const char* typeID)
{
    for (const auto& type : phazite::getDeviceTypes (context))
        if (type.description.fileOrIdentifier == juce::String (typeID))
            return type;

    FAIL ("Phazite offers no device of type " << typeID);
    return {};
}

// The tracks of the representative project that its parts share.
struct Tracks
{
    phazite::TrackID drums, keys, pad, tone, group, bus, reverbReturn;
};

// Containers, what their chains hold and what the devices in them play: on Pad, an instrument container of two layers, a
// wavetable synth for the low keys and the soft notes and a sampler for the others, with a macro; on Tone, effects split
// into bands, into the middle and the sides, and into left and right; and Tone's other devices: one that ducks under the
// drums, one switched off, one whose window was moved, and a plug-in.
void addContainersAndDevices (phazite::Song& song, phazite::ProjectContext& context, const Tracks& tracks, const juce::File& scratch)
{
    using namespace phazite::devices;
    using phazite::decibelsToFaderPosition;

    const auto layers = phazite::addInstrumentContainer (context, tracks.pad);
    REQUIRE (layers.isValid());
    phazite::renameContainer (context, layers, "Layers");
    phazite::getMix (context, layers).setValue (0.75f);

    const auto low = phazite::getChains (context, layers).front().id;
    phazite::chooseChainInstrument (context, low, findType (context, WavetableSynth::typeID));
    phazite::renameChain (context, low, "Low");
    phazite::setChainKeys (context, low, { 24, 59 });
    phazite::setChainVelocities (context, low, { 20, 100 });
    phazite::toggleChainSolo (context, low);

    const auto high = phazite::addChain (context, layers);
    REQUIRE (high.isValid());
    phazite::chooseChainInstrument (context, high, findType (context, Sampler::typeID));
    phazite::addEffectToChain (context, high, findType (context, Delay::typeID));
    phazite::setChainKeys (context, high, { 60, 127 });
    phazite::getChainVolume (context, high).setValue (decibelsToFaderPosition (-6.0f));
    phazite::getChainPan (context, high).setValue (0.25f);
    phazite::toggleChainMute (context, high);

    // The wavetable synth: a factory table for its first oscillator and a sound file for its second.
    const auto wavetable = phazite::getChains (context, layers)[0].devices.front().id;
    phazite::setFactoryWavetable (song, wavetable, 0, 1);
    REQUIRE (phazite::setWavetableFile (song, wavetable, 1, phazite::test::writeSound (scratch.getChildFile ("Wave.wav"), 0.5, 110.0)));

    // The sampler: a sound file cut into equal slices, one of them taken away and one put in by hand.
    const auto sampler = phazite::getChains (context, layers)[1].devices.front().id;
    REQUIRE (phazite::setSampleFile (song, sampler, phazite::test::writeSound (scratch.getChildFile ("Hit.wav"), 1.0, 330.0)));
    phazite::getParameter (context, { sampler, "mode" }).setValue (2.0f);    // Slice
    phazite::getParameter (context, { sampler, "sliceBy" }).setValue (2.0f); // Equal
    phazite::removeSlice (song, sampler, 2);
    phazite::addSlice (song, sampler, 0.3f);

    // A third layer: a sampler that plays one of its factory sounds.
    const auto third = phazite::addChain (context, layers);
    REQUIRE (third.isValid());
    phazite::chooseChainInstrument (context, third, findType (context, Sampler::typeID));
    phazite::setFactorySample (song, phazite::getChains (context, layers)[2].devices.front().id, 1);

    // The container's first macro, named and turned, on the wavetable synth's cutoff and the high chain's volume.
    const auto macros = phazite::getContainerMacros (context, layers);
    REQUIRE (macros.size() == 8);
    phazite::renameMacro (context, macros[0].id, "Brightness");
    phazite::getModulatorParameter (context, macros[0].id, phazite::getModulatorParameters (context, macros[0].id).front().id)
        .setValue (0.5f);
    phazite::modulate (context, macros[0].id, { wavetable, "cutoff" }, 0.5f);
    phazite::modulate (context, macros[0].id, phazite::getChainVolume (context, high).getRef(), -0.25f);

    // Tone's effects: three containers, each split another way.
    const auto bands = phazite::addContainer (context, tracks.tone);
    REQUIRE (bands.isValid());
    phazite::renameContainer (context, bands, "Bands");
    phazite::getMix (context, bands).setValue (0.5f);
    const auto top = phazite::addChain (context, bands);
    REQUIRE (top.isValid());
    phazite::setContainerSplit (context, bands, phazite::ContainerSplit::bands);
    phazite::getChainCrossover (context, top).setValue (2000.0f);
    phazite::addEffectToChain (context, top, findType (context, Saturator::typeID));

    const auto midSide = phazite::addContainer (context, tracks.tone);
    REQUIRE (midSide.isValid());
    REQUIRE (phazite::addChain (context, midSide).isValid());
    phazite::setContainerSplit (context, midSide, phazite::ContainerSplit::midSide);

    const auto leftRight = phazite::addContainer (context, tracks.tone);
    REQUIRE (leftRight.isValid());
    REQUIRE (phazite::addChain (context, leftRight).isValid());
    phazite::setContainerSplit (context, leftRight, phazite::ContainerSplit::leftRight);
    phazite::setDeviceOn (context, leftRight, false);

    // A compressor that listens to the drums, a filter switched off, and an EQ whose window was moved.
    phazite::addEffect (context, tracks.tone, findType (context, Compressor::typeID));
    const auto compressor = phazite::getEffects (context, tracks.tone).back().id;
    REQUIRE (phazite::hasSidechain (context, compressor));
    phazite::setSidechainSource (context, compressor, tracks.drums);

    phazite::addEffect (context, tracks.tone, findType (context, FilterEffect::typeID));
    phazite::setDeviceOn (context, phazite::getEffects (context, tracks.tone).back().id, false);

    phazite::addEffect (context, tracks.tone, findType (context, Equalizer::typeID));
    phazite::rememberWindowPosition (song, phazite::getEffects (context, tracks.tone).back().id, { 120, 80 });
}

// The drum sampler on Drums: a factory sound on its kick's pad (from a kit's preset), a sound file on its snare's, another
// in the snare's second layer, and the snare on a track of its own.
phazite::TrackID addDrumSampler (phazite::Song& song, phazite::ProjectContext& context, const Tracks& tracks, const juce::File& scratch)
{
    phazite::chooseInstrument (context, tracks.drums, findType (context, phazite::devices::DrumSampler::typeID));
    const auto sampler = phazite::getInstrument (context, tracks.drums);
    REQUIRE (sampler.has_value());

    const auto kit = scratch.getChildFile ("Golden Kit.phazitepreset");
    REQUIRE (kit.getParentDirectory().createDirectory().wasOk());
    REQUIRE (kit.replaceWithText (R"({ "kind": "device", "device": { "type": "phaziteDrums", "name": "Golden Kit", "instrument": true,
        "parameters": {}, "sounds": { "pad36Sound": "808 Kick" } } })"));
    REQUIRE (phazite::loadPresetInto (context, sampler->id, kit));
    const auto drumSampler = phazite::getInstrument (context, tracks.drums)->id; // a preset may make the device anew

    REQUIRE (phazite::setPadSound (song, drumSampler, 38, phazite::test::writeSound (scratch.getChildFile ("Snare.wav"), 0.25, 180.0)));
    REQUIRE (
        phazite::setPadSound (song, drumSampler, 38, phazite::test::writeSound (scratch.getChildFile ("Snare Top.wav"), 0.25, 720.0), 1));
    const auto snare = phazite::givePadItsOwnTrack (song, drumSampler, 38);
    REQUIRE (snare.isValid());
    return snare;
}

// Modulators on Keys, each kind's values and what it modulates: an LFO on the synth's cutoff, a step sequencer on the
// track's pan, switched off, and a macro of the track's on its volume.
void addModulators (phazite::ProjectContext& context, const Tracks& tracks)
{
    const auto synth = phazite::getInstrument (context, tracks.keys);
    REQUIRE (synth.has_value());

    const auto lfo = phazite::addModulator (context, tracks.keys, phazite::ModulatorType::lfo);
    REQUIRE (lfo.isValid());
    phazite::getModulatorParameter (context, lfo, "rate").setValue (2.0f);
    phazite::modulate (context, lfo, { synth->id, "cutoff" }, 0.375f);

    const auto steps = phazite::addModulator (context, tracks.keys, phazite::ModulatorType::steps);
    REQUIRE (steps.isValid());
    phazite::setStep (context, steps, 0, 1.0f);
    phazite::setStep (context, steps, 2, -0.5f);
    phazite::modulate (context, steps, phazite::getPan (context, tracks.keys).getRef(), 0.25f);
    phazite::setModulatorOn (context, steps, false);

    const auto macro = phazite::addModulator (context, tracks.keys, phazite::ModulatorType::macro);
    REQUIRE (macro.isValid());
    phazite::renameMacro (context, macro, "Swell");
    phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro).front().id).setValue (0.25f);
    phazite::modulate (context, macro, phazite::getVolume (context, tracks.keys).getRef(), -0.5f);
}

// A MIDI file with a note and one of each kind of controller event a recording takes in.
juce::File writeMidiFile (const juce::File& file)
{
    juce::MidiMessageSequence events;
    const auto add = [&events] (juce::MidiMessage message, double ticks)
    {
        message.setTimeStamp (ticks);
        events.addEvent (message);
    };

    add (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 96), 0.0);
    add (juce::MidiMessage::controllerEvent (1, 64, 127), 0.0);
    add (juce::MidiMessage::pitchWheel (1, 9000), 240.0);
    add (juce::MidiMessage::channelPressureChange (1, 80), 480.0);
    add (juce::MidiMessage::aftertouchChange (1, 64, 70), 720.0);
    add (juce::MidiMessage::programChange (1, 5), 840.0);
    add (juce::MidiMessage::noteOff (1, 64), 960.0);
    events.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (480);
    midi.addTrack (events);

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out (file);
    REQUIRE (out.openedOk());
    REQUIRE (midi.writeTo (out));
    return file;
}

// What clips and notes can hold besides the first ones: a MIDI clip muted, trimmed at its start, with a slide note and a
// note that glides in; a clip imported with its controller events; and an audio clip that follows the song's tempo, with
// warp markers, pitched, reversed, louder, with S-curve fades.
void addMoreClips (phazite::Song& song, phazite::ProjectContext& context, const Tracks& tracks, const juce::File& scratch)
{
    const auto riff = phazite::insertMidiClip (song, tracks.pad, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (riff.isValid());
    phazite::addNote (song, riff, 48, beats (1.0), phazite::BeatDuration::fromBeats (1.0), 100);
    const auto slide = phazite::addNote (song, riff, 55, beats (1.5), phazite::BeatDuration::fromBeats (0.5), 100);
    const auto glide = phazite::addNote (song, riff, 60, beats (3.0), phazite::BeatDuration::fromBeats (1.0), 100);
    REQUIRE (slide.isValid());
    REQUIRE (glide.isValid());
    phazite::toggleNotesSlide (song, { slide });
    phazite::toggleNotesPortamento (song, { glide });
    phazite::trimClipStart (song, riff, beats (1.0));
    phazite::toggleClipsMuted (song, { riff });

    const auto imported = phazite::importFile (song, tracks.pad, writeMidiFile (scratch.getChildFile ("Pedal.mid")), beats (8.0));
    REQUIRE (imported.isValid());

    // The clicks are named for their tempo, so the clip follows the song's.
    const auto clicks = phazite::importFile (song, tracks.tone, phazite::test::writeClicks (scratch, 4, 100.0), beats (16.0));
    REQUIRE (phazite::hasAudioClips (song, { clicks }));
    REQUIRE (phazite::addWarpMarker (context, clicks, 0.6));
    REQUIRE (phazite::addWarpMarker (context, clicks, 1.2));
    phazite::moveWarpMarker (context, clicks, 1.2, beats (18.5));
    phazite::setClipPitch (context, clicks, -2.5f);
    phazite::setClipReversed (context, clicks, true);
    phazite::setClipGain (context, clicks, 3.0f);
    phazite::setClipFade (song, clicks, true, 0.1);
    phazite::setClipFade (song, clicks, false, 0.2);
    phazite::setFadeShape (song, { clicks }, phazite::sCurveFade);
}

// The tracks that record, and the folder they are in.
struct Recorded
{
    phazite::TrackID voice, guitar, folder;
};

// A take recorded round a looped range, onto a track that records the device's first input, unheard: a clip of three
// takes, the first of them chosen. And a track that records its second input, always heard. Both in a plain folder,
// whose VCA sets their volume. The engine is driven a block at a time, so that the same is recorded every time.
Recorded addTakes (phazite::Song& song, phazite::ProjectContext& context)
{
    const auto voice = phazite::addTrackAtEnd (song, false, nullptr);
    REQUIRE (voice.isValid());
    phazite::setTrackName (song, voice, "Voice");
    phazite::setTrackColour (song, voice, juce::Colour (0xff3366cc));

    const auto guitar = phazite::addTrackAtEnd (song, false, nullptr);
    REQUIRE (guitar.isValid());
    phazite::setTrackName (song, guitar, "Guitar");

    phazite::test::EnginePlayback playback { phazite::test::EnginePlayback::Input::loopback };
    phazite::setCountInOn (phazite::getAudioIO (phazite::test::getEngineContext()), false);
    phazite::setTrackInput (context, guitar, "2");
    phazite::setInputMonitoring (context, guitar, phazite::InputMonitoring::always);
    phazite::setTrackInput (context, voice, "1");
    phazite::setInputMonitoring (context, voice, phazite::InputMonitoring::never);
    phazite::setArmed (context, voice, true);
    REQUIRE (phazite::isArmed (context, voice));

    phazite::loopBeatRange (song, 0.0, 2.0);
    phazite::toggleRecording (song);
    REQUIRE (phazite::isRecording (song));
    playback.play (2.5); // twice round and a half, at 120 BPM
    phazite::toggleRecording (song);
    REQUIRE_FALSE (phazite::isRecording (song));
    phazite::setArmed (context, voice, false);

    const auto clips = phazite::getClips (context, voice);
    REQUIRE (clips.size() == 1);
    REQUIRE (phazite::getClipTakes (context, clips[0].id).count == 3);
    phazite::setClipTake (context, clips[0].id, 1);
    phazite::setPunchOn (context, true); // from now on: a punch keeps one take

    const auto folder = phazite::groupIntoFolder (song, { voice, guitar }, nullptr);
    REQUIRE (folder.isValid());
    phazite::setTrackName (song, folder, "Takes");
    phazite::setMixedAsBus (song, folder, false);
    return { voice, guitar, folder };
}

// The song's own: a tempo that ramps to another, patterns that keep their speed, the metronome, the project's notes; lanes
// on Keys (one taller, one collapsed) and on the master, with curves of the master's volume and pan, of a plain folder's
// VCA, of a send's level and of a chain's crossover, a point's line bent; tracks' automation modes; a track soloed; and the
// master collapsed, turned down and panned.
void addSongSettings (phazite::Song& song, phazite::ProjectContext& context, const Tracks& tracks, phazite::TrackID plainFolder)
{
    const auto change = phazite::addTempoChange (context, beats (16.0), 140.0);
    REQUIRE (change >= 0);
    phazite::setTempoCurve (context, 0, 0.25f);
    phazite::togglePatternsFollowTempo (song);
    phazite::setMetronomeOn (song, true);
    phazite::setProjectNotes (context, "Mix notes:\nturn the pad down");

    const auto keysVolume = phazite::getVolume (context, tracks.keys).getRef();
    const auto keysPan = phazite::getPan (context, tracks.keys).getRef();
    phazite::showAutomationLane (context, keysVolume);
    phazite::setAutomationLaneHeight (context, keysVolume, 96);
    phazite::showAutomationLane (context, keysPan);
    phazite::setAutomationLaneCollapsed (context, keysPan, true);
    phazite::setAutomationCurve (context, keysVolume, 0, 0.5f);

    const auto master = phazite::getMasterID (song);
    const phazite::ParameterRef masterVolume { phazite::getFader (song, master), phazite::getVolumeParameterID (song, master) };
    const phazite::ParameterRef masterPan { phazite::getFader (song, master), phazite::getPanParameterID (song, master) };
    phazite::addAutomationPoint (context, masterVolume, { beats (0.0), 0.75f, 0.0f });
    phazite::addAutomationPoint (context, masterPan, { beats (0.0), 0.0f, 0.0f });
    phazite::addAutomationPoint (context, masterPan, { beats (16.0), 0.25f, 0.0f });
    phazite::showAutomationLane (context, masterVolume);

    phazite::addAutomationPoint (context, phazite::getVolume (context, plainFolder).getRef(), { beats (0.0), 0.5f, 0.0f });

    const auto sends = phazite::getSends (context, tracks.keys);
    REQUIRE (! sends.empty());
    phazite::addAutomationPoint (context, phazite::getSendLevel (context, sends.front().id).getRef(), { beats (4.0), 0.5f, 0.0f });

    for (const auto& device : phazite::getEffects (context, tracks.tone))
        if (device.isContainer && phazite::getContainerSplit (context, device.id) == phazite::ContainerSplit::bands)
            phazite::addAutomationPoint (context,
                                         phazite::getChainCrossover (context, phazite::getChains (context, device.id).back().id).getRef(),
                                         { beats (8.0), 4000.0f, 0.0f });

    phazite::setAutomationMode (context, tracks.keys, phazite::AutomationMode::touch);
    phazite::setAutomationMode (context, tracks.pad, phazite::AutomationMode::latch);
    phazite::setAutomationMode (context, tracks.tone, phazite::AutomationMode::write);
    phazite::setAutomationMode (context, master, phazite::AutomationMode::touch);
    phazite::setTrackSoloed (song, tracks.keys, true);

    phazite::setCollapsed (song, master, true);
    phazite::setChannelVolume (song, master, -6.0f);
    phazite::setChannelPan (song, master, 0.125f);
    phazite::setCollapsed (song, tracks.group, true); // again: showing Keys' lanes opened its folder
}

// Freezes a track, and waits for its render.
void freeze (phazite::ProjectContext& context, phazite::TrackID track)
{
    std::optional<juce::Result> result;
    const auto render = phazite::startFreeze (context, track, [&result] (const juce::Result& r) { result = r; });
    REQUIRE (render != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, 60000));
    REQUIRE_MESSAGE (result->wasOk(), result->getErrorMessage().toStdString());
    REQUIRE (phazite::isFrozen (context, track));
}

//==============================================================================
// Every kind of track and every piece of Phazite's data that a project keeps, so that its golden file covers them all:
// the test below fails for a name Schema.h declares, or the song's rules name, that the project doesn't hold. The song is
// open in the context, as the app has it, for the model's functions that take one and for what plays it (a plug-in, a
// take).
void addRepresentativeContent (phazite::Project& project, phazite::ProjectContext& context, const juce::File& scratch)
{
    using phazite::test::makeBuiltInDevice;

    auto& song = project.getSong();
    phazite::setTempo (song, 120.0);
    phazite::setTimeSignature (song, { 4, 4 });

    const auto drums = phazite::getTrackIDs (song).front();
    phazite::setTrackName (song, drums, "Drums");

    // Sections: two from an outline, and one of their own.
    const auto outline = phazite::Outline::fromJSON (R"({ "id": "golden", "name": "Golden", "sections": [
        { "name": "Intro", "role": "intro", "bars": 2, "energy": 0.25, "tip": "Set the mood." },
        { "name": "Drop", "role": "drop", "bars": 4, "energy": 0.9, "colour": "Yellow", "tip": "Everything at once." } ] })");
    REQUIRE (outline.has_value());
    phazite::applyOutline (song, *outline);
    const phazite::SectionInfo outro { .name = "Outro",
                                       .length = phazite::BeatDuration::fromBeats (8.0),
                                       .role = "outro",
                                       .tip = "Let it fade.",
                                       .energy = 0.1f };
    REQUIRE (phazite::insertSection (song, phazite::BeatPosition::fromBeats (24.0), outro).isValid());

    // Drums: a beat, shown in drum mode with the snare's row first and a row hidden; the track pinned and muted.
    const auto beat = phazite::insertMidiClip (song, drums, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (beat.isValid());

    for (const auto start : { 0.0, 2.0 })
        phazite::addNote (song, beat, 36, beats (start), phazite::BeatDuration::fromBeats (0.25), 110);

    for (const auto start : { 1.0, 3.0 })
        phazite::addNote (song, beat, 38, beats (start), phazite::BeatDuration::fromBeats (0.25), 110);

    // A name of its own, which the playlist shows over it; the other MIDI clips have none, and are called after their tracks.
    phazite::renameClips (song, { beat }, "Groove");

    phazite::setDrumTrack (song, drums, true);
    const auto drumRows = phazite::ui::getDrumRows (phazite::getSamplerSounds (song, drums), phazite::getNoteNumbersPlayed (song, { beat }),
                                                    phazite::getDrumRowOrder (song, drums), phazite::getHiddenDrumRows (song, drums));
    phazite::setDrumRowOrder (song, drums, phazite::ui::getDrumOrderAfterMove (drumRows, 38, 0)); // the snare's row dragged to the top
    phazite::hideDrumRow (song, drums, 49);
    phazite::setPinned (song, drums, true);
    phazite::setTrackMuted (song, drums, true);

    // Keys: an instrument, and an effect with a mix of its own; a key; chords with a muted note, and an alias of them;
    // a height of its own, and a volume curve.
    const auto keys = phazite::insertTrack (song, drums, nullptr);
    REQUIRE (keys.isValid());
    phazite::setTrackName (song, keys, "Keys");
    REQUIRE (phazite::setInstrument (song, keys, makeBuiltInDevice (song, phazite::devices::Synth::typeID)).isValid());
    const auto reverb = phazite::insertDeviceState (song, keys, makeBuiltInDevice (song, phazite::devices::Reverb::typeID));
    REQUIRE (reverb.isValid());
    phazite::setMix (song, reverb, 0.25f);
    phazite::setTrackKey (song, keys, phazite::Key { 2, phazite::Scale::minor });

    const auto chords = phazite::insertMidiClip (song, keys, { beats (0.0), beats (8.0) }, nullptr);
    REQUIRE (chords.isValid());
    phazite::addNote (song, chords, 62, beats (0.0), phazite::BeatDuration::fromBeats (2.0), 90);
    const auto quiet = phazite::addNote (song, chords, 65, beats (2.0), phazite::BeatDuration::fromBeats (1.5), 70);
    REQUIRE (quiet.isValid());
    phazite::toggleNotesMuted (song, { quiet });

    const auto alias = phazite::copyClips (song, { chords }, true);
    REQUIRE (alias.size() == 1);
    phazite::moveClip (song, alias[0], beats (8.0), keys);
    phazite::setSavedTrackHeight (song, keys, 96);

    const phazite::ParameterRef keysVolume { phazite::getFader (song, keys), phazite::getVolumeParameterID (song, keys) };
    REQUIRE (keysVolume.isValid());
    phazite::addPoint (song, keysVolume, { phazite::BeatPosition(), phazite::engine::decibelsToFaderPosition (0.0f), 0.0f });
    phazite::addPoint (song, keysVolume,
                       { phazite::BeatPosition::fromBeats (8.0), phazite::engine::decibelsToFaderPosition (-6.0f), 0.0f });

    // Pad: a new track, which gets the key set last.
    const auto pad = phazite::insertTrack (song, keys, nullptr);
    REQUIRE (pad.isValid());
    phazite::setTrackName (song, pad, "Pad");

    // Tone: imported audio with fades of a shape of their own, on a track panned left. (No digits in the
    // file's name: Phazite would read a tempo in them and stretch the clip.)
    const auto tone = phazite::insertTrack (song, pad, nullptr);
    REQUIRE (tone.isValid());
    phazite::setTrackName (song, tone, "Tone");
    const auto audio = phazite::importFile (song, tone, phazite::test::writeSound (scratch.getChildFile ("Tone.wav"), 2.0), beats (4.0));
    REQUIRE (phazite::hasAudioClips (song, { audio }));
    phazite::setClipFade (song, audio, true, 0.25);
    phazite::setClipFade (song, audio, false, 0.5);
    phazite::setFadeShape (song, { audio }, phazite::convexFade);
    phazite::setChannelPan (song, tone, -0.5f);

    // Folders: Keys and Pad in a collapsed folder, and Tone in a bus at -3 dB.
    const auto group = phazite::groupIntoFolder (song, { keys, pad }, nullptr);
    REQUIRE (group.isValid());
    phazite::setTrackName (song, group, "Group");
    phazite::setCollapsed (song, group, true);

    const auto bus = phazite::groupIntoFolder (song, { tone }, nullptr);
    REQUIRE (bus.isValid());
    phazite::setTrackName (song, bus, "Bus");
    phazite::setMixedAsBus (song, bus, true);
    phazite::setChannelVolume (song, bus, -3.0f);

    // Returns: a reverb return that Tone sends to after its fader and Keys before its fader; and a reverb on the master.
    const auto reverbReturn = phazite::addReturnWithEffect (song, tone, makeBuiltInDevice (song, phazite::devices::Reverb::typeID));
    REQUIRE (reverbReturn.isValid());
    REQUIRE (phazite::addSend (song, keys, reverbReturn, true).isValid());
    REQUIRE (phazite::insertDeviceState (song, phazite::getMasterID (song), makeBuiltInDevice (song, phazite::devices::Reverb::typeID))
                 .isValid());

    // View settings: the playlist's grid and the piano roll's, and a range looped on purpose.
    phazite::saveGridSettings (song, phazite::GridOwner::playlist, { .snap = false, .adaptiveOffset = 1 });
    phazite::saveGridSettings (song, phazite::GridOwner::pianoRoll, { .adaptive = false, .fixedLevel = 8, .triplets = true });

    phazite::loopBeats (song, { beats (4.0), beats (12.0) });

    // The chord lane: a progression in D minor over the intro and the drop, with a borrowed chord in its second inversion,
    // and a key change to F major for the outro.
    const auto progression = phazite::Progression::fromJSON (
        juce::JSON::parse (R"({ "id": "golden", "name": "Golden", "scale": "minor", "chords": "i*2 VI VII/1 iim7b5 V7" })"));
    REQUIRE (progression.has_value());
    const auto at = [] (double b) { return phazite::BeatPosition::fromBeats (b); };
    phazite::setLaneKey (song, at (0.0), { 2, phazite::Scale::minor });
    phazite::putProgression (song, *progression, { at (0.0), at (24.0) });
    phazite::putLaneChord (song, { at (20.0), at (24.0) }, { 2, -1, phazite::ChordType::majorSeventh, 2 });
    phazite::setLaneKey (song, at (24.0), { 5, phazite::Scale::major });
    phazite::putLaneChord (song, { at (24.0), at (32.0) }, { 1, 0, phazite::ChordType::suspendedFourth, 0 });

    // What came since: containers and what their chains hold, the drum sampler's pads, modulators and macros, clips' and
    // notes' other properties, takes, a plug-in, and the song's settings; and a track frozen, last, as a change to it would
    // unfreeze it.
    const Tracks tracks { drums, keys, pad, tone, group, bus, reverbReturn };
    addContainersAndDevices (song, context, tracks, scratch);
    const auto snare = addDrumSampler (song, context, tracks, scratch);
    phazite::setTrackName (song, snare, "Snare");
    addModulators (context, tracks);
    addMoreClips (song, context, tracks, scratch);
    const auto recorded = addTakes (song, context);

    // Plug-ins, the tests' stand-ins, which play in a helper as every plug-in does: an effect on the return, at half its
    // mix, with a value it keeps in its own state and one set by hand; and an instrument on a track of its own.
    const auto plugin = phazite::test::TestPlugin::add (song, reverbReturn);
    REQUIRE (plugin.isValid());
    phazite::getMix (context, plugin).setValue (0.5f);
    phazite::test::TestPlugin::setGain (song, plugin, 0.5f);
    phazite::getParameter (context, { plugin, phazite::test::TestPlugin::toneID }).setValue (0.25f);

    const auto bells = phazite::addTrackAtEnd (song, false, nullptr);
    REQUIRE (bells.isValid());
    phazite::setTrackName (song, bells, "Bells");
    const auto synthName = phazite::test::TestPlugins::getDescription (phazite::test::TestPlugins::Kind::synth).name;

    for (const auto& type : phazite::getDeviceTypes (context))
        if (! type.isBuiltIn && type.name == synthName)
            phazite::chooseInstrument (context, bells, type);

    REQUIRE (phazite::getInstrument (context, bells).has_value());

    addSongSettings (song, context, tracks, recorded.folder);
    freeze (context, recorded.voice);
}

//==============================================================================
// The saves each golden project test compares: S1, the project as made; S2, saved after reopening S1; S3, saved after
// reopening S2.
struct Saves
{
    std::unique_ptr<juce::XmlElement> rawFirst;  // S1 as Phazite wrote it
    std::unique_ptr<juce::XmlElement> first;     // S1, normalised
    std::unique_ptr<juce::XmlElement> rawSecond; // S2 as Phazite wrote it
};

// Saves a project (S1), reopens and saves it (S2), and does that again (S3). S2 holds everything S1 does (opening it may
// add what every song has), S3 is S2, and S2 matches the golden file tests/golden/projects/<goldenName>.xml. Each
// reopened project has no unsaved changes once the message loop has run after it opened (openProject() lets it):
// opening changes nothing. A context the project's song is open in (which plays it for the first save, so that it stores
// what its plug-ins keep) lets go of it after that save; the reopened projects are open in none.
Saves saveReopenAndSaveAgain (std::unique_ptr<phazite::Project> project, const juce::String& goldenName, const juce::String& about,
                              phazite::ProjectContext* context = nullptr)
{
    const auto file = project->getFile();
    Saves saves;

    REQUIRE (project->save().wasOk());
    saves.rawFirst = phazite::test::readXml (file);
    saves.first = phazite::test::normaliseProject (*saves.rawFirst, file.getParentDirectory());

    if (context != nullptr)
        setSong (*context, nullptr);

    project = nullptr;

    project = openProject (file);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    saves.rawSecond = phazite::test::readXml (file);
    const auto second = phazite::test::normaliseProject (*saves.rawSecond, file.getParentDirectory());
    project = nullptr;

    project = openProject (file);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    const auto third = phazite::test::readNormalisedProject (file);
    project = nullptr;

    phazite::test::checkSameProject ("reopened and saved", *saves.first, *second, { .actualMayAdd = true });
    phazite::test::checkSameProject ("saved again", *second, *third);
    phazite::test::checkProjectGolden ("projects/" + goldenName + ".xml", *second, about);
    return saves;
}

//==============================================================================
// What a saved project holds, for the checks that the representative project holds every name Phazite saves: its
// elements' types ("CLIP"), each element's type in its parent ("TRACK/CLIP"), each property on its element ("CLIP@start"),
// and every value.
struct Held
{
    juce::StringArray types, children, properties, values;

    void add (const juce::XmlElement& element)
    {
        types.addIfNotAlreadyThere (element.getTagName());

        for (const auto& attribute : element.getAttributeIterator())
        {
            properties.addIfNotAlreadyThere (element.getTagName() + "@" + attribute.name.toString());
            values.addIfNotAlreadyThere (attribute.value);
        }

        for (const auto* child : element.getChildIterator())
        {
            children.addIfNotAlreadyThere (element.getTagName() + "/" + child->getTagName());
            add (*child);
        }
    }

    bool hasProperty (const juce::String& type, const juce::String& property) const { return properties.contains (type + "@" + property); }
};

// A name of the song's that Schema.h declares: the name it has there (trackHeight, or TRACK for an element's type), and
// what a project file holds for it ("height").
struct SchemaName
{
    juce::String declared, saved;
    bool isValue = false; // a value a property takes (trackIsFolder: "folder"), rather than a property or an element's type
};

// The names src/model/base/internal/Schema.h declares for a song, read from the header itself, so that a name added there
// is found here without being listed anywhere: each `inline const juce::Identifier name { "saved" }` (an element's type in
// capitals, or a property) and each `inline constexpr const char* name = "saved"` (a value). The names of the versions'
// store and of the presets' files (its namespaces versions and preset) aren't the song's.
std::vector<SchemaName> readSchemaNames()
{
    const auto header =
        phazite::test::getGoldenFolder().getParentDirectory().getParentDirectory().getChildFile ("src/model/base/internal/Schema.h");
    REQUIRE_MESSAGE (header.existsAsFile(), (header.getFullPathName() + " isn't there").toStdString());

    std::vector<SchemaName> names;
    juce::String skipping; // the namespace being skipped, to its end

    for (const auto& line : juce::StringArray::fromLines (header.loadFileAsString()))
    {
        const auto text = line.trim();

        if (skipping.isNotEmpty())
        {
            if (text == "} // namespace " + skipping)
                skipping.clear();

            continue;
        }

        if (text == "namespace versions" || text == "namespace preset")
        {
            skipping = text.fromFirstOccurrenceOf ("namespace ", false, false);
            continue;
        }

        if (text.startsWith ("inline const juce::Identifier "))
        {
            // One or more of `name { "saved" }`, between commas.
            for (auto rest = text.fromFirstOccurrenceOf ("juce::Identifier ", false, false); rest.contains ("{ \"");
                 rest = rest.fromFirstOccurrenceOf ("\" }", false, false))
                names.push_back ({ rest.upToFirstOccurrenceOf (" {", false, false).trimCharactersAtStart (", "),
                                   rest.fromFirstOccurrenceOf ("{ \"", false, false).upToFirstOccurrenceOf ("\"", false, false) });
        }
        else if (text.startsWith ("inline constexpr const char* ") && text.contains (" = \""))
        {
            names.push_back ({ text.fromFirstOccurrenceOf ("char* ", false, false).upToFirstOccurrenceOf (" =", false, false),
                               text.fromFirstOccurrenceOf ("= \"", false, false).upToFirstOccurrenceOf ("\"", false, false), true });
        }
    }

    return names;
}

// The elements that hold a property, by the first word of the name Schema.h gives it ("track" of trackHeight), or by the
// whole name where that word doesn't say. A property must be in the representative project on each of them.
juce::StringArray getHolders (const juce::String& declared)
{
    static const std::map<juce::String, juce::StringArray> byName {
        // The song's own.
        { "nextID", { "PHAZITE" } },
        { "loopFollowsSong", { "PHAZITE" } },
        { "loop", { "PHAZITE" } },
        { "loopStart", { "PHAZITE" } },
        { "loopEnd", { "PHAZITE" } },
        { "playStart", { "PHAZITE" } },
        { "metronome", { "PHAZITE" } },
        { "outline", { "PHAZITE" } },
        { "patternsKeepTheirSpeed", { "PHAZITE" } },
        { "notes", { "PHAZITE" } },
        { "lastInput", { "PHAZITE" } },
        { "punch", { "PHAZITE" } },
        { "bpm", { "TEMPO" } },
        { "numerator", { "TIMESIGNATURE" } },
        { "denominator", { "TIMESIGNATURE" } },
        // On more elements than their first word says, or on others.
        { "trackID", { "TRACK", "MASTER" } },
        { "trackAutomationMode", { "TRACK", "MASTER" } },
        { "deviceMix", { "DEVICE", "CONTAINER" } },
        { "chordStart", { "CHORD", "CHORDKEY" } },
    };

    static const std::map<juce::String, juce::StringArray> byFirstWord {
        { "track", { "TRACK" } },
        { "folder", { "TRACK" } },
        { "drum", { "TRACK" } },
        { "channel", { "TRACK", "MASTER" } },
        { "key", { "TRACK", "LASTKEY", "CHORDKEY" } },
        { "grid", { "PHAZITE", "PIANOROLL" } },
        { "map", { "TEMPO", "TIMESIGNATURE" } },
        { "tempo", { "TEMPO" } },
        { "lane", { "LANE" } },
        { "device", { "DEVICE" } },
        { "plugin", { "DEVICE" } },
        { "value", { "VALUE" } },
        { "container", { "CONTAINER" } },
        { "chain", { "CHAIN" } },
        { "send", { "SEND" } },
        { "modulator", { "MODULATOR" } },
        { "macro", { "MACRO" } },
        { "modulates", { "MODULATES" } },
        { "curve", { "CURVE" } },
        { "point", { "POINT" } },
        { "section", { "SECTION" } },
        { "chord", { "CHORD" } },
        { "clip", { "CLIP" } },
        { "note", { "NOTE" } },
        { "control", { "CONTROL" } },
        { "warp", { "WARP" } },
        { "sampler", { "SOUNDS" } },
    };

    if (const auto found = byName.find (declared); found != byName.end())
        return found->second;

    const auto firstWord = declared.initialSectionNotContaining ("ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    if (const auto found = byFirstWord.find (firstWord); found != byFirstWord.end())
        return found->second;

    return {};
}

// What the representative project needn't hold, each with why: "TYPE" for an element's type, "TYPE@property" for a
// property on an element, and for a value the name Schema.h declares it by. Anything else that Schema.h declares or the
// song's rules name must be in the project.
const std::map<juce::String, juce::String>& getNamesNotInTheProject()
{
    static const std::map<juce::String, juce::String> names {
        { "PHAZITE@playStart", "playback starts at the song's start again as a project opens: a project saved after reopening has none" },
        { "ownDeviceFormat", "the format a device of Phazite's own names in the device lists, never saved" },
        { "fadeIsCurve", "the shape of a fade without a shape of its own, which no clip writes" },
        { "DEVICE@formatID", "a VST3's class ID, as its preset file holds it: the tests' plug-ins are of a format of their own" },
        { "MASTER@vca", "the master has a fader, never a VCA: the rules name every channel's" },
        { "MASTER@height", "the master's row is collapsed or not, and nothing else: the rules name every track's view settings for it" },
        { "MASTER@pinned", "as MASTER@height" },
        { "MASTER@drumMode", "as MASTER@height" },
        { "MASTER@drumOrder", "as MASTER@height" },
        { "MASTER@drumHidden", "as MASTER@height" },
    };
    return names;
}

// Checks that the representative project holds each name asked of it, unless it is among those it needn't hold; and, at
// the end, that each of those is still asked for and still not there.
struct Coverage
{
    juce::StringArray asked, listedAndHeld;

    void check (bool held, const juce::String& name, const juce::String& what)
    {
        asked.addIfNotAlreadyThere (name);

        if (const auto& exceptions = getNamesNotInTheProject(); exceptions.find (name) != exceptions.end())
        {
            if (held)
                listedAndHeld.addIfNotAlreadyThere (name);

            return;
        }

        CHECK_MESSAGE (held, ("The representative project holds no " + what
                              + ": add one in addRepresentativeContent() (tests/GoldenProjectTests.cpp), or say in "
                                "getNamesNotInTheProject() why it can't hold one (\""
                              + name + "\")")
                                 .toStdString());
    }

    void checkExceptions() const
    {
        for (const auto& [name, why] : getNamesNotInTheProject())
        {
            CHECK_MESSAGE (why.isNotEmpty(), (name + " is listed as not in the representative project, without why").toStdString());
            CHECK_MESSAGE (
                asked.contains (name),
                (name + " is listed as not in the representative project, but nothing declares or names it any more").toStdString());
            CHECK_MESSAGE (! listedAndHeld.contains (name),
                           (name + " is listed as not in the representative project, but the project holds it").toStdString());
        }
    }
};

// A list of the song's rules (schema::describeSongRules()), which must be one.
juce::Array<juce::var> getRuleList (const juce::var& rules, const juce::String& name)
{
    const auto* list = rules[juce::Identifier (name)].getArray();
    REQUIRE_MESSAGE (list != nullptr, ("The song's rules have no list of " + name).toStdString());
    return *list;
}

// The representative project holds every name of a saved song: each one Schema.h declares, on each element that holds it,
// and each one the song's rules name (schema::describeSongRules(), the test vector tests/golden/format/song-rules.json).
void checkHoldsEverySavedName (const juce::XmlElement& project)
{
    Held held;
    held.add (project);
    Coverage coverage;

    // Schema.h's names.
    const auto names = readSchemaNames();
    REQUIRE (names.size() > 200); // the header was read as it is written

    for (const auto& name : names)
    {
        if (name.isValue)
        {
            coverage.check (held.values.contains (name.saved), name.declared,
                            "value \"" + name.saved + "\" (schema::" + name.declared + ")");
        }
        else if (name.declared == name.declared.toUpperCase())
        {
            coverage.check (held.types.contains (name.saved), name.saved, name.saved + " element");
        }
        else
        {
            const auto holders = getHolders (name.declared);
            CHECK_MESSAGE (! holders.isEmpty(), ("Schema.h declares " + name.declared
                                                 + ": say in getHolders() (tests/GoldenProjectTests.cpp) which elements hold it")
                                                    .toStdString());

            for (const auto& holder : holders)
                coverage.check (held.hasProperty (holder, name.saved), holder + "@" + name.saved,
                                "\"" + name.saved + "\" on " + holder + " (schema::" + name.declared + ")");
        }
    }

    // The names Schema.h makes rather than declares: what a device's SOUNDS hold of a pad's layers and an oscillator.
    namespace schema = phazite::schema;

    for (const auto* sound : { &schema::getPadSound (36), &schema::getPadSoundFile (38), &schema::getPadSoundFile (38, 1),
                               &schema::getOscillatorWavetable (0), &schema::getOscillatorWavetableFile (1) })
        coverage.check (held.hasProperty ("SOUNDS", sound->toString()), "SOUNDS@" + sound->toString(),
                        "\"" + sound->toString() + "\" on SOUNDS");

    // The song's rules: every element type and property they name.
    const auto rules = schema::describeSongRules();
    const auto checkType = [&coverage, &held] (const juce::var& type)
    { coverage.check (held.types.contains (type.toString()), type.toString(), type.toString() + " element"); };
    const auto checkProperty = [&coverage, &held] (const juce::var& type, const juce::var& property)
    {
        coverage.check (held.hasProperty (type.toString(), property.toString()), type.toString() + "@" + property.toString(),
                        "\"" + property.toString() + "\" on " + type.toString() + " (the song's rules name it)");
    };

    for (const auto& type : getRuleList (rules["items"], "types"))
        checkProperty (type, rules["items"]["idProperty"]);

    checkProperty (rules["items"]["alsoHoldingAnID"], rules["items"]["idProperty"]);

    for (const auto& key : getRuleList (rules, "keys"))
        for (const auto& property : getRuleList (key, "properties"))
            checkProperty (key["type"], property);

    for (const auto* list : { "unkeyed", "piecesOfTheirOwn", "inRuns" })
        for (const auto& type : getRuleList (rules, list))
            checkType (type);

    for (const auto* list : { "otherIDHolders", "idNamers", "bytes" })
        for (const auto& row : getRuleList (rules, list))
            checkProperty (row["type"], row["property"]);

    REQUIRE_MESSAGE (rules["orders"].getDynamicObject() != nullptr, "The song's rules have no orders");
    REQUIRE_MESSAGE (rules["uncountedProperties"].getDynamicObject() != nullptr, "The song's rules have no uncountedProperties");

    for (const auto& order : rules["orders"].getDynamicObject()->getProperties())
        checkType (order.name.toString());

    for (const auto& uncounted : rules["uncountedProperties"].getDynamicObject()->getProperties())
        for (const auto& property : getRuleList (rules["uncountedProperties"], uncounted.name.toString()))
            checkProperty (uncounted.name.toString(), property);

    for (const auto& row : getRuleList (rules, "uncountedElements"))
        coverage.check (held.children.contains (row["parent"].toString() + "/" + row["type"].toString()),
                        row["parent"].toString() + "/" + row["type"].toString(),
                        row["type"].toString() + " element in " + row["parent"].toString() + " (the song's rules name it)");

    coverage.checkExceptions();
}
} // namespace

//==============================================================================
TEST_SUITE ("golden")
{
    TEST_CASE ("Golden files: the project normaliser removes what changes from run to run, and nothing else")
    {
        // A save with all that the normaliser changes: a clip and its alias (one with a Windows path), float noise, -0,
        // unsorted attributes and children, and empty elements, one of them Phazite's.
        auto saved = juce::parseXML (
            R"(<PHAZITE outline="pop" nextID="1012">
  <TRACKS>
    <TRACK name="Lead" id="1004">
      <CLIP source="Audio\Tone.wav" name="Tone" id="1010" linkID="{6f0c51e2-3b4a-4d7e-9c1f-2a8b7d6e5f40}" length="0.6000000238418579"/>
      <SCRATCH/>
    </TRACK>
    <TRACK name="Bass" id="1003" pan="-0.0">
      <CLIP name="Tone" id="1011" linkID="{6f0c51e2-3b4a-4d7e-9c1f-2a8b7d6e5f40}" length="1.5"/>
    </TRACK>
  </TRACKS>
  <PIANOROLL/>
</PHAZITE>)");
        REQUIRE (saved != nullptr);

        // The alias's audio, at an absolute path in the project's folder.
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Normalised Song");
        auto* savedTracks = saved->getChildByName ("TRACKS");
        REQUIRE (savedTracks != nullptr);
        auto* savedBass = savedTracks->getChildByAttribute ("id", "1003");
        REQUIRE (savedBass != nullptr);
        auto* savedAlias = savedBass->getChildByName ("CLIP");
        REQUIRE (savedAlias != nullptr);
        savedAlias->setAttribute ("source", folder.getChildFile ("Audio").getChildFile ("Tone.wav").getFullPathName());

        const auto normalised = phazite::test::normaliseProject (*saved, folder, { "SCRATCH" });
        REQUIRE (normalised != nullptr);

        // The root's attributes are sorted, and its children by their tags; the tracks keep their order.
        CHECK (getAttributeNames (*normalised) == juce::String ("nextID outline"));
        CHECK (normalised->getStringAttribute ("nextID") == juce::String ("1012"));
        CHECK (getChildTags (*normalised) == juce::String ("PIANOROLL TRACKS"));
        auto* tracks = normalised->getChildByName ("TRACKS");
        REQUIRE (tracks != nullptr);
        CHECK (getChildTags (*tracks) == juce::String ("TRACK TRACK"));
        auto* lead = tracks->getChildElement (0);
        auto* bass = tracks->getChildElement (1);
        REQUIRE (lead != nullptr);
        REQUIRE (bass != nullptr);
        CHECK (lead->getStringAttribute ("id") == juce::String ("1004"));
        CHECK (bass->getStringAttribute ("id") == juce::String ("1003"));
        CHECK (bass->getStringAttribute ("pan") == juce::String ("0"));

        // The clip and its alias share link-1; paths have '/' and no project folder; the float noise is gone, and so is
        // the empty SCRATCH.
        CHECK (getChildTags (*lead) == juce::String ("CLIP"));
        auto* clip = lead->getChildByName ("CLIP");
        auto* alias = bass->getChildByName ("CLIP");
        REQUIRE (clip != nullptr);
        REQUIRE (alias != nullptr);
        CHECK (getAttributeNames (*clip) == juce::String ("id length linkID name source"));
        CHECK (clip->getStringAttribute ("linkID") == juce::String ("link-1"));
        CHECK (alias->getStringAttribute ("linkID") == juce::String ("link-1"));
        CHECK (clip->getStringAttribute ("source") == juce::String ("Audio/Tone.wav"));
        CHECK (alias->getStringAttribute ("source") == juce::String ("$PROJECT/Audio/Tone.wav"));
        CHECK (clip->getStringAttribute ("length") == juce::String ("0.6"));
        CHECK (alias->getStringAttribute ("length") == juce::String ("1.5"));

        // Phazite's empty PIANOROLL stays: if it vanished, a read that adds an empty one would go unnoticed.
        auto* pianoRoll = normalised->getChildByName ("PIANOROLL");
        REQUIRE (pianoRoll != nullptr);
        CHECK (pianoRoll->getNumAttributes() == 0);

        for (const auto* tag : { "PHAZITE", "PIANOROLL", "LASTKEY" })
            CHECK_FALSE (phazite::test::getDroppableEmptyTags().contains (tag));

        // The comparison: float noise beyond 7 digits isn't a difference; a changed value is one, with its path; an
        // attribute only the actual one has is one unless it may add some.
        const auto expected = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="64" volume="0.6376281"/></PHAZITE>)");
        const auto noisy = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="64" volume="0.6376282"/></PHAZITE>)");
        const auto taller = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="96" volume="0.6376281"/></PHAZITE>)");
        const auto added = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="64" volume="0.6376281" mute="1"/></PHAZITE>)");
        REQUIRE (expected != nullptr);
        REQUIRE (noisy != nullptr);
        REQUIRE (taller != nullptr);
        REQUIRE (added != nullptr);

        CHECK (phazite::test::findDifferences (*expected, *noisy).isEmpty());

        const auto heightChange = phazite::test::findDifferences (*expected, *taller);
        REQUIRE (heightChange.size() == 1);
        CHECK (heightChange[0].startsWith ("PHAZITE/TRACK#1003@height: "));

        CHECK (phazite::test::findDifferences (*expected, *added).size() == 1);
        CHECK (phazite::test::findDifferences (*expected, *added, { .actualMayAdd = true }).isEmpty());
    }

    TEST_CASE ("Golden files: a name counts in a save only as a whole word in a value, ignoring case")
    {
        const auto save = juce::parseXML (R"(<PHAZITE nextID="1011" savedTime="1791863965976" savedBy="Tim Smith">
  <TEMPOMAP>
    <TIMESIGNATURE numerator="4" denominator="4" beat="0"/>
  </TEMPOMAP>
  <TRACKS>
    <TRACK id="1003" name="Timeline">
      <CLIP id="1010" name="Tim's take" file="C:\Users\tim\Music\Tone.wav"/>
      <NOTES>written by TIM</NOTES>
    </TRACK>
    <TRACK id="1004" name="Jos&#233;"/>
    <TRACK id="1005" name="/home/dev/Sounds"/>
  </TRACKS>
</PHAZITE>)");
        REQUIRE (save != nullptr);

        // In values, at every depth, as a whole word whatever its case; not in tags or attribute names (TIMESIGNATURE,
        // savedTime), nor inside a longer word (Timeline).
        const auto tim = phazite::test::findValuesNaming (*save, { "tim" });
        REQUIRE (tim.size() == 4);
        CHECK (tim[0] == juce::String (R"(PHAZITE@savedBy: "Tim Smith")"));
        CHECK (tim[1] == juce::String (R"(PHAZITE/TRACKS/TRACK#1003/CLIP#1010@name: "Tim's take")"));
        CHECK (tim[2] == juce::String (R"(PHAZITE/TRACKS/TRACK#1003/CLIP#1010@file: "C:\Users\tim\Music\Tone.wav")"));
        CHECK (tim[3] == juce::String (R"(PHAZITE/TRACKS/TRACK#1003/NOTES/#text: "written by TIM")"));

        CHECK (phazite::test::findValuesNaming (*save, { "Tim Smith" }).joinIntoString ("\n")
               == juce::String (R"(PHAZITE@savedBy: "Tim Smith")"));
        CHECK (phazite::test::findValuesNaming (*save, { "dev" }).joinIntoString ("\n")
               == juce::String (R"(PHAZITE/TRACKS/TRACK#1005@name: "/home/dev/Sounds")"));

        // A letter written as a character reference counts as itself.
        const auto jose = juce::String (juce::CharPointer_UTF8 ("Jos\xc3\xa9"));
        CHECK (phazite::test::findValuesNaming (*save, juce::StringArray { jose }).joinIntoString ("\n")
               == "PHAZITE/TRACKS/TRACK#1004@name: \"" + jose + "\"");

        CHECK (phazite::test::findValuesNaming (*save, { "TIMESIGNATURE", "savedTime", "devices", "line", "Smit" }).isEmpty());
    }

    TEST_CASE ("Golden project: a new, empty project")
    {
        phazite::test::TemporaryFolder temp;
        auto project = createProject (temp.folder, "Empty");
        phazite::test::settle();

        saveReopenAndSaveAgain (std::move (project), "empty",
                                "A new, empty project (Project::create), saved, reopened and saved again: tests/GoldenProjectTests.cpp");
    }

    TEST_CASE ("Golden project: one of each kind of track and every piece of Phazite data")
    {
        // The stand-ins that the project's plug-ins are, known before its song has one.
        const phazite::test::TestPlugin testPlugin;
        const phazite::test::SandboxedPlugins sandboxedPlugins;
        phazite::test::TemporaryFolder temp;
        auto project = createProject (temp.folder, "Representative");
        phazite::ProjectContext context { phazite::test::getEngineContext() }; // the project open, as the app has it
        setSong (context, &project->getSong());
        addRepresentativeContent (*project, context, temp.folder.getChildFile ("Sounds"));
        phazite::test::settle();

        // The autosave, and a copy saved as another project, are compared with the project's file below.
        REQUIRE (project->autosave().wasOk());
        const auto autosave = phazite::test::normaliseProject (
            *phazite::test::readXml (phazite::Project::getAutosaveFile (project->getFile())), project->getFolder());

        juce::String error;
        const auto copyFile = project->saveCopyAs (temp.folder, "Representative Copy", error);
        REQUIRE_MESSAGE (copyFile.existsAsFile(), error.toStdString());
        const auto copy = phazite::test::readNormalisedProject (copyFile);

        const auto saves = saveReopenAndSaveAgain (std::move (project), "representative",
                                                   "A project with one of each kind of track and every piece of Phazite's data, saved, "
                                                   "reopened and saved again: addRepresentativeContent in tests/GoldenProjectTests.cpp",
                                                   &context);

        // Nothing in it names the person who made it.
        phazite::test::checkNoUserName ("The first save", *saves.rawFirst);

        phazite::test::checkSameProject ("in the autosave", *saves.first, *autosave);
        phazite::test::checkSameProject ("saved as a copy", *saves.first, *copy);

        // It holds every name a saved song can have.
        checkHoldsEverySavedName (*saves.rawSecond);
    }

    TEST_CASE ("Golden project: the check of every saved name reads Schema.h's names, and knows which elements hold each")
    {
        const auto names = readSchemaNames();
        const auto find = [&names] (const juce::String& declared) -> const SchemaName*
        {
            for (const auto& name : names)
                if (name.declared == declared)
                    return &name;

            return nullptr;
        };

        // An element's type, a property, a value, and one of several declared on a line; none of the versions' store's or
        // the presets' names.
        REQUIRE (find ("TRACK") != nullptr);
        CHECK (find ("TRACK")->saved == juce::String ("TRACK"));
        REQUIRE (find ("trackHeight") != nullptr);
        CHECK (find ("trackHeight")->saved == juce::String ("height"));
        CHECK_FALSE (find ("trackHeight")->isValue);
        REQUIRE (find ("trackIsFolder") != nullptr);
        CHECK (find ("trackIsFolder")->saved == juce::String ("folder"));
        CHECK (find ("trackIsFolder")->isValue);
        REQUIRE (find ("samplerSlicesRemoved") != nullptr);
        CHECK (find ("pieceHash") == nullptr);
        CHECK (find ("VERSION") == nullptr);
        CHECK (find ("modulates") == nullptr);
        CHECK (find ("deviceKind") == nullptr);

        // Every property has its elements.
        for (const auto& name : names)
            if (! name.isValue && name.declared != name.declared.toUpperCase())
                CHECK_MESSAGE (! getHolders (name.declared).isEmpty(), name.declared.toStdString());

        CHECK (getHolders ("trackHeight") == juce::StringArray { "TRACK" });
        CHECK (getHolders ("channelVolume") == juce::StringArray { "TRACK", "MASTER" });
        CHECK (getHolders ("loop") == juce::StringArray { "PHAZITE" });
        CHECK (getHolders ("somethingNew").isEmpty());

        // What a project holds, as the check reads it.
        const auto project =
            juce::parseXML (R"(<PHAZITE nextID="9"><TRACKS><TRACK id="4" kind="track"><CLIP id="7"/></TRACK></TRACKS></PHAZITE>)");
        REQUIRE (project != nullptr);
        Held held;
        held.add (*project);
        CHECK (held.types == juce::StringArray { "PHAZITE", "TRACKS", "TRACK", "CLIP" });
        CHECK (held.children.contains ("TRACK/CLIP"));
        CHECK (held.hasProperty ("TRACK", "kind"));
        CHECK_FALSE (held.hasProperty ("CLIP", "kind"));
        CHECK (held.values.contains ("track"));
    }
}

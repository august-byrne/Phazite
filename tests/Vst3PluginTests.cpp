// SPDX-License-Identifier: AGPL-3.0-or-later
// Real VST3s, hosted as a user's are: the tests' own effect and instrument (vst3/TestVst3s.h), built by JUCE's plug-in
// client, found by the plug-in scan in a helper, made by a song's session in the tests' helper and played by Phazite's
// engine through JUCE's VST3 host. What the stand-ins of host/TestPlugins.h can't show: how the scan describes a VST3
// (instrument or effect, its buses), its parameters by the VST3's own IDs, its state as its format's preset file
// (.vstpreset), its programs, its mono buses, how JUCE's host hands it notes and controllers, and its latency.
#include "DspProbe.h"
#include "EnginePlayback.h"
#include "TestSong.h"
#include "engine/nodes/DeviceRunner.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/PluginScanner.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "sandbox/core/Vst3State.h"
#include "vst3/TestVst3s.h"

#include <cmath>
#include <optional>

using phazite::test::EnginePlayback;
using phazite::test::TestSong;
namespace vst3 = phazite::test::vst3;

namespace
{
constexpr int scanSeconds = 60;
constexpr int stateMilliseconds = 10000;
constexpr float toneLevel = -18.05f; // a quarter of full scale's sine, in RMS through the master's -3 dB

phazite::PluginHost& getHost()
{
    return phazite::getPluginHost (phazite::test::getEngineContext());
}

// What the plug-in scan finds in a file, in a helper, as the app's scan finds it.
std::vector<juce::PluginDescription> scan (const juce::String& file)
{
    auto scanner = phazite::createPluginScanner (getHost(), scanSeconds);
    juce::VST3PluginFormat format; // its name, which the helper scans with
    juce::OwnedArray<juce::PluginDescription> found;
    CHECK (scanner->findPluginTypesFor (format, found, file));
    scanner->scanFinished();

    std::vector<juce::PluginDescription> descriptions;

    for (const auto* description : found)
        descriptions.push_back (*description);

    return descriptions;
}

// The one plug-in a file holds, scanned once for all the tests.
const juce::PluginDescription& getEffect()
{
    static const auto found = scan (PHAZITE_TEST_VST3_EFFECT);
    REQUIRE (found.size() == 1);
    return found.front();
}

const juce::PluginDescription& getSynth()
{
    static const auto found = scan (PHAZITE_TEST_VST3_SYNTH);
    REQUIRE (found.size() == 1);
    return found.front();
}

// While one lives, the plug-in host knows both, as after a scan: the device lists offer them, and a song makes them.
class Vst3Plugins
{
public:
    Vst3Plugins()
    {
        getHost().getKnownPlugins().addType (getEffect());
        getHost().getKnownPlugins().addType (getSynth());
    }

    ~Vst3Plugins()
    {
        getHost().getKnownPlugins().removeType (getEffect());
        getHost().getKnownPlugins().removeType (getSynth());
    }

    static const char* effectType() { return getEffect().fileOrIdentifier.toRawUTF8(); }
    static const char* synthType() { return getSynth().fileOrIdentifier.toRawUTF8(); }

private:
    JUCE_DECLARE_NON_COPYABLE (Vst3Plugins)
};

phazite::Song& getSong (TestSong& song)
{
    auto* found = phazite::findSong (song.context);
    REQUIRE (found != nullptr);
    return *found;
}

phazite::SandboxedInstance& getSandboxed (TestSong& song, phazite::DeviceID device)
{
    auto* sandboxed = phazite::findPluginInstance (getSong (song), device);
    REQUIRE (sandboxed != nullptr);
    return *sandboxed;
}

float levelOf (const juce::AudioBuffer<float>& audio, int side, double from, double to)
{
    const auto start = (int) (from * EnginePlayback::sampleRate);
    return juce::Decibels::gainToDecibels (audio.getRMSLevel (side, start, (int) (to * EnginePlayback::sampleRate) - start));
}

// The effect's parameters' IDs as its VST3 has them: numbers JUCE's plug-in client makes of the IDs it was written with.
juce::String getVst3ID (const char* juceID)
{
    return juce::String (juce::VST3ClientExtensions::convertJuceParameterId (juceID, true));
}

// A track that plays a sound file from the song's start: a 440 Hz sine at a quarter of full scale, or noise.
phazite::TrackID addToneTrack (TestSong& song, phazite::TrackID track, double seconds)
{
    song.addAudioClip (track, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), seconds, 440.0, 0.25f), 0.0);
    return track;
}

juce::File writeNoise (const juce::File& file, double seconds)
{
    juce::AudioBuffer<float> noise (1, (int) (seconds * EnginePlayback::sampleRate));
    juce::Random random (7);

    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample (0, i, 0.25f * (random.nextFloat() * 2.0f - 1.0f));

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (EnginePlayback::sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (noise, 0, noise.getNumSamples());
    return file;
}

// The effect's trim, the setting of its own that its programs set and its state keeps (vst3/TestVst3s.h): read from its
// state's processor stream, and set as its own window would, by a state with another trim.
float readTrim (const juce::MemoryBlock& preset)
{
    const auto state = phazite::sandbox::readVst3Preset (preset);
    REQUIRE (state.has_value());
    REQUIRE (state->component.getSize() >= 12);
    juce::MemoryInputStream in (state->component, false);
    in.skipNextBytes (8);
    return in.readFloat();
}

void setTrim (phazite::SandboxedInstance& instance, float trim)
{
    const auto preset = instance.getRemote().getState (stateMilliseconds);
    REQUIRE (preset.has_value());
    auto state = phazite::sandbox::readVst3Preset (*preset);
    REQUIRE (state.has_value());
    REQUIRE (state->component.getSize() >= 12);
    juce::MemoryOutputStream out;
    out.writeFloat (trim);
    state->component.copyFrom (out.getData(), 8, out.getDataSize());
    REQUIRE (instance.getRemote().setState (phazite::sandbox::writeVst3Preset (*state), stateMilliseconds));
}
} // namespace

TEST_CASE ("The plug-in scan finds the tests' VST3s in a helper and describes them: an effect in mono, an instrument with an input")
{
    const auto effects = scan (PHAZITE_TEST_VST3_EFFECT);
    REQUIRE (effects.size() == 1);
    const auto& effect = effects.front();
    CHECK (effect.name == vst3::effect::name);
    CHECK (effect.pluginFormatName == "VST3");
    CHECK (effect.manufacturerName == vst3::manufacturer);
    CHECK_FALSE (effect.isInstrument);
    CHECK (effect.numInputChannels == 1);
    CHECK (effect.numOutputChannels == 1);

    const auto synths = scan (PHAZITE_TEST_VST3_SYNTH);
    REQUIRE (synths.size() == 1);
    const auto& synth = synths.front();
    CHECK (synth.name == vst3::synth::name);
    CHECK (synth.isInstrument);
    CHECK (synth.numInputChannels == 2);
    CHECK (synth.numOutputChannels == 2);

    // Made in a song's session, each says the same of itself.
    Vst3Plugins plugins;
    TestSong song;
    const auto madeEffect = song.addEffect (song.firstTrack, Vst3Plugins::effectType());
    const auto& effectInfo = getSandboxed (song, madeEffect).getRemote().getInfo();
    CHECK_FALSE (effectInfo.isInstrument);
    CHECK (effectInfo.numInputChannels == 1);
    CHECK (effectInfo.numOutputChannels == 1);
    CHECK (effectInfo.latencySamples == vst3::effect::latencySamples);
    CHECK (effectInfo.programs == std::vector<juce::String> { "Unity", "Half" });

    const auto madeSynth = song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    const auto& synthInfo = getSandboxed (song, madeSynth).getRemote().getInfo();
    CHECK (synthInfo.isInstrument);
    CHECK (synthInfo.numInputChannels == 2);
    CHECK (synthInfo.numOutputChannels == 2);
}

TEST_CASE ("A VST3 instrument with an input bus plays its notes at their pitch and velocity, and Stop ends them")
{
    // The audit of 2026-11-13: a plug-in with an input was played as an effect and got no notes; and Stop sent nothing but
    // controller 123, which JUCE's VST3 host hands only to a plug-in that maps it (this one takes none it is handed).
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 32.0);
    song.addNote (clip, 69, 0.0, 2.0, 127); // A4 for a second, as loud as it goes
    song.addNote (clip, 81, 2.0, 2.0, 64);  // A5 for a second, at about half
    song.addNote (clip, 69, 4.0, 28.0, 127);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    const auto audio = playback.play (2.5);

    // The first second: 440 Hz, at a quarter of full scale through the master, on both sides; nothing at 880 Hz.
    namespace probe = phazite::test::probe;
    const auto toneAt = [&audio] (int side, double hertz, double from, double to)
    {
        constexpr auto rate = EnginePlayback::sampleRate;
        return probe::getToneLevel (audio, side, hertz, rate, (int) (from * rate), (int) ((to - from) * rate));
    };

    for (int side = 0; side < 2; ++side)
    {
        CHECK (levelOf (audio, side, 0.1, 0.9) == doctest::Approx (toneLevel).epsilon (0.002));
        CHECK (toneAt (side, 880.0, 0.1, 0.9) < 1.0e-3);
    }

    // The second: 880 Hz, 64/127 as loud; nothing at 440 Hz.
    CHECK (probe::inDecibels (toneAt (0, 880.0, 1.1, 1.9) / toneAt (0, 440.0, 0.1, 0.9))
           == doctest::Approx (juce::Decibels::gainToDecibels (64.0 / 127.0)).epsilon (0.002));
    CHECK (toneAt (0, 440.0, 1.1, 1.9) < 1.0e-3);

    // Stopped while the third note sounds: it ends with the first block after.
    CHECK (levelOf (audio, 0, 2.1, 2.4) == doctest::Approx (toneLevel).epsilon (0.002));
    song.run ("transport.playStop");
    const auto stopped = playback.play (0.4);
    CHECK (stopped.getMagnitude (EnginePlayback::blockSize, stopped.getNumSamples() - EnginePlayback::blockSize) < 1.0e-6f);
}

TEST_CASE ("A VST3 effect's parameters are found by the VST3's own IDs, set by hand and moved by automation")
{
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, 8.0);
    const auto effect = song.addEffect (track, Vst3Plugins::effectType());
    auto& instance = getSandboxed (song, effect);

    // Its Gain and its Invert, by the numbers its VST3 gives them, which aren't their places.
    const auto gainID = getVst3ID (vst3::effect::gainID), invertID = getVst3ID (vst3::effect::invertID);
    REQUIRE (instance.findParameter (gainID) >= 0);
    REQUIRE (instance.findParameter (invertID) >= 0);
    CHECK (gainID != juce::String (instance.findParameter (gainID)));
    const auto& gainInfo = instance.getParameterInfos()[(size_t) instance.findParameter (gainID)];
    CHECK (gainInfo.name == "Gain");
    CHECK (gainInfo.defaultValue == doctest::Approx (1.0f));
    CHECK_FALSE (gainInfo.isBoolean);

    // Its Invert, which JUCE's VST3 host tells as a parameter of two steps (a VST3's switch has one step): a switch, with
    // the plug-in's own texts for its two values.
    const auto invert = instance.findParameter (invertID);
    const auto& invertInfo = instance.getParameterInfos()[(size_t) invert];
    CHECK (invertInfo.numSteps == 2);
    CHECK (invertInfo.isBoolean);
    CHECK (instance.getParameters()[invert]->isBoolean());
    // (Asked of its helper without waiting, known once it has answered.)
    for (const auto& [value, text] : { std::pair (0.0f, "Off"), std::pair (1.0f, "On") })
    {
        instance.describeValue (invert, value);
        CHECK (phazite::test::dispatchMessagesUntil ([&, value = value, text = text]
                                                     { return instance.describeValue (invert, value) == text; }));
    }

    // Set by hand to half, and automated from 1 down to 0 over two seconds (four beats at 120): on its curve as it plays.
    const phazite::ParameterRef gain { effect, gainID };
    song.setParameter (effect, gainID, 0.5f);
    song.addAutomation (gain, { { phazite::BeatPosition(), 1.0f }, { phazite::BeatPosition::fromBeats (4.0), 0.0f } });
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    auto audio = playback.play (1.8);

    for (const auto& [second, value] : { std::pair (0.5, 0.75f), std::pair (1.0, 0.5f), std::pair (1.5, 0.25f) })
        CHECK (levelOf (audio, 0, second - 0.05, second + 0.05)
               == doctest::Approx (toneLevel + juce::Decibels::gainToDecibels (value)).epsilon (0.005));

    // Without its automation, back at its value set by hand: 6 dB quieter.
    phazite::deleteAutomationPoints (song.context, gain, { 0, 1 });
    REQUIRE (phazite::getAutomationPoints (song.context, gain).empty());
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));
}

TEST_CASE ("A VST3 effect in mono plays on both sides of its track, each side as the other")
{
    // The audit of 2026-11-13: a plug-in that takes one channel was given the left alone, and the right came out as it
    // went in.
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, 4.0);
    const auto effect = song.addEffect (track, Vst3Plugins::effectType());
    song.setParameter (effect, getVst3ID (vst3::effect::gainID), 0.5f);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    const auto audio = playback.play (0.6);

    for (int side = 0; side < 2; ++side)
        CHECK (levelOf (audio, side, 0.2, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));

    auto largestDifference = 0.0f;

    for (int i = 0; i < audio.getNumSamples(); ++i)
        largestDifference = std::max (largestDifference, std::abs (audio.getSample (0, i) - audio.getSample (1, i)));

    CHECK (largestDifference < 1.0e-6f);
}

TEST_CASE ("A VST3 effect's latency is compensated: its track lines up with one without it, to the sample")
{
    // Two tracks of the same noise, one through the effect turned upside down (Invert on): lined up, they cancel.
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto noise = writeNoise (song.getSoundsFolder().getChildFile ("Noise.wav"), 3.0);
    song.addAudioClip (song.firstTrack, noise, 0.0);
    const auto other = song.addTrack ("Dry");
    song.addAudioClip (other, noise, 0.0);
    const auto effect = song.addEffect (song.firstTrack, Vst3Plugins::effectType());
    CHECK (getSandboxed (song, effect).getLatencySamples() == vst3::effect::latencySamples);

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    EnginePlayback::waitForFiles();
    song.run ("transport.playStop");
    auto audio = playback.play (1.0);
    const auto from = (int) (0.1 * EnginePlayback::sampleRate), length = (int) (0.8 * EnginePlayback::sampleRate);
    CHECK (audio.getMagnitude (from, length) > 0.2f); // the two together, twice as loud as one

    song.setParameter (effect, getVst3ID (vst3::effect::invertID), 1.0f);
    audio = playback.play (1.0);
    CHECK (audio.getMagnitude (from, length) < 1.0e-4f);
}

TEST_CASE ("A VST3 effect's state goes into the song as a .vstpreset and back, its program set before it, on opening and reloading")
{
    // The audit of 2026-11-13: a plug-in opened from its project, or reloaded, was given its state, then its program, which
    // set what the state had set again as the program has it (a VST3's state needn't say which program it is on).
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, 30.0);
    const auto effect = song.addEffect (track, Vst3Plugins::effectType());
    auto playback = std::make_unique<EnginePlayback> (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    const auto levelNow = [&playback] { return levelOf (playback->play (0.6), 0, 0.2, 0.5); };
    const auto half = toneLevel - 6.02f, quarter = toneLevel - 12.04f;

    // Its program Half, which trims it to half.
    auto* instance = &getSandboxed (song, effect);
    REQUIRE (instance->getNumPrograms() == 2);
    CHECK (instance->getProgramName (1) == "Half");
    instance->setCurrentProgram (1);
    CHECK (instance->getCurrentProgram() == 1);
    CHECK (levelNow() == doctest::Approx (half).epsilon (0.002));

    // Its trim turned to a quarter in its own window, its program still Half.
    setTrim (*instance, 0.25f);
    CHECK (levelNow() == doctest::Approx (quarter).epsilon (0.002));

    // Saved: the song holds its state as a .vstpreset of its class, with its trim, and its program.
    song.saveWithPluginStates();
    const auto getStored = [&song, effect]
    {
        const auto bytes = phazite::readBytes (phazite::findDeviceState (getSong (song), effect)[phazite::schema::pluginState].toString());
        REQUIRE (bytes.has_value());
        return *bytes;
    };

    const auto stored = getStored();
    const auto preset = phazite::sandbox::readVst3Preset (stored);
    REQUIRE (preset.has_value());
    CHECK (preset->classID.length() == 32);
    CHECK (preset->classID == phazite::sandbox::readVst3ClassID (stored));
    CHECK (readTrim (stored) == doctest::Approx (0.25f));
    CHECK (phazite::findDeviceState (getSong (song), effect)[phazite::schema::pluginProgram] == juce::var (1));

    // Opened again, in a new helper: on its program, with its settings.
    playback = nullptr;
    song.reopen();
    instance = &getSandboxed (song, effect);
    CHECK (instance->getCurrentProgram() == 1);
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return getHost().getNumHelpers() == 1; }));
    playback = std::make_unique<EnginePlayback> (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelNow() == doctest::Approx (quarter).epsilon (0.002));

    // Its helper gone, as one that crashes, and the plug-in reloaded: as it was.
    instance->getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([instance] { return instance->hasCrashed(); }));
    // (In the background, waited for here.)
    auto reloaded = std::make_shared<std::optional<std::optional<juce::String>>>();
    phazite::reloadDevice (song.context, effect, [reloaded] (std::optional<juce::String> problem) { *reloaded = problem; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([reloaded] { return reloaded->has_value(); }));
    CHECK_FALSE (reloaded->value().has_value());
    REQUIRE_FALSE (instance->hasCrashed());
    CHECK (instance->getCurrentProgram() == 1);
    const auto reloadedState = instance->getRemote().getState (stateMilliseconds);
    REQUIRE (reloadedState.has_value());
    CHECK (readTrim (*reloadedState) == doctest::Approx (0.25f));
    CHECK (levelNow() == doctest::Approx (quarter).epsilon (0.002));
}

TEST_CASE (
    "A version opened with a real VST3 that Now lacks: it loads in the background as the version opens, and goes at Back to Now (times measured)")
{
    // docs/design/versions.md, A version open: what both have stays loaded; a plug-in only the version has loads in the
    // background, the song playing on, and is let go of at Back to Now. The times are said, not checked: they depend on the
    // machine.
    Vst3Plugins plugins;
    TestSong song;
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    const auto synth = song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    song.waitForPlugins();

    phazite::setVersionDraft (song.context, "With the synth", {});
    std::optional<juce::Result> saved;
    REQUIRE (phazite::saveVersion (song.context, [&saved] (juce::Result r) { saved = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved.has_value(); }));
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    const auto version = phazite::getLastVersion (song.context)->id;
    phazite::removeDevice (song.context, synth); // Now lacks it
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::findPluginInstance (getSong (song), synth) == nullptr; }));

    const auto loaded = [&song] { return phazite::getPluginsLoading (getSong (song)).isEmpty(); };
    const auto seconds = [] (double since) { return (juce::Time::getMillisecondCounterHiRes() - since) / 1000.0; };

    // The first open reads the version on the history's thread; the plug-in loads after.
    auto start = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (phazite::test::finished (song.context, [&] { return phazite::openVersion (song.context, version); }));
    const auto firstOpen = seconds (start);
    REQUIRE (
        phazite::test::dispatchMessagesUntil ([&] { return loaded() && phazite::findPluginInstance (getSong (song), synth) != nullptr; }));
    const auto firstLoaded = seconds (start);

    start = juce::Time::getMillisecondCounterHiRes();
    phazite::backToNow (song.context);
    const auto back = seconds (start);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return phazite::findPluginInstance (getSong (song), synth) == nullptr; }));

    // Again, as A/B does: its song kept, so the open is at once; the plug-in loads again.
    start = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (phazite::openVersion (song.context, version));
    const auto againOpen = seconds (start);
    REQUIRE (
        phazite::test::dispatchMessagesUntil ([&] { return loaded() && phazite::findPluginInstance (getSong (song), synth) != nullptr; }));
    const auto againLoaded = seconds (start);
    start = juce::Time::getMillisecondCounterHiRes();
    phazite::backToNow (song.context);
    const auto backAgain = seconds (start);

    MESSAGE ("Open with a VST3 Now lacks: first open " << firstOpen << " s (read), its plug-in loaded at " << firstLoaded << " s; back "
                                                       << back << " s; open again " << againOpen << " s, loaded at " << againLoaded
                                                       << " s; back " << backAgain << " s");
}

namespace
{
// The level of a tone in what was played, on the left, from one second to another, as an amplitude. A tone a semitone away
// shows in it at a few hundredths of its own level: a stretch of a few tenths of a second doesn't tell them apart further.
double toneIn (const juce::AudioBuffer<float>& audio, double hertz, double from, double to)
{
    constexpr auto rate = EnginePlayback::sampleRate;
    return phazite::test::probe::getToneLevel (audio, 0, hertz, rate, (int) (from * rate), (int) ((to - from) * rate));
}

// A note's pitch, bent by so many semitones, in hertz.
double hertzOf (int note, double semitones = 0.0)
{
    return 440.0 * std::pow (2.0, ((double) note - 69.0 + semitones) / 12.0);
}

// The amplitude of the synth's sine for a note at full velocity on channel 1, through the master's -3 dB; and on its second
// program, whose Level is lower.
const double fullTone = vst3::synth::fullLevel * juce::Decibels::decibelsToGain (-3.0);
const double secondProgramTone = fullTone * vst3::synth::programLevels[1];

// A message into live MIDI's track as a MIDI input's driver plays it there (AudioIO: the engine's event of it, by
// engine::toNoteEvent(), played live into the track; the computer keyboard plays notes alone).
void playFromMidiInput (phazite::Song& song, const juce::MidiMessage& message)
{
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    const auto event = phazite::engine::toNoteEvent (message, 0);
    REQUIRE (event.has_value());
    host.getEngine().playLive ((std::int64_t) phazite::findSession (song)->getLiveTrack().getRaw(), *event);
}
} // namespace

TEST_CASE ("A VST3 instrument bends for a pitch bend played live and one played from a clip, each channel's notes by its own bend")
{
    // The bend reaches a plug-in, live and from a clip, as the sine's pitch shows; and each MIDI channel's notes play at
    // their channel's level and bend by their channel's bend alone (vst3/TestVst3s.h).
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    auto& s = getSong (song);
    phazite::routeLiveMidiInput (s);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    playback.play (0.05);

    // Live: A4 on channel 1, then bent up a semitone, then a bend on channel 2, which leaves it as it is.
    playFromMidiInput (s, juce::MidiMessage::noteOn (1, 69, 1.0f));
    auto audio = playback.play (0.5);
    CHECK (toneIn (audio, hertzOf (69), 0.1, 0.5) == doctest::Approx (fullTone).epsilon (0.01));
    CHECK (toneIn (audio, hertzOf (69, 1.0), 0.1, 0.5) < 0.05 * fullTone);

    playFromMidiInput (s, juce::MidiMessage::pitchWheel (1, 12288)); // half way up: a semitone
    audio = playback.play (0.5);
    CHECK (toneIn (audio, hertzOf (69, 1.0), 0.1, 0.5) == doctest::Approx (fullTone).epsilon (0.01));
    CHECK (toneIn (audio, hertzOf (69), 0.1, 0.5) < 0.05 * fullTone);

    playFromMidiInput (s, juce::MidiMessage::pitchWheel (2, 0));
    audio = playback.play (0.5);
    CHECK (toneIn (audio, hertzOf (69, 1.0), 0.1, 0.5) == doctest::Approx (fullTone).epsilon (0.01));

    // A note on channel 4, live: a quarter as loud, unbent.
    playFromMidiInput (s, juce::MidiMessage::noteOn (4, 76, 1.0f));
    audio = playback.play (0.5);
    CHECK (toneIn (audio, hertzOf (76), 0.1, 0.5) == doctest::Approx (fullTone / 4.0).epsilon (0.01));

    playFromMidiInput (s, juce::MidiMessage::noteOff (1, 69));
    playFromMidiInput (s, juce::MidiMessage::noteOff (4, 76));
    playFromMidiInput (s, juce::MidiMessage::pitchWheel (1, 8192));
    playback.play (0.2);

    // From a clip: A4 on channel 1 and E5 on channel 4 from the start; channel 1 bent up a semitone at beat 2, channel 4
    // down a whole tone at beat 1. Playback starts with each channel's bend at its middle (chased: none before).
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 8.0);
    phazite::addNote (s, clip, 69, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (8.0), 127, 1);
    phazite::addNote (s, clip, 76, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (8.0), 127, 4);
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (2.0), phazite::ControlKind::pitchWheel, 0, 12288, 1);
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (1.0), phazite::ControlKind::pitchWheel, 0, 0, 4);
    song.run ("transport.playStop");
    audio = playback.play (1.8);
    CHECK (toneIn (audio, hertzOf (69), 0.05, 0.45) == doctest::Approx (fullTone).epsilon (0.01));
    CHECK (toneIn (audio, hertzOf (76), 0.05, 0.45) == doctest::Approx (fullTone / 4.0).epsilon (0.01));
    CHECK (toneIn (audio, hertzOf (76, -2.0), 0.55, 0.95) == doctest::Approx (fullTone / 4.0).epsilon (0.01)); // beat 1 on
    CHECK (toneIn (audio, hertzOf (69), 0.55, 0.95) == doctest::Approx (fullTone).epsilon (0.01));             // channel 1 as it was
    CHECK (toneIn (audio, hertzOf (69, 1.0), 1.05, 1.75) == doctest::Approx (fullTone).epsilon (0.01));        // beat 2 on
    CHECK (toneIn (audio, hertzOf (69), 1.05, 1.75) < 0.05 * fullTone);
    CHECK (toneIn (audio, hertzOf (76, -2.0), 1.05, 1.75) == doctest::Approx (fullTone / 4.0).epsilon (0.01));
    song.run ("transport.playStop");
}

TEST_CASE ("Stop ends a VST3 instrument's notes on every MIDI channel it was given one on, though it takes no all-notes-off")
{
    // A note held on channel 5 when the song stops: the helper ends the notes it gave the plug-in on each channel all notes
    // off comes on (host/core/Instance.h), and Phazite sends it on each channel the plug-in was given a note on.
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    auto& s = getSong (song);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 32.0);
    phazite::addNote (s, clip, 69, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (32.0), 127, 5);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (toneIn (playback.play (0.5), hertzOf (69), 0.1, 0.5) == doctest::Approx (fullTone / 5.0).epsilon (0.01));
    song.run ("transport.playStop");
    const auto stopped = playback.play (0.4);
    CHECK (stopped.getMagnitude (EnginePlayback::blockSize, stopped.getNumSamples() - EnginePlayback::blockSize) < 1.0e-6f);
}

TEST_CASE ("A program change sets a VST3's program as the song's own: played from a clip or live, chased only where it differs")
{
    // A VST3 reads a MIDI program change only as a legacy event, which few do (this one doesn't): its helper sets the plug-in's
    // program instead (host/core/Instance.h), as the song's own choice, as a program Phazite sets is. Playing it makes no
    // unsaved change: the song's program is as it was until the next save stores it. The synth's second program plays an
    // octave higher.
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto synth = song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    auto& s = getSong (song);
    auto& instance = getSandboxed (song, synth);
    REQUIRE (instance.getNumPrograms() == 2);
    REQUIRE (instance.getCurrentProgram() == 0);

    // A note through the clip; program 1 at beat 4.
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 32.0);
    phazite::addNote (s, clip, 69, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (32.0), 127, 1);
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (4.0), phazite::ControlKind::programChange, 0, 1, 1);
    song.saveWithPluginStates();
    REQUIRE_FALSE (song.getProject().hasUnsavedChanges());
    const auto storedProgram = phazite::findDeviceState (s, synth)[phazite::schema::pluginProgram];
    const auto stepsBefore = s.getUndoManager().getUndoDescriptions().size();

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    phazite::routeLiveMidiInput (s);
    const auto onProgram = [&instance] (int program)
    { return phazite::test::dispatchMessagesUntil ([&instance, program] { return instance.getCurrentProgram() == program; }); };

    // Played: the first program, then from beat 4 on, once the helper has set it, the second, an octave up.
    song.run ("transport.playStop");
    auto audio = playback.play (1.5);
    CHECK (toneIn (audio, hertzOf (69), 0.1, 1.4) == doctest::Approx (fullTone).epsilon (0.01));
    playback.play (1.0); // past beat 4
    REQUIRE (onProgram (1));
    audio = playback.play (0.5);
    CHECK (toneIn (audio, hertzOf (81), 0.1, 0.5) == doctest::Approx (secondProgramTone).epsilon (0.01));
    CHECK (toneIn (audio, hertzOf (69), 0.1, 0.5) < 0.01 * fullTone);

    // The song as it was: no unsaved change, no step to undo, its program the one saved, until a save stores the plug-in's.
    // The Level the program set is Phazite's too, as a control reads it, and no parameter touched.
    const phazite::ParameterRef level { synth, getVst3ID (vst3::synth::levelID) };
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return juce::exactlyEqual (phazite::getParameter (song.context, level).getValue(), vst3::synth::programLevels[1]); }));
    phazite::test::dispatchPendingMessages();
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
    CHECK (s.getUndoManager().getUndoDescriptions().size() == stepsBefore);
    CHECK (phazite::getLastTouchedParameter (song.context) != level);
    CHECK (phazite::findDeviceState (s, synth)[phazite::schema::pluginProgram] == storedProgram);

    // A jump back to before the program change: the second program plays on, nothing being chased there.
    phazite::movePlayhead (s, phazite::BeatPosition::fromBeats (1.0));
    audio = playback.play (0.5);
    CHECK (toneIn (audio, hertzOf (81), 0.1, 0.4) == doctest::Approx (secondProgramTone).epsilon (0.01));
    CHECK (instance.getCurrentProgram() == 1);

    // Live, on any channel: back to the first, as the song's too.
    playFromMidiInput (s, juce::MidiMessage::programChange (3, 0));
    playback.play (0.1);
    REQUIRE (onProgram (0));
    audio = playback.play (0.3);
    CHECK (toneIn (audio, hertzOf (69), 0.05, 0.3) == doctest::Approx (fullTone).epsilon (0.01));
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
    song.run ("transport.playStop");

    // Saved: the song stores the program the plug-in is on.
    playFromMidiInput (s, juce::MidiMessage::programChange (1, 1));
    playback.play (0.1);
    REQUIRE (onProgram (1));
    song.saveWithPluginStates();
    CHECK ((int) phazite::findDeviceState (s, synth)[phazite::schema::pluginProgram] == 1);
}

TEST_CASE ("A VST3's program is chased where playback jumps: the one before there, where it differs from the one sent last")
{
    // Program 0 at the clip's start and program 1 at beat 4: a jump back to between them plays the first again, and one from
    // before the second to after it plays the second.
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto synth = song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    auto& s = getSong (song);
    auto& instance = getSandboxed (song, synth);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 32.0);
    phazite::addNote (s, clip, 69, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (32.0), 127, 1);
    phazite::addControl (s, clip, phazite::BeatPosition(), phazite::ControlKind::programChange, 0, 0, 1);
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (4.0), phazite::ControlKind::programChange, 0, 1, 1);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    const auto onProgram = [&instance] (int program)
    { return phazite::test::dispatchMessagesUntil ([&instance, program] { return instance.getCurrentProgram() == program; }); };

    song.run ("transport.playStop");
    playback.play (2.5); // past beat 4
    REQUIRE (onProgram (1));

    // Back to beat 2: the first program, chased.
    phazite::movePlayhead (s, phazite::BeatPosition::fromBeats (2.0));
    playback.play (0.1);
    REQUIRE (onProgram (0));
    auto audio = playback.play (0.4);
    CHECK (toneIn (audio, hertzOf (69), 0.05, 0.4) == doctest::Approx (fullTone).epsilon (0.01));

    // From there to beat 6: the second, chased.
    phazite::movePlayhead (s, phazite::BeatPosition::fromBeats (6.0));
    playback.play (0.1);
    REQUIRE (onProgram (1));
    audio = playback.play (0.4);
    CHECK (toneIn (audio, hertzOf (81), 0.05, 0.4) == doctest::Approx (secondProgramTone).epsilon (0.01));
    song.run ("transport.playStop");
}

TEST_CASE ("In a render, a program change plays from the start of its block, the same in every render")
{
    // A render's blocks come as fast as they play, faster than the helper's message thread sets a program: a block that
    // holds a program change waits for it to be set. Program 1 at the song's start, under a note from there; program 0 in
    // the silence between that note and the next. Each note plays its program from its first sample, in both renders.
    Vst3Plugins plugins;
    TestSong song;
    song.setTempo (120.0);
    const auto synth = song.setInstrument (song.firstTrack, Vst3Plugins::synthType());
    auto& s = getSong (song);
    REQUIRE (getSandboxed (song, synth).getCurrentProgram() == 0);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 8.0);
    phazite::addControl (s, clip, phazite::BeatPosition(), phazite::ControlKind::programChange, 0, 1, 1);
    phazite::addNote (s, clip, 69, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (2.0), 127, 1); // 0 s to 1 s
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (3.0), phazite::ControlKind::programChange, 0, 0, 1);
    phazite::addNote (s, clip, 69, phazite::BeatPosition::fromBeats (4.0), phazite::BeatDuration::fromBeats (2.0), 127, 1); // 2 s to 3 s
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);

    const auto render = [&]
    {
        phazite::movePlayhead (s, phazite::BeatPosition());
        song.run ("transport.playStop");
        auto audio = playback.play (3.2);
        song.run ("transport.playStop");
        return audio;
    };

    const auto first = render();
    CHECK (toneIn (first, hertzOf (81), 0.05, 0.9) == doctest::Approx (secondProgramTone).epsilon (0.01));
    CHECK (toneIn (first, hertzOf (69), 0.0, 0.9) < 0.01 * fullTone);
    CHECK (toneIn (first, hertzOf (69), 2.05, 2.9) == doctest::Approx (fullTone).epsilon (0.01));
    CHECK (toneIn (first, hertzOf (81), 2.0, 2.9) < 0.01 * fullTone);

    // The second the same, but for the first render's first moment, where the track's path, new to the song, fades in.
    const auto second = render();
    auto largest = 0.0f;

    for (int side = 0; side < 2; ++side)
        for (int i = (int) (0.01 * EnginePlayback::sampleRate); i < first.getNumSamples(); ++i)
            largest = std::max (largest, std::abs (first.getSample (side, i) - second.getSample (side, i)));

    CHECK (largest < 1.0e-6f);
}

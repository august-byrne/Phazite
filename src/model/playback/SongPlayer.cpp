// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongPlayer.h"
#include "devices/drums/DrumSampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumSamplerPlugin.h"
#include "model/devices/internal/WavetablePlugin.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace phazite
{
namespace
{
    using EventType = devices::NoteEvent::Type;

    std::int64_t getID (const juce::ValueTree& state, const juce::Identifier& property)
    {
        return (juce::int64) state[property];
    }

    // What a track's MIDI clips play: each clip's notes and controller events where the clip is, cut to it (a note that
    // started before the clip's start plays from it, one that goes past its end stops there); none of a muted clip or note.
    std::shared_ptr<const engine::NoteSequence> compileNotes (const juce::ValueTree& trackState)
    {
        std::vector<engine::NoteSequence::Note> notes;
        std::vector<engine::NoteSequence::Control> controls;

        for (const auto& clip : getClipStates (trackState))
        {
            if (clip[schema::clipKind].toString() != schema::clipIsMidi || (bool) clip[schema::clipMute])
                continue;

            const auto start = (double) clip[schema::clipStart];
            const auto end = start + (double) clip[schema::clipLength];
            const auto contentStart = start - (double) clip.getProperty (schema::clipOffset, 0.0);

            for (const auto& child : clip)
            {
                if (child.hasType (schema::NOTE) && ! (bool) child[schema::noteMute])
                {
                    const auto from = contentStart + (double) child[schema::noteStart];
                    const auto to = from + (double) child[schema::noteLength];

                    if (const auto playedFrom = std::max (from, start), playedTo = std::min (to, end); playedTo > playedFrom)
                        notes.push_back (
                            { playedFrom, playedTo, (int) child[schema::notePitch], (float) (int) child[schema::noteVelocity] / 127.0f });
                }
                else if (child.hasType (schema::CONTROL))
                {
                    const auto beat = contentStart + (double) child[schema::controlBeat];
                    const auto kind = child[schema::controlKind].toString();
                    const auto value = (int) child[schema::controlValue];

                    if (beat < start || beat >= end)
                        continue;

                    if (kind == schema::controlIsController)
                        controls.push_back ({ beat, EventType::controller, (int) child[schema::controlNumber], (float) value / 127.0f });
                    else if (kind == schema::controlIsPitchWheel)
                        controls.push_back ({ beat, EventType::pitchBend, 0, (float) (value - 8192) / 8192.0f });
                }
            }
        }

        if (notes.empty() && controls.empty())
            return nullptr;

        return std::make_shared<const engine::NoteSequence> (std::move (notes), std::move (controls));
    }

    // A file's samples as JUCE's formats read them, for the engine's reading thread (or a render's).
    class FormatReader final : public engine::SampleReader
    {
    public:
        explicit FormatReader (std::unique_ptr<juce::AudioFormatReader> formatReader)
            : reader (std::move (formatReader))
        {
        }

        int getNumChannels() const override { return (int) reader->numChannels; }
        double getSampleRate() const override { return reader->sampleRate; }
        std::int64_t getLength() const override { return reader->lengthInSamples; }

        bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
        {
            return reader->read (&buffer, 0, numSamples, start, true, true);
        }

    private:
        std::unique_ptr<juce::AudioFormatReader> reader;
    };

    // A fade's shape, by its name in the song (Schema.h: clipFadeInShape).
    engine::FadeShape getEngineFadeShape (const juce::ValueTree& clipState, bool fadeIn)
    {
        const auto name = clipState[fadeIn ? schema::clipFadeInShape : schema::clipFadeOutShape].toString();

        if (name == schema::fadeIsConvex)
            return engine::FadeShape::convex;

        if (name == schema::fadeIsConcave)
            return engine::FadeShape::concave;

        if (name == schema::fadeIsSCurve)
            return engine::FadeShape::sCurve;

        return engine::FadeShape::linear;
    }
} // namespace

SongPlayer::SongPlayer (te::Edit& e)
    : edit (e)
{
}

std::unique_ptr<engine::Plan> SongPlayer::compile (double sampleRate, int maximumBlockSize, bool isRender)
{
    // Another rate or block size: everything that plays is made anew for it.
    if (! juce::exactlyEqual (sampleRate, preparedRate) || maximumBlockSize != preparedBlockSize)
    {
        devices.clear();
        strips.clear();
        delays.clear();
        preparedRate = sampleRate;
        preparedBlockSize = maximumBlockSize;
    }

    auto plan = std::make_unique<engine::Plan>();

    // The tempo map, the loop and the metronome.
    std::vector<engine::TempoMap::Tempo> tempos;
    std::vector<engine::TempoMap::TimeSignature> signatures;

    for (const auto& change : getTempoChanges (edit))
        tempos.push_back ({ change.beat.inBeats(), change.bpm });

    for (const auto& change : getTimeSignatureChanges (edit))
        signatures.push_back ({ change.beat.inBeats(), change.signature.numerator, change.signature.denominator });

    plan->tempo = engine::TempoMap (std::move (tempos), std::move (signatures), sampleRate);

    if (! isRender && isLoopOn (edit))
    {
        const auto loop = getLoopBeats (edit);
        plan->loop = engine::Plan::BeatRange { loop.getStart().inBeats(), loop.getEnd().inBeats() };
    }

    plan->metronome = ! isRender && isMetronomeOn (edit);

    // Each track that holds clips, through its devices.
    lowLatency = ! isRender && isLowLatency (edit.engine);
    used.clear();
    usedFiles.clear();
    usedRegions.clear();

    for (const auto& trackState : getTrackStates (edit))
    {
        if (isFolderState (trackState) || isReturnState (trackState))
            continue;

        // A frozen track plays its frozen file, through the devices after its freeze point; any other its clips.
        engine::Plan::Strip strip;
        strip.id = getID (trackState, schema::trackID);
        strip.state = getStrip (strip.id);
        const auto frozen = addFrozenFile (strip, trackState);

        if (! frozen)
        {
            strip.notes = compileNotes (trackState);
            addAudio (strip, trackState);
        }

        addDevices (strip, trackState, frozen);
        setChannel (trackState, false);
        plan->strips.push_back (std::move (strip));
    }

    updateAudibility();

    // The master adds them up, each delayed to come out as late as the latest (delay compensation), through its devices.
    engine::Plan::Strip master;
    master.state = getStrip (0);
    const auto masterState = findPhaziteState (edit).getChildWithName (schema::MASTER);
    addDevices (master, masterState);
    setChannel (masterState, true);

    auto latest = 0;

    for (const auto& strip : plan->strips)
        latest = std::max (latest, strip.latency);

    for (int i = 0; i < (int) plan->strips.size(); ++i)
    {
        const auto& strip = plan->strips[(size_t) i];
        engine::Plan::Input input { i, nullptr };

        if (const auto delay = latest - strip.latency; delay > 0)
        {
            auto& kept = delays[strip.id];

            if (kept == nullptr || kept->getLength() != delay)
                kept = std::make_shared<engine::Delay> (delay);

            input.delay = kept;
        }

        master.inputs.push_back (std::move (input));
    }

    master.latency += latest;
    plan->strips.push_back (std::move (master));

    // What the song doesn't hold any more goes with the last plan that plays it.
    std::set<std::int64_t> tracks;

    for (const auto& strip : plan->strips)
        tracks.insert (strip.id);

    std::erase_if (devices, [this] (const auto& device) { return used.count (device.first) == 0; });
    std::erase_if (files, [this] (const auto& file) { return usedFiles.count (file.first) == 0; });
    std::erase_if (regions, [this] (const auto& region) { return usedRegions.count (region.first) == 0; });
    std::erase_if (strips, [&tracks] (const auto& strip) { return tracks.count (strip.first) == 0; });
    std::erase_if (delays, [&tracks] (const auto& delay) { return tracks.count (delay.first) == 0; });
    return plan;
}

std::shared_ptr<engine::StripState> SongPlayer::getStrip (std::int64_t id)
{
    auto& strip = strips[id];

    if (strip == nullptr)
    {
        strip = std::make_shared<engine::StripState>();
        strip->prepare (preparedRate, preparedBlockSize);
    }

    return strip;
}

std::shared_ptr<engine::DeviceNode> SongPlayer::getDevice (const juce::ValueTree& deviceState)
{
    // Phazite's own devices; a plug-in or a container plays nothing yet (T2 steps 6 and 7).
    const auto* info = findOwnDevice (deviceState[schema::deviceType].toString().toStdString());

    if (info == nullptr)
        return nullptr;

    const auto id = getID (deviceState, schema::deviceID);
    auto& node = devices[id];

    if (node == nullptr || &node->getDeviceInfo() != info)
    {
        node = std::make_shared<engine::DeviceNode> (*info, createOwnDevice (info->typeID));
        node->getRunner().prepare (preparedRate, preparedBlockSize);
        loadSounds (*node, deviceState);
    }

    setValues (*node, deviceState);
    used.insert (id);
    return node;
}

void SongPlayer::addDevices (engine::Plan::Strip& strip, const juce::ValueTree& channelState, bool skipFrozen)
{
    // While Low Latency is on, a device that looks ahead lets the sound through as it comes (model/engine/internal/EngineSetup.h).
    const auto lowLatencyLimit = (int) (lowLatencyLimitSeconds * preparedRate);

    for (const auto& deviceState : channelState)
    {
        if (! deviceState.hasType (schema::DEVICE))
            continue;

        // A frozen track's devices before its freeze point are in its frozen file.
        if (skipFrozen)
            if (const auto plugin = te::findPluginForID (edit, toTracktion (getDeviceID (deviceState))))
                if (isFrozenDevice (*plugin))
                    continue;

        auto node = getDevice (deviceState);

        if (node == nullptr)
            continue;

        const auto latency = node->isOn() ? node->getRunner().getLatencySamples() : 0;

        if (lowLatency && latency > lowLatencyLimit)
            continue;

        if (strip.devices.empty() && node->getDeviceInfo().kind == devices::DeviceKind::instrument)
            strip.hasInstrument = true;

        strip.latency += latency;
        strip.devices.push_back (std::move (node));
    }
}

void SongPlayer::setValues (engine::DeviceNode& node, const juce::ValueTree& deviceState)
{
    // Those set by hand, the others at their defaults.
    const auto& parameters = node.getDeviceInfo().parameters;
    const auto values = deviceState.getChildWithName (schema::VALUES);

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        const auto id = juce::Identifier (juce::String (parameters[i].id.data(), parameters[i].id.size()));
        node.setValue ((int) i, (float) values.getProperty (id, parameters[i].defaultValue));
    }

    node.setOn (! (bool) deviceState[schema::deviceOff]);
}

void SongPlayer::loadSounds (engine::DeviceNode& node, const juce::ValueTree& deviceState)
{
    // What a drum sampler's pads and a wavetable synth's oscillators play: the song's SOUNDS name it as the plug-in's state
    // does.
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);
    auto& processor = node.getRunner().getProcessor();
    const auto type = node.getDeviceInfo().typeID;

    if (type == devices::DrumSampler::typeID)
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            loadPadSound (static_cast<devices::DrumSampler&> (processor), edit, sounds, pad);

    if (type == devices::WavetableSynth::typeID)
        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
            loadOscillatorTable (static_cast<devices::WavetableSynth&> (processor), edit, sounds, osc);
}

void SongPlayer::setChannel (const juce::ValueTree& channelState, bool isMaster)
{
    if (auto* strip = findStrip (isMaster ? 0 : getID (channelState, schema::trackID)))
    {
        auto& channel = strip->getChannel();
        channel.setVolume ((float) channelState.getProperty (schema::channelVolume, isMaster ? defaultMasterVolume : 0.0f));
        channel.setPan ((float) channelState.getProperty (schema::channelPan, 0.0f));
    }
}

void SongPlayer::updateAudibility()
{
    // A track is heard unless it is muted, or in a muted folder, or other tracks are soloed and neither it nor a folder it is
    // in is (a return is never silenced by a solo).
    const auto trackStates = getTrackStates (edit);
    const auto anySoloed =
        std::any_of (trackStates.begin(), trackStates.end(), [] (const auto& state) { return (bool) state[schema::trackSolo]; });

    for (const auto& state : trackStates)
    {
        auto* strip = findStrip (getID (state, schema::trackID));

        if (strip == nullptr)
            continue;

        auto muted = false, soloed = false;

        for (auto track = state; track.hasType (schema::TRACK); track = track.getParent())
        {
            muted = muted || (bool) track[schema::trackMute];
            soloed = soloed || (bool) track[schema::trackSolo];
        }

        strip->getChannel().setAudible (! muted && (! anySoloed || soloed || isReturnState (state)));
    }
}

bool SongPlayer::takeValue (const juce::ValueTree& tree, const juce::Identifier& property)
{
    // A parameter's value set by hand, or its default again.
    if (tree.hasType (schema::VALUES))
    {
        if (auto* node = findDevice (getID (tree.getParent(), schema::deviceID)))
        {
            const auto& info = node->getDeviceInfo();

            if (const auto index = devices::findParameter (info, property.toString().toStdString()); index >= 0)
                node->setValue (index, (float) tree.getProperty (property, info.parameters[(size_t) index].defaultValue));
        }

        return true;
    }

    // A pad's sound or an oscillator's table.
    if (tree.hasType (schema::SOUNDS))
    {
        if (auto* node = findDevice (getID (tree.getParent(), schema::deviceID)))
            loadSounds (*node, tree.getParent());

        return true;
    }

    const auto isChannel = tree.hasType (schema::TRACK) || tree.hasType (schema::MASTER);

    if (isChannel && (property == schema::channelVolume || property == schema::channelPan))
    {
        setChannel (tree, tree.hasType (schema::MASTER));
        return true;
    }

    if (tree.hasType (schema::TRACK) && (property == schema::trackMute || property == schema::trackSolo))
    {
        updateAudibility();
        return true;
    }

    // How a track shows changes nothing that plays.
    if (tree.hasType (schema::TRACK) && schema::isTrackViewProperty (property))
        return true;

    // A device switched: at once, and a new plan for the delays that line the tracks up, which its latency changes.
    if (tree.hasType (schema::DEVICE) && property == schema::deviceOff)
        if (auto* node = findDevice (getID (tree, schema::deviceID)))
            node->setOn (! (bool) tree[schema::deviceOff]);

    return false;
}

//==============================================================================
void SongPlayer::readLive (engine::FileReader& fileReader, const std::atomic<std::uint64_t>& blocksPlayed)
{
    reader = &fileReader;
    blocks = &blocksPlayed;
    files.clear();
}

std::shared_ptr<engine::AudioFile> SongPlayer::getAudioFile (const juce::File& file)
{
    const auto path = file.getFullPathName();
    auto& kept = files[path];

    if (kept == nullptr)
    {
        // A file that can't be read (gone, or a render not made yet) plays nothing until a plan finds it.
        auto formatReader =
            std::unique_ptr<juce::AudioFormatReader> (edit.engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));

        if (formatReader == nullptr)
        {
            files.erase (path);
            return nullptr;
        }

        kept = std::make_shared<engine::AudioFile> (std::make_unique<FormatReader> (std::move (formatReader)), blocks);

        if (reader != nullptr)
            reader->add (kept);
    }

    usedFiles.insert (path);
    return kept;
}

engine::AudioRegion SongPlayer::makeRegion (std::int64_t id, std::shared_ptr<engine::AudioFile> file)
{
    engine::AudioRegion region;
    region.file = std::move (file);
    auto& state = regions[id];

    if (state == nullptr)
        state = std::make_shared<engine::RegionState>();

    region.state = state;
    usedRegions.insert (id);
    return region;
}

void SongPlayer::addAudio (engine::Plan::Strip& strip, const juce::ValueTree& trackState)
{
    for (const auto& clip : getClipStates (trackState))
    {
        if (! isAudioState (clip) || (bool) clip[schema::clipMute])
            continue;

        // What Tracktion plays for it: its file, or Phazite's render of it, where it plays in seconds.
        auto file = getAudioFile (getPlayedFile (edit, clip));

        if (file == nullptr)
            continue;

        const auto position = getPlayedPosition (edit, clip);
        auto region = makeRegion (getID (clip, schema::clipID), file);
        region.start = std::llround (position.getStart().inSeconds() * preparedRate);
        region.length = std::llround (position.getEnd().inSeconds() * preparedRate) - region.start;
        region.fileStart = position.offset.inSeconds() * file->getSampleRate();
        region.gain = juce::Decibels::decibelsToGain ((float) clip.getProperty (schema::clipGain, 0.0f));

        // Its fades, scaled down together where they would overlap, as Tracktion fits them.
        const auto seconds = position.getLength().inSeconds();
        auto fadeIn = getClipFade (clip, true), fadeOut = getClipFade (clip, false);

        if (fadeIn + fadeOut > seconds && fadeIn + fadeOut > 0.0)
        {
            const auto scale = seconds / (fadeIn + fadeOut);
            fadeIn *= scale;
            fadeOut *= scale;
        }

        region.fadeIn = (int) std::llround (fadeIn * preparedRate);
        region.fadeOut = (int) std::llround (fadeOut * preparedRate);
        region.fadeInShape = getEngineFadeShape (clip, true);
        region.fadeOutShape = getEngineFadeShape (clip, false);
        file->keep ((std::int64_t) region.fileStart);
        strip.audio.push_back (std::move (region));
    }
}

bool SongPlayer::addFrozenFile (engine::Plan::Strip& strip, const juce::ValueTree& trackState)
{
    // Frozen as Tracktion plays it (model/mixer/internal/Freeze.h): its file from the song's start, if it is there.
    auto* track = dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, toTracktion (getTrackID (trackState))));

    if (! (bool) trackState[schema::trackFrozen] || track == nullptr)
        return false;

    auto file = getAudioFile (te::TemporaryFileManager::getFreezeFileForTrack (*track));

    if (file == nullptr)
        return false;

    auto region = makeRegion (strip.id, file);
    region.length = std::llround ((double) file->getLength() / file->getSampleRate() * preparedRate);
    file->keep (0);
    strip.audio.push_back (std::move (region));
    return true;
}

engine::DeviceNode* SongPlayer::findDevice (std::int64_t id) const
{
    const auto found = devices.find (id);
    return found != devices.end() ? found->second.get() : nullptr;
}

engine::StripState* SongPlayer::findStrip (std::int64_t id) const
{
    const auto found = strips.find (id);
    return found != strips.end() ? found->second.get() : nullptr;
}
} // namespace phazite

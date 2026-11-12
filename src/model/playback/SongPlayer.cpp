// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongPlayer.h"
#include "devices/drums/DrumSampler.h"
#include "devices/sampler/Sampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "internal/PlaybackHost.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/devices/internal/Wavetables.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedProcessor.h"
#include "model/transport/internal/AudioRecorder.h"
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
                        notes.push_back ({ playedFrom, playedTo, (int) child[schema::notePitch],
                                           (float) (int) child[schema::noteVelocity] / 127.0f, (bool) child[schema::noteSlide],
                                           (bool) child[schema::notePortamento] });
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

    // A device's values set by hand, in its description's order: as the song holds them, or as its node has them now.
    std::vector<float> readValuesSetByHand (const devices::DeviceInfo& info, const juce::ValueTree& deviceState)
    {
        std::vector<float> values;

        for (const auto& parameter : info.parameters)
            values.push_back (readValueSetByHand (deviceState, parameter));

        return values;
    }

    std::vector<float> readValuesSetByHand (const engine::DeviceNode& node)
    {
        std::vector<float> values;

        for (int i = 0; i < (int) node.getDeviceInfo().parameters.size(); ++i)
            values.push_back (node.getValue (i));

        return values;
    }

    // Whether these values set by hand would make a device's sound come later or sooner than the latency it was prepared
    // with (the compressor looks ahead only while it ducks: devices/core/Processor.h).
    bool changesLatency (const engine::DeviceNode& node, std::span<const float> values)
    {
        const auto latency = node.getRunner().getProcessor().getLatencySamplesFor (values);
        return latency >= 0 && latency != node.getRunner().getLatencySamples();
    }
} // namespace

SongPlayer::SongPlayer (Song& s, PluginInstances* p)
    : song (s),
      plugins (p),
      songState (s.getState())
{
    songState.addListener (this);
}

SongPlayer::~SongPlayer()
{
    songState.removeListener (this);
}

//==============================================================================
std::shared_ptr<const engine::NoteSequence> SongPlayer::getNotes (const juce::ValueTree& trackState)
{
    const auto id = getID (trackState, schema::trackID);
    usedNotes.insert (id);

    if (const auto found = notes.find (id); found != notes.end() && ! notesChanged.contains (id))
        return found->second;

    auto compiled = compileNotes (trackState);
    notes[id] = compiled;
    notesChanged.erase (id);
    return compiled;
}

void SongPlayer::markNotes (const juce::ValueTree& changed)
{
    // A change in a clip, or to a clip: its track's notes (a tempo change moves none: they are in beats).
    for (auto element = changed; element.isValid(); element = element.getParent())
        if (element.hasType (schema::CLIP))
            return (void) notesChanged.insert (getID (element.getParent(), schema::trackID));
}

void SongPlayer::markTracksIn (const juce::ValueTree& element)
{
    if (element.hasType (schema::TRACK))
        notesChanged.insert (getID (element, schema::trackID));

    if (element.hasType (schema::TRACK) || element.hasType (schema::TRACKS) || element.hasType (schema::PHAZITE))
        for (const auto& child : element)
            if (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS))
                markTracksIn (child);
}

void SongPlayer::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A clip added to a track, or something to a clip; or tracks added, which may hold notes already (an undo).
    if (child.hasType (schema::CLIP))
        markNotes (child);
    else
        markNotes (parent);

    markTracksIn (child);
}

void SongPlayer::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    // A clip taken off its track (which it has no more: its old parent's).
    if (child.hasType (schema::CLIP) && parent.hasType (schema::TRACK))
        notesChanged.insert (getID (parent, schema::trackID));
    else
        markNotes (parent);

    markTracksIn (child);
}

void SongPlayer::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent.hasType (schema::TRACK))
        notesChanged.insert (getID (parent, schema::trackID));
    else
        markNotes (parent);
}

void SongPlayer::letGoOfPlugins()
{
    for (const auto& [id, node] : devices)
        if (auto* processor = dynamic_cast<SandboxedProcessor*> (&node->getRunner().getProcessor()))
            processor->getInstance().setMovedByEngine (std::span<const int>());
}

void SongPlayer::makePluginsAnew()
{
    std::erase_if (devices, [] (const auto& device)
                   { return dynamic_cast<SandboxedProcessor*> (&device.second->getRunner().getProcessor()) != nullptr; });
}

std::unique_ptr<engine::Plan> SongPlayer::compile (double sampleRate, int maximumBlockSize, const std::optional<Live>& live,
                                                   const std::optional<RenderOf>& of)
{
    // Another rate or block size: everything that plays is made anew for it.
    if (! juce::exactlyEqual (sampleRate, preparedRate) || maximumBlockSize != preparedBlockSize)
    {
        devices.clear();
        strips.clear();
        delays.clear();
        modulators.clear();
        containers.clear();
        chains.clear();
        preparedRate = sampleRate;
        preparedBlockSize = maximumBlockSize;
    }

    auto plan = std::make_unique<engine::Plan>();
    plan->maximumBlockSize = preparedBlockSize;

    // The tempo map, with its ramps, the loop and the metronome.
    plan->tempo = getTempoMap (song, sampleRate);

    if (const auto loop = live.has_value() ? getLoop (song) : std::nullopt)
        plan->loop = engine::Plan::BeatRange { loop->getStart().inBeats(), loop->getEnd().inBeats() };

    plan->metronome = live.has_value() && isMetronomeOn (song);

    // Each track that holds clips, through its devices.
    lowLatency = live.has_value() && live->lowLatency;
    armed = live.has_value() ? live->armed : std::set<TrackID>();
    rendered = of;
    used.clear();
    usedFiles.clear();
    usedRegions.clear();
    usedNotes.clear();
    usedModulators.clear();
    usedSends.clear();
    usedContainers.clear();
    usedChains.clear();
    modulated.clear();
    readCurves();

    // The song's channels, each a strip: its tracks, its buses (folders with faders of their own: a plain folder's VCA
    // turns its tracks) and its returns, in an order in which each comes after what it takes in.
    usedDelays.clear();
    const auto channels = getChannels();
    std::map<std::int64_t, int> places; // each channel's strip, by its ID

    // The pads on mixer channels of their own, by their samplers, which play each into an output of its own.
    separatedPads.clear();
    partLatencies.clear();
    usedPartOutputs.clear();

    for (const auto& channel : channels)
        if (channel.pad >= 0 && channel.padSource != 0 && ! channel.refuses (channel.padSource))
            separatedPads[channel.padDevice].insert (channel.pad);

    for (const auto& channel : channels)
    {
        engine::Plan::Strip strip;
        strip.id = channel.id;
        strip.state = getStrip (strip.id);

        // A bus adds up its tracks; a return what is sent to it.
        std::vector<engine::Plan::Input> inputs;
        const auto isReturn = isReturnState (channel.state);

        for (const auto& other : channels)
        {
            if (! places.contains (other.id) || channel.refuses (other.id))
                continue;

            if (! isReturn)
            {
                if (other.output == channel.id)
                    inputs.push_back ({ places[other.id], nullptr });

                continue;
            }

            for (const auto& send : other.sends)
            {
                if (getID (send, schema::sendTo) == channel.id)
                {
                    const auto beforeFader = (bool) send[schema::sendPreFader];
                    inputs.push_back ({ places[other.id], nullptr, beforeFader, getSendLevel (send) });
                    plan->strips[(size_t) places[other.id]].feedsBeforeFader |= beforeFader;
                }
            }
        }

        // A pad track adds up its sampler's pad, as the sampler plays it into an output of its own; the sampler's track then
        // plays while it is muted, so that the pad follows its own track's mute.
        if (channel.pad >= 0 && places.contains (channel.padSource) && ! channel.refuses (channel.padSource))
        {
            if (const auto output = partOutputs.find ({ channel.padDevice, channel.pad });
                output != partOutputs.end() && usedPartOutputs.contains (output->first))
            {
                inputs.push_back ({ places[channel.padSource], nullptr, false, nullptr, output->second });
                plan->strips[(size_t) places[channel.padSource]].feedsParts = true;
            }
        }

        addInputs (*plan, strip, std::move (inputs));

        // A frozen track plays its frozen file, through the devices after its freeze point; any other its clips.
        auto frozen = false;

        if (! isFolderState (channel.state) && ! isReturnState (channel.state))
        {
            frozen = addFrozenFile (strip, channel.state);

            if (! frozen)
            {
                strip.notes = getNotes (channel.state);
                addAudio (strip, channel.state);
            }
        }

        const auto* only = of.has_value() && channel.id == of->channel && of->devices.has_value() ? &*of->devices : nullptr;
        const auto deviceIDs = addDevices (*plan, places, channel, strip, channel.state, frozen, only);

        if (live.has_value() && ! strip.hasInstrument && ! isFolderState (channel.state) && ! isReturnState (channel.state))
            strip.inputChannels = getHeardInput (channel.state);

        addFaderAutomation (strip, channel.state, false);
        addModulation (strip, channel.state, deviceIDs);
        tellWhatMoves (strip);
        setChannel (channel.state, false);
        places[channel.id] = (int) plan->strips.size();
        plan->strips.push_back (std::move (strip));
    }

    updateAudibility();

    // The master adds up what goes to it: the tracks and buses in no bus, and the returns; or what a render plays of the
    // song alone, as it is.
    engine::Plan::Strip master;
    master.state = getStrip (0);
    std::vector<engine::Plan::Input> inputs;

    if (! of.has_value())
    {
        for (const auto& channel : channels)
            if (channel.output == 0 && places.contains (channel.id))
                inputs.push_back ({ places[channel.id], nullptr });
    }
    else if (places.contains (of->channel))
    {
        inputs.push_back ({ places[of->channel], nullptr, of->beforeFader });
        plan->strips[(size_t) places[of->channel]].feedsBeforeFader |= of->beforeFader;
    }

    addInputs (*plan, master, std::move (inputs));

    if (! of.has_value())
    {
        const auto masterState = song.getState().getChildWithName (schema::MASTER);
        const auto masterDeviceIDs = addDevices (*plan, places, {}, master, masterState);
        addFaderAutomation (master, masterState, true);
        addModulation (master, masterState, masterDeviceIDs);
        tellWhatMoves (master);
        setChannel (masterState, true);
    }

    plan->strips.push_back (std::move (master));

    // A version's bounce compared with the song, while it plays live.
    if (live.has_value() && comparison.has_value())
        addComparison (*plan, sampleRate);

    // What the song doesn't hold any more goes with the last plan that plays it (a plug-in told that nothing of it moves
    // now, as it lives on in its track's freeze, or in the undo history).
    std::set<std::int64_t> tracks;

    for (const auto& strip : plan->strips)
        tracks.insert (strip.id);

    for (const auto& [id, node] : devices)
        if (auto* processor = dynamic_cast<SandboxedProcessor*> (&node->getRunner().getProcessor());
            processor != nullptr && ! used.contains (id))
            processor->getInstance().setMovedByEngine (std::span<const int>());

    std::erase_if (devices, [this] (const auto& device) { return used.count (device.first) == 0; });
    std::erase_if (modulators, [this] (const auto& modulator) { return usedModulators.count (modulator.first) == 0; });
    std::erase_if (files, [this] (const auto& file) { return usedFiles.count (file.first) == 0; });
    std::erase_if (regions, [this] (const auto& region) { return usedRegions.count (region.first) == 0; });
    std::erase_if (notes, [this] (const auto& track) { return usedNotes.count (track.first) == 0; });
    notesChanged.clear(); // those of tracks it played were compiled again, and the others' notes are let go
    std::erase_if (strips, [&tracks] (const auto& strip) { return tracks.count (strip.first) == 0; });
    std::erase_if (delays, [this] (const auto& delay) { return usedDelays.count (delay.first) == 0; });
    std::erase_if (sendLevels, [this] (const auto& level) { return usedSends.count (level.first) == 0; });
    std::erase_if (containers, [this] (const auto& container) { return usedContainers.count (container.first) == 0; });
    std::erase_if (chains, [this] (const auto& chain) { return usedChains.count (chain.first) == 0; });
    std::erase_if (partOutputs, [this] (const auto& output) { return ! usedPartOutputs.contains (output.first); });
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
    // A plug-in: its instance in its helper (model/plugins/internal/PluginInstances.h), which a render holds while it
    // plays a block, the engine skipping it then (SandboxedProcessor.h).
    if (deviceState[schema::deviceType].toString() == schema::deviceIsPlugin)
        return getPluginDevice (deviceState);

    // Phazite's own devices.
    const auto* info = findOwnDevice (deviceState[schema::deviceType].toString().toStdString());

    if (info == nullptr)
        return nullptr;

    const auto id = getID (deviceState, schema::deviceID);
    auto& node = devices[id];
    const auto values = readValuesSetByHand (*info, deviceState);

    // Made anew for another kind of device, and for values set by hand that make its sound come later or sooner (the
    // compressor looks ahead only while it ducks): its runner takes the latency as it is prepared, as a plug-in's does.
    if (node == nullptr || &node->getDeviceInfo() != info || changesLatency (*node, values))
    {
        auto processor = createOwnDevice (info->typeID);
        processor->takeValuesBeforePrepare (values);
        node = std::make_shared<engine::DeviceNode> (*info, std::move (processor));
        node->getRunner().prepare (preparedRate, preparedBlockSize);
        loadSounds (*node, deviceState);
    }

    setValues (*node, deviceState);
    used.insert (id);
    return node;
}

std::shared_ptr<engine::DeviceNode> SongPlayer::getPluginDevice (const juce::ValueTree& deviceState)
{
    // One that is loaded (not one missing on this computer), in its helper, kept by its ID while the song's instances keep
    // the same instance: through its channel, with its mix as the engine's.
    const auto sandboxed = plugins != nullptr ? plugins->find (getDeviceID (deviceState)) : nullptr;

    if (sandboxed == nullptr)
        return nullptr;

    const auto id = getID (deviceState, schema::deviceID);
    auto& node = devices[id];
    const auto* playing = node != nullptr ? dynamic_cast<const SandboxedProcessor*> (&node->getRunner().getProcessor()) : nullptr;

    // Made anew for another instance, and for a latency the plug-in changed (its look-ahead set longer in its own window): its
    // runner takes it as it is prepared, for its strip's and its mix's delays.
    if (playing == nullptr || &playing->getInstance() != sandboxed.get()
        || playing->getLatencySamples() != node->getRunner().getLatencySamples())
    {
        auto processor = std::make_unique<SandboxedProcessor> (sandboxed);
        const auto* made = processor.get();
        node = std::make_shared<engine::DeviceNode> (made->getInfo(), std::move (processor));
        node->getRunner().prepare (preparedRate, preparedBlockSize);

        // Its parameters' values set by hand, which its modulation moves from (takeValueSetByHand() keeps them).
        for (int i = 0; i < (int) sandboxed->getParameterInfos().size(); ++i)
            node->setValue (i, sandboxed->getParameterValue (i));
    }

    if (const auto mix = devices::findParameter (node->getDeviceInfo(), devices::mixParameterID); mix >= 0)
        node->setValue (mix, (float) deviceState.getProperty (schema::deviceMix, 1.0f));

    node->setOn (! (bool) deviceState[schema::deviceOff]);
    used.insert (id);
    return node;
}

std::vector<std::int64_t> SongPlayer::addDevices (engine::Plan& plan, const std::map<std::int64_t, int>& places, const ChannelInfo& channel,
                                                  engine::Plan::Strip& strip, const juce::ValueTree& channelState, bool skipFrozen,
                                                  const std::set<std::int64_t>* only)
{
    std::vector<std::int64_t> ids;

    // While Low Latency is on, a device that looks ahead lets the sound through as it comes (PlaybackHost.h).
    const auto lowLatencyLimit = (int) (lowLatencyLimitSeconds * preparedRate);

    // A frozen track's devices before its freeze point are in its frozen file.
    const auto isFrozen = [this, skipFrozen] (const juce::ValueTree& state)
    { return skipFrozen && isFrozenDevice (song, getDeviceID (state)); };

    const auto add = [&] (const juce::ValueTree& deviceState)
    {
        if (isFrozen (deviceState))
            return;

        auto node = getDevice (deviceState);

        if (node == nullptr)
            return;

        const auto latency = node->isOn() ? node->getRunner().getLatencySamples() : 0;

        if (lowLatency && latency > lowLatencyLimit)
            return;

        if (strip.devices.empty() && node->getDeviceInfo().kind == devices::DeviceKind::instrument)
            strip.hasInstrument = true;

        // Its parameters' automation, as late as its sound comes: the song's curves of the device (a plug-in can have
        // thousands of parameters, few automated).
        engine::Plan::Device device { std::move (node), {}, strip.latency };
        const auto id = getDeviceID (deviceState).getRaw();

        for (auto curve = curves.lower_bound ({ id, juce::String() }); curve != curves.end() && curve->first.first == id; ++curve)
            if (const auto index = findParameter (*device.node, curve->first.second); index >= 0)
                device.automation.push_back ({ index, curve->second });

        // What its sidechain listens to, lined up with its sound where it can be (a track that comes later than the device
        // can't be heard earlier).
        if (const auto source = (std::int64_t) getID (deviceState, schema::deviceSidechain);
            source != 0 && places.contains (source) && ! channel.refuses (source))
        {
            auto& heard = plan.strips[(size_t) places.at (source)];
            heard.feedsSidechain = true;
            device.sidechain = places.at (source);
            device.sidechainDelay = getDelay ({ (std::int64_t) id, source }, strip.latency - heard.latency);
        }

        // Its pads on mixer channels of their own, each played into an output of its own, as late as the device's sound comes.
        if (const auto pads = separatedPads.find ((std::int64_t) id); pads != separatedPads.end())
        {
            for (const auto pad : pads->second)
            {
                const std::pair key ((std::int64_t) id, pad);
                auto& output = partOutputs[key];

                if (output == nullptr || output->getMaximumBlockSize() != preparedBlockSize)
                    output = std::make_shared<engine::PartOutput> (pad, preparedBlockSize);

                usedPartOutputs.insert (key);
                partLatencies[output.get()] = strip.latency + latency;
                device.partOutputs.push_back (output);
                device.parts.push_back (output->describe());
            }
        }

        strip.latency += latency;
        strip.devices.push_back (std::move (device));
        ids.push_back ((std::int64_t) id);
    };

    for (const auto& deviceState : channelState)
    {
        // A render of some of them (a freeze's): the others left out.
        if (only != nullptr && ! only->contains ((std::int64_t) getID (deviceState, schema::deviceID)))
            continue;

        if (deviceState.hasType (schema::DEVICE))
        {
            add (deviceState);
        }
        else if (deviceState.hasType (schema::CONTAINER) && ! (bool) deviceState[schema::deviceOff] && ! isFrozen (deviceState))
        {
            addContainer (strip, deviceState, add);
        }
    }

    return ids;
}

void SongPlayer::addContainer (engine::Plan::Strip& strip, const juce::ValueTree& containerState,
                               const std::function<void (const juce::ValueTree&)>& add)
{
    // Its chains side by side, each its devices from the sound as it comes in, as late as it comes there, lined up with the
    // latest; then its mix of what they put out with what came in, delayed as much. An instrument container is its strip's
    // instrument.
    engine::Plan::Container container;
    container.first = (int) strip.devices.size();
    container.instrument = (bool) containerState[schema::containerInstrument];
    const auto split = containerState[schema::containerSplit].toString();
    container.split = split == schema::splitBands       ? engine::Split::bands
                      : split == schema::splitMidSide   ? engine::Split::midSide
                      : split == schema::splitLeftRight ? engine::Split::leftRight
                                                        : engine::Split::layers;
    const auto latencyBefore = strip.latency;
    auto latest = 0;

    if (container.instrument && container.first == 0)
        strip.hasInstrument = true;

    for (const auto& chainState : getChainStates (containerState))
    {
        if ((int) container.chains.size() == engine::maximumChains)
            break;

        engine::Plan::Container::Chain chain;
        chain.id = getID (chainState, schema::chainID);
        chain.first = (int) strip.devices.size();
        strip.latency = latencyBefore;

        for (const auto& inside : chainState)
            if (inside.hasType (schema::DEVICE))
                add (inside);

        chain.count = (int) strip.devices.size() - chain.first;
        chain.latency = strip.latency;
        latest = std::max (latest, strip.latency - latencyBefore);
        chain.state = getChain (chainState);
        chain.volume = findCurve ((std::uint64_t) chain.id, schema::chainVolumeParameter);
        chain.pan = findCurve ((std::uint64_t) chain.id, schema::chainPanParameter);
        chain.crossover = findCurve ((std::uint64_t) chain.id, schema::crossoverParameter);
        container.chains.push_back (std::move (chain));
    }

    for (auto& chain : container.chains)
        chain.delay = getDelay ({ chain.id, 0 }, latencyBefore + latest - chain.latency);

    strip.latency = latencyBefore + latest;
    container.count = (int) strip.devices.size() - container.first;

    const auto id = getID (containerState, schema::deviceID);
    auto& kept = containers[id];

    if (kept == nullptr || kept->getLatency() != latest)
        kept = std::make_shared<engine::ContainerState> (preparedBlockSize, latest);

    kept->setMix ((float) containerState.getProperty (schema::deviceMix, 1.0f));
    container.state = kept;
    usedContainers.insert (id);
    updateChainAudibility (containerState);
    strip.containers.push_back (std::move (container));
}

std::shared_ptr<engine::ChainState> SongPlayer::getChain (const juce::ValueTree& chainState)
{
    const auto id = getID (chainState, schema::chainID);
    auto& chain = chains[id];

    if (chain == nullptr)
    {
        chain = std::make_shared<engine::ChainState>();
        chain->prepare (preparedRate, preparedBlockSize);
    }

    setChainValues (*chain, chainState);
    usedChains.insert (id);
    return chain;
}

void SongPlayer::setChainValues (engine::ChainState& chain, const juce::ValueTree& chainState)
{
    // Its fader's volume and pan, as a channel's; its crossover, the default for its place where it has none of its own; and
    // the notes it plays.
    auto& fader = chain.getFader();
    fader.setVolume (std::max ((float) chainState.getProperty (schema::channelVolume, 0.0f), engine::Channel::silenceDb));
    fader.setPan ((float) chainState.getProperty (schema::channelPan, 0.0f));

    const auto siblings = getChainStates (chainState.getParent());
    const auto index = (int) (std::find (siblings.begin(), siblings.end(), chainState) - siblings.begin());
    chain.setCrossover ((float) chainState.getProperty (schema::chainCrossover, getDefaultCrossover (index, (int) siblings.size())));
    chain.setNotes ((int) chainState.getProperty (schema::chainLowKey, allKeys.low),
                    (int) chainState.getProperty (schema::chainHighKey, allKeys.high),
                    (int) chainState.getProperty (schema::chainLowVelocity, allVelocities.low),
                    (int) chainState.getProperty (schema::chainHighVelocity, allVelocities.high));
}

void SongPlayer::updateChainAudibility (const juce::ValueTree& containerState)
{
    // A chain is heard unless it is muted, or another of its container's chains is soloed and it isn't.
    const auto states = getChainStates (containerState);
    const auto anySoloed = std::any_of (states.begin(), states.end(), [] (const auto& state) { return (bool) state[schema::chainSolo]; });

    for (const auto& state : states)
        if (const auto found = chains.find (getID (state, schema::chainID)); found != chains.end())
            found->second->getFader().setAudible (! (bool) state[schema::chainMute] && (! anySoloed || (bool) state[schema::chainSolo]));
}

engine::ChainState* SongPlayer::findChain (std::int64_t id) const
{
    const auto found = chains.find (id);
    return found != chains.end() ? found->second.get() : nullptr;
}

std::vector<SongPlayer::ChannelInfo> SongPlayer::getChannels() const
{
    // Each track's, bus's and return's: where it goes (the bus it is in, or the master), and what it takes in.
    std::vector<ChannelInfo> infos;

    for (const auto& state : getTrackStates (song))
    {
        if (isFolderState (state) && ! state.hasProperty (schema::channelFader))
            continue; // a plain folder's VCA turns its tracks (setChannel())

        ChannelInfo info { state, getID (state, schema::trackID) };

        if (! isReturnState (state))
            for (auto parent = state.getParent(); parent.hasType (schema::TRACK) && info.output == 0; parent = parent.getParent())
                if (parent.hasProperty (schema::channelFader))
                    info.output = getID (parent, schema::trackID);

        for (const auto& child : state)
            if (child.hasType (schema::SEND))
                info.sends.push_back (child);

        // A pad track plays its drum sampler's pad, after the sampler's track.
        if (state.hasProperty (schema::trackPad))
        {
            info.padDevice = getID (state, schema::trackPadDevice);
            info.pad = (int) state[schema::trackPad];
            info.padSource = (std::int64_t) getDeviceChannel (song, DeviceID::fromRaw ((std::uint64_t) info.padDevice)).getRaw();
        }

        infos.push_back (std::move (info));
    }

    for (auto& info : infos)
    {
        const auto isReturn = isReturnState (info.state);

        for (const auto& other : infos)
        {
            if (other.output == info.id && ! isReturn)
                info.needs.push_back (other.id);

            if (isReturn)
                for (const auto& send : other.sends)
                    if (getID (send, schema::sendTo) == info.id)
                        info.needs.push_back (other.id);
        }

        const auto addSidechains = [&info] (const juce::ValueTree& chain, auto& addInside) -> void
        {
            for (const auto& device : chain)
            {
                if (device.hasType (schema::DEVICE))
                {
                    if (const auto source = getID (device, schema::deviceSidechain); source != 0)
                        info.needs.push_back (source);
                }
                else if (device.hasType (schema::CONTAINER) || device.hasType (schema::CHAIN))
                {
                    addInside (device, addInside);
                }
            }
        };

        addSidechains (info.state, addSidechains);

        if (info.padSource != 0)
            info.needs.push_back (info.padSource);
    }

    // In an order in which each comes after what it takes in; what would feed itself (a sidechain or a send round a loop)
    // is refused.
    std::vector<ChannelInfo> ordered;
    std::map<std::int64_t, int> marks; // 1 while visiting, 2 once placed
    std::map<std::int64_t, size_t> indices;

    for (size_t i = 0; i < infos.size(); ++i)
        indices[infos[i].id] = i;

    const auto visit = [&] (size_t index, auto& visitNext) -> void
    {
        auto& info = infos[index];
        marks[info.id] = 1;

        for (const auto need : info.needs)
        {
            if (! indices.contains (need))
                continue;

            if (marks[need] == 1)
                info.refused.insert (need);
            else if (marks[need] == 0)
                visitNext (indices[need], visitNext);
        }

        marks[info.id] = 2;
        ordered.push_back (info);
    };

    for (size_t i = 0; i < infos.size(); ++i)
        if (marks[infos[i].id] == 0)
            visit (i, visit);

    return ordered;
}

void SongPlayer::addInputs (engine::Plan& plan, engine::Plan::Strip& strip, std::vector<engine::Plan::Input> inputs)
{
    // How late each comes: a strip's sound after its devices; a device's part as late as the device's sound comes out of it.
    const auto getLatency = [this, &plan] (const engine::Plan::Input& input)
    {
        if (input.part != nullptr)
            if (const auto found = partLatencies.find (input.part.get()); found != partLatencies.end())
                return found->second;

        return plan.strips[(size_t) input.strip].latency;
    };

    // Each lined up with the latest (delay compensation): the strip's own sound comes in as late as that.
    for (const auto& input : inputs)
        strip.latency = std::max (strip.latency, getLatency (input));

    for (auto& input : inputs)
    {
        const auto& from = plan.strips[(size_t) input.strip];
        const auto key = input.part != nullptr ? std::pair (strip.id, -1 - (from.id * engine::maximumParts + input.part->getPart()))
                                               : std::pair (strip.id, from.id * 2 + (input.beforeFader ? 1 : 0));
        input.delay = getDelay (key, strip.latency - getLatency (input));
        strip.inputs.push_back (std::move (input));
    }
}

std::shared_ptr<engine::Delay> SongPlayer::getDelay (std::pair<std::int64_t, std::int64_t> key, int length)
{
    if (length <= 0)
        return nullptr;

    auto& kept = delays[key];

    if (kept == nullptr || kept->getLength() != length)
        kept = std::make_shared<engine::Delay> (length);

    usedDelays.insert (key);
    return kept;
}

std::shared_ptr<engine::SendLevel> SongPlayer::getSendLevel (const juce::ValueTree& sendState)
{
    const auto id = getID (sendState, schema::sendID);
    auto& kept = sendLevels[id];

    if (kept == nullptr)
        kept = std::make_shared<engine::SendLevel>();

    kept->setDecibels ((float) sendState.getProperty (schema::sendLevel, 0.0f));
    usedSends.insert (id);
    return kept;
}

//==============================================================================
std::shared_ptr<engine::ModulatorNode> SongPlayer::getModulator (const juce::ValueTree& state)
{
    // A macro, or a modulator of one of the six kinds, kept by its ID.
    std::optional<devices::ModulatorKind> kind;

    if (state.hasType (schema::MODULATOR))
    {
        kind = getModulatorKind (state);

        if (! kind.has_value())
            return nullptr;
    }

    const auto id = getID (state, schema::modulatorID);
    auto& node = modulators[id];

    if (node == nullptr || node->getKind() != kind)
    {
        node = kind.has_value() ? std::make_shared<engine::ModulatorNode> (*kind) : std::make_shared<engine::ModulatorNode>();
        node->prepare (preparedRate, preparedBlockSize, (std::uint32_t) id);
    }

    setModulatorValues (*node, state);
    usedModulators.insert (id);
    return node;
}

void SongPlayer::setModulatorValues (engine::ModulatorNode& node, const juce::ValueTree& state)
{
    if (node.isMacro())
    {
        node.setMacroValue ((float) state.getProperty (schema::macroValue, 0.0f));
        return;
    }

    // Those set by hand, the others at their defaults (devices/modulation/Modulators.h); its steps; and its switch.
    const auto parameters = devices::getModulatorParameters (*node.getKind());

    for (size_t i = 0; i < parameters.size(); ++i)
        node.setParameter ((int) i, readValueSetByHand (state, parameters[i]));

    const auto steps = getModulatorSteps (state);

    for (int i = 0; i < devices::Modulator::maxSteps; ++i)
        node.setStep (i, (size_t) i < steps.size() ? steps[(size_t) i] : 0.0f);

    node.setOn (! (bool) state[schema::deviceOff]);
}

void SongPlayer::addModulation (engine::Plan::Strip& strip, const juce::ValueTree& channelState, const std::vector<std::int64_t>& deviceIDs)
{
    // Its modulators and macros, in their order, then its containers' macros, and what each moves: its devices' parameters
    // (a container's macros those in it) and its fader.
    std::vector<juce::ValueTree> sources;

    const auto addSources = [this, &strip, &sources] (const juce::ValueTree& parent)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
            {
                if (auto node = getModulator (child))
                {
                    strip.modulators.push_back (std::move (node));
                    sources.push_back (child);
                }
            }
        }
    };

    addSources (channelState);

    for (const auto& child : channelState)
        if (child.hasType (schema::CONTAINER))
            addSources (child);

    const auto fader = (std::int64_t) getID (channelState, schema::channelFader);

    for (size_t source = 0; source < sources.size(); ++source)
    {
        for (const auto& modulates : sources[source])
        {
            if (! modulates.hasType (schema::MODULATES))
                continue;

            const auto target = getModulatedParameter (modulates);
            const auto amount = (float) modulates[schema::modulatesAmount];
            const auto device = (std::int64_t) target.device.getRaw();
            modulated.insert ({ target.device.getRaw(), target.parameter });

            if (device == fader)
            {
                if (target.parameter == "volume" || target.parameter == "master volume")
                    strip.volumeModulation.push_back ({ (int) source, amount });
                else if (target.parameter == "pan" || target.parameter == "master pan")
                    strip.panModulation.push_back ({ (int) source, amount });

                continue;
            }

            // A container's chain's volume, pan or crossover.
            if (auto* chain = findPlannedChain (strip, device))
            {
                if (target.parameter == schema::chainVolumeParameter)
                    chain->volumeModulation.push_back ({ (int) source, amount });
                else if (target.parameter == schema::chainPanParameter)
                    chain->panModulation.push_back ({ (int) source, amount });
                else if (target.parameter == schema::crossoverParameter)
                    chain->crossoverModulation.push_back ({ (int) source, amount });

                continue;
            }

            const auto place = std::find (deviceIDs.begin(), deviceIDs.end(), device);

            if (place == deviceIDs.end())
                continue;

            auto& entry = strip.devices[(size_t) (place - deviceIDs.begin())];
            const auto& info = entry.node->getDeviceInfo();
            const auto index = findParameter (*entry.node, target.parameter);

            if (index < 0)
                continue;

            if (entry.modulation == nullptr)
            {
                entry.modulation = std::make_shared<engine::Modulation>();
                entry.modulation->sources.resize (sources.size());
                entry.modulation->isModulated.assign (info.parameters.size(), false);
            }

            entry.modulation->routes.push_back ({ index, (int) source, amount });
            entry.modulation->isModulated[(size_t) index] = true;
        }
    }
}

engine::Plan::Container::Chain* SongPlayer::findPlannedChain (engine::Plan::Strip& strip, std::int64_t id)
{
    for (auto& container : strip.containers)
        for (auto& chain : container.chains)
            if (chain.id == id)
                return &chain;

    return nullptr;
}

void SongPlayer::tellWhatMoves (const engine::Plan::Strip& strip)
{
    for (const auto& device : strip.devices)
    {
        auto* processor = dynamic_cast<SandboxedProcessor*> (&device.node->getRunner().getProcessor());

        if (processor == nullptr)
            continue;

        moved.clear();

        for (const auto& automated : device.automation)
            if (! device.node->isWriting (automated.parameter))
                moved.push_back (automated.parameter);

        if (device.modulation != nullptr)
            for (const auto& route : device.modulation->routes)
                moved.push_back (route.parameter);

        processor->getInstance().setMovedByEngine (moved);
    }
}

int SongPlayer::findParameter (const engine::DeviceNode& node, const juce::String& id)
{
    // A plug-in's by its place, at once; one of Phazite's own devices' by its ID.
    if (auto* processor = dynamic_cast<const SandboxedProcessor*> (&node.getRunner().getProcessor()))
        return processor->findParameter (id);

    return devices::findParameter (node.getDeviceInfo(), id.toStdString());
}

void SongPlayer::takeValueSetByHand (const ParameterRef& parameter, float value)
{
    // Only a plug-in in a helper: Phazite's own devices take theirs from the song (takeValue()).
    auto* node = findDevice ((std::int64_t) parameter.device.getRaw());

    if (node == nullptr || dynamic_cast<const SandboxedProcessor*> (&node->getRunner().getProcessor()) == nullptr)
        return;

    if (const auto index = findParameter (*node, parameter.parameter); index >= 0)
        node->setValue (index, value);
}

engine::ModulatorNode* SongPlayer::findModulator (std::int64_t id) const
{
    const auto found = modulators.find (id);
    return found != modulators.end() ? found->second.get() : nullptr;
}

bool SongPlayer::isMoved (const ParameterRef& parameter) const
{
    return findCurve (parameter) != nullptr || modulated.count ({ parameter.device.getRaw(), parameter.parameter }) > 0;
}

void SongPlayer::readCurves()
{
    curves.clear();
    faders.clear();

    for (const auto& state : getCurveStates (song))
    {
        std::vector<engine::CurvePoint> points;

        for (const auto& point : state)
            if (point.hasType (schema::POINT))
                points.push_back ({ (double) point[schema::pointBeat], (float) point[schema::pointValue],
                                    (float) point.getProperty (schema::pointCurve, 0.0f) });

        if (const auto parameter = getCurveParameter (state); ! points.empty())
            curves[{ parameter.device.getRaw(), parameter.parameter }] = std::make_shared<const engine::Curve> (std::move (points));
    }
}

std::shared_ptr<const engine::Curve> SongPlayer::findCurve (std::uint64_t device, const juce::String& parameter) const
{
    const auto found = curves.find ({ device, parameter });
    return found != curves.end() ? found->second : nullptr;
}

void SongPlayer::addFaderAutomation (engine::Plan::Strip& strip, const juce::ValueTree& channelState, bool isMaster)
{
    // The fader's volume and pan, by its parameters' IDs (the master's are its own).
    const auto fader = (std::uint64_t) getID (channelState, schema::channelFader);
    faders[fader] = strip.id;
    strip.volume = findCurve (fader, isMaster ? "master volume" : "volume");
    strip.pan = findCurve (fader, isMaster ? "master pan" : "pan");
}

void SongPlayer::setValues (engine::DeviceNode& node, const juce::ValueTree& deviceState)
{
    // Those set by hand, the others at their defaults.
    const auto& parameters = node.getDeviceInfo().parameters;

    for (size_t i = 0; i < parameters.size(); ++i)
        node.setValue ((int) i, readValueSetByHand (deviceState, parameters[i]));

    node.setOn (! (bool) deviceState[schema::deviceOff]);
}

void SongPlayer::loadSounds (engine::DeviceNode& node, const juce::ValueTree& deviceState)
{
    // What a drum sampler's pads, a wavetable synth's oscillators and a sampler play: the song's SOUNDS name it as the
    // plug-in's state does.
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);
    auto& processor = node.getRunner().getProcessor();
    const auto type = node.getDeviceInfo().typeID;

    if (type == devices::DrumSampler::typeID)
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            loadPadSound (static_cast<devices::DrumSampler&> (processor), song, sounds, pad);

    if (type == devices::WavetableSynth::typeID)
        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
            loadOscillatorTable (static_cast<devices::WavetableSynth&> (processor), song, sounds, osc);

    if (type == devices::Sampler::typeID)
    {
        loadSamplerSound (static_cast<devices::Sampler&> (processor), song, sounds);
        loadSamplerSlices (static_cast<devices::Sampler&> (processor), sounds);
    }
}

void SongPlayer::loadSound (engine::DeviceNode& node, const juce::ValueTree& deviceState, const juce::Identifier& property)
{
    // Only the pad or the oscillator whose sound changed, as Swap changes one at each sound it tries; all of them for
    // anything else.
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);
    auto& processor = node.getRunner().getProcessor();
    const auto type = node.getDeviceInfo().typeID;

    if (type == devices::DrumSampler::typeID)
    {
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
        {
            for (int layer = 0; layer < devices::DrumSampler::numLayers; ++layer)
            {
                if (property == schema::getPadSound (pad, layer) || property == schema::getPadSoundFile (pad, layer))
                {
                    loadPadSound (static_cast<devices::DrumSampler&> (processor), song, sounds, pad);
                    return;
                }
            }
        }
    }

    if (type == devices::WavetableSynth::typeID)
    {
        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
        {
            if (property == schema::getOscillatorWavetable (osc) || property == schema::getOscillatorWavetableFile (osc))
            {
                loadOscillatorTable (static_cast<devices::WavetableSynth&> (processor), song, sounds, osc);
                return;
            }
        }
    }

    // A sampler's slices by hand, which change as they are dragged, without reading its sound again.
    if (type == devices::Sampler::typeID && (property == schema::samplerSlices || property == schema::samplerSlicesRemoved))
    {
        loadSamplerSlices (static_cast<devices::Sampler&> (processor), sounds);
        return;
    }

    loadSounds (node, deviceState);
}

void SongPlayer::setChannel (const juce::ValueTree& channelState, bool isMaster)
{
    if (auto* strip = findStrip (isMaster ? 0 : getID (channelState, schema::trackID)))
    {
        // Its fader, turned by the VCAs of the plain folders it is in.
        auto volume = (float) channelState.getProperty (schema::channelVolume, isMaster ? defaultMasterVolume : 0.0f);

        for (auto parent = channelState.getParent(); ! isMaster && parent.hasType (schema::TRACK); parent = parent.getParent())
            if (isFolderState (parent) && ! parent.hasProperty (schema::channelFader))
                volume += (float) parent.getProperty (schema::channelVolume, 0.0f);

        auto& channel = strip->getChannel();
        channel.setVolume (std::max (volume, engine::Channel::silenceDb));
        channel.setPan ((float) channelState.getProperty (schema::channelPan, 0.0f));
    }
}

void SongPlayer::updateAudibility()
{
    // A track is heard unless it is muted, or in a muted folder, or other tracks are soloed and neither it nor a folder it is
    // in is (a return is never silenced by a solo); a pad's track, as its sampler's track is too. A render before a channel's
    // fader (a freeze, a bounce: RenderOf) hears that channel whatever its mute switch says, and what feeds it whatever
    // other tracks' solos say: it plays the channel alone already.
    const auto trackStates = getTrackStates (song);
    const auto beforeFader = rendered.has_value() && rendered->beforeFader;
    const auto anySoloed =
        ! beforeFader
        && std::any_of (trackStates.begin(), trackStates.end(), [] (const auto& state) { return (bool) state[schema::trackSolo]; });

    // A bus carries a soloed track in it.
    const auto holdsSoloed = [] (const juce::ValueTree& folder, auto& holdsNext) -> bool
    {
        for (const auto& child : folder)
            if (child.hasType (schema::TRACK) && ((bool) child[schema::trackSolo] || holdsNext (child, holdsNext)))
                return true;

        return false;
    };

    for (const auto& state : trackStates)
    {
        auto* strip = findStrip (getID (state, schema::trackID));

        if (strip == nullptr)
            continue;

        auto muted = false, soloed = false;
        const auto addFrom = [&muted, &soloed] (const juce::ValueTree& from)
        {
            for (auto track = from; track.hasType (schema::TRACK); track = track.getParent())
            {
                muted = muted || (bool) track[schema::trackMute];
                soloed = soloed || (bool) track[schema::trackSolo];
            }
        };

        addFrom (state);

        // A pad's track as its sampler's track is too, whose mute and solo cover its pads on tracks of their own.
        if (state.hasProperty (schema::trackPad))
            addFrom (
                findTrackState (song, getDeviceChannel (song, DeviceID::fromRaw ((std::uint64_t) getID (state, schema::trackPadDevice)))));

        soloed = soloed || (isFolderState (state) && holdsSoloed (state, holdsSoloed));
        const auto isRendered = beforeFader && getID (state, schema::trackID) == rendered->channel;
        strip->getChannel().setAudible (isRendered || (! muted && (! anySoloed || soloed || isReturnState (state))));
    }
}

bool SongPlayer::takeValue (const juce::ValueTree& tree, const juce::Identifier& property)
{
    // A modulator's parameter, steps or switch, and a macro's value: at once.
    if (const auto parent = tree.getParent(); tree.hasType (schema::VALUES) && parent.hasType (schema::MODULATOR))
    {
        if (auto* node = findModulator (getID (parent, schema::modulatorID)))
            setModulatorValues (*node, parent);

        return true;
    }

    if ((tree.hasType (schema::MODULATOR) && (property == schema::modulatorSteps || property == schema::deviceOff))
        || (tree.hasType (schema::MACRO) && property == schema::macroValue))
    {
        if (auto* node = findModulator (getID (tree, schema::modulatorID)))
            setModulatorValues (*node, tree);

        return true;
    }

    if (tree.hasType (schema::MACRO) && property == schema::macroName)
        return true;

    // A send's level, and a container's mix: at once.
    if (tree.hasType (schema::SEND) && property == schema::sendLevel)
    {
        if (const auto found = sendLevels.find (getID (tree, schema::sendID)); found != sendLevels.end())
            found->second->setDecibels ((float) tree.getProperty (schema::sendLevel, 0.0f));

        return true;
    }

    if (tree.hasType (schema::CONTAINER) && property == schema::deviceMix)
    {
        if (const auto found = containers.find (getID (tree, schema::deviceID)); found != containers.end())
            found->second->setMix ((float) tree.getProperty (schema::deviceMix, 1.0f));

        return true;
    }

    // A container's chain's volume, pan and crossover, its mute and solo and the notes it plays, at once; its name changes
    // nothing that plays, nor a container's.
    if (tree.hasType (schema::CHAIN))
    {
        if (property == schema::chainMute || property == schema::chainSolo)
            updateChainAudibility (tree.getParent());
        else if (const auto found = chains.find (getID (tree, schema::chainID)); found != chains.end())
            setChainValues (*found->second, tree);

        return true;
    }

    if (tree.hasType (schema::CONTAINER) && property == schema::containerName)
        return true;

    // A plain folder's VCA turns the tracks in it.
    if (tree.hasType (schema::TRACK) && isFolderState (tree) && ! tree.hasProperty (schema::channelFader)
        && property == schema::channelVolume)
    {
        const auto turn = [this] (const juce::ValueTree& folder, auto& turnNext) -> void
        {
            for (const auto& child : folder)
            {
                if (child.hasType (schema::TRACK))
                {
                    setChannel (child, false);
                    turnNext (child, turnNext);
                }
            }
        };

        turn (tree, turn);
        return true;
    }

    // A parameter's value set by hand, or its default again; in a new plan, with the device made anew (getDevice()), if it
    // makes the device's sound come later or sooner.
    if (tree.hasType (schema::VALUES))
    {
        if (auto* node = findDevice (getID (tree.getParent(), schema::deviceID)))
        {
            const auto& info = node->getDeviceInfo();

            if (const auto index = devices::findParameter (info, property.toString().toStdString()); index >= 0)
                node->setValue (index, readSavedValue (info.parameters[(size_t) index], tree.getProperty (property)));

            if (changesLatency (*node, readValuesSetByHand (*node)))
                return false;
        }

        return true;
    }

    // A plug-in's value set by hand, by its own ID; and its settings and program, which its instance takes
    // (model/plugins/internal/PluginInstances.h).
    if (tree.hasType (schema::VALUE))
    {
        if (auto* node = findDevice (getID (tree.getParent(), schema::deviceID)))
            if (const auto index = findParameter (*node, tree[schema::valueParameter].toString()); index >= 0)
                node->setValue (index, (float) tree[schema::valueSetByHand]);

        return true;
    }

    if (tree.hasType (schema::DEVICE) && (property == schema::pluginState || property == schema::pluginProgram))
        return true;

    // A pad's sound or an oscillator's table.
    if (tree.hasType (schema::SOUNDS))
    {
        if (auto* node = findDevice (getID (tree.getParent(), schema::deviceID)))
            loadSound (*node, tree.getParent(), property);

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
    // By its path, when it was written and how long it is: a file written again at the same path (a track frozen again) is
    // read again.
    const auto path =
        file.getFullPathName() + "|" + juce::String (file.getLastModificationTime().toMilliseconds()) + "|" + juce::String (file.getSize());
    auto& kept = files[path];

    if (kept == nullptr)
    {
        // A file that can't be read (gone, or a render not made yet) plays nothing until a plan finds it.
        auto formatReader = std::unique_ptr<juce::AudioFormatReader> (getAudioFormats().createReaderFor (file));

        if (formatReader == nullptr)
        {
            files.erase (path);
            return nullptr;
        }

        kept = std::make_shared<engine::AudioFile> (makeSampleReader (std::move (formatReader)), blocks);

        if (reader != nullptr)
            reader->add (kept);
    }

    usedFiles.insert (path);
    return kept;
}

void SongPlayer::setComparison (std::optional<Comparison> next)
{
    const auto sameFile = comparison.has_value() && next.has_value() && comparison->file == next->file;
    comparison = std::move (next);

    if (! comparison.has_value())
    {
        comparisonState = nullptr; // freed with the last plan that plays it
        comparisonRegion = nullptr;
        return;
    }

    if (! sameFile)
        comparisonState = nullptr;

    prepareComparison (preparedRate > 0.0 ? preparedRate : 48000.0, preparedBlockSize > 0 ? preparedBlockSize : 512);
}

void SongPlayer::prepareComparison (double sampleRate, int maximumBlockSize)
{
    if (comparisonState != nullptr && juce::exactlyEqual (comparisonState->getSampleRate(), sampleRate)
        && comparisonState->getMaximumBlockSize() == maximumBlockSize)
        return;

    const auto hearing = comparisonState != nullptr && comparisonState->isHearingVersion();
    comparisonState = std::make_shared<engine::ComparisonState>();
    comparisonState->prepare (sampleRate, maximumBlockSize);
    comparisonRegion = nullptr; // made for the bounce's file (addComparison())

    if (hearing)
        comparisonState->hear (true);
}

void SongPlayer::addComparison (engine::Plan& plan, double sampleRate)
{
    auto file = getAudioFile (comparison->file);

    if (file == nullptr)
        return;

    prepareComparison (sampleRate, preparedBlockSize);

    if (comparisonRegion == nullptr || ! comparisonRegion->playsAt (*file, sampleRate))
        comparisonRegion = std::make_shared<engine::RegionState> (*file, sampleRate);

    // The whole bounce from the version's start, which the comparison plays at the song's beats.
    engine::AudioRegion bounce;
    bounce.file = file;
    bounce.state = comparisonRegion;
    bounce.length = (std::int64_t) std::ceil ((double) file->getLength() * sampleRate / file->getSampleRate());
    plan.comparison = engine::Plan::Comparison { std::move (bounce), getTempoMap (comparison->song, sampleRate), comparisonState };
}

engine::AudioRegion SongPlayer::makeRegion (std::int64_t id, std::shared_ptr<engine::AudioFile> file)
{
    engine::AudioRegion region;
    region.file = std::move (file);
    auto& state = regions[id];

    // Made anew for a file at another rate, or an engine at another (resampling the one where they differ).
    if (state == nullptr || ! state->playsAt (*region.file, preparedRate))
        state = std::make_shared<engine::RegionState> (*region.file, preparedRate);

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

        // What plays for it: its file, or Phazite's render of it, where it plays in seconds.
        auto file = getAudioFile (getPlayedFile (song, clip));

        if (file == nullptr)
            continue;

        const auto position = getPlayedPosition (song, clip);
        auto region = makeRegion (getID (clip, schema::clipID), file);
        region.start = std::llround (position.start * preparedRate);
        region.length = std::llround (position.end * preparedRate) - region.start;
        region.fileStart = position.offset * file->getSampleRate();
        region.gain = juce::Decibels::decibelsToGain ((float) clip.getProperty (schema::clipGain, 0.0f));

        // Its fades, scaled down together where they would overlap.
        const auto [fadeIn, fadeOut] = getFittedFades (song, clip);

        region.fadeIn = (int) std::llround (fadeIn * preparedRate);
        region.fadeOut = (int) std::llround (fadeOut * preparedRate);
        region.fadeInCurve = toEngineFade (getClipFadeCurve (clip, true));
        region.fadeOutCurve = toEngineFade (getClipFadeCurve (clip, false));
        file->keep ((std::int64_t) region.fileStart);
        strip.audio.push_back (std::move (region));
    }
}

std::vector<int> SongPlayer::getHeardInput (const juce::ValueTree& trackState) const
{
    const auto monitoring = trackState[schema::trackMonitoring].toString();
    const auto isArmed = armed.contains (getTrackID (trackState));

    if (monitoring == schema::monitorAlways || (isArmed && monitoring.isEmpty()))
        return getInputChannels (trackState[schema::trackInput].toString());

    return {};
}

bool SongPlayer::addFrozenFile (engine::Plan::Strip& strip, const juce::ValueTree& trackState)
{
    // Its frozen file (model/mixer/internal/Freeze.h) from the song's start, if it is there.
    if (! (bool) trackState[schema::trackFrozen])
        return false;

    auto file = getAudioFile (getFrozenFile (song, getTrackID (trackState)));

    if (file == nullptr)
        return false;

    auto region = makeRegion (strip.id, file);
    region.length = std::llround ((double) file->getLength() / file->getSampleRate() * preparedRate);
    file->keep (0);
    strip.audio.push_back (std::move (region));
    return true;
}

const engine::Curve* SongPlayer::findCurve (const ParameterRef& parameter) const
{
    return findCurve (parameter.device.getRaw(), parameter.parameter).get();
}

std::optional<float> SongPlayer::getPlayedValue (const ParameterRef& parameter, bool withModulation) const
{
    const auto id = (std::int64_t) parameter.device.getRaw();

    if (const auto* node = findDevice (id))
    {
        if (const auto index = findParameter (*node, parameter.parameter); index >= 0)
            return withModulation ? node->getPlayedValue (index) : node->getPlayedBaseValue (index);

        return {};
    }

    // A container's chain's volume, pan and crossover.
    if (const auto* chain = findChain (id))
    {
        if (parameter.parameter == schema::chainVolumeParameter)
            return decibelsToFaderPosition (chain->getFader().getPlayedVolume (withModulation));

        if (parameter.parameter == schema::chainPanParameter)
            return chain->getFader().getPlayedPan (withModulation);

        if (parameter.parameter == schema::crossoverParameter)
            return chain->getPlayedCrossover (withModulation);

        return {};
    }

    const auto fader = faders.find (parameter.device.getRaw());
    auto* strip = fader != faders.end() ? findStrip (fader->second) : nullptr;

    if (strip == nullptr)
        return {};

    if (parameter.parameter == "volume" || parameter.parameter == "master volume")
        return decibelsToFaderPosition (strip->getChannel().getPlayedVolume (withModulation));

    if (parameter.parameter == "pan" || parameter.parameter == "master pan")
        return strip->getChannel().getPlayedPan (withModulation);

    return {};
}

void SongPlayer::setWriting (const ParameterRef& parameter, bool isWriting)
{
    const auto id = (std::int64_t) parameter.device.getRaw();

    if (auto* node = findDevice (id))
    {
        const auto index = findParameter (*node, parameter.parameter);
        node->setWriting (index, isWriting);

        // A plug-in's written by hand plays its value set by hand, which goes to it at once as it is set, unless modulated.
        if (auto* processor = dynamic_cast<SandboxedProcessor*> (&node->getRunner().getProcessor()))
            processor->getInstance().setMovedByEngine (index,
                                                       (! isWriting && findCurve (parameter) != nullptr)
                                                           || modulated.contains ({ parameter.device.getRaw(), parameter.parameter }));

        return;
    }

    if (auto* chain = findChain (id))
    {
        if (parameter.parameter == schema::chainVolumeParameter)
            chain->getFader().setVolumeWriting (isWriting);
        else if (parameter.parameter == schema::chainPanParameter)
            chain->getFader().setPanWriting (isWriting);
        else if (parameter.parameter == schema::crossoverParameter)
            chain->setCrossoverWriting (isWriting);

        return;
    }

    const auto fader = faders.find (parameter.device.getRaw());

    if (auto* strip = fader != faders.end() ? findStrip (fader->second) : nullptr)
    {
        if (parameter.parameter == "volume" || parameter.parameter == "master volume")
            strip->getChannel().setVolumeWriting (isWriting);
        else if (parameter.parameter == "pan" || parameter.parameter == "master pan")
            strip->getChannel().setPanWriting (isWriting);
    }
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

std::unique_ptr<engine::SampleReader> makeSampleReader (std::unique_ptr<juce::AudioFormatReader> reader)
{
    return std::make_unique<FormatReader> (std::move (reader));
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongPlayer.h"
#include "devices/drums/DrumSampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "internal/PluginProcessor.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumSamplerPlugin.h"
#include "model/devices/internal/WavetablePlugin.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
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

    // A modulator's kind, as the engine plays it.
    devices::ModulatorKind toKind (ModulatorType type)
    {
        switch (type)
        {
            case ModulatorType::envelopeFollower:
                return devices::ModulatorKind::envelopeFollower;
            case ModulatorType::steps:
                return devices::ModulatorKind::steps;
            case ModulatorType::random:
                return devices::ModulatorKind::random;
            case ModulatorType::breakpoints:
                return devices::ModulatorKind::breakpoints;
            case ModulatorType::noteFollower:
                return devices::ModulatorKind::noteFollower;
            case ModulatorType::lfo:
            case ModulatorType::macro:
                break;
        }

        return devices::ModulatorKind::lfo;
    }

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
        modulators.clear();
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
    compilingRender = isRender;
    used.clear();
    usedFiles.clear();
    usedRegions.clear();
    usedModulators.clear();
    modulated.clear();
    readCurves();

    // The song's channels, each a strip: its tracks, its buses (folders with faders of their own: a plain folder's VCA
    // turns its tracks) and its returns, in an order in which each comes after what it takes in.
    usedDelays.clear();
    const auto channels = getChannels();
    std::map<std::int64_t, int> places; // each channel's strip, by its ID

    for (const auto& channel : channels)
    {
        engine::Plan::Strip strip;
        strip.id = channel.id;
        strip.state = getStrip (strip.id);

        // A bus adds up its tracks; a return what is sent to it.
        std::vector<engine::Plan::Input> inputs;

        for (const auto& other : channels)
        {
            if (! places.contains (other.id) || channel.refuses (other.id))
                continue;

            if (other.output == channel.id && ! isReturnState (channel.state))
                inputs.push_back ({ places[other.id], nullptr });

            for (const auto& send : other.state)
            {
                if (send.hasType (schema::SEND) && getID (send, schema::sendTo) == channel.id && isReturnState (channel.state))
                {
                    const auto beforeFader = (bool) send[schema::sendPreFader];
                    inputs.push_back ({ places[other.id], nullptr, beforeFader, getSendLevel (send) });
                    plan->strips[(size_t) places[other.id]].feedsBeforeFader |= beforeFader;
                }
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
                strip.notes = compileNotes (channel.state);
                addAudio (strip, channel.state);
            }
        }

        const auto deviceIDs = addDevices (*plan, places, channel, strip, channel.state, frozen);

        if (! isRender && ! strip.hasInstrument && ! isFolderState (channel.state) && ! isReturnState (channel.state))
            strip.inputChannels = getHeardInput (channel.state);

        addFaderAutomation (strip, channel.state, false);
        addModulation (strip, channel.state, deviceIDs);
        setChannel (channel.state, false);
        places[channel.id] = (int) plan->strips.size();
        plan->strips.push_back (std::move (strip));
    }

    updateAudibility();

    // The master adds up what goes to it: the tracks and buses in no bus, and the returns.
    engine::Plan::Strip master;
    master.state = getStrip (0);
    std::vector<engine::Plan::Input> inputs;

    for (const auto& channel : channels)
        if (channel.output == 0 && places.contains (channel.id))
            inputs.push_back ({ places[channel.id], nullptr });

    addInputs (*plan, master, std::move (inputs));
    const auto masterState = findPhaziteState (edit).getChildWithName (schema::MASTER);
    const auto masterDeviceIDs = addDevices (*plan, places, {}, master, masterState);
    addFaderAutomation (master, masterState, true);
    addModulation (master, masterState, masterDeviceIDs);
    setChannel (masterState, true);
    plan->strips.push_back (std::move (master));

    // What the song doesn't hold any more goes with the last plan that plays it.
    std::set<std::int64_t> tracks;

    for (const auto& strip : plan->strips)
        tracks.insert (strip.id);

    std::erase_if (devices, [this] (const auto& device) { return used.count (device.first) == 0; });
    std::erase_if (modulators, [this] (const auto& modulator) { return usedModulators.count (modulator.first) == 0; });
    std::erase_if (files, [this] (const auto& file) { return usedFiles.count (file.first) == 0; });
    std::erase_if (regions, [this] (const auto& region) { return usedRegions.count (region.first) == 0; });
    std::erase_if (strips, [&tracks] (const auto& strip) { return tracks.count (strip.first) == 0; });
    std::erase_if (delays, [this] (const auto& delay) { return usedDelays.count (delay.first) == 0; });
    std::erase_if (sendLevels, [this] (const auto& level) { return usedSends.count (level.first) == 0; });
    std::erase_if (containers, [this] (const auto& container) { return usedContainers.count (container.first) == 0; });
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
    // A plug-in, live: the instance Tracktion loaded (a render plays plug-ins from T5, which renders on Phazite's engine).
    if (deviceState[schema::deviceType].toString() == schema::deviceIsPlugin)
        return compilingRender ? nullptr : getPluginDevice (deviceState);

    // Phazite's own devices.
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

std::shared_ptr<engine::DeviceNode> SongPlayer::getPluginDevice (const juce::ValueTree& deviceState)
{
    // One that is loaded (not one missing on this computer), kept by its ID while Tracktion keeps the same plug-in.
    const auto plugin = te::findPluginForID (edit, toTracktion (getDeviceID (deviceState)));
    auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());

    if (external == nullptr || external->getAudioPluginInstance() == nullptr)
        return nullptr;

    const auto id = getID (deviceState, schema::deviceID);
    auto& node = devices[id];
    const auto* playing = node != nullptr ? dynamic_cast<const PluginProcessor*> (&node->getRunner().getProcessor()) : nullptr;

    if (playing == nullptr || &playing->getPlugin() != plugin.get())
    {
        auto processor = std::make_unique<PluginProcessor> (*plugin);
        const auto& info = processor->getInfo();
        node = std::make_shared<engine::DeviceNode> (info, std::move (processor));
        node->getRunner().prepare (preparedRate, preparedBlockSize);
    }

    node->setOn (! (bool) deviceState[schema::deviceOff]);
    used.insert (id);
    return node;
}

std::vector<std::int64_t> SongPlayer::addDevices (engine::Plan& plan, const std::map<std::int64_t, int>& places, const ChannelInfo& channel,
                                                  engine::Plan::Strip& strip, const juce::ValueTree& channelState, bool skipFrozen)
{
    std::vector<std::int64_t> ids;

    // While Low Latency is on, a device that looks ahead lets the sound through as it comes (model/engine/internal/EngineSetup.h).
    const auto lowLatencyLimit = (int) (lowLatencyLimitSeconds * preparedRate);

    // A frozen track's devices before its freeze point are in its frozen file.
    const auto isFrozen = [this, skipFrozen] (const juce::ValueTree& state)
    {
        if (skipFrozen)
            if (const auto plugin = te::findPluginForID (edit, toTracktion (getDeviceID (state))))
                return isFrozenDevice (*plugin);

        return false;
    };

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

        // Its parameters' automation, as late as its sound comes.
        engine::Plan::Device device { std::move (node), {}, strip.latency };
        const auto id = getDeviceID (deviceState).getRaw();
        const auto& parameters = device.node->getDeviceInfo().parameters;

        for (size_t i = 0; i < parameters.size(); ++i)
            if (auto curve = findCurve (id, juce::String (parameters[i].id.data(), parameters[i].id.size())))
                device.automation.push_back ({ (int) i, std::move (curve) });

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

        strip.latency += latency;
        strip.devices.push_back (std::move (device));
        ids.push_back ((std::int64_t) id);
    };

    for (const auto& deviceState : channelState)
    {
        if (deviceState.hasType (schema::DEVICE))
        {
            add (deviceState);
        }
        else if (deviceState.hasType (schema::CONTAINER) && ! (bool) deviceState[schema::deviceOff] && ! isFrozen (deviceState))
        {
            // A container: its devices, then its mix of their sound with what came in, delayed as much as they delay it.
            const auto first = (int) strip.devices.size();
            const auto latencyBefore = strip.latency;

            for (const auto& inside : deviceState)
                if (inside.hasType (schema::DEVICE))
                    add (inside);

            if ((int) strip.devices.size() > first)
            {
                const auto id = getID (deviceState, schema::deviceID);
                auto& kept = containers[id];

                if (kept == nullptr || kept->getLatency() != strip.latency - latencyBefore)
                    kept = std::make_shared<engine::ContainerState> (preparedBlockSize, strip.latency - latencyBefore);

                kept->setMix ((float) deviceState.getProperty (schema::deviceMix, 1.0f));
                usedContainers.insert (id);
                strip.containers.push_back ({ first, (int) strip.devices.size() - 1, kept });
            }
        }
    }

    return ids;
}

std::vector<SongPlayer::ChannelInfo> SongPlayer::getChannels() const
{
    // Each track's, bus's and return's: where it goes (the bus it is in, or the master), and what it takes in.
    std::vector<ChannelInfo> infos;

    for (const auto& state : getTrackStates (edit))
    {
        if (isFolderState (state) && ! state.hasProperty (schema::channelFader))
            continue; // a plain folder's VCA turns its tracks (setChannel())

        ChannelInfo info { state, getID (state, schema::trackID) };

        if (! isReturnState (state))
            for (auto parent = state.getParent(); parent.hasType (schema::TRACK) && info.output == 0; parent = parent.getParent())
                if (parent.hasProperty (schema::channelFader))
                    info.output = getID (parent, schema::trackID);

        infos.push_back (std::move (info));
    }

    for (auto& info : infos)
    {
        for (const auto& other : infos)
        {
            if (other.output == info.id && ! isReturnState (info.state))
                info.needs.push_back (other.id);

            for (const auto& send : other.state)
                if (send.hasType (schema::SEND) && getID (send, schema::sendTo) == info.id && isReturnState (info.state))
                    info.needs.push_back (other.id);
        }

        const auto addSidechains = [&info] (const juce::ValueTree& chain, auto& addInside) -> void
        {
            for (const auto& device : chain)
            {
                if (const auto source = getID (device, schema::deviceSidechain); device.hasType (schema::DEVICE) && source != 0)
                    info.needs.push_back (source);
                else if (device.hasType (schema::CONTAINER))
                    addInside (device, addInside);
            }
        };

        addSidechains (info.state, addSidechains);
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
    // Each lined up with the latest (delay compensation): the strip's own sound comes in as late as that.
    for (const auto& input : inputs)
        strip.latency = std::max (strip.latency, plan.strips[(size_t) input.strip].latency);

    for (auto& input : inputs)
    {
        const auto& from = plan.strips[(size_t) input.strip];
        input.delay = getDelay ({ strip.id, from.id * 2 + (input.beforeFader ? 1 : 0) }, strip.latency - from.latency);
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
        const auto type = findModulatorType (state[schema::modulatorType].toString());

        if (! type.has_value() || *type == ModulatorType::macro)
            return nullptr;

        kind = toKind (*type);
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

    // Those set by hand, the others at Tracktion's defaults; its steps; and its switch.
    const auto parameters = devices::getModulatorParameters (*node.getKind());
    const auto values = state.getChildWithName (schema::VALUES);

    for (size_t i = 0; i < parameters.size(); ++i)
        node.setParameter ((int) i,
                           (float) values.getProperty (juce::Identifier (juce::String (parameters[i].id.data(), parameters[i].id.size())),
                                                       parameters[i].defaultValue));

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

            const auto place = std::find (deviceIDs.begin(), deviceIDs.end(), device);

            if (place == deviceIDs.end())
                continue;

            auto& entry = strip.devices[(size_t) (place - deviceIDs.begin())];
            const auto& info = entry.node->getDeviceInfo();
            const auto index = devices::findParameter (info, target.parameter.toStdString());

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

    for (const auto& state : getCurveStates (edit))
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
    // in is (a return is never silenced by a solo).
    const auto trackStates = getTrackStates (edit);
    const auto anySoloed =
        std::any_of (trackStates.begin(), trackStates.end(), [] (const auto& state) { return (bool) state[schema::trackSolo]; });

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

        for (auto track = state; track.hasType (schema::TRACK); track = track.getParent())
        {
            muted = muted || (bool) track[schema::trackMute];
            soloed = soloed || (bool) track[schema::trackSolo];
        }

        soloed = soloed || (isFolderState (state) && holdsSoloed (state, holdsSoloed));
        strip->getChannel().setAudible (! muted && (! anySoloed || soloed || isReturnState (state)));
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

std::vector<int> SongPlayer::getHeardInput (const juce::ValueTree& trackState) const
{
    const auto monitoring = trackState[schema::trackMonitoring].toString();
    const auto* recorder = AudioRecorder::findFor (edit);
    const auto armed = recorder != nullptr && recorder->isArmed (getTrackID (trackState));

    if (monitoring == schema::monitorAlways || (armed && monitoring.isEmpty()))
        return getInputChannels (trackState[schema::trackInput].toString());

    return {};
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

const engine::Curve* SongPlayer::findCurve (const ParameterRef& parameter) const
{
    return findCurve (parameter.device.getRaw(), parameter.parameter).get();
}

std::optional<float> SongPlayer::getPlayedValue (const ParameterRef& parameter, bool withModulation) const
{
    const auto id = (std::int64_t) parameter.device.getRaw();

    if (const auto* node = findDevice (id))
    {
        if (const auto index = devices::findParameter (node->getDeviceInfo(), parameter.parameter.toStdString()); index >= 0)
            return withModulation ? node->getPlayedValue (index) : node->getPlayedBaseValue (index);

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
        node->setWriting (devices::findParameter (node->getDeviceInfo(), parameter.parameter.toStdString()), isWriting);
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
} // namespace phazite

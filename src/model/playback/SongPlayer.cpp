// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongPlayer.h"
#include "devices/drums/DrumSampler.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "internal/PlaybackHost.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Numbers.h"
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
#include "model/notes/internal/NoteElements.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedProcessor.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    // A note's or a controller's MIDI channel, as the engine takes it: the song's 1 to 16 as 0 to 15.
    std::uint8_t toEngineChannel (int channel)
    {
        return (std::uint8_t) (juce::jlimit (1, 16, channel) - 1);
    }

    // A number mixed into what tells a strip's instrument from another (engine::Plan::Strip::instrument), FNV-1a's way.
    std::uint64_t mixInto (std::uint64_t mixed, std::uint64_t number) noexcept
    {
        constexpr std::uint64_t basis = 14695981039346656037ull, prime = 1099511628211ull;
        return ((mixed == 0 ? basis : mixed) ^ number) * prime;
    }

    // What a track's MIDI clips play: each clip's notes and controller events where the clip is, cut to it (a note that
    // started before the clip's start plays from it, one that goes past its end stops there), each on its channel; none of a
    // muted clip or note. The clips in the order the track holds them (Clips.h: getClipValues()), which ties between their
    // events keep.
    std::shared_ptr<const engine::NoteSequence> compileNotes (const Song& song, const juce::ValueTree& trackState)
    {
        std::vector<engine::NoteSequence::Note> notes;
        std::vector<engine::NoteSequence::Control> controls;

        for (const auto& clip : getClipValues (song, getTrackID (trackState)))
        {
            if (! clip.isMidi() || clip.muted)
                continue;

            const auto start = clip.start.inBeats();
            const auto end = start + clip.length.inBeats();
            const auto contentStart = start - clip.offset.inBeats();

            forEachEvent (
                getClipNotesValue (song, clip.id),
                [&] (const NoteValues& note)
                {
                    if (note.muted)
                        return;

                    const auto from = contentStart + note.start.inBeats();
                    const auto to = from + note.length.inBeats();

                    if (const auto playedFrom = std::max (from, start), playedTo = std::min (to, end); playedTo > playedFrom)
                        notes.push_back ({ playedFrom, playedTo, note.pitch, (float) note.velocity / 127.0f, note.slide, note.portamento,
                                           toEngineChannel (note.channel) });
                },
                [&] (const ControlValues& control)
                {
                    const auto beat = contentStart + control.beat;

                    if (beat < start || beat >= end)
                        return;

                    const auto value = control.value;
                    const auto channel = toEngineChannel (control.channel);

                    switch (control.kind)
                    {
                        case ControlKind::controller:
                            controls.push_back ({ beat, EventType::controller, control.number, (float) value / 127.0f, channel });
                            break;
                        case ControlKind::pitchWheel:
                            controls.push_back ({ beat, EventType::pitchBend, 0, (float) (value - 8192) / 8192.0f, channel });
                            break;
                        case ControlKind::channelPressure:
                            controls.push_back (
                                { beat, EventType::channelPressure, 0, (float) juce::jlimit (0, 127, value) / 127.0f, channel });
                            break;
                        case ControlKind::aftertouch:
                            controls.push_back (
                                { beat, EventType::aftertouch, control.number, (float) juce::jlimit (0, 127, value) / 127.0f, channel });
                            break;
                        case ControlKind::programChange:
                            controls.push_back ({ beat, EventType::programChange, juce::jlimit (0, 127, value), 0.0f, channel });
                            break;
                    }
                });
        }

        if (notes.empty() && controls.empty())
            return nullptr;

        return std::make_shared<const engine::NoteSequence> (std::move (notes), std::move (controls));
    }

    // Whether two tracks' notes play alike, event for event (a kept track's and its notes compiled again, while checking).
    bool isSameSequence (const engine::NoteSequence* a, const engine::NoteSequence* b)
    {
        if (a == nullptr || b == nullptr)
            return a == b;

        const auto same = [] (const engine::NoteSequence::Event& x, const engine::NoteSequence::Event& y)
        {
            return juce::exactlyEqual (x.beat, y.beat) && x.type == y.type && x.number == y.number && juce::exactlyEqual (x.value, y.value)
                   && x.channel == y.channel;
        };

        return std::equal (a->getEvents().begin(), a->getEvents().end(), b->getEvents().begin(), b->getEvents().end(), same);
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

        // JUCE's WAV, AIFF and FLAC readers give a place read again exactly as before (without loss, sought to the sample);
        // its MP3 reader starts decoding again from a frame before the place, and Windows Media's seeks by time: not those,
        // nor Ogg Vorbis, whose decoder isn't shown to give the same after a seek.
        bool readsAgainTheSame() const override
        {
            const auto& name = reader->getFormatName();
            return name == "WAV file" || name == "AIFF file" || name == "FLAC file";
        }

    private:
        std::unique_ptr<juce::AudioFormatReader> reader;
    };

    // A device's values set by hand, in its description's order, as its node has them now (SongPlayer::readValues() reads
    // the song's).
    std::vector<float> readValuesSetByHand (const engine::DeviceNode& node)
    {
        std::vector<float> values;

        for (int i = 0; i < (int) node.getDeviceInfo().parameters.size(); ++i)
            values.push_back (node.getValue (i));

        return values;
    }

    // The second half of the key of a device's own sound held back for its sidechain (Plan::Device::inputDelay): no track,
    // chain or part has it.
    constexpr std::int64_t ownSound = -1;

    // The room a path's ring has besides its delay: a third of a second at a rate, as a power of two (16,384 samples at 44.1
    // and 48 kHz, 65,536 at 192 kHz).
    int getRingRoom (double sampleRate)
    {
        auto room = 1;

        while (room < toWhole<int> (std::ceil (sampleRate / 3.0)))
            room *= 2;

        return room;
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

    if (auto* values = song.getValues())
        values->addListener (this);
}

SongPlayer::~SongPlayer()
{
    if (auto* values = song.getValues())
        values->removeListener (this);

    songState.removeListener (this);
}

void SongPlayer::clipNotesChanged (const std::vector<ClipID>& clips)
{
    // Their tracks are found as the next compile starts (compile()): a listener of the values reads nothing of the song.
    changes.noteClips.insert (changes.noteClips.end(), clips.begin(), clips.end());
}

void SongPlayer::clipsChanged (const std::vector<SongValues::ClipChange>& clips, const std::vector<TrackID>& reordered)
{
    // The tracks a clip was on and is on, and those whose clips play in another order (a track's notes are put together, and
    // its audio's regions kept, in its clips' order): their notes and audio.
    for (const auto& clip : clips)
        for (const auto track : { clip.before, clip.after })
            if (track.isValid())
                changes.notes.insert ((std::int64_t) track.getRaw());

    for (const auto track : reordered)
        changes.notes.insert ((std::int64_t) track.getRaw());
}

void SongPlayer::curvesChanged (const std::vector<ParameterRef>& parameters)
{
    // Each curve read again by the next compile (readCurves()): a curve given another parameter is told by both, the one it
    // had (gone from the song) and the one it has.
    for (const auto& parameter : parameters)
        changes.curves.insert ({ parameter.device.getRaw(), parameter.parameter });
}

void SongPlayer::modulationChanged (const std::vector<SongValues::ModulationChange>& changed, const std::vector<ModulatorPlace>& reordered)
{
    // The channel each was on and is on (the master's under 0), as a change at or under a modulator marked the channel it is
    // on (markChange()): every part, a value taken at once too (the next plan reads its devices again either way).
    const auto mark = [this] (const ModulatorPlace& place)
    {
        if (place.onMaster)
            changes.channels.insert (0);
        else if (place.track.isValid())
            changes.channels.insert ((std::int64_t) place.track.getRaw());
    };

    for (const auto& modulator : changed)
    {
        if (modulator.wasThere)
            mark (modulator.before);

        if (modulator.isThere)
            mark (modulator.after);
    }

    for (const auto& place : reordered)
        mark (place);
}

void SongPlayer::sendsChanged (const std::vector<SongValues::SendChange>& changed, const std::vector<TrackID>& reordered,
                               const SongSends& now)
{
    // The channel a send was added to, taken out of, or came to and from, as a child added to a channel's element or taken out
    // of it marks it (markChange()); a route or a level in place none.
    using Change = SongValues::SendChange;

    for (const auto& send : changed)
        if (send.has (Change::made))
            for (const auto track : { send.fromBefore, send.fromAfter })
                if (track.isValid())
                    changes.channels.insert ((std::int64_t) track.getRaw());

    for (const auto track : reordered)
        changes.channels.insert ((std::int64_t) track.getRaw());

    heldSends = now; // what updateAudibility() reads, as the song holds it now
}

namespace
{
    std::atomic<bool> checking { false };
    std::atomic<std::uint64_t> devicesRead { 0 }, curvesRead { 0 }, filesAsked { 0 };
} // namespace

void SongPlayer::setChecking (bool on) noexcept
{
    checking = on;
}

bool SongPlayer::isChecking() noexcept
{
    return checking;
}

std::uint64_t SongPlayer::countDevicesRead() noexcept
{
    return devicesRead;
}

std::uint64_t SongPlayer::countCurvesRead() noexcept
{
    return curvesRead;
}

std::uint64_t SongPlayer::countFilesAsked() noexcept
{
    return filesAsked;
}

bool SongPlayer::isCheckedPart (std::uint64_t part, std::uint64_t plan, std::uint64_t every) noexcept
{
    // Part j of plan p where j and p leave the same remainder: any `every` plans in a row check every part at least once,
    // however many parts each keeps.
    return every <= 1 || part % every == plan % every;
}

bool SongPlayer::shouldCheck()
{
    // Each kept part in a song of up to 64 devices; in a bigger one, one in every so many, its phase moving on each plan,
    // so that the checks cost about as much as what they check would for 64 devices.
    return checking && isCheckedPart (checkedParts++, plansCompiled, checkEvery);
}

//==============================================================================
std::shared_ptr<const engine::NoteSequence> SongPlayer::getNotes (const juce::ValueTree& trackState)
{
    const auto id = getID (trackState, schema::trackID);
    usedNotes.insert (id);

    if (const auto found = notes.find (id); found != notes.end() && ! compiling.hasNotes (id))
    {
        if (shouldCheck())
        {
            [[maybe_unused]] const auto same = isSameSequence (found->second.get(), compileNotes (song, trackState).get());
            jassert (same); // the track's notes changed without the song saying so (markChange())
        }

        return found->second;
    }

    auto compiled = compileNotes (song, trackState);
    notes[id] = compiled;
    return compiled;
}

void SongPlayer::markChange (const juce::ValueTree& changed, bool structure)
{
    // The clips and their notes are values beside the tree, heard by clipsChanged() and clipNotesChanged(); the tempo map is
    // heard by tempoMapChanged() (SongValues.h: its tree callbacks below leave it alone, isHeardAsKept()), which marks
    // every track's audio, which plays in seconds (a tempo change moves no note: they are in beats); the curves by
    // curvesChanged() (isHeardAsKept(), through isHeardAsCurves()), each by its parameter; the modulators and macros by
    // modulationChanged() (the tree holds none of them), each by its channel; the sends by sendsChanged() (the tree holds
    // none of them either), by the tracks they were added to or taken out of. In a channel (a track, a
    // folder, a return or the master), its devices' values are read again only for what can change them: a change at or under
    // a device, a container or a chain, or an element of the channel added, removed or moved; not the channel's
    // own properties (its height, mute, solo, volume, pan), which no device reads, nor a plug-in's own values (VALUE), which
    // the plug-in keeps, the engine reading none.
    auto inDevice = structure;

    for (auto element = changed; element.isValid(); element = element.getParent())
    {
        // A device's sounds (a pad's, an oscillator's table, a sampler's): loaded again by a kept node (getDevice()); one set by
        // hand (a property of the SOUNDS element), unless its session loaded it at once (takeValue()).
        if (element.hasType (schema::SOUNDS) && element.getParent().hasType (schema::DEVICE))
            (! structure && element == changed ? changes.soundsSet : changes.sounds).insert (getID (element.getParent(), schema::deviceID));

        if (element.hasType (schema::VALUE) || schema::isUncountedElement (element.getParent(), element))
            return; // a plug-in's value, or view state (a track's lanes)

        if (element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::CHAIN))
            inDevice = true;

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
        {
            if (inDevice)
                changes.channels.insert (element.hasType (schema::MASTER) ? 0 : getID (element, schema::trackID));

            return;
        }
    }
}

void SongPlayer::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // The areas' (isHeardAsKept()): the tempo map's, tempoMapChanged(); the chord lane's, which nothing played reads
    // (chordLaneChanged(): SongPlayer follows none of it); the curves', curvesChanged().
    if (isHeardAsKept (tree))
        return;

    // A track's ID changed in place (a merge's renumbering): what the plan before kept under it may be another track's, and
    // its own is under an ID no longer known.
    if (tree.hasType (schema::TRACK) && property == schema::trackID)
        changes.allNotes = changes.allChannels = true;

    // What a device's player doesn't read: where its window was, and a plug-in's own state, which every save stores
    // (Song::storeDeviceStates()).
    if (tree.hasType (schema::DEVICE)
        && (schema::isDeviceViewProperty (property) || property == schema::pluginState || property == schema::pluginProgram))
        return;

    markChange (tree, false);
}

void SongPlayer::markTracksIn (const juce::ValueTree& element)
{
    if (element.hasType (schema::TRACK))
    {
        changes.notes.insert (getID (element, schema::trackID));
        changes.channels.insert (getID (element, schema::trackID));
    }

    if (element.hasType (schema::MASTER))
        changes.channels.insert (0);

    if (element.hasType (schema::TRACK) || element.hasType (schema::TRACKS) || element.hasType (schema::PHAZITE))
        for (const auto& child : element)
            if (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS) || child.hasType (schema::MASTER))
                markTracksIn (child);
}

void SongPlayer::markSoundsIn (const juce::ValueTree& element)
{
    // Sounds added whole, with what they hold (an undo, a song applied, a preset's element): no property of theirs changes.
    if (element.hasType (schema::SOUNDS) && element.getParent().hasType (schema::DEVICE))
        changes.sounds.insert (getID (element.getParent(), schema::deviceID));

    if (element.hasType (schema::DEVICE))
        changes.sounds.insert (getID (element, schema::deviceID));

    for (const auto& child : element)
        markSoundsIn (child);
}

void SongPlayer::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // Something added to a channel; or tracks added, which may hold notes and devices already (an undo), or the master in
    // place of another (a song applied). View state added (a track's lanes) changes nothing that plays. The areas'
    // (isHeardAsKept()): the tempo map's, tempoMapChanged(); the chord lane's, which nothing played reads; the curves',
    // curvesChanged().
    if (isHeardAsKept (parent, child))
        return;

    if (! schema::isUncountedElement (parent, child))
        markChange (parent, true);

    markTracksIn (child);
    markSoundsIn (child);
}

void SongPlayer::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (isHeardAsKept (parent, child))
        return; // the areas': tempoMapChanged(); the chord lane's, which nothing played reads; curvesChanged()

    // A device's sounds taken away whole: its own again.
    if (child.hasType (schema::SOUNDS) && parent.hasType (schema::DEVICE))
        changes.sounds.insert (getID (parent, schema::deviceID));

    if (! schema::isUncountedElement (parent, child))
        markChange (parent, true);

    markTracksIn (child);
}

void SongPlayer::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    // A track's devices, a chain's devices, a channel's modulators, tracks in a folder: reordered. The areas'
    // (isHeardAsKept()): the tempo map's, tempoMapChanged(); the chord lane's, which nothing played reads; the curves',
    // curvesChanged().
    if (! isHeardAsKept (parent))
        markChange (parent, true);
}

void SongPlayer::letGoOfPlugins()
{
    for (const auto& [id, node] : devices)
        if (auto* processor = dynamic_cast<SandboxedProcessor*> (&node->getRunner().getProcessor()))
            processor->getInstance().setMovedByEngine (std::span<const int>());
}

void SongPlayer::releaseUnused()
{
    for (const auto& [id, node] : devices)
        node->getRunner().getProcessor().releaseUnused();
}

void SongPlayer::makePluginsAnew()
{
    std::erase_if (devices, [] (const auto& device)
                   { return dynamic_cast<SandboxedProcessor*> (&device.second->getRunner().getProcessor()) != nullptr; });
}

std::unique_ptr<engine::Plan> SongPlayer::compile (double sampleRate, int maximumBlockSize, const std::optional<Live>& live,
                                                   const std::optional<RenderOf>& of)
{
    // What changed since the plan before, taken whole as the compile starts: a change heard while it compiles is the next
    // plan's. And the parts a check looks at, in turn (shouldCheck()).
    compiling = std::exchange (changes, {});

    // A clip whose notes changed: its track's notes, as a change in the clip's element marks them (markChange()). One that is
    // gone isn't on a track any more: taking it off marked the track it was on.
    for (const auto clip : compiling.noteClips)
        if (const auto track = getClipTrack (song, clip); track.isValid())
            compiling.notes.insert ((std::int64_t) track.getRaw());

    compiling.noteClips.clear();
    ++plansCompiled;
    checkedParts = 0;
    checkEvery = std::max<std::uint64_t> (1, devices.size() / 64);

    // Another rate or block size: everything that plays is made anew for it.
    if (! juce::exactlyEqual (sampleRate, preparedRate) || maximumBlockSize != preparedBlockSize)
    {
        devices.clear();
        strips.clear();
        delays.clear();
        pathLengths.clear();
        pathDestinations.clear();
        leftInto.clear();
        taps.clear();
        modulators.clear();
        containers.clear();
        chains.clear();
        audio.clear();
        preparedRate = sampleRate;
        preparedBlockSize = maximumBlockSize;
    }

    // The tracks' audio placed again, every one, for another tempo, or with its files found in other folders (the project
    // saved somewhere else).
    if (const auto folders = song.getFile().getFullPathName() + "|" + song.getCacheFolder().getFullPathName();
        compiling.tempo || folders != audioFolders)
    {
        audio.clear();

        if (folders != audioFolders)
            fileKeys.clear();

        audioFolders = folders;
    }

    askedForPlan.clear();
    filesChangedForPlan = false;
    trustedForPlan.clear();
    std::optional<ClipTempo> clipTempo; // read once, if a track's audio is placed again

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
    usedSources.clear();
    usedRegions.clear();
    usedNotes.clear();
    usedModulators.clear();
    usedSends.clear();
    usedContainers.clear();
    usedChains.clear();
    modulated.clear();
    sendModulation.clear();
    readCurves();

    // The song's channels, each a strip: its tracks, its buses (folders with faders of their own: a plain folder's VCA
    // turns its tracks) and its returns, in an order in which each comes after what it takes in.
    usedDelays.clear();
    tappedPaths.clear();
    tapParts.clear();
    compilingAfresh = pathLengths.empty(); // the first plan, or the first for another rate or block size: no path is new
    compilingLive = live.has_value();
    const auto songSends = getSendsValue (song); // the song's sends, read once for every channel's (getChannels())
    const auto channels = getChannels (songSends);
    std::map<std::int64_t, int> places; // each channel's strip, by its ID

    // The pads on mixer channels of their own, by their samplers, which play each into an output of its own.
    separatedPads.clear();
    partLatencies.clear();
    usedPartOutputs.clear();

    for (const auto& channel : channels)
        if (channel.pad >= 0 && channel.padSource != 0 && ! channel.refuses (channel.padSource))
            separatedPads[channel.padDevice].insert (channel.pad);

    // The song's modulators and macros, read once for every channel's (addModulation()).
    const auto songModulation = getModulationValue (song);

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
                if ((std::int64_t) send.to.getRaw() == channel.id)
                {
                    // After the fader, the track's sound before its mute: the send's cut says whether it sends
                    // (updateAudibility()), so that a track soloed out still sends to a return that is soloed. Its level
                    // follows its automation and its track's modulators (addModulation(), as the track was compiled).
                    const auto beforeFader = send.preFader;
                    const auto sendID = (std::int64_t) send.id.getRaw();
                    const auto modulation = sendModulation.find (sendID);
                    inputs.push_back ({ places[other.id], nullptr, beforeFader, getSendLevel (send), nullptr, ! beforeFader,
                                        findCurve ((std::uint64_t) sendID, schema::sendLevelParameter),
                                        modulation != sendModulation.end() ? modulation->second : std::vector<engine::Plan::Route>() });
                    plan->strips[(size_t) places[other.id]].feedsBeforeFader |= beforeFader;
                    plan->strips[(size_t) places[other.id]].feedsBeforeMute |= ! beforeFader;
                }
            }
        }

        // A pad track adds up its sampler's pad, as the sampler plays it into an output of its own (whose track plays while it
        // is muted, as every track does), so that the pad follows its own track's mute.
        if (channel.pad >= 0 && places.contains (channel.padSource) && ! channel.refuses (channel.padSource))
            if (const auto output = partOutputs.find ({ channel.padDevice, channel.pad });
                output != partOutputs.end() && usedPartOutputs.contains (output->first))
                inputs.push_back ({ places[channel.padSource], nullptr, false, nullptr, output->second });

        addInputs (*plan, strip, std::move (inputs));

        // A frozen track plays its frozen file, through the devices after its freeze point; any other its clips.
        auto frozen = false;

        if (! isFolderState (channel.state) && ! isReturnState (channel.state))
        {
            frozen = addFrozenFile (strip, channel.state);

            if (! frozen)
            {
                strip.notes = getNotes (channel.state);
                addAudio (strip, channel.state, clipTempo);
            }
        }

        const auto* only = of.has_value() && channel.id == of->channel && of->devices.has_value() ? &*of->devices : nullptr;
        const auto deviceIDs = addDevices (*plan, places, channel, strip, channel.state, frozen, only);

        if (live.has_value() && ! strip.hasInstrument && ! isFolderState (channel.state) && ! isReturnState (channel.state))
            strip.inputChannels = getHeardInput (channel.state);

        addFaderAutomation (strip, channel.state, false);
        addModulation (strip, songModulation, channel.state, deviceIDs);
        tellWhatMoves (strip);
        setChannel (channel.state, false);
        places[channel.id] = (int) plan->strips.size();
        plan->strips.push_back (std::move (strip));
    }

    // A file this plan found changed after it kept the audio of a track that plays it, or made it from what it trusted of
    // the file (a later track's clips changed, and asked about it again): that track's audio made again, so that every
    // track plays the file as it is now. One whose audio isn't kept whole (a render not made yet) made again in any case.
    if (filesChangedForPlan)
        for (const auto& [id, trackState] : std::exchange (trustedForPlan, {}))
            if (const auto kept = audio.find (id); kept == audio.end() || ! isOnDiskAsKept (kept->second))
                addAudio (plan->strips[(size_t) places[id]], trackState, clipTempo);

    // The song's sends as this plan read them (getChannels()), which sendsChanged() brings up to date until the next plan, for
    // what decides which send sends.
    heldSends = songSends;
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
        addModulation (master, songModulation, masterState, masterDeviceIDs);
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
    std::erase_if (fileKeys,
                   [this] (const auto& file) { return ! usedFiles.contains (file.second) && ! usedSources.contains (file.first); });
    std::erase_if (regions, [this] (const auto& region) { return usedRegions.count (region.first) == 0; });
    std::erase_if (notes, [this] (const auto& track) { return usedNotes.count (track.first) == 0; });
    std::erase_if (audio, [this] (const auto& track) { return usedNotes.count (track.first) == 0; });
    std::erase_if (strips, [&tracks] (const auto& strip) { return tracks.count (strip.first) == 0; });
    // A path gone (a track deleted or moved to another bus, a send taken away or switched before or after its fader): its
    // delay marked as going with this plan, which the engine fades it out alone across, nothing else changing (a path that
    // comes fades in itself); with the plans' departures, numbered (engine::Plan::departures). (A render's path with no
    // delay has none, and a render's plans don't fade.)
    auto departs = false;

    for (const auto& [key, delay] : delays)
    {
        if (usedDelays.count (key) != 0 || delay == nullptr)
            continue;

        // A device's own sound held back for its sidechain never leaves: its strip plays on without it. Where it waited
        // (its sidechain taken away, its key's track or the device deleted), the sound comes that much earlier from this
        // plan on, which the plans' alignment moves on for, whether or not another path's delay changed with it (a song of
        // the two tracks alone: the master's latency fell as much as the strip's, and no other path moved).
        if (key.second == ownSound)
        {
            realigned = realigned || delay->getLength() > 0;
            continue;
        }

        delay->setDeparture (departures + 1);
        departs = true;

        // What it went into holds what it added as it faded (markWhatReadsLeaving()).
        if (const auto into = pathDestinations.find (key); into != pathDestinations.end())
            leftInto[into->second] = juce::Time::getMillisecondCounter();
    }

    if (departs)
        ++departures;

    plan->departures = departures;
    std::erase_if (delays, [this] (const auto& delay) { return usedDelays.count (delay.first) == 0; });
    std::erase_if (pathLengths, [this] (const auto& path) { return usedDelays.count (path.first) == 0; });
    std::erase_if (pathDestinations, [this] (const auto& path) { return usedDelays.count (path.first) == 0; });

    // Its delay compensation, numbered (engine::Plan::alignment): it moves on when a path that plays on was lined up
    // otherwise in the plan before (the compensation really moved), and the engine fades what goes out across it. Any such
    // path: whether its sound is heard can't be told here for sure (a track unmuted between this and the audio thread
    // taking the plan), and a path from a channel that isn't heard can be through what listens to it (a send before a muted
    // track's fader, a soloed return, a sidechain), where its read point's jump would be heard; a 6 ms dip for a change
    // nobody hears is the lesser fault (the review of known bug 167).
    if (std::exchange (realigned, false))
        ++alignment;

    plan->alignment = alignment;

    // A render leaves out what it can't hear; then the rings what is left reads are made, on the strips that write them.
    if (! live.has_value())
        leaveOutWhatIsNotHeard (*plan);

    finishRings (*plan);

    if (live.has_value())
        markWhatReadsLeaving (*plan);

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

std::shared_ptr<engine::DeviceNode> SongPlayer::getDevice (const juce::ValueTree& deviceState, bool channelChanged)
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
    used.insert (id);

    // One on a channel whose elements didn't change since the plan before plays on as it is: its values set by hand are
    // the song's already (takeValue() takes each as it is set), and reading every device's again for each plan was most of a
    // big song's compile (known bug 145).
    if (node != nullptr && &node->getDeviceInfo() == info && ! channelChanged)
    {
        if (shouldCheck())
            checkValues (*node, deviceState);

        return node;
    }

    const auto values = readValues (*info, deviceState);
    ++devicesRead;

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
    else if (compiling.sounds.contains (id) || (compiling.soundsSet.contains (id) && ! compiling.soundsTaken.contains (id)))
    {
        // Its sounds changed since the plan before: as the song holds them now. Not a sound set by hand that its session loaded
        // at once (takeValue()): read again, every pad's or oscillator's, a sound file's comes back as a new sound, and every hit
        // of it still ringing fades out. Sounds that came whole (an undo, a song applied), and a player's without a session (a
        // render's), are loaded here.
        loadSounds (*node, deviceState);
    }

    // Those set by hand, the others at their defaults.
    for (size_t i = 0; i < values.size(); ++i)
        node->setValue ((int) i, values[i]);

    node->setOn (! (bool) deviceState[schema::deviceOff]);
    return node;
}

std::vector<float> SongPlayer::readValues (const devices::DeviceInfo& info, const juce::ValueTree& deviceState)
{
    auto& names = parameterNames[&info];

    if (names.size() != info.parameters.size())
    {
        names.clear();

        for (const auto& parameter : info.parameters)
            names.emplace_back (juce::String (parameter.id.data(), parameter.id.size()));
    }

    const auto saved = deviceState.getChildWithName (schema::VALUES);
    std::vector<float> values;
    values.reserve (info.parameters.size());

    for (size_t i = 0; i < info.parameters.size(); ++i)
        values.push_back (readSavedValue (info.parameters[i], saved.getProperty (names[i])));

    return values;
}

void SongPlayer::checkValues (const engine::DeviceNode& node, const juce::ValueTree& deviceState)
{
    // As getDevice() would set them, each kept to its range: a difference is a change of the song's that its player didn't
    // hear of (markChange()).
    const auto& info = node.getDeviceInfo();
    const auto values = readValues (info, deviceState);
    auto same = node.isOn() == ! (bool) deviceState[schema::deviceOff];

    for (size_t i = 0; i < values.size(); ++i)
        same = same && juce::exactlyEqual (node.getValue ((int) i), devices::constrain (info.parameters[i], values[i]));

    jassert (same);
    juce::ignoreUnused (same);
}

std::shared_ptr<engine::DeviceNode> SongPlayer::getPluginDevice (const juce::ValueTree& deviceState)
{
    // One that is loaded (not one missing on this computer), in its helper, kept by its ID while the song's instances keep
    // the same instance: through its channel, with its mix as the engine's.
    const auto sandboxed = plugins != nullptr ? plugins->find (getDeviceID (deviceState)) : nullptr;
    const auto id = getID (deviceState, schema::deviceID);

    if (sandboxed == nullptr)
    {
        // One still loading as its project opened: silence in its place (an effect that can't play is silent, never dry),
        // until it has, then its sound fades in (model/plugins/internal/SandboxedProcessor.h: LoadingPlugin).
        if (plugins == nullptr || ! plugins->isLoading (getDeviceID (deviceState)))
            return nullptr;

        auto& loading = devices[id];

        if (loading == nullptr || dynamic_cast<const LoadingPlugin*> (&loading->getRunner().getProcessor()) == nullptr)
        {
            auto processor = std::make_unique<LoadingPlugin> (getPluginIdentity (deviceState).isInstrument);
            const auto& info = processor->getInfo();
            loading = std::make_shared<engine::DeviceNode> (info, std::move (processor));
            loading->getRunner().prepare (preparedRate, preparedBlockSize);
        }

        loading->setOn (! (bool) deviceState[schema::deviceOff]);
        used.insert (id);
        return loading;
    }

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
    const auto lowLatencyLimit = toWhole<int> (lowLatencyLimitSeconds * preparedRate);

    // A frozen track's devices before its freeze point are in its frozen file.
    const auto isFrozen = [this, skipFrozen] (const juce::ValueTree& state)
    { return skipFrozen && isFrozenDevice (song, getDeviceID (state)); };

    const auto changed = compiling.hasChannel (channel.id); // the master's is 0

    const auto add = [&] (const juce::ValueTree& deviceState)
    {
        if (isFrozen (deviceState))
            return;

        auto node = getDevice (deviceState, changed);

        if (node == nullptr)
            return;

        const auto latency = node->isOn() ? node->getRunner().getLatencySamples() : 0;

        if (lowLatency && latency > lowLatencyLimit)
            return;

        if (strip.devices.empty() && node->getDeviceInfo().kind == devices::DeviceKind::instrument)
            strip.hasInstrument = true;

        engine::Plan::Device device { std::move (node) };
        const auto id = getDeviceID (deviceState).getRaw();

        // What tells an instrument from the one before in its place, from plan to plan: its device, and a plug-in's instance
        // with the times it was loaded again after a crash (one still loading, silent meanwhile, is another).
        if (device.node->getDeviceInfo().kind == devices::DeviceKind::instrument)
        {
            strip.instrument = mixInto (strip.instrument, (std::uint64_t) id);

            if (const auto* sandboxed = dynamic_cast<const SandboxedProcessor*> (&device.node->getRunner().getProcessor()))
            {
                strip.instrument = mixInto (strip.instrument, (std::uint64_t) reinterpret_cast<std::uintptr_t> (&sandboxed->getInstance()));
                strip.instrument = mixInto (strip.instrument,
                                            (std::uint64_t) (plugins != nullptr ? plugins->getGeneration (getDeviceID (deviceState)) : 0));
            }
        }

        // What its sidechain listens to, lined up with its sound. One that comes later than the device's own sound (a kick
        // through a linear-phase EQ keying a compressor on a bass) holds back what comes into the device by as much, so that
        // the duck meets its kick: the strip's sound waits for the key, and comes out as much later (its latency counts the
        // wait, which what is after it follows). The key's delay then needs none. The delay of its own is keyed by the
        // device's ID and ownSound (-1), which no other path has: a sidechain's is keyed by the device's ID and the track's (an ID,
        // never negative), a chain's and an input's by a chain's or a strip's ID, which no device has (the song gives each
        // item an ID of its own: Song::makeID()). Live, a device with a sidechain keeps one, as every path keeps a ring
        // (getDelay()), so that a wait that comes has its past. It is the strip's own sound, which goes on with or without it:
        // never a path that comes (it doesn't fade in as its sidechain is chosen) or leaves (compile() marks it with no
        // departure: with the sidechain taken away, the next plan plays the sound as it comes, across the realigning dip
        // where it had waited). An instrument's sound isn't held back (it takes notes): its
        // sidechain is lined up where it can be, a later track not heard earlier; nor is one that is off, which doesn't play
        // and so isn't waited for, as its latency isn't.
        if (const auto source = (std::int64_t) getID (deviceState, schema::deviceSidechain);
            source != 0 && places.contains (source) && ! channel.refuses (source))
        {
            auto& heard = plan.strips[(size_t) places.at (source)];
            heard.feedsSidechain = true;
            device.sidechain = places.at (source);

            if (device.node->getDeviceInfo().kind != devices::DeviceKind::instrument)
            {
                const auto wait = device.node->isOn() ? std::max (0, heard.latency - strip.latency) : 0;
                device.inputDelay = getDelay ({ (std::int64_t) id, ownSound }, wait, std::nullopt, {}, false);
                strip.latency += wait;
            }

            device.sidechainDelay = getDelay ({ (std::int64_t) id, source }, strip.latency - heard.latency,
                                              TapKey { source, engine::Plan::Strip::TapPoint::beforeMute });
        }

        // Its parameters' automation, as late as its sound comes (after a wait for its sidechain too): the song's curves of
        // the device (a plug-in can have thousands of parameters, few automated).
        device.latency = strip.latency;

        for (auto curve = curves.lower_bound ({ id, juce::String() }); curve != curves.end() && curve->first.first == id; ++curve)
            if (const auto index = findParameter (*device.node, curve->first.second); index >= 0)
                device.automation.push_back ({ index, curve->second });

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
    {
        chain.delay = getDelay ({ chain.id, 0 }, latencyBefore + latest - chain.latency);
        pathDestinations[{ chain.id, 0 }] = strip.id;
    }

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

std::vector<SongPlayer::ChannelInfo> SongPlayer::getChannels (const SongSends& songSends) const
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

        if (const auto* list = songSends.tracks.find ((std::uint64_t) info.id))
            for (const auto id : *list)
                if (const auto* send = songSends.find (id))
                    info.sends.push_back (*send);

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
                    if ((std::int64_t) send.to.getRaw() == info.id)
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

    // Each lined up with the latest (delay compensation): the strip's own sound comes in as late as that, which a send's
    // automation follows the song as late as.
    for (const auto& input : inputs)
        strip.latency = std::max (strip.latency, getLatency (input));

    for (auto& input : inputs)
    {
        input.latency = strip.latency;
        const auto& from = plan.strips[(size_t) input.strip];
        using Point = engine::Plan::Strip::TapPoint;
        const auto part = input.part != nullptr ? -1 - (from.id * engine::maximumParts + input.part->getPart()) : 0;
        const auto key =
            input.part != nullptr ? std::pair (strip.id, part) : std::pair (strip.id, from.id * 2 + (input.beforeFader ? 1 : 0));
        const auto point = input.part != nullptr ? Point::part
                           : input.beforeFader   ? Point::beforeFader
                           : input.beforeMute    ? Point::beforeMute
                                                 : Point::output;
        input.delay = getDelay (key, strip.latency - getLatency (input), TapKey { from.id, point, part }, input.part);
        pathDestinations[key] = strip.id;
        strip.inputs.push_back (std::move (input));
    }
}

std::shared_ptr<engine::Delay> SongPlayer::getDelay (std::pair<std::int64_t, std::int64_t> key, int length, std::optional<TapKey> tap,
                                                     std::shared_ptr<engine::PartOutput> part, bool comesAndGoes)
{
    // A path the last plan lined up otherwise (a device's latency changed, or one with latency came or went): the plans'
    // alignment moves on (compile()). A new path isn't one: its delay plays nothing until it holds as much of the past as it
    // reaches back, and fades in then. Nor is one that doesn't come and go with what the song routes (a device's own sound held back
    // for its sidechain): it never fades in as a path new to a song that plays does.
    const auto samples = std::max (0, length);

    const auto found = pathLengths.find (key);
    const auto isNew = comesAndGoes && found == pathLengths.end() && ! compilingAfresh;

    if (found != pathLengths.end() && found->second != samples)
        realigned = true;

    pathLengths[key] = samples;
    usedDelays.insert (key);
    auto& kept = delays[key];

    // A render plays the song once, as it is: a path with no delay needs none, and one with a delay a ring as long and no
    // longer (finishRings()), so that what a render keeps is what its delays reach back, however many paths it has.
    if (! compilingLive && samples == 0)
    {
        kept = nullptr;
        return nullptr;
    }

    // Live, every path keeps a ring, delayed or not, and keeps it when its delay goes back to none, so that one that comes
    // to need a delay has its past (known bug 167: the first latency on a sum silenced every other path for as long): room
    // for a third of a second more than its delay at the device's rate (16,384 samples at 48 kHz), so that a change within
    // that never goes silent; past it, a longer ring that reads the past from this one. The paths from one point of a strip
    // (a track's sends to its returns, a sidechain listening to it) share one, its tap, which it writes once a stretch, as
    // long as the longest of them needs and the stretch it writes; the others' (a container's chains) have their own.
    // Made empty: a render's plan may leave out the path (finishRings() makes the room of those it plays).
    const auto room = compilingLive ? getRingRoom (preparedRate) : 0;
    const auto fade = std::max (1, roundToWhole<int> (engine::Engine::fadeSeconds * preparedRate));

    if (tap.has_value())
    {
        tappedPaths[key] = { *tap, isNew };

        if (part != nullptr)
            tapParts[*tap] = std::move (part);

        auto& ring = taps[*tap];
        const auto needed = samples + preparedBlockSize;

        if (ring == nullptr || ring->getCapacity() < needed)
            ring = std::make_shared<engine::DelayLine> (needed + room, ring, false);

        if (kept == nullptr || ! kept->isTapped() || kept->getLength() != samples || kept->getLine() != ring)
            replace (kept, std::make_shared<engine::Delay> (ring, samples, fade, isNew, true));

        return kept;
    }

    if (kept == nullptr || kept->isTapped() || kept->getLength() != samples)
    {
        auto line = kept != nullptr && ! kept->isTapped() ? kept->getLine() : nullptr;

        if (line == nullptr || line->getCapacity() < samples || (! compilingLive && line->getCapacity() != samples))
            line = std::make_shared<engine::DelayLine> (samples + room, std::move (line), false);

        replace (kept, std::make_shared<engine::Delay> (std::move (line), samples, fade, isNew));
    }

    return kept;
}

void SongPlayer::replace (std::shared_ptr<engine::Delay>& kept, std::shared_ptr<engine::Delay> by) const
{
    // Live, the delay it takes the place of is held for the engine to see how much further back it reads, while that one
    // hasn't played (engine/core/Engine.h: Realign::hold); a render's plans don't fade.
    if (compilingLive)
        by->takePlaceOf (std::move (kept));

    kept = std::move (by);
}

void SongPlayer::finishRings (engine::Plan& plan)
{
    // The delays the plan plays (a render's, those of what it doesn't leave out).
    std::set<const engine::Delay*> played;

    const auto forEachDelay = [&plan] (auto&& take)
    {
        for (auto& strip : plan.strips)
        {
            for (auto& input : strip.inputs)
                take (input.delay);

            for (auto& device : strip.devices)
            {
                take (device.sidechainDelay);
                take (device.inputDelay);
            }

            for (auto& container : strip.containers)
                for (auto& chain : container.chains)
                    take (chain.delay);
        }
    };

    forEachDelay (
        [&played] (const std::shared_ptr<engine::Delay>& delay)
        {
            if (delay != nullptr)
                played.insert (delay.get());
        });

    // Each tap as long as the longest path it plays needs and the stretch its strip writes (a render's no longer: one that
    // was made longer for a path the render left out is made again, holding nothing yet); one nothing the plan plays reads
    // let go of.
    std::map<TapKey, int> needed;

    for (const auto& [key, path] : tappedPaths)
        if (const auto& delay = delays[key]; delay != nullptr && played.contains (delay.get()))
            needed[path.tap] = std::max (needed[path.tap], delay->getLength() + preparedBlockSize);

    std::erase_if (taps, [&needed] (const auto& tap) { return ! needed.contains (tap.first); });

    for (auto& [tap, ring] : taps)
        if (! compilingLive && ring->getCapacity() != needed[tap])
            ring = std::make_shared<engine::DelayLine> (needed[tap], ring, false);

    // The paths that read a tap made again since their delays were (a longer path from the same point came after them):
    // each a delay of the same length reading the new one.
    std::map<const engine::Delay*, std::shared_ptr<engine::Delay>> repointed;
    const auto fade = std::max (1, roundToWhole<int> (engine::Engine::fadeSeconds * preparedRate));

    for (const auto& [key, path] : tappedPaths)
    {
        auto& delay = delays[key];

        if (delay != nullptr && played.contains (delay.get()) && delay->getLine() != taps.at (path.tap))
        {
            const auto* was = delay.get();
            replace (delay, std::make_shared<engine::Delay> (taps.at (path.tap), delay->getLength(), fade, path.isNew, true));
            repointed[was] = delay;
        }
    }

    forEachDelay (
        [&repointed] (std::shared_ptr<engine::Delay>& delay)
        {
            if (delay != nullptr)
            {
                if (const auto found = repointed.find (delay.get()); found != repointed.end())
                    delay = found->second;

                delay->getLine()->allocate();
            }
        });

    // Each tap on the strip that writes it, its room made.
    std::map<std::int64_t, engine::Plan::Strip*> byID;

    for (auto& strip : plan.strips)
        byID[strip.id] = &strip;

    for (const auto& [tap, ring] : taps)
    {
        const auto strip = byID.find (tap.strip);

        if (strip == byID.end())
        {
            jassertfalse; // a path the plan plays comes from a strip it plays
            continue;
        }

        ring->allocate();
        const auto part = tapParts.find (tap);
        strip->second->taps.push_back ({ ring, tap.point, part != tapParts.end() ? part->second : nullptr });
    }

    // The rings the rings took the place of, and the delays the delays did, let go of once they are done with them.
    for (const auto& [tap, ring] : taps)
        ring->letGoOfTheOld();

    for (const auto& [key, delay] : delays)
    {
        if (delay != nullptr)
        {
            delay->letGoOfThePrevious();

            if (delay->getLine() != nullptr)
                delay->getLine()->letGoOfTheOld();
        }
    }
}

void SongPlayer::markWhatReadsLeaving (engine::Plan& plan)
{
    // The strips a path that left went into (a return a send went to, a bus, a strip a container's chain was in) hold what
    // it added as it faded, and so does what they go into: a path that reads one of them, its delay grown, would play
    // that again after the engine's dip, which it holds the mix silent across for as long (engine/core/Engine.h:
    // Realign::hold). The others' growth plays again only what is there now, as any change of the compensation does, under
    // the dip. A second after such a path left, the engine has taken its plan long since: what it went into is no longer
    // counted (whose rings hold a third of a second more than their delays).
    const auto now = juce::Time::getMillisecondCounter();
    std::erase_if (leftInto, [now] (const auto& into) { return now - into.second > 1000; });

    if (leftInto.empty())
        return;

    std::set<std::int64_t> holding;

    for (const auto& [id, when] : leftInto)
        holding.insert (id);

    // In the plan's order, each after what it takes in.
    for (auto& strip : plan.strips)
    {
        for (auto& input : strip.inputs)
        {
            if (holding.contains (plan.strips[(size_t) input.strip].id))
            {
                if (input.delay != nullptr)
                    input.delay->markReadsLeaving();

                holding.insert (strip.id);
            }
        }

        for (auto& device : strip.devices)
        {
            if (device.sidechain >= 0 && device.sidechainDelay != nullptr && holding.contains (plan.strips[(size_t) device.sidechain].id))
                device.sidechainDelay->markReadsLeaving();

            // What a device holds back for its sidechain is its strip's own sound.
            if (device.inputDelay != nullptr && holding.contains (strip.id))
                device.inputDelay->markReadsLeaving();
        }

        if (holding.contains (strip.id))
            for (auto& container : strip.containers)
                for (auto& chain : container.chains)
                    if (chain.delay != nullptr)
                        chain.delay->markReadsLeaving();
    }
}

void SongPlayer::leaveOutWhatIsNotHeard (engine::Plan& plan)
{
    // A render plays the song as it is, once, nothing unmuted meanwhile: what it can't hear is left out, so that it costs
    // nothing (a muted track's instrument plays on live, for when it is heard again). From the master back, in the plan's
    // order reversed (each strip after what it takes in), a strip is heard if one heard takes it in: after its fader while
    // its channel is heard, through a send that sends (a muted track's sends before its fader go on), as a sidechain one
    // listens to, or as the sampler a pad's track adds a part of.
    if (plan.strips.empty())
        return;

    std::vector<bool> heard (plan.strips.size(), false);
    heard.back() = true;

    for (auto i = plan.strips.size(); i-- > 0;)
    {
        if (! heard[i])
            continue;

        const auto& strip = plan.strips[i];

        for (const auto& input : strip.inputs)
        {
            auto& from = *plan.strips[(size_t) input.strip].state;
            const auto sends = input.level != nullptr ? ! input.level->isCut()
                                                      : input.part != nullptr || input.beforeFader || from.getChannel().isAudible();
            if (sends)
                heard[(size_t) input.strip] = true;
        }

        for (const auto& device : strip.devices)
            if (device.sidechain >= 0)
                heard[(size_t) device.sidechain] = true;
    }

    // The others left out, what they fed with them, and what points at a strip by its place moved with it.
    std::vector<int> places (plan.strips.size(), -1);
    std::vector<engine::Plan::Strip> kept;

    for (size_t i = 0; i < plan.strips.size(); ++i)
    {
        if (heard[i])
        {
            places[i] = (int) kept.size();
            kept.push_back (std::move (plan.strips[i]));
        }
    }

    for (auto& strip : kept)
    {
        std::erase_if (strip.inputs, [&places] (const auto& input) { return places[(size_t) input.strip] < 0; });

        for (auto& input : strip.inputs)
            input.strip = places[(size_t) input.strip];

        for (auto& device : strip.devices)
            if (device.sidechain >= 0)
                device.sidechain = places[(size_t) device.sidechain];
    }

    plan.strips = std::move (kept);
}

std::shared_ptr<engine::SendLevel> SongPlayer::getSendLevel (const SendValue& send)
{
    const auto id = (std::int64_t) send.id.getRaw();
    auto& kept = sendLevels[id];

    if (kept == nullptr)
        kept = std::make_shared<engine::SendLevel>();

    kept->setDecibels (send.level);
    usedSends.insert (id);
    return kept;
}

//==============================================================================
std::shared_ptr<engine::ModulatorNode> SongPlayer::getModulator (const ModulatorValue& value)
{
    // A macro, or a modulator of one of the six kinds, kept by its ID. (A song has few: their values are read again for
    // each plan.)
    const auto id = (std::int64_t) value.id.getRaw();
    auto& node = modulators[id];

    if (node == nullptr || node->getKind() != value.kind)
    {
        node = value.kind.has_value() ? std::make_shared<engine::ModulatorNode> (*value.kind) : std::make_shared<engine::ModulatorNode>();
        node->prepare (preparedRate, preparedBlockSize, (std::uint32_t) id);
    }

    setModulatorValues (*node, value);
    usedModulators.insert (id);
    return node;
}

void SongPlayer::setModulatorValues (engine::ModulatorNode& node, const ModulatorValue& value)
{
    if (node.isMacro())
    {
        node.setMacroValue (value.value);
        return;
    }

    // Those set by hand, the others at their defaults (devices/modulation/Modulators.h); its steps; and its switch. (Its
    // value is of the node's kind: one of another kind makes a new node, getModulator().)
    const auto parameters = devices::getModulatorParameters (*node.getKind());
    const auto sameKind = value.kind == node.getKind();

    for (size_t i = 0; i < parameters.size(); ++i)
        node.setParameter ((int) i, sameKind ? value.getParameterValue ((int) i) : parameters[i].defaultValue);

    for (int i = 0; i < devices::Modulator::maxSteps; ++i)
        node.setStep (i, (size_t) i < value.steps.size() ? value.steps[(size_t) i] : 0.0f);

    node.setOn (! value.off);
}

void SongPlayer::addModulation (engine::Plan::Strip& strip, const SongModulation& modulation, const juce::ValueTree& channelState,
                                const std::vector<std::int64_t>& deviceIDs)
{
    // Its modulators and macros, in their order, then its containers' macros, and what each moves: its devices' parameters
    // (a container's macros those in it), its fader and its sends' levels (which the strips they go to take, by the sends'
    // IDs: compile()). Each owner's from the song's modulation (ModulationValue.h): the channel's by its track's ID, the
    // master's under 0, a container's by its ID.
    std::vector<const ModulatorValue*> sources;

    const auto addSources = [this, &strip, &sources, &modulation] (const immer::flex_vector<ModulatorID>* list)
    {
        if (list == nullptr)
            return;

        for (const auto& id : *list)
        {
            if (const auto* value = modulation.find (id))
            {
                strip.modulators.push_back (getModulator (*value));
                sources.push_back (value);
            }
        }
    };

    const auto isMaster = channelState.hasType (schema::MASTER);
    const auto channel = isMaster ? TrackID() : getTrackID (channelState);
    addSources (modulation.channels.find (isMaster ? 0 : channel.getRaw()));

    for (const auto& child : channelState)
        if (child.hasType (schema::CONTAINER))
            addSources (modulation.containers.find (getDeviceID (child).getRaw()));

    const auto fader = (std::int64_t) getID (channelState, schema::channelFader);
    std::optional<std::vector<DeviceID>> sends;

    for (size_t source = 0; source < sources.size(); ++source)
    {
        for (const auto& modulates : sources[source]->modulations)
        {
            const auto& target = modulates.parameter;
            const auto amount = modulates.amount;
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

            // A send of this channel's (Mixer.h: its sends, read once; the master has none).
            if (target.parameter == schema::sendLevelParameter)
            {
                if (! sends.has_value())
                    sends = isMaster ? std::vector<DeviceID>() : getSends (song, channel);

                if (std::find (sends->begin(), sends->end(), target.device) != sends->end())
                    sendModulation[device].push_back ({ (int) source, amount });

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

            if (! entry.modulation->isModulated[(size_t) index])
            {
                entry.modulation->isModulated[(size_t) index] = true;
                entry.modulation->modulated.push_back (index);
            }
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
    // Each parameter's curve kept from the plan before while nothing of it changed (known bug 124: every plan read all the
    // song's automation again), the others read from the song's curves as values, in place (Automation.h: getCurvesValue(),
    // which holds one curve for a parameter).
    using Points = std::vector<engine::CurvePoint>;

    const auto toPoints = [] (const CurveValue& curve)
    {
        Points points;
        points.reserve (curve.points.size());

        for (const auto& point : curve.points)
            points.push_back ({ point.beat.inBeats(), point.position, point.curve });

        return points;
    };

    const auto& songCurves = getCurvesValue (song); // nothing sets it while the plan is compiled
    std::map<CurveKey, ParameterRef> parameters;    // the song's curves by their parameters

    for (const auto& parameter : songCurves.order)
        parameters.emplace (CurveKey { parameter.device.getRaw(), parameter.parameter }, parameter);

    auto before = std::exchange (curves, {});
    const auto emptyBefore = std::exchange (emptyCurves, {});
    const auto withoutTravelBefore = std::exchange (curvesWithoutTravel, {});
    faders.clear();

    // Which are kept and which of those checked, by their keys' order; the points of the others and of those checked, read.
    struct Reading
    {
        bool kept = false, checked = false;
        size_t read = 0; // its place among those read
    };

    std::vector<Reading> readings;
    std::vector<CurveValue> values; // those read, each a copy that shares its points
    readings.reserve (parameters.size());

    for (const auto& [key, parameter] : parameters)
    {
        Reading reading;
        reading.kept = ! compiling.hasCurve (key) && (before.contains (key) || emptyBefore.contains (key));
        reading.checked = reading.kept && shouldCheck();

        if (! reading.kept || reading.checked)
        {
            reading.read = values.size();
            const auto* curve = songCurves.find (parameter);
            values.push_back (curve != nullptr ? *curve : CurveValue());
        }

        readings.push_back (reading);
    }

    auto reading = readings.begin();

    for (const auto& entry : parameters)
    {
        const auto& key = entry.first;
        const auto& [isKept, isChecked, readAt] = *reading++;
        const auto kept = before.find (key);

        if (isKept)
        {
            if (isChecked)
            {
                const auto points = toPoints (values[readAt]);
                const auto& keptPoints = kept != before.end() ? kept->second->getPoints() : Points();
                [[maybe_unused]] const auto same = std::equal (points.begin(), points.end(), keptPoints.begin(), keptPoints.end(),
                                                               [] (const engine::CurvePoint& a, const engine::CurvePoint& b)
                                                               {
                                                                   return juce::exactlyEqual (a.beat, b.beat)
                                                                          && juce::exactlyEqual (a.position, b.position)
                                                                          && juce::exactlyEqual (a.curve, b.curve);
                                                               });
                jassert (same); // the curve changed without the song saying so (markChange())
            }

            if (kept == before.end())
            {
                emptyCurves.insert (key);
            }
            else if (! withoutTravelBefore.contains (key))
            {
                curves[key] = kept->second;
            }
            else if (const auto travel = getControlTravel (song, { DeviceID::fromRaw (key.first), key.second }))
            {
                // Its parameter is there now (its device brought back): its points, kept, along its travel.
                curves[key] = std::make_shared<const engine::Curve> (kept->second->getPoints(), *travel);
            }
            else
            {
                curves[key] = kept->second;
                curvesWithoutTravel.insert (key);
            }

            continue;
        }

        ++curvesRead;

        // Its points are positions along its parameter's control's travel, which the engine turns into the parameter's
        // units (engine/automation/Curve.h). One of a parameter the song hasn't (a device's that is gone, kept for an undo)
        // plays from 0 to 1 until it has, which each plan looks for again.
        if (auto points = toPoints (values[readAt]); ! points.empty())
        {
            const auto travel = getControlTravel (song, { DeviceID::fromRaw (key.first), key.second });

            if (! travel.has_value())
                curvesWithoutTravel.insert (key);

            curves[key] = std::make_shared<const engine::Curve> (std::move (points), travel.value_or (devices::Travel()));
        }
        else
        {
            emptyCurves.insert (key);
        }
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

void SongPlayer::loadSounds (engine::DeviceNode& node, const juce::ValueTree& deviceState)
{
    // What a drum sampler's pads, a synth's oscillators and a sampler play: the song's SOUNDS name it as the
    // plug-in's state does.
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);
    auto& processor = node.getRunner().getProcessor();
    const auto type = node.getDeviceInfo().typeID;

    if (type == devices::DrumSampler::typeID)
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            loadPadSound (static_cast<devices::DrumSampler&> (processor), song, sounds, pad);

    if (type == devices::Synth::typeID)
        for (int osc = 0; osc < devices::Synth::numOscillators; ++osc)
            loadOscillatorTable (static_cast<devices::Synth&> (processor), song, sounds, osc);

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

    if (type == devices::Synth::typeID)
    {
        for (int osc = 0; osc < devices::Synth::numOscillators; ++osc)
        {
            if (property == schema::getOscillatorWavetable (osc) || property == schema::getOscillatorWavetableFile (osc))
            {
                loadOscillatorTable (static_cast<devices::Synth&> (processor), song, sounds, osc);
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

    // What the song's sidechains listen to, the master's and those in containers too.
    std::set<std::int64_t> keys;
    const auto addKeys = [&keys] (const juce::ValueTree& chain, auto& addInside) -> void
    {
        for (const auto& device : chain)
        {
            if (device.hasType (schema::DEVICE))
            {
                if (const auto source = getID (device, schema::deviceSidechain); source != 0)
                    keys.insert (source);
            }
            else if (device.hasType (schema::CONTAINER) || device.hasType (schema::CHAIN))
            {
                addInside (device, addInside);
            }
        }
    };

    for (const auto& state : trackStates)
        addKeys (state, addKeys);

    addKeys (song.getState().getChildWithName (schema::MASTER), addKeys);

    // A pad's track goes with its sampler's track, whose mute and solo cover its pads on tracks of their own.
    const auto getSamplerTrack = [this] (const juce::ValueTree& padState)
    {
        return findTrackState (song, getDeviceChannel (song, DeviceID::fromRaw ((std::uint64_t) getID (padState, schema::trackPadDevice))));
    };

    // Whether a track is muted or soloed, by its own switches or a folder's it is in.
    struct Switches
    {
        bool muted = false, soloed = false;
    };

    const auto getSwitches = [&] (const juce::ValueTree& state)
    {
        Switches switches;
        const auto addFrom = [&switches] (const juce::ValueTree& from)
        {
            for (auto track = from; track.hasType (schema::TRACK); track = track.getParent())
            {
                switches.muted = switches.muted || (bool) track[schema::trackMute];
                switches.soloed = switches.soloed || (bool) track[schema::trackSolo];
            }
        };

        addFrom (state);

        if (state.hasProperty (schema::trackPad))
            addFrom (getSamplerTrack (state));

        return switches;
    };

    const auto isRendered = [&] (const juce::ValueTree& state)
    { return beforeFader && getID (state, schema::trackID) == rendered->channel; };
    const auto isSoloedOut = [&] (const juce::ValueTree& state)
    {
        const auto soloed = getSwitches (state).soloed || (isFolderState (state) && holdsSoloed (state, holdsSoloed));
        return ! isRendered (state) && anySoloed && ! soloed && ! isReturnState (state);
    };
    const auto isHeard = [&] (const juce::ValueTree& state)
    { return isRendered (state) || (! getSwitches (state).muted && ! isSoloedOut (state)); };

    // A return that is soloed hears what is sent to it, from tracks its solo silences too, as a soloed return does in
    // Ableton Live, Logic Pro and Cubase: else soloing a return, to hear an effect alone, would play silence.
    const auto isSoloedReturn = [&] (std::int64_t id)
    {
        const auto target = findTrackState (song, TrackID::fromRaw ((std::uint64_t) id));
        return isReturnState (target) && getSwitches (target).soloed && isHeard (target);
    };

    // Whether a track, or a folder it is in, is muted below a bus: the bus's own mute, and its folders', don't count.
    const auto isMutedBelow = [] (juce::ValueTree track, const juce::ValueTree& bus)
    {
        for (; track.hasType (schema::TRACK) && track != bus && ! bus.isAChildOf (track); track = track.getParent())
            if ((bool) track[schema::trackMute])
                return true;

        return false;
    };

    // Whether a send of a channel goes on. Mute silences a channel's output only (the owner's decision;
    // docs/design/decisions.md): its sends before its fader go on, and those after it are silent with it. What another
    // track's solo silences sends nothing; but a track that only a solo silences (not its mute) still sends to a return
    // that is soloed.
    const auto sendGoesOn = [&] (const juce::ValueTree& state, const SendValue& send)
    {
        const auto heard = isHeard (state);
        return heard || (send.preFader && ! isSoloedOut (state))
               || (! heard && ! getSwitches (state).muted && isSoloedReturn ((std::int64_t) send.to.getRaw()));
    };

    // A track's sends, by its element's ID, as the player holds them (heldSends: never the song's, as this runs in the tree's
    // callback of a track muted or soloed, takeValue()), each in turn until `each` returns true; whether one did.
    const auto anySendOf = [this] (const juce::ValueTree& state, const std::function<bool (const SendValue&)>& each)
    {
        if (const auto* list = heldSends.tracks.find ((std::uint64_t) getID (state, schema::trackID)))
            for (const auto id : *list)
                if (const auto* send = heldSends.find (id); send != nullptr && each (*send))
                    return true;

        return false;
    };

    // Whether something still listens to a bus that isn't heard: a sidechain keyed from it, or a send of it that goes on
    // (one before its fader, while it is muted).
    const auto isListenedTo = [&] (const juce::ValueTree& bus)
    {
        return keys.contains (getID (bus, schema::trackID))
               || anySendOf (bus, [&] (const SendValue& send) { return sendGoesOn (bus, send); });
    };

    // A track that isn't heard plays on into a bus that something still listens to while that bus isn't heard either
    // (soloed out, or muted: a muted bus feeds its key, as a muted track does, and its sends before its fader), unless it is
    // muted below the bus, a pad on a track of its own by its sampler's track too (known bug 18): the bus's own mute or solo
    // silences it after the bus has taken it in, and what else it sends, after its fader, is silent meanwhile. A bus that
    // is heard plays its tracks as they are heard, so a track soloed in it plays alone (the owner, 2026-11-13: soloing a
    // drum in the Drums bus played the bus's other drums).
    const auto playsForListener = [&] (const juce::ValueTree& state)
    {
        for (auto bus = state.getParent(); bus.hasType (schema::TRACK); bus = bus.getParent())
            if (! isHeard (bus) && isListenedTo (bus))
                return ! isMutedBelow (state, bus)
                       && ! (state.hasProperty (schema::trackPad) && isMutedBelow (getSamplerTrack (state), bus));

        return false;
    };

    for (const auto& state : trackStates)
    {
        auto* strip = findStrip (getID (state, schema::trackID));

        if (strip == nullptr)
            continue;

        // A track plays on, its devices too, whether it is heard or not; each of its sends as it goes on.
        anySendOf (state,
                   [&] (const SendValue& send)
                   {
                       if (const auto found = sendLevels.find ((std::int64_t) send.id.getRaw()); found != sendLevels.end())
                           found->second->setCut (! sendGoesOn (state, send));

                       return false;
                   });

        strip->getChannel().setAudible (isHeard (state) || playsForListener (state));
    }
}

void SongPlayer::takeModulator (ModulatorID id, const ModulatorValue& value)
{
    if (auto* node = findModulator ((std::int64_t) id.getRaw()))
        setModulatorValues (*node, value);
}

void SongPlayer::takeSendLevel (DeviceID id, float decibels)
{
    if (const auto found = sendLevels.find ((std::int64_t) id.getRaw()); found != sendLevels.end())
        found->second->setDecibels (decibels);
}

bool SongPlayer::takeValue (const juce::ValueTree& tree, const juce::Identifier& property)
{
    // (A modulator's parameter, steps or switch, and a macro's value: the song's values tell them, takeModulator(); a send's
    // level too, takeSendLevel().)

    // A container's mix: at once.
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

    // A pad's sound or an oscillator's table: loaded now into the node the next plan keeps, which doesn't load it again.
    if (tree.hasType (schema::SOUNDS))
    {
        const auto id = getID (tree.getParent(), schema::deviceID);

        if (auto* node = findDevice (id))
        {
            loadSound (*node, tree.getParent(), property);
            changes.soundsTaken.insert (id);
        }

        return true;
    }

    const auto isChannel = tree.hasType (schema::TRACK) || tree.hasType (schema::MASTER);

    if (isChannel && (property == schema::channelVolume || property == schema::channelPan))
    {
        setChannel (tree, tree.hasType (schema::MASTER));
        return true;
    }

    // A track muted or soloed: what is heard, and which sends send, from the sends the player holds (heldSends), as this is a
    // tree's callback.
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
    fileKeys.clear();
    audio.clear(); // which plays them
}

juce::String SongPlayer::getFileKey (const juce::File& file, bool askAgain)
{
    // Its identity on the disk (model/engine/AudioFiles.h), the disk asked once for a plan at most, however many clips play
    // the file; for a file this player knows already, only if asked again, or once what was known of it is too old to trust
    // (known bug 124: every plan asked about each clip's file; known bug 198: one written again in its place played as it
    // was first read). A key that changed is a file this plan found written again (compile()).
    const auto path = file.getFullPathName();
    auto& known = fileKeys[path];

    if (! askedForPlan.contains (path))
    {
        auto asked = askAgain || known.isEmpty();
        const auto key = asked ? askFileIdentity (file) : getFileIdentity (file, &asked);

        if (asked)
        {
            askedForPlan.insert (path);
            ++filesAsked;
        }

        filesChangedForPlan = filesChangedForPlan || (known.isNotEmpty() && key != known);
        known = key;
    }

    return known;
}

bool SongPlayer::isOnDiskAsKept (const TrackAudio& kept)
{
    for (size_t i = 0; i < kept.played.size(); ++i)
        if (getFileKey (kept.played[i], false) != kept.files[i])
            return false;

    for (const auto& [source, key] : kept.sources)
        if (getFileKey (source, false) != key)
            return false;

    return true;
}

std::shared_ptr<engine::AudioFile> SongPlayer::getAudioFile (const juce::File& file, bool askAgain, juce::String* keyOut)
{
    const auto path = getFileKey (file, askAgain);

    if (keyOut != nullptr)
        *keyOut = path;

    auto& kept = files[path];

    if (kept == nullptr)
    {
        // A file that can't be read (gone, or a render not made yet) plays nothing until a plan finds it; nor one that says
        // it has no rate, or no length, as one from another program might (where it plays would be no number).
        auto formatReader = std::unique_ptr<juce::AudioFormatReader> (getAudioFormats().createReaderFor (file));

        if (formatReader == nullptr || ! std::isfinite (formatReader->sampleRate) || formatReader->sampleRate <= 0.0
            || formatReader->lengthInSamples < 0)
        {
            files.erase (path);
            return nullptr;
        }

        kept = std::make_shared<engine::AudioFile> (makeSampleReader (std::move (formatReader)), blocks);
        kept->setOrigin (path); // its path and what was known of it then (tests/PlanCheck.h)

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
    auto file = getAudioFile (comparison->file, true);

    if (file == nullptr)
        return;

    prepareComparison (sampleRate, preparedBlockSize);

    if (comparisonRegion == nullptr || ! comparisonRegion->playsAt (*file, sampleRate))
        comparisonRegion = std::make_shared<engine::RegionState> (*file, sampleRate);

    // The whole bounce from the version's start, which the comparison plays at the song's beats.
    engine::AudioRegion bounce;
    bounce.file = file;
    bounce.state = comparisonRegion;
    bounce.length = toWhole<std::int64_t> (std::ceil ((double) file->getLength() * sampleRate / file->getSampleRate()));
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

void SongPlayer::placeRegion (engine::AudioRegion& region, const ClipValue& clip, const PlayedClip& played) const
{
    // Where it plays in samples, from where in its file, at its gain, with its fades (fitted: scaled down together where
    // they would overlap).
    region.start = roundToWhole<std::int64_t> (played.position.start * preparedRate);
    region.length = roundToWhole<std::int64_t> (played.position.end * preparedRate) - region.start;
    region.fileStart = played.position.offset * region.file->getSampleRate();
    region.gain = juce::Decibels::decibelsToGain (clip.gain);
    region.fadeIn = roundToWhole<int> (played.fadeIn * preparedRate);
    region.fadeOut = roundToWhole<int> (played.fadeOut * preparedRate);
    region.fadeInCurve = toEngineFade (getFadeCurve (clip.fadeIn));
    region.fadeOutCurve = toEngineFade (getFadeCurve (clip.fadeOut));
}

SongPlayer::TrackAudio SongPlayer::makeAudio (const juce::ValueTree& trackState, std::optional<ClipTempo>& tempo, bool askAgain)
{
    TrackAudio made;

    // The track's clips in the order it holds them (Clips.h: getClipValues()), which checkAudio() compares the regions in.
    for (const auto& clip : getClipValues (song, getTrackID (trackState)))
    {
        if (! clip.isAudio() || clip.muted)
            continue;

        // What plays for it: its file, or Phazite's render of it, where it plays in seconds, the song's tempo read once for
        // the plan (known bug 124). Its file asked about first, as a render is named for it as it is known (getRenderFile():
        // one written again asks for a render of its own).
        if (! tempo.has_value())
            tempo.emplace (song);

        const auto source = getClipFile (song, clip);
        const auto sourceKey = getFileKey (source, askAgain);
        const auto played = getPlayedClip (song, clip, *tempo);
        juce::String key;
        auto file = getAudioFile (played.file, askAgain, &key);

        if (file == nullptr)
        {
            made.whole = false;
            continue;
        }

        const auto id = (std::int64_t) clip.id.getRaw();
        auto region = makeRegion (id, file);
        placeRegion (region, clip, played);
        file->keep (toWhole<std::int64_t> (region.fileStart));
        made.regions.push_back (std::move (region));
        made.clips.push_back (id);
        made.files.push_back (key);
        made.played.push_back (played.file);

        if (played.file != source)
        {
            made.sources.emplace_back (source, sourceKey);
            usedSources.insert (source.getFullPathName());
        }
    }

    return made;
}

void SongPlayer::addAudio (engine::Plan::Strip& strip, const juce::ValueTree& trackState, std::optional<ClipTempo>& tempo)
{
    // As the plan before played it, while nothing of its clips, the tempo, the rate, the song's folders or its files on the
    // disk changed, nor the sources its renders are named for (each asked about only once what was known of it is too old:
    // getFileKey()); placed again otherwise, its files asked about again if its clips changed.
    const auto changedClips = compiling.hasNotes (strip.id);

    if (const auto kept = audio.find (strip.id); kept != audio.end() && ! changedClips && isOnDiskAsKept (kept->second))
    {
        if (shouldCheck())
            checkAudio (kept->second, trackState, tempo);

        for (size_t i = 0; i < kept->second.regions.size(); ++i)
        {
            usedRegions.insert (kept->second.clips[i]);
            usedFiles.insert (kept->second.files[i]);
        }

        for (const auto& source : kept->second.sources)
            usedSources.insert (source.first.getFullPathName());

        trustedForPlan.emplace_back (strip.id, trackState);
        strip.audio = kept->second.regions;
        return;
    }

    auto made = makeAudio (trackState, tempo, changedClips);
    strip.audio = made.regions;

    if (! changedClips)
        trustedForPlan.emplace_back (strip.id, trackState); // made from what was known of its files, not asked again

    if (made.whole)
        audio[strip.id] = std::move (made);
    else
        audio.erase (strip.id);
}

void SongPlayer::checkAudio (const TrackAudio& kept, const juce::ValueTree& trackState, std::optional<ClipTempo>& tempo)
{
    // What makeAudio() would make of the track now, without making anything: the same clips, each the same file (as this
    // player knows it now: a file that changed under the same name, once it asked, makes the track's audio again), the same
    // place in it and the same region, as the plan before placed them. A difference is a change of the song's that its
    // player didn't hear of (markChange()), or a file it found changed and didn't make the track's audio again for.
    auto same = true;
    size_t index = 0;

    for (const auto& clip : getClipValues (song, getTrackID (trackState)))
    {
        if (! clip.isAudio() || clip.muted)
            continue;

        if (! tempo.has_value())
            tempo.emplace (song);

        const auto played = getPlayedClip (song, clip, *tempo);
        const auto known = fileKeys.find (played.file.getFullPathName());
        const auto file = known != fileKeys.end() ? files.find (known->second) : files.end();
        const auto state = regions.find ((std::int64_t) clip.id.getRaw());

        if (index >= kept.regions.size() || file == files.end() || state == regions.end())
        {
            same = false;
            break;
        }

        const auto& region = kept.regions[index++];
        auto placed = region;
        placeRegion (placed, clip, played);
        same = same && region.file == file->second && region.state == state->second && state->second->playsAt (*file->second, preparedRate)
               && region.start == placed.start && region.length == placed.length && juce::exactlyEqual (region.fileStart, placed.fileStart)
               && juce::exactlyEqual (region.gain, placed.gain) && region.fadeIn == placed.fadeIn && region.fadeOut == placed.fadeOut
               && region.fadeInCurve.shape == placed.fadeInCurve.shape
               && juce::exactlyEqual (region.fadeInCurve.bend, placed.fadeInCurve.bend)
               && region.fadeOutCurve.shape == placed.fadeOutCurve.shape
               && juce::exactlyEqual (region.fadeOutCurve.bend, placed.fadeOutCurve.bend);
    }

    same = same && index == kept.regions.size();
    jassert (same);
    juce::ignoreUnused (same);
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

    auto file = getAudioFile (getFrozenFile (song, getTrackID (trackState)), true);

    if (file == nullptr)
        return false;

    auto region = makeRegion (strip.id, file);
    region.length = roundToWhole<std::int64_t> ((double) file->getLength() / file->getSampleRate() * preparedRate);
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

    // A send's level, in the fader's positions, once it has played.
    if (const auto send = sendLevels.find (id); send != sendLevels.end())
    {
        if (const auto played = send->second->getPlayedPosition (withModulation);
            parameter.parameter == schema::sendLevelParameter && played >= 0.0f)
            return played;

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

    if (const auto send = sendLevels.find (id); send != sendLevels.end())
    {
        if (parameter.parameter == schema::sendLevelParameter)
            send->second->setWriting (isWriting);

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

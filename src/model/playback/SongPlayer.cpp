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
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/TracksValue.h"
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
#include <string_view>
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

    // A channel's ID as the plans key it: a track's raw ID (the master's is 0, which no track's is).
    std::int64_t getChannelKey (const TrackValue& channel)
    {
        return channel.isMaster() ? 0 : (std::int64_t) channel.id.getRaw();
    }

    // A device's, a container's or a chain's ID as the plans key it.
    std::int64_t getDeviceKey (const DeviceValue& value)
    {
        return (std::int64_t) value.id.getRaw();
    }

    // Whether a value is a device's, of Phazite's own or a plug-in (not a container nor a chain).
    bool isDeviceForm (const DeviceValue& value)
    {
        return value.form == DeviceValue::Form::own || value.form == DeviceValue::Form::plugin;
    }

    // The place of what a channel holds (a track's by its ID, the master's by its flag), as the devices' area lists it.
    DevicePlace getChannelPlaceOf (const TrackValue& channel)
    {
        return makeChannelPlace (channel.id, channel.isMaster());
    }

    // Each item of a channel's list in the devices' values, in its order (a device, or a container, which holds the rest).
    template <typename Each>
    void forEachOnChannel (const SongDevices& all, const TrackValue& channel, Each&& each)
    {
        if (const auto* list = all.findList (getChannelPlaceOf (channel)))
            for (const auto id : *list)
                if (const auto* value = all.find (id))
                    each (*value);
    }

    // What a container holds (its chains) or a chain (its devices), in their order, each as the values hold it.
    template <typename Each>
    void forEachInside (const SongDevices& all, const DeviceValue& owner, Each&& each)
    {
        const auto* list = owner.form == DeviceValue::Form::container ? all.chains.find (owner.id.getRaw())
                           : owner.form == DeviceValue::Form::chain   ? all.chainDevices.find (owner.id.getRaw())
                                                                      : nullptr;

        if (list != nullptr)
            for (const auto id : *list)
                if (const auto* value = all.find (id))
                    each (*value);
    }

    // A device's sounds as a SOUNDS element (those of the properties from `first` on, `count` of them): what the sounds'
    // loaders take (DrumKit.h, Wavetables.h, SamplerSound.h).
    juce::ValueTree makeSoundsElement (const DeviceValue& device, int first, int count)
    {
        const auto& properties = getSoundPropertiesOf (std::string_view (device.type.toRawUTF8()));
        juce::ValueTree sounds (schema::SOUNDS);

        for (auto i = first; i < first + count && i < (int) properties.size(); ++i)
            if (const auto* sound = device.sounds.find ((std::uint16_t) i))
                sounds.setProperty (properties[(size_t) i], *sound, nullptr);

        return sounds;
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
    std::shared_ptr<const engine::NoteSequence> compileNotes (const Song& song, const TrackValue& track)
    {
        std::vector<engine::NoteSequence::Note> notes;
        std::vector<engine::NoteSequence::Control> controls;

        for (const auto& clip : getClipValues (song, track.id))
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

struct SongPlayer::HeldTracks
{
    SongTracks tracks {};
    TrackSwitches switches {};
};

SongPlayer::SongPlayer (Song& s, PluginInstances* p)
    : song (s),
      plugins (p),
      songState (s.getState()),
      heldTracks (std::make_unique<HeldTracks>())
{
    holdTracks();
    holdDevices();
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

void SongPlayer::devicesChanged (const std::vector<SongValues::DeviceChange>& changed, const std::vector<DevicePlace>& reordered,
                                 const SongDevices&)
{
    // The channel each was on and is on (the master's under 0), as a change at or under a device, a container or a chain
    // marked the channel it is on (markChange()): every part, a value taken at once too (the next plan reads its channel's
    // devices again either way).
    using Change = SongValues::DeviceChange;

    const auto mark = [this] (const DevicePlace& place)
    {
        if (place.onMaster)
            changes.channels.insert (0);
        else if (place.track.isValid())
            changes.channels.insert ((std::int64_t) place.track.getRaw());
    };

    for (const auto& device : changed)
    {
        const auto id = (std::int64_t) device.id.getRaw();

        if (device.wasThere)
            mark (device.before);

        if (device.isThere)
            mark (device.after);

        // Its sounds, which a kept node loads again: those of one that came; and those set in place, its first that came and
        // its last that went among them, unless its session loaded them at once (takeDevice(), which says so: soundsTaken).
        if (device.isThere && device.has (Change::made))
            changes.sounds.insert (id);
        else if (device.isThere && (device.has (Change::sounds) || device.has (Change::soundsAppeared)))
            changes.soundsSet.insert (id);

        // What decides what is heard: its sidechain's track and its channel, as the song holds them now.
        if (device.now != nullptr && device.now->form != DeviceValue::Form::chain && device.now->form != DeviceValue::Form::container)
            heldDevices[device.id.getRaw()] = { .sidechain = (std::int64_t) device.now->sidechain.getRaw(),
                                                .channel = device.after.onMaster ? 0 : (std::int64_t) device.after.track.getRaw() };
        else if (! device.isThere)
            heldDevices.erase (device.id.getRaw());
    }

    for (const auto& place : reordered)
        mark (place);
}

void SongPlayer::deviceStoreChanged (const std::vector<SongValues::DeviceStoreChange>& changed, const DeviceStore& now)
{
    // The channel of a plug-in whose first value set by hand came or whose last went, and of a chain muted or soloed, as a
    // VALUE added or taken out and a CHAIN's switch marked the channel they were on (markChange()): the next plan reads its
    // devices again. A value in place, a state, a program, a format ID and a version: nothing.
    using Kind = SongValues::DeviceStoreChange::Kind;

    for (const auto& change : changed)
    {
        const auto marks = (change.kind == Kind::pluginValue && (change.appeared || change.went)) || change.kind == Kind::chainMute
                           || change.kind == Kind::chainSolo;

        if (! marks || ! change.isInSong)
            continue;

        if (change.place.onMaster)
            changes.channels.insert (0);
        else if (change.place.track.isValid())
            changes.channels.insert ((std::int64_t) change.place.track.getRaw());
    }

    heldStore = now; // what updateChainAudibility() reads, as the song holds it now
}

void SongPlayer::tracksChanged (const std::vector<TrackChange>& changed, const std::vector<TrackID>& reordered, const SongTracks& now)
{
    // A track added or taken out (each track in a folder added or taken out told too), or come to another folder (a move: one
    // change, place): its notes and channel, and the channels of the folders it was in and came to; the master put in whole,
    // its channel. Any other part of a track marks nothing (design J.8).
    const auto markFolder = [this] (TrackID folder)
    {
        if (folder.isValid())
            changes.channels.insert ((std::int64_t) folder.getRaw());
    };

    for (const auto& track : changed)
    {
        if (track.onMaster)
        {
            if (track.has (TrackChange::made))
                changes.channels.insert (0);

            continue;
        }

        if (! track.has (TrackChange::made) && ! track.has (TrackChange::place))
            continue;

        const auto id = (std::int64_t) track.id.getRaw();
        changes.notes.insert (id);
        changes.channels.insert (id);

        if (track.wasThere)
            markFolder (track.parentBefore);

        if (track.isThere)
            markFolder (track.parentAfter);
    }

    for (const auto folder : reordered)
        markFolder (folder);

    heldTracks->tracks = now; // what updateAudibility() reads, as the song holds it now
}

void SongPlayer::trackSwitchesChanged (const std::vector<TrackSwitchChange>&, const TrackSwitches& now)
{
    // Nothing marked, as a TRACK's switch marked nothing: mute and solo are played at once (takeTrackSwitches()), a frozen
    // switch and an automation mode by the next plan, which reads them.
    heldTracks->switches = now;
}

bool SongPlayer::takeTracks (const std::vector<TrackChange>& changed, const SongTracks& now)
{
    heldTracks->tracks = now;
    auto taken = false;

    // A plain folder's VCA turns the tracks in it, those in its folders too, from the lists handed.
    const std::function<void (TrackID)> turn = [this, &now, &turn] (TrackID folder)
    {
        if (const auto* list = now.findList (folder))
        {
            for (const auto id : *list)
            {
                if (const auto* track = now.tracks.find (id.getRaw()))
                {
                    setChannelFrom (now, *track);
                    turn (id);
                }
            }
        }
    };

    for (const auto& track : changed)
    {
        if (! track.has (TrackChange::mixer) || track.has (TrackChange::made) || track.now == nullptr)
            continue;

        taken = true;

        if (track.now->isFolder() && ! track.now->fader.isValid())
            turn (track.id);
        else
            setChannelFrom (now, *track.now);
    }

    return taken;
}

bool SongPlayer::takeTrackSwitches (const std::vector<TrackSwitchChange>& changed, const TrackSwitches& now)
{
    heldTracks->switches = now;

    // A track muted or soloed: what is heard, and which sends send, from what the player holds.
    const auto heard =
        std::any_of (changed.begin(), changed.end(), [] (const TrackSwitchChange& change)
                     { return change.field == TrackSwitchChange::Field::mute || change.field == TrackSwitchChange::Field::solo; });

    if (heard)
        updateAudibility();

    return heard;
}

void SongPlayer::holdTracks()
{
    // As the tracks' and the switches' areas hold them, so that what is held here and what tracksChanged() and
    // trackSwitchesChanged() keep up to date never disagree; read from the song's tree for a song without values (none that
    // plays).
    const auto* values = song.getValues();
    heldTracks->tracks = getTracksValue (song);
    heldTracks->switches = values != nullptr ? values->trackSwitches.get() : readTrackSwitches (song.getState());
}

void SongPlayer::holdDevices()
{
    // Each device (its own or a plug-in, its containers' chains' too) by its ID: its sidechain's track and its channel, from
    // the song's devices' area (SongValues.h: devices), which tells what devicesChanged() keeps up to date: so the two never
    // disagree. Not in a callback: as the player is made.
    heldDevices.clear();
    const auto* values = song.getValues();

    if (values == nullptr)
        return;

    for (const auto& [id, device] : values->devices.get().devices)
        if (device.form == DeviceValue::Form::own || device.form == DeviceValue::Form::plugin)
            heldDevices[id] = { .sidechain = (std::int64_t) device.sidechain.getRaw(),
                                .channel = device.place.onMaster ? 0 : (std::int64_t) device.place.track.getRaw() };
}

namespace
{
    std::atomic<bool> checking { false };
    std::atomic<std::uint64_t> devicesRead { 0 }, curvesRead { 0 }, filesAsked { 0 }, soundLoads { 0 };
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

std::uint64_t SongPlayer::countSoundLoads() noexcept
{
    return soundLoads;
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
std::shared_ptr<const engine::NoteSequence> SongPlayer::getNotes (const TrackValue& trackState)
{
    const auto id = getChannelKey (trackState);
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

void SongPlayer::markChange (const juce::ValueTree& changed)
{
    // The clips and their notes are values beside the tree, heard by clipsChanged() and clipNotesChanged(); the tempo map is
    // heard by tempoMapChanged() (SongValues.h: its tree callbacks below leave it alone, isHeardAsKept()), which marks
    // every track's audio, which plays in seconds (a tempo change moves no note: they are in beats); the curves by
    // curvesChanged() (isHeardAsKept(), through isHeardAsCurves()), each by its parameter; the modulators and macros by
    // modulationChanged() (the tree holds none of them), each by its channel; the sends by sendsChanged() (the tree holds
    // none of them either), by the tracks they were added to or taken out of; the devices, containers and chains, by
    // devicesChanged(), by their channels, their sounds by their devices, and what the store holds of them (a plug-in's
    // state's format ID and version, a chain's mute and solo, a plug-in's first value set by hand) by deviceStoreChanged():
    // the song's tree holds none of them. In a channel (a track, a folder, a return or the master), its devices' values are
    // read again only for an element of the channel added, removed or moved; not for the channel's own properties (its
    // height, mute, solo, volume, pan), which no device reads (valueTreePropertyChanged() marks nothing here).
    for (auto element = changed; element.isValid(); element = element.getParent())
    {
        if (schema::isUncountedElement (element.getParent(), element))
            return; // view state (a track's lanes)

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
        {
            changes.channels.insert (element.hasType (schema::MASTER) ? 0 : getID (element, schema::trackID));
            return;
        }
    }
}

void SongPlayer::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    // Nothing that plays is marked from a property of the tree. The areas tell theirs (isHeardAsKept()): the tempo map's,
    // tempoMapChanged(); the chord lane's, which nothing played reads (chordLaneChanged(): SongPlayer follows none of it); the
    // curves', curvesChanged(); the devices', devicesChanged() (the tree holds none of them); the tracks', tracksChanged()
    // (a track's ID given anew in place is told as one track taken out and another added). What is left (a property Phazite
    // doesn't know) plays nothing.
}

void SongPlayer::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // Something added to a channel: devices' sounds are devicesChanged()'s, which tells each device that came. View state
    // added changes nothing that plays. The areas' (isHeardAsKept()): the tempo map's, tempoMapChanged(); the chord lane's,
    // which nothing played reads; the curves', curvesChanged(); the devices', devicesChanged(); the tracks', tracksChanged()
    // (tracks added, the master in place of another, a track's lanes).
    if (isHeardAsKept (parent, child))
        return;

    if (! schema::isUncountedElement (parent, child))
        markChange (parent);
}

void SongPlayer::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (isHeardAsKept (parent, child))
        return; // the areas': tempoMapChanged(); the chord lane's, which nothing played reads; ..., tracksChanged()

    if (! schema::isUncountedElement (parent, child))
        markChange (parent);
}

void SongPlayer::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    // A track's devices, a chain's devices, a channel's modulators, tracks in a folder: reordered. The areas'
    // (isHeardAsKept()): the tempo map's, tempoMapChanged(); the chord lane's, which nothing played reads; the curves',
    // curvesChanged().
    if (! isHeardAsKept (parent))
        markChange (parent);
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

    // The devices' store, read once for every container's chains (updateChainAudibility()), and each container's chains as
    // this plan finds them.
    if (const auto* values = song.getValues())
        heldStore = values->deviceStore.get();

    containerChains.clear();
    const auto songTracks = getTracksValue (song); // the song's tracks, read once (a copy, which shares what it holds)
    const auto channels = getChannels (songTracks, songSends);
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
        const auto isReturn = channel.value.isReturn();

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

        if (channel.value.holdsClips())
        {
            frozen = addFrozenFile (strip, channel.value);

            if (! frozen)
            {
                strip.notes = getNotes (channel.value);
                addAudio (strip, channel.value, clipTempo);
            }
        }

        const auto* only = of.has_value() && channel.id == of->channel && of->devices.has_value() ? &*of->devices : nullptr;
        const auto deviceIDs = addDevices (*plan, places, channel, strip, channel.value, frozen, only);

        if (live.has_value() && ! strip.hasInstrument && channel.value.holdsClips())
            strip.inputChannels = getHeardInput (channel.value);

        addFaderAutomation (strip, channel.value, false);
        addModulation (strip, songModulation, channel.value, deviceIDs);
        tellWhatMoves (strip);
        setChannelFrom (songTracks, channel.value);
        places[channel.id] = (int) plan->strips.size();
        plan->strips.push_back (std::move (strip));
    }

    // A file this plan found changed after it kept the audio of a track that plays it, or made it from what it trusted of
    // the file (a later track's clips changed, and asked about it again): that track's audio made again, so that every
    // track plays the file as it is now. One whose audio isn't kept whole (a render not made yet) made again in any case.
    if (filesChangedForPlan)
        for (const auto& [id, track] : std::exchange (trustedForPlan, {}))
            if (const auto kept = audio.find (id); kept == audio.end() || ! isOnDiskAsKept (kept->second))
                addAudio (plan->strips[(size_t) places[id]], track, clipTempo);

    // The song's sends as this plan read them (getChannels()), which sendsChanged() brings up to date until the next plan, for
    // what decides which send sends.
    heldSends = songSends;
    holdTracks(); // and the tracks and their switches as the keeper told them (heldTracks)
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
        const auto& masterValue = songTracks.master;
        const auto masterDeviceIDs = addDevices (*plan, places, {}, master, masterValue);
        addFaderAutomation (master, masterValue, true);
        addModulation (master, songModulation, masterValue, masterDeviceIDs);
        tellWhatMoves (master);
        setChannelFrom (songTracks, masterValue);
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

std::shared_ptr<engine::DeviceNode> SongPlayer::getDevice (const DeviceValue& deviceState, bool channelChanged)
{
    // A plug-in: its instance in its helper (model/plugins/internal/PluginInstances.h), which a render holds while it
    // plays a block, the engine skipping it then (SandboxedProcessor.h).
    if (deviceState.isPlugin())
        return getPluginDevice (deviceState);

    // Phazite's own devices.
    const auto* info = findOwnDevice (std::string_view (deviceState.type.toRawUTF8()));

    if (info == nullptr)
        return nullptr;

    const auto id = getDeviceKey (deviceState);
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

    node->setOn (! deviceState.off);
    return node;
}

std::vector<float> SongPlayer::readValues (const devices::DeviceInfo& info, const DeviceValue& deviceState)
{
    // Each by its index, a look in the device's values' map (DevicesValue.h: getParameterValue()), the parameter's default for
    // one it holds none of.
    std::vector<float> values;
    values.reserve (info.parameters.size());

    for (size_t i = 0; i < info.parameters.size(); ++i)
    {
        const auto* held = deviceState.values.find ((std::uint32_t) i);
        values.push_back (held != nullptr ? *held : info.parameters[i].defaultValue);
    }

    return values;
}

void SongPlayer::checkValues (const engine::DeviceNode& node, const DeviceValue& deviceState)
{
    // As getDevice() would set them, each kept to its range: a difference is a change of the song's that its player didn't
    // hear of (markChange()).
    const auto& info = node.getDeviceInfo();
    const auto values = readValues (info, deviceState);
    auto same = node.isOn() == ! deviceState.off;

    for (size_t i = 0; i < values.size(); ++i)
        same = same && juce::exactlyEqual (node.getValue ((int) i), devices::constrain (info.parameters[i], values[i]));

    jassert (same);
    juce::ignoreUnused (same);
}

std::shared_ptr<engine::DeviceNode> SongPlayer::getPluginDevice (const DeviceValue& deviceState)
{
    // One that is loaded (not one missing on this computer), in its helper, kept by its ID while the song's instances keep
    // the same instance: through its channel, with its mix as the engine's.
    const auto sandboxed = plugins != nullptr ? plugins->find (deviceState.id) : nullptr;
    const auto id = getDeviceKey (deviceState);

    if (sandboxed == nullptr)
    {
        // One still loading as its project opened: silence in its place (an effect that can't play is silent, never dry),
        // until it has, then its sound fades in (model/plugins/internal/SandboxedProcessor.h: LoadingPlugin).
        if (plugins == nullptr || ! plugins->isLoading (deviceState.id))
            return nullptr;

        auto& loading = devices[id];

        if (loading == nullptr || dynamic_cast<const LoadingPlugin*> (&loading->getRunner().getProcessor()) == nullptr)
        {
            auto processor = std::make_unique<LoadingPlugin> (deviceState.plugin.instrument);
            const auto& info = processor->getInfo();
            loading = std::make_shared<engine::DeviceNode> (info, std::move (processor));
            loading->getRunner().prepare (preparedRate, preparedBlockSize);
        }

        loading->setOn (! deviceState.off);
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
        node->setValue (mix, deviceState.mix);

    node->setOn (! deviceState.off);
    used.insert (id);
    return node;
}

std::vector<std::int64_t> SongPlayer::addDevices (engine::Plan& plan, const std::map<std::int64_t, int>& places, const ChannelInfo& channel,
                                                  engine::Plan::Strip& strip, const TrackValue& channelState, bool skipFrozen,
                                                  const std::set<std::int64_t>* only)
{
    std::vector<std::int64_t> ids;

    // While Low Latency is on, a device that looks ahead lets the sound through as it comes (PlaybackHost.h).
    const auto lowLatencyLimit = toWhole<int> (lowLatencyLimitSeconds * preparedRate);

    // A frozen track's devices before its freeze point are in its frozen file.
    const auto isFrozen = [this, skipFrozen] (const DeviceValue& state) { return skipFrozen && isFrozenDevice (song, state.id); };

    // The song's devices as its area holds them now (a copy, which shares what it holds: what the compile reads stays while it
    // reads it, whatever opens meanwhile).
    const auto all = getDevicesValue (song);

    const auto changed = compiling.hasChannel (channel.id); // the master's is 0

    const auto add = [&] (const DeviceValue& deviceState)
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
        const auto id = deviceState.id.getRaw();

        // What tells an instrument from the one before in its place, from plan to plan: its device, and a plug-in's instance
        // with the times it was loaded again after a crash (one still loading, silent meanwhile, is another).
        if (device.node->getDeviceInfo().kind == devices::DeviceKind::instrument)
        {
            strip.instrument = mixInto (strip.instrument, (std::uint64_t) id);

            if (const auto* sandboxed = dynamic_cast<const SandboxedProcessor*> (&device.node->getRunner().getProcessor()))
            {
                strip.instrument = mixInto (strip.instrument, (std::uint64_t) reinterpret_cast<std::uintptr_t> (&sandboxed->getInstance()));
                strip.instrument =
                    mixInto (strip.instrument, (std::uint64_t) (plugins != nullptr ? plugins->getGeneration (deviceState.id) : 0));
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
        if (const auto source = (std::int64_t) deviceState.sidechain.getRaw();
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

    forEachOnChannel (all, channelState,
                      [&] (const DeviceValue& deviceState)
                      {
                          // A render of some of them (a freeze's): the others left out.
                          if (only != nullptr && ! only->contains (getDeviceKey (deviceState)))
                              return;

                          if (isDeviceForm (deviceState))
                              add (deviceState);
                          else if (deviceState.form == DeviceValue::Form::container && ! deviceState.off && ! isFrozen (deviceState))
                              addContainer (strip, deviceState, all, add);
                      });

    return ids;
}

void SongPlayer::addContainer (engine::Plan::Strip& strip, const DeviceValue& containerState, const SongDevices& all,
                               const std::function<void (const DeviceValue&)>& add)
{
    // Its chains side by side, each its devices from the sound as it comes in, as late as it comes there, lined up with the
    // latest; then its mix of what they put out with what came in, delayed as much. An instrument container is its strip's
    // instrument.
    engine::Plan::Container container;
    container.first = (int) strip.devices.size();
    container.instrument = containerState.instrument;

    switch (containerState.split)
    {
        case ContainerSplit::bands:
            container.split = engine::Split::bands;
            break;
        case ContainerSplit::midSide:
            container.split = engine::Split::midSide;
            break;
        case ContainerSplit::leftRight:
            container.split = engine::Split::leftRight;
            break;
        case ContainerSplit::layers:
            container.split = engine::Split::layers;
            break;
    }

    const auto latencyBefore = strip.latency;
    auto latest = 0;

    if (container.instrument && container.first == 0)
        strip.hasInstrument = true;

    std::vector<DeviceValue> chainValues;
    forEachInside (all, containerState, [&chainValues] (const DeviceValue& each) { chainValues.push_back (each); });

    for (const auto& chainState : chainValues)
    {
        if ((int) container.chains.size() == engine::maximumChains)
            break;

        engine::Plan::Container::Chain chain;
        chain.id = getDeviceKey (chainState);
        chain.first = (int) strip.devices.size();
        strip.latency = latencyBefore;

        forEachInside (all, chainState,
                       [&add] (const DeviceValue& inside)
                       {
                           if (isDeviceForm (inside))
                               add (inside);
                       });

        chain.count = (int) strip.devices.size() - chain.first;
        chain.latency = strip.latency;
        latest = std::max (latest, strip.latency - latencyBefore);
        chain.state = getChain (chainState, all);
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

    const auto id = getDeviceKey (containerState);
    auto& kept = containers[id];

    if (kept == nullptr || kept->getLatency() != latest)
        kept = std::make_shared<engine::ContainerState> (preparedBlockSize, latest);

    kept->setMix (containerState.mix);
    container.state = kept;
    usedContainers.insert (id);

    // Its chains, each by its ID, which what is heard is worked out over.
    auto& listed = containerChains[id];
    listed.clear();

    for (const auto& chainState : chainValues)
        listed.push_back (getDeviceKey (chainState));

    updateChainAudibility (id);
    strip.containers.push_back (std::move (container));
}

std::shared_ptr<engine::ChainState> SongPlayer::getChain (const DeviceValue& chainState, const SongDevices& all)
{
    const auto id = getDeviceKey (chainState);
    auto& chain = chains[id];

    if (chain == nullptr)
    {
        chain = std::make_shared<engine::ChainState>();
        chain->prepare (preparedRate, preparedBlockSize);
    }

    setChainValues (*chain, chainState, all);
    usedChains.insert (id);
    return chain;
}

void SongPlayer::setChainValues (engine::ChainState& chain, const DeviceValue& value, const SongDevices& all)
{
    // As the song's values told it (takeDevice()): its fader's volume and pan; its crossover, the default for its place among
    // its container's chains (as the song's devices handed whole say it) where it has none of its own; and the notes it plays.
    auto& fader = chain.getFader();
    fader.setVolume (std::max (value.volume, engine::Channel::silenceDb));
    fader.setPan (value.pan);

    auto index = 0, count = 1;

    if (const auto* siblings = all.findList (getDeviceListPlace (value.place, value.form)))
    {
        count = (int) siblings->size();

        for (size_t i = 0; i < siblings->size(); ++i)
            if ((*siblings)[i] == value.id)
                index = (int) i;
    }

    chain.setCrossover (value.crossover.value_or (getDefaultCrossover (index, count)));
    chain.setNotes (value.keys.low, value.keys.high, value.velocities.low, value.velocities.high);
}

void SongPlayer::updateChainAudibility (std::int64_t container)
{
    // A chain is heard unless it is muted, or another of its container's chains is soloed and it isn't: each read from the
    // store the player holds (heldStore), by the chain's ID.
    const auto listed = containerChains.find (container);

    if (listed == containerChains.end())
        return;

    const auto isSoloed = [this] (std::int64_t chain) { return isChainSoloedIn (heldStore, DeviceID::fromRaw ((std::uint64_t) chain)); };
    const auto anySoloed = std::any_of (listed->second.begin(), listed->second.end(), isSoloed);

    for (const auto chain : listed->second)
        if (const auto found = chains.find (chain); found != chains.end())
            found->second->getFader().setAudible (! isChainMutedIn (heldStore, DeviceID::fromRaw ((std::uint64_t) chain))
                                                  && (! anySoloed || isSoloed (chain)));
}

engine::ChainState* SongPlayer::findChain (std::int64_t id) const
{
    const auto found = chains.find (id);
    return found != chains.end() ? found->second.get() : nullptr;
}

std::vector<SongPlayer::ChannelInfo> SongPlayer::getChannels (const SongTracks& songTracks, const SongSends& songSends) const
{
    // Each track's, bus's and return's: where it goes (the bus it is in, or the master), and what it takes in.
    std::vector<ChannelInfo> infos;
    const auto& all = getDevicesValue (song);

    for (const auto id : getTrackOrder (song).order)
    {
        const auto* value = songTracks.tracks.find (id.getRaw());

        if (value == nullptr || (value->isFolder() && ! value->fader.isValid()))
            continue; // a plain folder's VCA turns its tracks (setChannelFrom())

        ChannelInfo info;
        info.value = *value;
        info.id = getChannelKey (*value);

        if (! value->isReturn())
            for (const auto* parent = songTracks.tracks.find (value->parent.getRaw()); parent != nullptr && info.output == 0;
                 parent = songTracks.tracks.find (parent->parent.getRaw()))
                if (parent->fader.isValid())
                    info.output = getChannelKey (*parent);

        if (const auto* list = songSends.tracks.find ((std::uint64_t) info.id))
            for (const auto sendID : *list)
                if (const auto* send = songSends.find (sendID))
                    info.sends.push_back (*send);

        // A pad track plays its drum sampler's pad, after the sampler's track.
        if (value->pad.has_value())
        {
            info.padDevice = (std::int64_t) value->padDevice.getRaw();
            info.pad = *value->pad;
            info.padSource = (std::int64_t) getDeviceChannel (song, DeviceID::fromRaw ((std::uint64_t) info.padDevice)).getRaw();
        }

        infos.push_back (std::move (info));
    }

    for (auto& info : infos)
    {
        const auto isReturn = info.value.isReturn();

        for (const auto& other : infos)
        {
            if (other.output == info.id && ! isReturn)
                info.needs.push_back (other.id);

            if (isReturn)
                for (const auto& send : other.sends)
                    if ((std::int64_t) send.to.getRaw() == info.id)
                        info.needs.push_back (other.id);
        }

        const std::function<void (const DeviceValue&)> addSidechains = [&info, &all, &addSidechains] (const DeviceValue& device)
        {
            if (isDeviceForm (device))
            {
                if (const auto source = (std::int64_t) device.sidechain.getRaw(); source != 0)
                    info.needs.push_back (source);
            }
            else
            {
                forEachInside (all, device, addSidechains);
            }
        };

        forEachOnChannel (all, info.value, addSidechains);

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

void SongPlayer::addModulation (engine::Plan::Strip& strip, const SongModulation& modulation, const TrackValue& channelState,
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

    const auto isMaster = channelState.isMaster();
    const auto channel = isMaster ? TrackID() : channelState.id;
    addSources (modulation.channels.find (isMaster ? 0 : channel.getRaw()));

    forEachOnChannel (getDevicesValue (song), channelState,
                      [&addSources, &modulation] (const DeviceValue& child)
                      {
                          if (child.form == DeviceValue::Form::container)
                              addSources (modulation.containers.find (child.id.getRaw()));
                      });

    const auto fader = (std::int64_t) channelState.fader.getRaw();
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
        // units (engine/automation/Curve.h). One of a parameter the song hasn't (one its device doesn't have) plays from 0
        // to 1 until it has, which each plan looks for again.
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

void SongPlayer::addFaderAutomation (engine::Plan::Strip& strip, const TrackValue& channelState, bool isMaster)
{
    // The fader's volume and pan, by its parameters' IDs (the master's are its own).
    const auto fader = channelState.fader.getRaw();
    faders[fader] = strip.id;
    strip.volume = findCurve (fader, isMaster ? "master volume" : "volume");
    strip.pan = findCurve (fader, isMaster ? "master pan" : "pan");
}

void SongPlayer::loadSounds (engine::DeviceNode& node, const DeviceValue& deviceState)
{
    // What a drum sampler's pads, a synth's oscillators and a sampler play: the song's SOUNDS name it as the
    // plug-in's state does.
    soundLoads.fetch_add (1, std::memory_order_relaxed);
    const auto sounds =
        makeSoundsElement (deviceState, 0, (int) getSoundPropertiesOf (std::string_view (deviceState.type.toRawUTF8())).size());
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

void SongPlayer::setChannelFrom (const SongTracks& tracks, const TrackValue& channel)
{
    if (auto* strip = findStrip (channel.isMaster() ? 0 : (std::int64_t) channel.id.getRaw()))
    {
        // Its fader, turned by the VCAs of the plain folders it is in (the master's -3 dB where it has none: TracksValue.h),
        // from the values handed: the compile's, and what tracksChanged() tells.
        auto volume = channel.volume;

        if (! channel.isMaster())
            for (const auto* parent = tracks.tracks.find (channel.parent.getRaw()); parent != nullptr;
                 parent = tracks.tracks.find (parent->parent.getRaw()))
                if (parent->isFolder() && ! parent->fader.isValid())
                    volume += parent->volume;

        auto& faderChannel = strip->getChannel();
        faderChannel.setVolume (std::max (volume, engine::Channel::silenceDb));
        faderChannel.setPan (channel.pan);
    }
}

void SongPlayer::updateAudibility()
{
    // A track is heard unless it is muted, or in a muted folder, or other tracks are soloed and neither it nor a folder it is
    // in is (a return is never silenced by a solo); a pad's track, as its sampler's track is too. A render before a channel's
    // fader (a freeze, a bounce: RenderOf) hears that channel whatever its mute switch says, and what feeds it whatever
    // other tracks' solos say: it plays the channel alone already. From the tracks and their switches the player holds
    // (heldTracks: never the song's, as this runs as the song's values tell a track muted or soloed, takeTrackSwitches()).
    const auto& tracks = heldTracks->tracks;
    const auto& held = heldTracks->switches;
    const auto beforeFader = rendered.has_value() && rendered->beforeFader;

    const auto isOn = [] (const immer::map<std::uint64_t, bool>& map, const TrackValue& track)
    {
        const auto* on = map.find (track.id.getRaw());
        return on != nullptr && *on;
    };

    const auto findHeld = [&tracks] (TrackID id) { return tracks.tracks.find (id.getRaw()); };
    const auto anySoloed = ! beforeFader
                           && std::any_of (tracks.tracks.begin(), tracks.tracks.end(),
                                           [&isOn, &held] (const auto& entry) { return isOn (held.solos, entry.second); });

    // A bus carries a soloed track in it.
    const std::function<bool (TrackID)> holdsSoloed = [&] (TrackID folder)
    {
        if (const auto* list = tracks.findList (folder))
            for (const auto id : *list)
                if (const auto* child = findHeld (id); child != nullptr && (isOn (held.solos, *child) || holdsSoloed (id)))
                    return true;

        return false;
    };

    // What the song's sidechains listen to, the master's and those in containers too, from the devices the player holds
    // (heldDevices: never the song's).
    std::set<std::int64_t> keys;

    for (const auto& [id, device] : heldDevices)
        if (device.sidechain != 0)
            keys.insert (device.sidechain);

    // A pad's track goes with its sampler's track, whose mute and solo cover its pads on tracks of their own (its sampler's
    // channel as the player holds it; none on the master).
    const auto getSamplerTrack = [this, &findHeld] (const TrackValue& pad) -> const TrackValue*
    {
        const auto found = heldDevices.find (pad.padDevice.getRaw());
        return found != heldDevices.end() && found->second.channel != 0
                   ? findHeld (TrackID::fromRaw ((std::uint64_t) found->second.channel))
                   : nullptr;
    };

    // Whether a track is muted or soloed, by its own switches or a folder's it is in.
    struct Switches
    {
        bool muted = false, soloed = false;
    };

    const auto getSwitches = [&] (const TrackValue& track)
    {
        Switches switches;
        const auto addFrom = [&] (const TrackValue* from)
        {
            for (const auto* each = from; each != nullptr; each = findHeld (each->parent))
            {
                switches.muted = switches.muted || isOn (held.mutes, *each);
                switches.soloed = switches.soloed || isOn (held.solos, *each);
            }
        };

        addFrom (&track);

        if (track.pad.has_value())
            addFrom (getSamplerTrack (track));

        return switches;
    };

    const auto isRendered = [&] (const TrackValue& track) { return beforeFader && (std::int64_t) track.id.getRaw() == rendered->channel; };
    const auto isSoloedOut = [&] (const TrackValue& track)
    {
        const auto soloed = getSwitches (track).soloed || (track.isFolder() && holdsSoloed (track.id));
        return ! isRendered (track) && anySoloed && ! soloed && ! track.isReturn();
    };
    const auto isHeard = [&] (const TrackValue& track)
    { return isRendered (track) || (! getSwitches (track).muted && ! isSoloedOut (track)); };

    // A return that is soloed hears what is sent to it, from tracks its solo silences too, as a soloed return does in
    // Ableton Live, Logic Pro and Cubase: else soloing a return, to hear an effect alone, would play silence.
    const auto isSoloedReturn = [&] (std::int64_t id)
    {
        const auto* target = findHeld (TrackID::fromRaw ((std::uint64_t) id));
        return target != nullptr && target->isReturn() && getSwitches (*target).soloed && isHeard (*target);
    };

    // Whether a track is a folder a bus is in, or the bus.
    const auto isAboveOrIs = [&findHeld] (const TrackValue& track, const TrackValue& bus)
    {
        for (const auto* each = &bus; each != nullptr; each = findHeld (each->parent))
            if (each->id == track.id)
                return true;

        return false;
    };

    // Whether a track, or a folder it is in, is muted below a bus: the bus's own mute, and its folders', don't count.
    const auto isMutedBelow = [&] (const TrackValue* track, const TrackValue& bus)
    {
        for (const auto* each = track; each != nullptr && ! isAboveOrIs (*each, bus); each = findHeld (each->parent))
            if (isOn (held.mutes, *each))
                return true;

        return false;
    };

    // Whether a send of a channel goes on. Mute silences a channel's output only (the owner's decision;
    // docs/design/decisions.md): its sends before its fader go on, and those after it are silent with it. What another
    // track's solo silences sends nothing; but a track that only a solo silences (not its mute) still sends to a return
    // that is soloed.
    const auto sendGoesOn = [&] (const TrackValue& track, const SendValue& send)
    {
        const auto heard = isHeard (track);
        return heard || (send.preFader && ! isSoloedOut (track))
               || (! heard && ! getSwitches (track).muted && isSoloedReturn ((std::int64_t) send.to.getRaw()));
    };

    // A track's sends, by its ID, as the player holds them (heldSends: never the song's), each in turn until `each` returns
    // true; whether one did.
    const auto anySendOf = [this] (const TrackValue& track, const std::function<bool (const SendValue&)>& each)
    {
        if (const auto* list = heldSends.tracks.find (track.id.getRaw()))
            for (const auto id : *list)
                if (const auto* send = heldSends.find (id); send != nullptr && each (*send))
                    return true;

        return false;
    };

    // Whether something still listens to a bus that isn't heard: a sidechain keyed from it, or a send of it that goes on
    // (one before its fader, while it is muted).
    const auto isListenedTo = [&] (const TrackValue& bus)
    {
        return keys.contains ((std::int64_t) bus.id.getRaw())
               || anySendOf (bus, [&] (const SendValue& send) { return sendGoesOn (bus, send); });
    };

    // A track that isn't heard plays on into a bus that something still listens to while that bus isn't heard either
    // (soloed out, or muted: a muted bus feeds its key, as a muted track does, and its sends before its fader), unless it is
    // muted below the bus, a pad on a track of its own by its sampler's track too (known bug 18): the bus's own mute or solo
    // silences it after the bus has taken it in, and what else it sends, after its fader, is silent meanwhile. A bus that
    // is heard plays its tracks as they are heard, so a track soloed in it plays alone (the owner, 2026-11-13: soloing a
    // drum in the Drums bus played the bus's other drums).
    const auto playsForListener = [&] (const TrackValue& track)
    {
        for (const auto* bus = findHeld (track.parent); bus != nullptr; bus = findHeld (bus->parent))
            if (! isHeard (*bus) && isListenedTo (*bus))
                return ! isMutedBelow (&track, *bus) && ! (track.pad.has_value() && isMutedBelow (getSamplerTrack (track), *bus));

        return false;
    };

    for (const auto& entry : tracks.tracks)
    {
        const auto& track = entry.second;
        auto* strip = findStrip ((std::int64_t) entry.first);

        if (strip == nullptr)
            continue;

        // A track plays on, its devices too, whether it is heard or not; each of its sends as it goes on.
        anySendOf (track,
                   [&] (const SendValue& send)
                   {
                       if (const auto found = sendLevels.find ((std::int64_t) send.id.getRaw()); found != sendLevels.end())
                           found->second->setCut (! sendGoesOn (track, send));

                       return false;
                   });

        strip->getChannel().setAudible (isHeard (track) || playsForListener (track));
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

bool SongPlayer::takeStoreEntry (const SongValues::DeviceStoreChange& change)
{
    using Kind = SongValues::DeviceStoreChange::Kind;

    switch (change.kind)
    {
        case Kind::pluginValue:
        {
            // A plug-in's value set by hand, by its own ID; its settings and program its instance takes
            // (model/plugins/internal/PluginInstances.h).
            if (! change.valueToGive.has_value())
                return false;

            if (auto* node = findDevice ((std::int64_t) change.item.getRaw()))
                if (const auto index = findParameter (*node, change.parameter); index >= 0)
                    node->setValue (index, *change.valueToGive);

            return true;
        }

        case Kind::chainMute:
        case Kind::chainSolo:
        {
            // A container's chain's mute and solo, its container's chains heard as the store holds them now.
            for (const auto& [container, listed] : containerChains)
                if (std::find (listed.begin(), listed.end(), (std::int64_t) change.item.getRaw()) != listed.end())
                    updateChainAudibility (container);

            return true;
        }

        case Kind::state:
        case Kind::program:
        case Kind::formatID:
        case Kind::version:
            break;
    }

    return false;
}

SongPlayer::DeviceTaken SongPlayer::takeDevice (const SongValues::DeviceChange& change, const SongDevices& all)
{
    using Change = SongValues::DeviceChange;
    using Form = DeviceValue::Form;
    DeviceTaken taken;

    if (change.was == nullptr || change.now == nullptr || change.has (Change::made) || change.has (Change::place))
        return taken;

    const auto& was = *change.was;
    const auto& now = *change.now;
    const auto id = (std::int64_t) change.id.getRaw();
    auto* node = (now.form == Form::own || now.form == Form::plugin) ? findDevice (id) : nullptr;

    // Its settings: a device switched plays at once, and needs a new plan for the delays that line the tracks up, which its
    // latency changes; a container's or a chain's name alone changes nothing that plays; anything else (a sidechain, a type,
    // which plug-in, a container's switch, split or kind) needs a plan.
    if (change.has (Change::settings))
    {
        const auto nameAlone = was.form == now.form && was.off == now.off && was.sidechain == now.sidechain && was.type == now.type
                               && was.plugin == now.plugin && was.split == now.split && was.instrument == now.instrument;

        if (nameAlone)
        {
            taken.atOnce = true;
        }
        else
        {
            if (node != nullptr && was.off != now.off)
                node->setOn (! now.off);

            taken.needsPlan = true;
        }
    }

    // A container's mix at once; a plug-in's in a new plan.
    if (change.has (Change::mix))
    {
        if (now.form == Form::container)
        {
            if (const auto found = containers.find (id); found != containers.end())
                found->second->setMix (now.mix);

            taken.atOnce = true;
        }
        else
        {
            taken.needsPlan = true;
        }
    }

    // Phazite's own device's values that differ, each at once (the parameter's default for one set back to it); in a new plan,
    // with the device made anew (getDevice()), if they make its sound come later or sooner.
    if (change.has (Change::values))
    {
        if (node != nullptr && now.form == Form::own)
        {
            const auto set = [node, &now] (std::uint32_t index) { node->setValue ((int) index, now.getParameterValue ((int) index)); };

            immer::diff (
                was.values, now.values, [&set] (const auto& entry) { set (entry.first); },
                [&set] (const auto& entry) { set (entry.first); }, [&set] (const auto& entry, const auto&) { set (entry.first); });

            taken.changesLatency = changesLatency (*node, readValuesSetByHand (*node));
        }

        taken.atOnce = taken.atOnce || ! taken.changesLatency; // one that waits for its plan was taken at once in part only
    }

    // Its sounds that differ: loaded now into the node the next plan keeps, which doesn't load them again. Its first sound
    // and its last too (soundsAppeared), which make a plan as well: what asks the node for its sound right after the sound
    // was set (an editor's picture of it) finds it there, and the plan that follows doesn't load it a second time.
    if (change.has (Change::sounds) || change.has (Change::soundsAppeared))
    {
        if (node != nullptr && now.form == Form::own)
        {
            loadSoundsChanged (*node, was, now);
            changes.soundsTaken.insert (id);
        }

        taken.atOnce = true;
    }

    // A container's chain's volume, pan and crossover and the notes it plays, at once.
    if (change.has (Change::chainFader) || change.has (Change::chainRange))
    {
        if (const auto found = chains.find (id); found != chains.end())
            setChainValues (*found->second, now, all);

        taken.atOnce = true;
    }

    return taken;
}

void SongPlayer::loadSoundsChanged (engine::DeviceNode& node, const DeviceValue& was, const DeviceValue& now)
{
    // Which sounds differ, by their indices among the type's (DevicesValue.h: getSoundPropertiesOf()).
    std::set<int> indices;
    const auto add = [&indices] (std::uint16_t index) { indices.insert ((int) index); };
    immer::diff (
        was.sounds, now.sounds, [&add] (const auto& entry) { add (entry.first); }, [&add] (const auto& entry) { add (entry.first); },
        [&add] (const auto& entry, const auto&) { add (entry.first); });

    if (indices.empty())
        return;

    soundLoads.fetch_add (1, std::memory_order_relaxed);

    // What a loader reads of the device's sounds, as an element made of the value after (those of the properties from `first`
    // on, `count` of them), the song's tree read nowhere: the loaders take a SOUNDS element (DrumKit.h, Wavetables.h,
    // SamplerSound.h).
    const auto& properties = getSoundPropertiesOf (std::string_view (now.type.toRawUTF8()));
    const auto makeSounds = [&properties, &now] (int first, int count)
    {
        juce::ValueTree sounds (schema::SOUNDS);

        for (auto i = first; i < first + count && i < (int) properties.size(); ++i)
            if (const auto* sound = now.sounds.find ((std::uint16_t) i))
                sounds.setProperty (properties[(size_t) i], *sound, nullptr);

        return sounds;
    };

    auto& processor = node.getRunner().getProcessor();
    const auto type = node.getDeviceInfo().typeID;

    // A pad's layers' sounds and files, an oscillator's table and file: each pad or oscillator whose sound changed, once, as
    // Swap changes one at each sound it tries.
    if (type == devices::DrumSampler::typeID)
    {
        constexpr auto perPad = devices::DrumSampler::numLayers * 2;
        std::set<int> pads;

        for (const auto index : indices)
            pads.insert (index / perPad);

        for (const auto pad : pads)
            if (juce::isPositiveAndBelow (pad, devices::DrumSampler::numPads))
                loadPadSound (static_cast<devices::DrumSampler&> (processor), song, makeSounds (pad * perPad, perPad), pad);
    }

    if (type == devices::Synth::typeID)
    {
        std::set<int> oscillators;

        for (const auto index : indices)
            oscillators.insert (index / 2);

        for (const auto osc : oscillators)
            if (juce::isPositiveAndBelow (osc, devices::Synth::numOscillators))
                loadOscillatorTable (static_cast<devices::Synth&> (processor), song, makeSounds (osc * 2, 2), osc);
    }

    // A sampler's sound and its file (and with them its slices), or its slices by hand alone, which change as they are
    // dragged, without reading its sound again.
    if (type == devices::Sampler::typeID)
    {
        const auto sounds = makeSounds (0, (int) properties.size());

        if (indices.contains (0) || indices.contains (1))
            loadSamplerSound (static_cast<devices::Sampler&> (processor), song, sounds);

        loadSamplerSlices (static_cast<devices::Sampler&> (processor), sounds);
    }
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

SongPlayer::TrackAudio SongPlayer::makeAudio (const TrackValue& trackState, std::optional<ClipTempo>& tempo, bool askAgain)
{
    TrackAudio made;

    // The track's clips in the order it holds them (Clips.h: getClipValues()), which checkAudio() compares the regions in.
    for (const auto& clip : getClipValues (song, trackState.id))
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

void SongPlayer::addAudio (engine::Plan::Strip& strip, const TrackValue& trackState, std::optional<ClipTempo>& tempo)
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

void SongPlayer::checkAudio (const TrackAudio& kept, const TrackValue& trackState, std::optional<ClipTempo>& tempo)
{
    // What makeAudio() would make of the track now, without making anything: the same clips, each the same file (as this
    // player knows it now: a file that changed under the same name, once it asked, makes the track's audio again), the same
    // place in it and the same region, as the plan before placed them. A difference is a change of the song's that its
    // player didn't hear of (markChange()), or a file it found changed and didn't make the track's audio again for.
    auto same = true;
    size_t index = 0;

    for (const auto& clip : getClipValues (song, trackState.id))
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

std::vector<int> SongPlayer::getHeardInput (const TrackValue& trackState) const
{
    const auto monitoring = trackState.monitoring;
    const auto isArmed = armed.contains (trackState.id);

    if (monitoring == TrackValue::Monitoring::always || (isArmed && monitoring == TrackValue::Monitoring::whileArmed))
        return getInputChannels (trackState.input);

    return {};
}

bool SongPlayer::addFrozenFile (engine::Plan::Strip& strip, const TrackValue& trackState)
{
    // Its frozen file (model/mixer/internal/Freeze.h) from the song's start, if it is there (its frozen switch by a look in the
    // switches: Tracks.h).
    if (! isTrackFrozen (song, trackState.id))
        return false;

    auto file = getAudioFile (getFrozenFile (song, trackState.id), true);

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

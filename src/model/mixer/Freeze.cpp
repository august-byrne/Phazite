// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Freeze.h"
#include "FreezeModel.h"
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Consolidate.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/engine/internal/AudioIO.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/OfflineRender.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    constexpr const char* frozenFilePrefix = "Frozen ";

    // A track that can be frozen or bounced: one of the playlist's with sound of its own, not a folder or a return (nor the
    // master), by its kind (model/arrangement/internal/Tracks.h).
    bool canRender (const Song& song, TrackID track)
    {
        return getTrackKind (song, track) == TrackValue::Kind::track;
    }

    // Whether a device on a channel, or a device in a container on it, listens to another track through its sidechain: by
    // their IDs, among the channel's outline (Mixer.h: getDeviceOutline()).
    bool listensToSidechain (const Song& song, const std::vector<DeviceOutline>& outline, const DeviceOutline& device)
    {
        if (getSidechainSource (song, device.id).isValid())
            return true;

        return device.form == DeviceValue::Form::container
               && std::any_of (outline.begin(), outline.end(),
                               [&song, &device] (const DeviceOutline& inside)
                               {
                                   return inside.container == device.id && inside.form != DeviceValue::Form::chain
                                          && getSidechainSource (song, inside.id).isValid();
                               });
    }

    // Why a track can't be rendered on its own, frozen or bounced (what it would be, and what is done to it); empty if it
    // can.
    juce::String getWhyNotRender (const Song* song, TrackID track, const juce::String& verb, const juce::String& done)
    {
        const auto kind = song != nullptr ? getTrackKind (*song, track) : std::nullopt;

        if (! kind.has_value() || *kind == TrackValue::Kind::master)
            return "Select a track to " + verb + ".";

        if (*kind == TrackValue::Kind::folder)
            return "Folders and buses can't be " + done + ": their sound is their tracks'.";

        if (*kind == TrackValue::Kind::returnTrack)
            return "Returns can't be " + done + ": their sound comes from the tracks that send to them.";

        if (isRecording (*song))
            return "Stop recording first.";

        // A drum sampler's pads on tracks of their own leave its track, so its render would leave them out.
        if (hasPadsOnOwnTracks (*song, track))
            return getTrackName (*song, track) + " has pads on tracks of their own: bring them back to it first.";

        if (! hasClips (*song, track))
            return getTrackName (*song, track) + " has nothing to " + verb + ": it has no clips.";

        return {};
    }

    // Properties of an element that don't change what a frozen track sounds like: where a device's window was, view state.
    bool isIgnoredProperty (const juce::Identifier& property)
    {
        return schema::isDeviceViewProperty (property);
    }

    // Whether a device, a container or a chain sounds now as it did as its track was frozen: alike in all the devices' value
    // covers but its place (DevicesValue.h: DeviceValue::operator==(), each map by identity first), what the elements it was
    // compared as before held (a window's place is no part of the value, and a plug-in's state is the store's: below).
    bool isSameSound (const DeviceValue& now, const DeviceValue& then)
    {
        if (now.isSameAs (then))
            return true;

        auto placed = now;
        placed.place = then.place;
        return placed == then;
    }

    // Whether the store holds the same now of an item as the values kept as it was frozen: a plug-in's values set by hand,
    // program, format ID and version, a chain's mute and solo; not a plug-in's state, which every save stores again.
    bool holdSameStoreOf (const DeviceStore& now, const DeviceStore& then, DeviceID id)
    {
        const auto withoutState = [] (DeviceStore entries)
        {
            entries.states = {};
            return entries;
        };

        return withoutState (takeEntriesOf (now, id)) == withoutState (takeEntriesOf (then, id));
    }

    // What a container holds (its chains) or a chain (its devices), in their order; none for anything else.
    const immer::flex_vector<DeviceID>* findInside (const SongDevices& devices, const DeviceValue& value)
    {
        if (value.form == DeviceValue::Form::container)
            return devices.chains.find (value.id.getRaw());

        return value.form == DeviceValue::Form::chain ? devices.chainDevices.find (value.id.getRaw()) : nullptr;
    }

    // Whether a frozen part (a device, or a container with its chains and their devices) sounds now as it did as its track
    // was frozen, by the song's devices and store now and as they were kept then (Holds::values): each item alike
    // (isSameSound()) with the store's entries of a device and a chain (holdSameStoreOf(), skipped where the whole store is
    // the same value), and what it holds the same IDs in the same order, each alike in turn. Nothing written, nothing read
    // into a value: looks in the two values by ID.
    bool soundsAsFrozen (const SongDevices& now, const DeviceStore& storeNow, const ValuesKept& then, DeviceID id)
    {
        const auto* is = now.find (id);
        const auto* was = then.devices.find (id);

        if (is == nullptr || was == nullptr)
            return false;

        if (! isSameSound (*is, *was))
            return false;

        if (is->form != DeviceValue::Form::container && ! storeNow.isSameAs (then.deviceStore)
            && ! holdSameStoreOf (storeNow, then.deviceStore, id))
            return false;

        const auto* listNow = findInside (now, *is);
        const auto* listThen = findInside (then.devices, *was);

        if (listNow == nullptr || listThen == nullptr)
            return listNow == listThen;

        if (listNow->identity() != listThen->identity() && ! (*listNow == *listThen))
            return false;

        return std::all_of (listNow->begin(), listNow->end(),
                            [&now, &storeNow, &then] (DeviceID inside) { return soundsAsFrozen (now, storeNow, then, inside); });
    }

    // A frozen track's modulators and macros as values (Modulation.h: getOwnedModulators()): its own, in their order, then
    // each frozen container's macros, in the frozen devices' order.
    std::vector<ModulatorValue> getFrozenModulators (const Song& song, TrackID track)
    {
        const ModulatorPlace channel { .track = track };
        auto modulators = getOwnedModulators (song, channel);

        for (const auto id : getFrozenDevices (song, track))
        {
            if (! isContainer (song, id))
                continue;

            const auto macros = getOwnedModulators (song, { .track = track, .container = id });
            modulators.insert (modulators.end(), macros.begin(), macros.end());
        }

        return modulators;
    }

    // What a frozen track's file plays the automation of, by the IDs its curves name (Automation.h): its frozen devices, the
    // devices in its frozen containers, and their chains, whose volume, pan and crossover are automated by their IDs.
    std::set<DeviceID> getFrozenCurveOwners (const Song& song, TrackID track)
    {
        std::set<DeviceID> devices;
        const auto outline = getDeviceOutline (song, track);

        // Each frozen device, and what is in a frozen container: its chains and their devices (Mixer.h: getDeviceOutline()).
        for (const auto id : getFrozenDevices (song, track))
        {
            devices.insert (id);

            for (const auto& inside : outline)
                if (inside.container == id)
                    devices.insert (inside.id);
        }

        return devices;
    }

    // The curves of what a frozen track's file plays the automation of, as values (Automation.h: getCurveValue()), in the
    // song's order of its curves.
    SongCurves getFrozenCurves (const Song& song, TrackID track)
    {
        const auto owners = getFrozenCurveOwners (song, track);
        SongCurves curves;

        for (const auto& parameter : getCurveKeys (song))
            if (owners.contains (parameter.device))
                if (auto curve = getCurveValue (song, parameter))
                    curves = withCurve (curves, parameter, std::move (*curve));

        return curves;
    }

    FreezeKeeper::Holds copyWhatTheFreezeHolds (const Song& song, TrackID track)
    {
        return { .parts = getFrozenDevices (song, track),
                 .values = keepValues (song),
                 .tempos = getTempoMapValue (song).tempos,
                 .curves = getFrozenCurves (song, track),
                 .modulators = getFrozenModulators (song, track) };
    }

    // Whether a frozen track's file holds what it plays now; its curves left out where they are known to (compareCurves
    // false: FreezeKeeper::curvesAsFrozen), and its modulators and macros the same (compareModulators false:
    // FreezeKeeper::modulatorsAsFrozen).
    bool holdsWhatWasFrozen (const Song& song, TrackID track, const FreezeKeeper::Holds& frozen, bool compareCurves, bool compareModulators)
    {
        if (getFrozenDevices (song, track) != frozen.parts)
            return false;

        // Each as it is in the song, against what copyWhatTheFreezeHolds() kept: as values, since this runs at every change in
        // a frozen track (a fader dragged): its frozen devices, the same IDs in the same order, each alike in the song's
        // devices' value now and as kept then (the same value at once: nothing compared), never written into elements; its
        // clips with their notes as values, the same values or ones alike; the song's tempos, the same value or one alike (a
        // tempo changed and changed back compares as the same); and its frozen devices' curves, the same curves in the same
        // order, each alike, and its modulators and macros, read into values only where asked. Its parts first: what its
        // curves are of (getFrozenCurveOwners()) and the containers whose macros it holds are in them, so curves and macros
        // left out are of the same owners as before.
        if (const auto* values = song.getValues())
        {
            const auto& devices = values->devices.get();
            const auto& store = values->deviceStore.get();

            if (! (devices.isSameAs (frozen.values.devices) && store.isSameAs (frozen.values.deviceStore)))
                for (const auto id : frozen.parts)
                    if (! soundsAsFrozen (devices, store, frozen.values, id))
                        return false;
        }

        if (const auto tempos = getTempoMapValue (song).tempos;
            tempos.identity() != frozen.tempos.identity() && ! (tempos == frozen.tempos))
            return false;

        if (compareCurves && ! (getFrozenCurves (song, track) == frozen.curves))
            return false;

        if (compareModulators && getFrozenModulators (song, track) != frozen.modulators)
            return false;

        return holdSameTrackClips (keepValues (song), frozen.values, track);
    }

    // The element of a type a tree is in (itself, if it is one), or an invalid one.
    juce::ValueTree findEnclosing (juce::ValueTree tree, const juce::Identifier& type)
    {
        for (; tree.isValid(); tree = tree.getParent())
            if (tree.hasType (type))
                return tree;

        return {};
    }

    // Where a track's sound ends in the song, before its tails: the end of its last clip, or of the last clip of a track
    // whose sound reaches it (the source of one of its sidechains), which plays through it in its render.
    BeatPosition getTrackEnd (const Song& song, TrackID track)
    {
        BeatPosition end;

        for (const auto other : getTrackIDs (song))
        {
            if (other != track)
                if (const auto fed = getTracksFedBy (song, other); std::find (fed.begin(), fed.end(), track) == fed.end())
                    continue;

            for (const auto& clip : getClipValues (song, other))
                end = std::max (end, getLiveClipRange (song, clip).getEnd());
        }

        return end;
    }

    // The rate a track renders at, and its blocks: the audio device's, as its file plays alongside what it plays
    // (model/engine/internal/AudioIO.h), or 44.1 kHz in blocks of 256 without one.
    struct RenderFormat
    {
        double sampleRate = 44100.0;
        int blockSize = 256;
    };

    RenderFormat getRenderFormat (const PlaybackSession* session)
    {
        RenderFormat format;

        if (auto* device = session != nullptr ? session->getAudioIO().getDevices().getCurrentAudioDevice() : nullptr)
        {
            if (device->getCurrentSampleRate() > 7000.0)
                format.sampleRate = device->getCurrentSampleRate();

            if (device->getCurrentBufferSizeSamples() > 0)
                format.blockSize = device->getCurrentBufferSizeSamples();
        }

        return format;
    }

    // Whether a track can be frozen through all its devices (none listens to another track): what Bounce in Place freezes.
    bool canFreezeWhole (const Song& song, TrackID track)
    {
        if (! canRender (song, track))
            return false;

        const auto outline = getDeviceOutline (song, track);
        const auto onChannel =
            std::count_if (outline.begin(), outline.end(), [] (const DeviceOutline& each) { return ! each.container.isValid(); });
        return getFrozenDevices (song, track).size() == (size_t) onChannel;
    }

    // A file's length in seconds, as JUCE's formats read it; 0 for one they can't.
    double getFileSeconds (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        return reader != nullptr && reader->sampleRate > 0.0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
    }
} // namespace

//==============================================================================
std::vector<DeviceID> getFrozenDevices (const Song& song, TrackID track)
{
    std::vector<DeviceID> devices;

    if (! canRender (song, track))
        return devices;

    // Its devices and containers on its channel, by their IDs (Mixer.h: getDeviceOutline()), up to the first that listens to
    // another track.
    const auto outline = getDeviceOutline (song, track);

    for (const auto& device : outline)
    {
        if (device.container.isValid())
            continue;

        if (listensToSidechain (song, outline, device))
            break;

        devices.push_back (device.id);
    }

    return devices;
}

juce::File getFrozenFile (const Song& song, TrackID track)
{
    return song.getCacheFolder().getChildFile (frozenFilePrefix + juce::String ((juce::int64) track.getRaw()) + ".wav");
}

void freezeFromFile (Song& song, TrackID track, const juce::File& rendered, juce::UndoManager* undo)
{
    if (! canRender (song, track))
        return;

    // The rendered file takes the place of an earlier one.
    const auto file = getFrozenFile (song, track);
    file.getParentDirectory().createDirectory();

    if (rendered != file && ! rendered.moveFileTo (file) && ! rendered.copyFileTo (file))
        return;

    findTrackState (song, track).setProperty (schema::trackFrozen, true, undo);

    // What the track holds as it is frozen, taken here by what keeps the song's frozen tracks (FreezeKeeper, the song's
    // writer of what is pending), not in its tree callback, which reads no clip of the song's (SongValues.h): so what
    // changes after it, in the same turn too, unfreezes it.
    song.writePending();
}

void unfreeze (Song& song, TrackID track)
{
    if (auto state = findTrackState (song, track); state.hasType (schema::TRACK))
        state.removeProperty (schema::trackFrozen, nullptr);
}

bool isFrozenDevice (const Song& song, DeviceID id)
{
    // On a frozen track's channel (not the master's), by its ID and its place there (Mixer.h: getDeviceOutline()).
    const auto channel = getDeviceChannel (song, id);

    if (! isTrackFrozen (song, channel))
        return false;

    // A device in a container is frozen with its container.
    const auto outline = getDeviceOutline (song, channel);
    const auto found = std::find_if (outline.begin(), outline.end(), [id] (const DeviceOutline& each) { return each.id == id; });

    if (found == outline.end())
        return false;

    const auto device = found->container.isValid() ? found->container : id;
    const auto frozen = getFrozenDevices (song, channel);
    return std::find (frozen.begin(), frozen.end(), device) != frozen.end();
}

std::vector<TrackID> getFrozenTracks (const Song& song)
{
    std::vector<TrackID> frozen;

    // Each track's switch read from the element the song's index gives it (TrackSwitchesValue.h), one walk of the order.
    for (const auto& state : getTrackStates (song))
        if (readTrackFrozen (state))
            frozen.push_back (readTrackElementID (state));

    return frozen;
}

//==============================================================================
void forgetMissingFrozenFiles (Song& song)
{
    for (auto state : getTrackStates (song))
        if ((bool) state[schema::trackFrozen] && ! getFrozenFile (song, getTrackID (state)).existsAsFile())
            state.removeProperty (schema::trackFrozen, nullptr);
}

void purgeCache (Song& song)
{
    purgeClipRenders (song);

    // The frozen files of tracks that aren't frozen, or are gone, and what a freeze left half rendered.
    std::set<juce::String> used;

    for (const auto track : getFrozenTracks (song))
        used.insert (getFrozenFile (song, track).getFileName());

    if (const auto folder = song.getCacheFolder(); folder.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, juce::String (frozenFilePrefix) + "*.wav"))
            if (! used.contains (entry.getFile().getFileName()))
                entry.getFile().deleteFile();
}

//==============================================================================
FreezeKeeper::FreezeKeeper (Song& s)
    : song (s),
      state (s.getState())
{
    takeFrozenTracks();
    state.addListener (this);

    if (auto* values = song.getValues())
        values->addListener (this);

    song.addPendingWriter (this);
}

FreezeKeeper::~FreezeKeeper()
{
    cancelPendingUpdate();
    song.removePendingWriter (this);

    if (auto* values = song.getValues())
        values->removeListener (this);

    state.removeListener (this);
}

void FreezeKeeper::writePending()
{
    if (! song.getUndoManager().isPerformingUndoRedo())
        compareMarked();
}

void FreezeKeeper::clipNotesChanged (const std::vector<ClipID>& clips)
{
    if (frozen.empty())
        return;

    notesChanged.insert (notesChanged.end(), clips.begin(), clips.end());
    triggerAsyncUpdate();
}

void FreezeKeeper::clipsChanged (const std::vector<SongValues::ClipChange>& clips, const std::vector<TrackID>& reordered)
{
    // The frozen tracks among those the clips were on and are on, and those whose clips came in another order (a track's
    // notes play in its clips' order where they meet): this one's own knowledge, nothing of the song's.
    const auto mark = [this] (TrackID track)
    {
        if (frozen.contains (track))
            clipTracks.insert (track);
    };

    for (const auto& clip : clips)
    {
        mark (clip.before);
        mark (clip.after);
    }

    for (const auto track : reordered)
        mark (track);

    if (! clipTracks.empty())
        triggerAsyncUpdate();
}

void FreezeKeeper::tempoMapChanged()
{
    // Any frozen track's file holds the song's tempos: every one compared at the later moment (as check() takes a change),
    // but what this changes itself (unfreezing) and what keepThrough() compares as it applies.
    if (checking)
        return;

    waiting.all = true;
    triggerAsyncUpdate();
}

void FreezeKeeper::curvesChanged (const std::vector<ParameterRef>& parameters)
{
    // The devices the curves are of: which frozen tracks' files hold them is worked out at the later moment (compareMarked()),
    // which reads the song. But what this changes itself (unfreezing) and what keepThrough() compares as it applies, and
    // anything while no track is frozen.
    if (checking || frozen.empty())
        return;

    for (const auto& parameter : parameters)
        curveOwners.insert (parameter.device);

    triggerAsyncUpdate();
}

void FreezeKeeper::modulationChanged (const std::vector<SongValues::ModulationChange>& modulators,
                                      const std::vector<ModulatorPlace>& reordered)
{
    // The frozen tracks among the channels they were on and are on (none is the master), this one's own knowledge: compared at
    // the later moment, as check() takes a change in a frozen track. But what this changes itself (unfreezing) and what
    // keepThrough() compares as it applies, and anything while no track is frozen.
    if (checking || frozen.empty())
        return;

    const auto mark = [this] (const ModulatorPlace& place)
    {
        if (! place.onMaster && frozen.contains (place.track))
            modulatorTracks.insert (place.track);
    };

    for (const auto& modulator : modulators)
    {
        if (modulator.wasThere)
            mark (modulator.before);

        if (modulator.isThere)
            mark (modulator.after);
    }

    for (const auto& place : reordered)
        mark (place);

    if (! modulatorTracks.empty())
        triggerAsyncUpdate();
}

void FreezeKeeper::devicesChanged (const std::vector<SongValues::DeviceChange>& devices, const std::vector<DevicePlace>& reordered,
                                   const SongDevices&)
{
    // The frozen tracks among the channels they were on and are on (none is the master), this one's own knowledge: compared at
    // the later moment, as check() takes a change in a frozen track. But what this changes itself (unfreezing) and what
    // keepThrough() compares as it applies, and anything while no track is frozen.
    if (checking || frozen.empty())
        return;

    auto marked = false;
    const auto mark = [this, &marked] (const DevicePlace& place)
    {
        if (! place.onMaster && frozen.contains (place.track))
        {
            waiting.tracks.insert (place.track);
            marked = true;
        }
    };

    for (const auto& device : devices)
    {
        if (device.wasThere)
            mark (device.before);

        if (device.isThere)
            mark (device.after);
    }

    for (const auto& place : reordered)
        mark (place);

    if (marked)
        triggerAsyncUpdate();
}

void FreezeKeeper::deviceStoreChanged (const std::vector<SongValues::DeviceStoreChange>& entries, const DeviceStore&)
{
    if (checking || frozen.empty())
        return;

    auto marked = false;

    for (const auto& entry : entries)
    {
        if (entry.kind == SongValues::DeviceStoreChange::Kind::state || ! entry.changesSong || ! entry.isInSong || entry.place.onMaster
            || ! frozen.contains (entry.place.track))
            continue;

        waiting.tracks.insert (entry.place.track);
        marked = true;
    }

    if (marked)
        triggerAsyncUpdate();
}

void FreezeKeeper::tracksChanged (const std::vector<TrackChange>& tracks, const std::vector<TrackID>& reordered, const SongTracks&)
{
    // But what this changes itself (unfreezing) and what keepThrough() compares as it applies. A track changed in place is
    // compared, as check() took a change of a TRACK; one added or taken out (or come to another folder) has the frozen
    // tracks noted again, and the folder it went from or came to compared, as a TRACK added to its parent or taken out of
    // it did.
    if (checking)
        return;

    for (const auto& track : tracks)
    {
        if (track.onMaster)
            continue;

        if (track.wasThere && track.isThere && ! track.has (TrackChange::made) && ! track.has (TrackChange::place))
        {
            markTrack (track.id, false, false); // in place: the same tracks are frozen as before
            continue;
        }

        const auto cameIn = track.isThere;

        if (track.wasThere)
            markTrack (track.parentBefore, false, true);

        if (track.isThere)
            markTrack (track.parentAfter, cameIn, true);
    }

    for (const auto folder : reordered)
        markTrack (folder, false, true);

    if (! tracks.empty() || ! reordered.empty())
        triggerAsyncUpdate();
}

void FreezeKeeper::trackSwitchesChanged (const std::vector<TrackSwitchChange>& switches, const TrackSwitches&)
{
    // A track frozen or played live again, muted, soloed or of another automation mode: as a TRACK's property was (but what
    // this changes itself, unfreezing).
    if (checking)
        return;

    for (const auto& change : switches)
        if (! change.onMaster)
            markTrack (change.track, false, change.field == TrackSwitchChange::Field::frozen);

    if (! switches.empty())
        triggerAsyncUpdate();
}

void FreezeKeeper::markTrack (TrackID track, bool cameIn, bool mayChangeFrozen)
{
    // The track compared at the later moment (none for the track list), and the frozen tracks noted again then: during an
    // undo or a redo taken again then (takeFrozenTracks()), else noted (noteFrozenTracks()), as if come in where one came.
    // Noted only where the change can make another set of tracks the frozen ones (a track came, went or moved, or one was
    // frozen or played live again): a fader moved or a track renamed leaves them the tracks they were, and noting them walks
    // every track of the song.
    if (track.isValid())
        waiting.tracks.insert (track);

    if (song.getUndoManager().isPerformingUndoRedo())
    {
        waiting.takeTracks = true;
    }
    else if (mayChangeFrozen)
    {
        waiting.noteTracks = true;
        waiting.cameIn = waiting.cameIn || cameIn;
    }
}

void FreezeKeeper::compareMarked()
{
    if (checking)
        return;

    // The frozen tracks noted again first, as a change of the tracks asked (markTrack()), from the song as it is now.
    if (std::exchange (waiting.noteTracks, false))
        noteFrozenTracks (std::exchange (waiting.cameIn, false));

    auto wait = std::exchange (waiting, {});
    auto tracks = std::exchange (wait.tracks, {});

    if (! wait.takeTracks && ! wait.all && tracks.empty() && notesChanged.empty() && clipTracks.empty() && toTake.empty()
        && curveOwners.empty() && modulatorTracks.empty())
        return;

    const juce::ScopedValueSetter<bool> busy (checking, true);

    // The tracks frozen in a step since (noteFrozenTracks()), each with what it holds now, if it is still frozen.
    for (const auto track : std::exchange (toTake, {}))
    {
        if (frozen.contains (track) || ! isTrackFrozen (song, track))
            continue;

        const auto holds = copyWhatTheFreezeHolds (song, track);
        frozen[track] = holds;
        files[track] = holds;
        curvesAsFrozen.erase (track);
        modulatorsAsFrozen.erase (track);
    }

    // A track frozen or played live again, or tracks added, removed or moved by the undo or the redo (as check() takes them).
    // One that came back frozen meanwhile is compared with what its file holds (files): what it was as it came back is gone
    // by now where the steps after it (a walk of undos and redos) changed it.
    if (wait.takeTracks)
        for (const auto track : takeFrozenTracks (true))
            tracks.insert (track);

    // Each frozen track one of whose clips, or of whose clips' notes, changed: compared whole now (a clip gone is one its
    // track's clips changed by, which clipsChanged() heard).
    for (const auto clip : std::exchange (notesChanged, {}))
        if (const auto track = getClipTrack (song, clip); frozen.contains (track))
            tracks.insert (track);

    tracks.merge (clipTracks);
    clipTracks.clear();

    // Each frozen track a modulator or a macro of which changed: its modulators compared too.
    for (const auto track : std::exchange (modulatorTracks, {}))
    {
        tracks.insert (track);
        modulatorsAsFrozen.erase (track);
    }

    // Each frozen track whose file plays the automation of a device whose curve changed: its curves compared too.
    if (const auto owners = std::exchange (curveOwners, {}); ! owners.empty())
    {
        for (const auto& entry : frozen)
        {
            if (const auto held = getFrozenCurveOwners (song, entry.first);
                std::any_of (owners.begin(), owners.end(), [&held] (DeviceID device) { return held.contains (device); }))
            {
                tracks.insert (entry.first);
                curvesAsFrozen.erase (entry.first);
            }
        }
    }

    if (wait.all)
        for (const auto& entry : frozen)
            tracks.insert (entry.first);

    for (const auto track : tracks)
        compare (track);
}

void FreezeKeeper::forgetMarked()
{
    notesChanged.clear();
    clipTracks.clear();
    toTake.clear();
    curveOwners.clear();
    modulatorTracks.clear();
    waiting = {};
}

void FreezeKeeper::noteFrozenTracks (bool cameIn)
{
    // Which tracks are frozen now, from the tree alone: one known as it was; one whose holdings wait for the later moment
    // still waits; one that came into the song frozen (cameIn: a track moved, which JUCE takes out and puts in again, or
    // brought back) with what its file was kept with, if this kept it, compared at the later moment; any other (frozen in
    // place, by freezeFromFile(), which has it taken at once, or by a version applied) waits for the later moment to take
    // what it holds.
    std::map<TrackID, Holds> now;
    std::set<TrackID> later, known;

    for (const auto track : getFrozenTracks (song))
    {
        if (const auto kept = frozen.find (track); kept != frozen.end())
        {
            now[track] = kept->second;
            known.insert (track);
        }
        else if (const auto file = files.find (track); cameIn && ! toTake.contains (track) && file != files.end())
        {
            now[track] = file->second;
            waiting.tracks.insert (track);
        }
        else
        {
            later.insert (track);
        }
    }

    frozen = std::move (now);
    toTake = std::move (later);
    keepAsFrozen (known);

    for (const auto& [track, holds] : frozen)
        files[track] = holds;
}

void FreezeKeeper::keepAsFrozen (const std::set<TrackID>& known)
{
    // A track whose holdings were taken again (from its file, or anew) is compared whole at its next comparison.
    const auto taken = [&known] (TrackID track) { return ! known.contains (track); };
    std::erase_if (curvesAsFrozen, taken);
    std::erase_if (modulatorsAsFrozen, taken);
}

std::vector<TrackID> FreezeKeeper::takeFrozenTracks (bool fromFiles)
{
    // The tracks that are frozen now, each with what its file holds, kept from before if it was frozen then; one frozen
    // since, a copy of what it holds now, or (fromFiles) what its file was kept with, if it was: those returned.
    std::map<TrackID, Holds> now;
    std::vector<TrackID> fromFile;
    std::set<TrackID> known;

    for (const auto track : getFrozenTracks (song))
    {
        if (const auto kept = frozen.find (track); kept != frozen.end())
        {
            now[track] = kept->second;
            known.insert (track);
        }
        else if (const auto file = files.find (track); fromFiles && file != files.end())
        {
            now[track] = file->second;
            fromFile.push_back (track);
        }
        else
            now[track] = copyWhatTheFreezeHolds (song, track);
    }

    frozen = std::move (now);
    keepAsFrozen (known);

    for (const auto& [track, holds] : frozen)
        files[track] = holds;

    return fromFile;
}

bool FreezeKeeper::keepThrough (const std::function<bool()>& apply)
{
    compareMarked(); // what changed before

    const juce::ScopedValueSetter<bool> busy (checking, true); // what it changes, and the unfreezing, aren't checked

    if (! apply())
        return false;

    forgetMarked(); // what apply changed: compared below

    // A frozen track stays frozen where its file holds what it plays now, its curves and its modulators compared too (what
    // apply changed of them was told while this checked nothing).
    frozen.clear();
    curvesAsFrozen.clear();
    modulatorsAsFrozen.clear();

    for (const auto track : getFrozenTracks (song))
    {
        if (const auto file = files.find (track);
            file != files.end() && getFrozenFile (song, track).existsAsFile() && holdsWhatWasFrozen (song, track, file->second, true, true))
        {
            frozen[track] = file->second;
            curvesAsFrozen.insert (track);
            modulatorsAsFrozen.insert (track);
        }
        else
        {
            unfreeze (song, track);
        }
    }

    return true;
}

void FreezeKeeper::compare (TrackID track)
{
    // What the freeze holds changed, or its file is gone (a Redo of Bounce in Place after the cache let go of it): the track
    // plays it live again. Its curves are read and compared only where they may differ from its file's: at its first
    // comparison with these holdings, or once one of them was told changed since (curvesAsFrozen); else they are as they
    // compared last, a fader dragged on it reading none of them. Its modulators and macros the same (modulatorsAsFrozen).
    const auto known = frozen.find (track);

    if (known == frozen.end())
        return;

    if (getFrozenFile (song, track).existsAsFile()
        && holdsWhatWasFrozen (song, track, known->second, ! curvesAsFrozen.contains (track), ! modulatorsAsFrozen.contains (track)))
    {
        curvesAsFrozen.insert (track);
        modulatorsAsFrozen.insert (track);
        return;
    }

    frozen.erase (known);
    curvesAsFrozen.erase (track);
    modulatorsAsFrozen.erase (track);
    unfreeze (song, track);
}

void FreezeKeeper::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // How tracks show and where windows were change nothing that plays; nor does the song's count of IDs, which every new
    // ID moves, or how it plays and its grid, on its root. The areas' of the song's values (isHeardAsKept()): the tempo map's,
    // tempoMapChanged(); the chord lane's, which no frozen file holds; the curves', curvesChanged().
    if ((tree == state && schema::isUncountedProperty (tree, property)) || isHeardAsKept (tree, property))
        return;

    if (! schema::isTrackViewProperty (property) && ! isIgnoredProperty (property))
        check (tree, false);
}

void FreezeKeeper::check (const juce::ValueTree& changed, bool cameIn)
{
    if (checking)
        return;

    // What the change asks to compare waits for the next turn (the class's comment), or for the song's being stored or
    // compiled, which writes what is pending first: a tree's callback reads no clip of the song's, with no exception
    // (SongValues.h: there is no order among the song's tree listeners, so the keeper of the song's values may not have
    // given a track it moved its clips back yet, and an undo puts the clips back after the tree).
    const juce::ScopedValueSetter<bool> busy (checking, true);
    triggerAsyncUpdate();
    const auto track = findEnclosing (changed, schema::TRACK);

    // What changed of the song itself, or of a track, that its values don't tell (a property Phazite doesn't know): the
    // frozen tracks noted again at the later moment, as a change of the tracks asks (markTrack(): taken again after an undo
    // or a redo), never here, which reads the song (tracksChanged(), trackSwitchesChanged() tell the rest: design J.4).
    if (changed == state || changed.hasType (schema::TRACKS) || (track.isValid() && changed == track))
        markTrack ({}, cameIn, true);

    if (track.isValid())
    {
        waiting.tracks.insert (getTrackID (track));
        return;
    }

    // The song's element: every frozen track. Its tempos: tempoMapChanged(); its curves: curvesChanged().
    if (changed == state)
        waiting.all = true;
}

//==============================================================================
namespace
{
    // A track rendered by Phazite's engine in the background (model/playback/internal/OfflineRender.h), before its fader,
    // through its devices or some of them, what feeds it playing only through it, from the song's start to the end of its
    // last clip and on as long as its tails sound, into a WAV file of 32-bit floats; then made something of on the message
    // thread: frozen, or bounced. Nothing of the song changes meanwhile.
    class RenderOfTrack : public TrackRender::Impl
    {
    public:
        RenderOfTrack (ProjectContext& c, Song& s, TrackID t, juce::String title, std::function<void (const juce::Result&)> callback)
            : context (c),
              song (&s),
              track (t),
              renderTitle (std::move (title)),
              onFinished (std::move (callback)),
              alive (std::make_shared<RenderOfTrack*> (this))
        {
        }

        ~RenderOfTrack() override
        {
            alive = nullptr; // no callback from here on
            render = nullptr;

            for (const auto& file : { rendered, copyTo })
                if (file.existsAsFile())
                    file.deleteFile();
        }

        float getProgress() const override { return render != nullptr ? render->getProgress() : 0.0f; }
        juce::String getTitle() const override { return renderTitle; }
        std::optional<RenderWait> getWait() const override { return render != nullptr ? render->findWait() : std::nullopt; }
        bool hasStarted() const noexcept { return render != nullptr; }

    protected:
        ProjectContext& context;
        const Song::Ptr song;
        const TrackID track;
        juce::File rendered; // what the engine plays into, used once it is complete
        juce::File copyTo;   // if any, where a copy of it goes, made on the render's thread once it is complete

        // Starts the render: of the track, with so much of its devices (all of them for none), at the audio device's rate.
        // The clips' renders it plays that aren't there yet are made on the render's thread first (OfflineRender.h).
        void start (std::optional<std::set<std::int64_t>> devices)
        {
            if (isSongPlaying (*song))
                stopAndReturn (*song);

            rendered.getParentDirectory().createDirectory();

            auto* session = findSession (*song);
            const auto format = getRenderFormat (session);
            const OfflineRender::Job job { rendered, toSeconds (*song, getTrackEnd (*song, track)), tailSeconds,
                                           SongPlayer::RenderOf { (std::int64_t) track.getRaw(), true, std::move (devices) } };
            const OfflineRender::Setup setup {
                .session = session,
                .makePlayer = [played = song, session]
                { return std::make_unique<SongPlayer> (*played, session != nullptr ? &session->getPlugins() : nullptr); },
                .clipsOf = song.get(),
                .numThreads = session != nullptr ? session->getHost().getNumThreads() : 1,
                .sampleRate = format.sampleRate,
                .blockSize = format.blockSize
            };

            // The copy made on the render's thread, not the message thread's once it is done (a bounce of five minutes is
            // 115 MB). One that can't be made fails nothing: there is no copy, and use() goes on without it (a bounce leaves
            // the original unfrozen).
            const auto copyIt = [from = rendered, to = copyTo] (OfflineRender&, int, const engine::LoudnessMeter::Readings&)
            {
                jassert (! juce::MessageManager::existsAndIsCurrentThread());

                if (to != juce::File() && ! (to.getParentDirectory().createDirectory().wasOk() && from.copyFileTo (to)))
                    to.deleteFile();

                return juce::Result::ok();
            };

            render = std::make_unique<OfflineRender> (setup, std::vector { job }, copyIt,
                                                      [weakAlive = std::weak_ptr<RenderOfTrack*> (alive)] (const juce::Result& result)
                                                      {
                                                          juce::MessageManager::callAsync (
                                                              [weakAlive, result]
                                                              {
                                                                  if (const auto locked = weakAlive.lock())
                                                                      (*locked)->finish (result);
                                                              });
                                                      });
        }

        // What the rendered file becomes, on the message thread once the render has finished.
        virtual juce::Result use (const juce::File&) = 0;

    private:
        // How long a track's tails may sound after its last clip, at most.
        static constexpr double tailSeconds = 10.0;

        const juce::String renderTitle;
        const std::function<void (const juce::Result&)> onFinished;
        std::shared_ptr<RenderOfTrack*> alive; // what a callback on its way holds on to
        std::unique_ptr<OfflineRender> render;

        void finish (juce::Result result)
        {
            render = nullptr; // the project plays its plug-ins again

            if (result.wasOk())
            {
                if (canRender (*song, track) && rendered.existsAsFile())
                    result = use (rendered);
                else
                    result = juce::Result::fail ("The track is gone, or Phazite couldn't render it.");
            }

            // What use() didn't take.
            for (const auto& file : { rendered, copyTo })
                if (file.existsAsFile())
                    file.deleteFile();

            if (const auto callback = onFinished; callback != nullptr)
                callback (result); // last: it may delete this
        }

        JUCE_DECLARE_NON_COPYABLE (RenderOfTrack)
    };

    //==============================================================================
    // Freezing: the track through its frozen devices (getFrozenDevices()), at the audio device's rate, into the file that
    // becomes its frozen file.
    class FreezeRender final : public RenderOfTrack
    {
    public:
        FreezeRender (ProjectContext& c, Song& s, TrackID t, std::function<void (const juce::Result&)> callback)
            : RenderOfTrack (c, s, t, "Freezing " + getTrackName (s, t), std::move (callback))
        {
            const auto frozenFile = getFrozenFile (s, t);
            rendered =
                frozenFile.getSiblingFile (frozenFile.getFileNameWithoutExtension() + " (rendering).wav").getNonexistentSibling (false);

            std::set<std::int64_t> frozen;

            for (const auto device : getFrozenDevices (s, t))
                frozen.insert ((std::int64_t) device.getRaw());

            start (std::move (frozen));
        }

    private:
        juce::Result use (const juce::File& file) override
        {
            freezeFromFile (*song, track, file);
            return isTrackFrozen (*song, track) ? juce::Result::ok() : juce::Result::fail ("Phazite couldn't keep the frozen file.");
        }
    };

    //==============================================================================
    // Bouncing: the track before its fader, through all its devices, into a WAV file of floating-point samples in the
    // song's recordings folder (its project's Audio folder).
    class BounceRender final : public RenderOfTrack
    {
    public:
        BounceRender (ProjectContext& c, Song& s, TrackID t, std::function<void (const juce::Result&)> callback)
            : RenderOfTrack (c, s, t, "Bouncing " + getTrackName (s, t), std::move (callback)),
              name (getTrackName (s, t) + " (Bounce)")
        {
            target = getRecordingsFolder (s).getNonexistentChildFile (makeUsableFileName (name), ".wav", false);
            rendered = target.getSiblingFile (target.getFileNameWithoutExtension() + " (rendering).wav").getNonexistentSibling (false);

            // The original is frozen too, the bounce its frozen file, where all its devices can be (use()): copied into the
            // Cache folder on the render's thread.
            if (canFreezeWhole (s, t))
                copyTo = getFrozenFile (s, t).getSiblingFile ("Bounced.wav").getNonexistentSibling (false);

            start (std::nullopt);
        }

    private:
        const juce::String name;
        juce::File target;

        juce::Result use (const juce::File& file) override
        {
            if (! file.moveFileTo (target))
                return juce::Result::fail ("Phazite couldn't write " + target.getFullPathName() + ".");

            const auto length = getFileSeconds (target);

            if (length <= 0.0)
                return juce::Result::fail ("Phazite couldn't read the bounced file back.");

            // One undo step: the new track under the original, with the original's color and mixer settings, and the
            // original muted.
            auto& bouncedSong = *song;
            const UndoStep step (bouncedSong, "Bounce in Place");
            const auto bounced = insertPlainTrack (bouncedSong, track);

            if (! bounced.isValid())
                return juce::Result::fail ("Phazite couldn't add a track for the bounce.");

            const auto tracks = getTrackIDs (bouncedSong);
            const auto row = (int) std::distance (tracks.begin(), std::find (tracks.begin(), tracks.end(), track));
            setTrackName (bouncedSong, bounced, name);
            setTrackColour (bouncedSong, bounced, getTrackColour (bouncedSong, track, row));

            setChannelVolume (bouncedSong, bounced, getChannelVolume (bouncedSong, track));
            setChannelPan (bouncedSong, bounced, getChannelPan (bouncedSong, track));
            CopiedIDs copied { { getFader (bouncedSong, track).getRaw(), getFader (bouncedSong, bounced).getRaw() } };

            for (const auto send : getSends (bouncedSong, track))
            {
                if (const auto copy = addSend (bouncedSong, bounced, getSendTarget (bouncedSong, send), isPreFader (bouncedSong, send));
                    copy.isValid())
                {
                    setSendLevel (bouncedSong, copy, getSendLevel (bouncedSong, send));
                    copied[send.getRaw()] = copy.getRaw();
                }
            }

            // The render is the track before its fader: what moves its fader and its sends along the song, their automation
            // and the track's modulators and macros that modulate them, goes on moving the new track's, which plays in its
            // place. The modulators first, with their copies' IDs among the copied: a copied macro's automated value is named
            // by the copy's ID, which the curves' copies then follow.
            copyModulators (bouncedSong, track, bounced, copied);
            copyCurves (bouncedSong, copied);

            // The sends move to the new track, with their lanes and modulation: the original is muted, but a muted track's
            // sends before its fader go on, and with a copy of each on the new track the return played the sound twice (known
            // bug 194). So each send is the new track's alone; the original keeps its clips, devices and fader, and Undo gives
            // its sends back with the rest of the step. What modulated a send there goes with it (forgetWhatNamesTheGone()).
            for (const auto send : getSends (bouncedSong, track))
                if (copied.contains (send.getRaw()))
                    removeSend (bouncedSong, send);

            auto bouncedClip = makeAudioClip (bouncedSong, name, target, BeatPosition(), { 0.0, length });
            bouncedClip.track = bounced;

            if (! addClip (bouncedSong, bouncedClip).isValid())
                return juce::Result::fail ("Phazite couldn't put the bounced file on its track.");

            // Muted in the step, and shown as it was: collapsing it, view state, would be outside the step, and Undo would
            // leave it collapsed (as Live, Logic Pro and REAPER mute the original and leave its look alone). A muted track
            // plays on underneath (its instrument and effects, for when it is heard again), so it is frozen too, in the same
            // step, where every device of its can be (none listens to another track): the bounce is what its freeze would
            // render, and it then costs a file's read at most. Undo unmutes it and plays it live again.
            setTrackMuted (bouncedSong, track, true, &bouncedSong.getUndoManager());

            if (canFreezeWhole (bouncedSong, track) && copyTo.existsAsFile())
                freezeFromFile (bouncedSong, track, copyTo, &bouncedSong.getUndoManager());

            getSelection (context).selectOnly (bounced);
            return juce::Result::ok();
        }
    };

    //==============================================================================
    // Joins every part Consolidate has (model/arrangement/internal/Consolidate.h), each with the file rendered for its track's
    // audio, if any, as one undo step, "Consolidate". What it made is selected, unless a time range was: that stays over it.
    void joinParts (ProjectContext& context, Song& song, const std::vector<ConsolidatePart>& parts,
                    const std::function<juce::File (TrackID)>& renderedFor)
    {
        const auto keepsRange = ! getRangeSelection (context).isEmpty();
        std::vector<ClipID> made;

        {
            const UndoStep step (song, "Consolidate");

            for (const auto& part : parts)
                for (const auto clip : consolidatePart (song, part, renderedFor (part.track)))
                    made.push_back (clip);
        }

        if (auto& selection = getSelection (context); ! made.empty() && ! keepsRange)
        {
            selection.selectOnly (made.front());

            for (size_t i = 1; i < made.size(); ++i)
                selection.addToSelection (made[i]);
        }
    }

    // Consolidating with audio: each part's audio clips before its track's devices, from the part's start to its end, into a
    // WAV file of floating-point samples in the song's recordings folder (its project's Audio folder), all on a thread of
    // their own; then every part joined, MIDI ones too. Nothing of the song changes meanwhile.
    class ConsolidateRender final : public TrackRender::Impl
    {
    public:
        ConsolidateRender (ProjectContext& c, Song& s, std::vector<ConsolidatePart> toJoin,
                           std::function<void (const juce::Result&)> callback)
            : context (c),
              song (&s),
              parts (std::move (toJoin)),
              onFinished (std::move (callback)),
              alive (std::make_shared<ConsolidateRender*> (this))
        {
            if (isSongPlaying (*song))
                stopAndReturn (*song);

            // A frozen track plays its frozen file rather than its clips: live again first, as changing its clips would make it.
            for (const auto& part : parts)
                if (part.audio && isTrackFrozen (*song, part.track))
                    unfreeze (*song, part.track);

            std::vector<OfflineRender::Job> jobs;
            std::set<juce::String> taken;
            const auto folder = getRecordingsFolder (*song);

            for (const auto& part : parts)
            {
                if (! part.audio)
                    continue;

                // "Bass (Consolidated).wav", or the first "Bass (Consolidated) 2.wav"... that no file, nor another part, has.
                const auto name = makeUsableFileName (getTrackName (*song, part.track) + " (Consolidated)");
                auto target = folder.getChildFile (name + ".wav");

                for (int number = 2; target.exists() || taken.contains (target.getFullPathName()); ++number)
                    target = folder.getChildFile (name + " " + juce::String (number) + ".wav");

                taken.insert (target.getFullPathName());
                const auto rendered =
                    target.getSiblingFile (target.getFileNameWithoutExtension() + " (rendering).wav").getNonexistentSibling (false);
                files.push_back ({ part.track, target, rendered });

                OfflineRender::Job job;
                job.file = rendered;
                job.startSeconds = toSeconds (*song, part.range.getStart());
                job.endSeconds = toSeconds (*song, part.range.getEnd());
                job.of = SongPlayer::RenderOf { (std::int64_t) part.track.getRaw(), true, std::set<std::int64_t>() }; // no devices
                jobs.push_back (std::move (job));
            }

            // The clips' renders it plays that aren't there yet made on the render's thread first (OfflineRender.h).
            auto* session = findSession (*song);
            const auto format = getRenderFormat (session);
            const OfflineRender::Setup setup {
                .session = session,
                .makePlayer = [played = song, session]
                { return std::make_unique<SongPlayer> (*played, session != nullptr ? &session->getPlugins() : nullptr); },
                .clipsOf = song.get(),
                .numThreads = session != nullptr ? session->getHost().getNumThreads() : 1,
                .sampleRate = format.sampleRate,
                .blockSize = format.blockSize
            };

            render = std::make_unique<OfflineRender> (setup, jobs, OfflineRender::Rendered(),
                                                      [weakAlive = std::weak_ptr<ConsolidateRender*> (alive)] (const juce::Result& result)
                                                      {
                                                          juce::MessageManager::callAsync (
                                                              [weakAlive, result]
                                                              {
                                                                  if (const auto locked = weakAlive.lock())
                                                                      (*locked)->finish (result);
                                                              });
                                                      });
        }

        ~ConsolidateRender() override
        {
            alive = nullptr; // no callback from here on
            render = nullptr;
            deleteRenderedFiles();
        }

        float getProgress() const override { return render != nullptr ? render->getProgress() : 0.0f; }
        juce::String getTitle() const override { return "Consolidating"; }
        std::optional<RenderWait> getWait() const override { return render != nullptr ? render->findWait() : std::nullopt; }
        bool hasStarted() const noexcept { return render != nullptr; }

    private:
        struct RenderedFile
        {
            TrackID track;
            juce::File target, rendered; // where it goes, and what the engine plays into
        };

        ProjectContext& context;
        const Song::Ptr song;
        const std::vector<ConsolidatePart> parts;
        const std::function<void (const juce::Result&)> onFinished;
        std::vector<RenderedFile> files;
        std::shared_ptr<ConsolidateRender*> alive; // what a callback on its way holds on to
        std::unique_ptr<OfflineRender> render;

        void deleteRenderedFiles()
        {
            for (const auto& file : files)
                if (file.rendered.existsAsFile())
                    file.rendered.deleteFile();
        }

        void finish (juce::Result result)
        {
            render = nullptr; // the project plays its plug-ins again

            if (result.wasOk())
                result = use();

            deleteRenderedFiles();

            if (const auto callback = onFinished; callback != nullptr)
                callback (result); // last: it may delete this
        }

        juce::Result use()
        {
            for (const auto& file : files)
                if (! file.rendered.moveFileTo (file.target))
                    return juce::Result::fail ("Phazite couldn't write " + file.target.getFullPathName() + ".");

            joinParts (context, *song, parts,
                       [this] (TrackID track)
                       {
                           const auto found = std::find_if (files.begin(), files.end(),
                                                            [track] (const RenderedFile& file) { return file.track == track; });
                           return found != files.end() ? found->target : juce::File();
                       });

            return juce::Result::ok();
        }

        JUCE_DECLARE_NON_COPYABLE (ConsolidateRender)
    };

    std::vector<ConsolidatePart> getPartsToConsolidate (const ProjectContext& context)
    {
        auto* song = findSong (context);
        return song != nullptr ? getConsolidateParts (*song, getSelection (context), getRangeSelection (context))
                               : std::vector<ConsolidatePart>();
    }

    TrackID getSelectedTrackID (const ProjectContext& context)
    {
        const auto selected = getSelectedTracks (getSelection (context));
        return selected.empty() ? TrackID() : selected.front();
    }

    // What a render of a track is called before it starts: "Freezing Bass", or "Freezing the track" without one.
    juce::String getRenderTitle (const ProjectContext& context, TrackID track, const juce::String& doing)
    {
        auto* song = findSong (context);
        const auto kind = song != nullptr ? getTrackKind (*song, track) : std::nullopt;
        const auto isTrack = kind.has_value() && *kind != TrackValue::Kind::master;
        return doing + " " + (isTrack ? getTrackName (*song, track) : juce::String ("the track"));
    }

    // Starts a render in the host, or tells the host why it can't.
    void runOrExplain (TrackRenderHost& host, const juce::String& title, const juce::String& whyNot,
                       const std::function<std::unique_ptr<TrackRender> (std::function<void (const juce::Result&)>)>& start)
    {
        if (whyNot.isNotEmpty())
        {
            host.renderFinished (title, juce::Result::fail (whyNot));
            return;
        }

        auto render = start ([&host, title] (const juce::Result& result) { host.renderFinished (title, result); });

        if (render != nullptr)
            host.runRender (std::move (render));
        else
            host.renderFinished (title, juce::Result::fail ("Phazite couldn't start rendering."));
    }
} // namespace

TrackRender::TrackRender (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

TrackRender::~TrackRender() = default;

std::optional<RenderWait> TrackRender::getWait() const
{
    return impl->getWait();
}

juce::String TrackRender::getGoOnName() const
{
    return impl->getGoOnName();
}

void TrackRender::goOn()
{
    impl->goOn();
}

bool TrackRender::isHold() const
{
    return impl->isHold();
}

float TrackRender::getProgress() const
{
    return impl->getProgress();
}

juce::String TrackRender::getTitle() const
{
    return impl->getTitle();
}

void TrackRender::cancel()
{
    impl->cancel();
}

//==============================================================================
bool isFrozen (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && id.isValid() && isTrackFrozen (*song, id);
}

bool isFrozenDevice (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr && isFrozenDevice (*song, id);
}

juce::String getWhyNotFreeze (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (isFrozen (context, id))
        return getTrackName (*song, id) + " is frozen already.";

    return getWhyNotRender (song, id, "freeze", "frozen");
}

juce::String getWhyNotBounce (const ProjectContext& context, TrackID id)
{
    return getWhyNotRender (findSong (context), id, "bounce", "bounced");
}

std::unique_ptr<TrackRender> startFreeze (ProjectContext& context, TrackID id, std::function<void (const juce::Result&)> onFinished)
{
    auto* song = findSong (context);

    if (song == nullptr || getWhyNotFreeze (context, id).isNotEmpty())
        return {};

    auto render = std::make_unique<FreezeRender> (context, *song, id, std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

std::unique_ptr<TrackRender> startBounceInPlace (ProjectContext& context, TrackID id, std::function<void (const juce::Result&)> onFinished)
{
    auto* song = findSong (context);

    if (song == nullptr || getWhyNotBounce (context, id).isNotEmpty())
        return {};

    auto render = std::make_unique<BounceRender> (context, *song, id, std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

void unfreezeTrack (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context); song != nullptr && isFrozen (context, id))
        unfreeze (*song, id);
}

juce::String getWhyNotConsolidate (const ProjectContext& context)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return "Open a project first.";

    if (isRecording (*song))
        return "Stop recording first.";

    if (getPartsToConsolidate (context).empty())
        return "Select the clips to join, or a time range across tracks.";

    return {};
}

bool doesConsolidateRender (const ProjectContext& context)
{
    const auto parts = getPartsToConsolidate (context);
    return std::any_of (parts.begin(), parts.end(), [] (const ConsolidatePart& part) { return part.audio; });
}

std::unique_ptr<TrackRender> startConsolidate (ProjectContext& context, std::function<void (const juce::Result&)> onFinished)
{
    auto* song = findSong (context);

    if (song == nullptr || getWhyNotConsolidate (context).isNotEmpty() || ! doesConsolidateRender (context))
        return {};

    auto render = std::make_unique<ConsolidateRender> (context, *song, getPartsToConsolidate (context), std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

//==============================================================================
void toggleFreeze (ProjectContext& context, TrackRenderHost& host)
{
    const auto id = getSelectedTrackID (context);

    if (isFrozen (context, id))
    {
        unfreezeTrack (context, id);
        return;
    }

    runOrExplain (host, getRenderTitle (context, id, "Freezing"), getWhyNotFreeze (context, id),
                  [&context, id] (std::function<void (const juce::Result&)> done) { return startFreeze (context, id, std::move (done)); });
}

bool canToggleFreeze (const ProjectContext& context)
{
    // Not while an old version is open in the song's place (Song::isShowingOther()): a freeze is no undo step, and would be
    // of the version's track.
    const auto id = getSelectedTrackID (context);
    const auto* song = findSong (context);
    return (song == nullptr || ! song->isShowingOther()) && (isFrozen (context, id) || getWhyNotFreeze (context, id).isEmpty());
}

bool isSelectedTrackFrozen (const ProjectContext& context)
{
    return isFrozen (context, getSelectedTrackID (context));
}

juce::String getFreezeCommandName (const ProjectContext& context)
{
    return isSelectedTrackFrozen (context) ? "Unfreeze Track" : "Freeze Track";
}

void bounceSelectedTrack (ProjectContext& context, TrackRenderHost& host)
{
    const auto id = getSelectedTrackID (context);
    runOrExplain (host, getRenderTitle (context, id, "Bouncing"), getWhyNotBounce (context, id),
                  [&context, id] (std::function<void (const juce::Result&)> done)
                  { return startBounceInPlace (context, id, std::move (done)); });
}

bool canBounceSelectedTrack (const ProjectContext& context)
{
    return getWhyNotBounce (context, getSelectedTrackID (context)).isEmpty();
}

void consolidateSelection (ProjectContext& context, TrackRenderHost& host)
{
    const auto title = juce::String ("Consolidating");

    if (const auto whyNot = getWhyNotConsolidate (context); whyNot.isNotEmpty())
    {
        host.renderFinished (title, juce::Result::fail (whyNot));
        return;
    }

    // MIDI alone needs no render: joined at once.
    if (! doesConsolidateRender (context))
    {
        joinParts (context, *findSong (context), getPartsToConsolidate (context), [] (TrackID) { return juce::File(); });
        return;
    }

    runOrExplain (host, title, {},
                  [&context] (std::function<void (const juce::Result&)> done) { return startConsolidate (context, std::move (done)); });
}

bool canConsolidateSelection (const ProjectContext& context)
{
    return getWhyNotConsolidate (context).isEmpty();
}

void addFreezeCommands (CommandRegistry& registry, ProjectContext& context, TrackRenderHost& host)
{
    registry.add ({ .id = "track.freeze",
                    .name = "Freeze Track",
                    .category = "Track",
                    .description = "Plays the selected track from a file of its sound up to its fader, so that its instrument and "
                                   "effects take no CPU; again to play it live. Changing what it froze unfreezes it.",
                    .synonyms = { "unfreeze", "render", "cpu", "commit", "snowflake" },
                    .perform = [&context, &host] { toggleFreeze (context, host); },
                    .isEnabled = [&context] { return canToggleFreeze (context); },
                    .isTicked = [&context] { return isSelectedTrackFrozen (context); },
                    .currentName = [&context] { return getFreezeCommandName (context); } });

    registry.add ({ .id = "track.bounceInPlace",
                    .name = "Bounce in Place",
                    .category = "Track",
                    .description = "Renders the selected track through its instrument and effects onto a new audio track under it, "
                                   "and mutes the original.",
                    .synonyms = { "render", "bounce to new track", "resample", "print", "audio", "flatten", "commit" },
                    .perform = [&context, &host] { bounceSelectedTrack (context, host); },
                    .isEnabled = [&context] { return canBounceSelectedTrack (context); } });

    registry.add ({ .id = "clip.consolidate",
                    .name = "Consolidate",
                    .category = "Clip",
                    .description = "Joins the selected clips on each track, or what lies in the selected time range, into one clip: a MIDI "
                                   "clip of their notes, or one audio file of what the audio clips play, before the track's effects.",
                    .synonyms = { "join", "glue", "merge clips", "combine", "render clips", "flatten clips" },
                    .perform = [&context, &host] { consolidateSelection (context, host); },
                    .isEnabled = [&context] { return canConsolidateSelection (context); } });
}
} // namespace phazite

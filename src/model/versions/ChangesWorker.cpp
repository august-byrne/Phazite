// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ChangesWorker.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsArea.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongState.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/WholeState.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    std::atomic<juce::int64> tracksCompared { 0 }, tracksCopied { 0 }, copiedOnMessageThread { 0 };

    // Copies made of parts of the song (a track, the master, the song whole) counted, and where.
    void countCopied (juce::int64 copies)
    {
        tracksCopied += copies;

        if (juce::MessageManager::existsAndIsCurrentThread())
            copiedOnMessageThread += copies;
    }

    // The plug-ins' devices in a list of the song's devices (a channel's, a container's chains, a chain's devices), those in its
    // containers' chains too, by their IDs, read from the devices' value (no tree made).
    void findPluginDevices (const SongDevices& devices, const immer::flex_vector<DeviceID>* list, std::set<std::uint64_t>& found)
    {
        if (list == nullptr)
            return;

        for (const auto id : *list)
        {
            const auto* value = devices.find (id);

            if (value == nullptr)
                continue;

            if (value->form == DeviceValue::Form::plugin)
                found.insert (id.getRaw());
            else if (value->form == DeviceValue::Form::container)
                findPluginDevices (devices, devices.chains.find (id.getRaw()), found);
            else if (value->form == DeviceValue::Form::chain)
                findPluginDevices (devices, devices.chainDevices.find (id.getRaw()), found);
        }
    }

    // A track's own parts, or the master's, found by its ID in a state (the master's by its ID then), none for one the state
    // hasn't: without the tracks in it (a folder's), which are parts of their own. Whole state of one track: its own
    // properties as whole state writes them (TracksValue.h: writeTrackElement()); its clips first, in its order, each whole
    // with its notes (ClipsValue.h: writeClipElement()); then its devices and containers, each whole (WholeState.h:
    // writeDeviceTree(): a container with its chains and macros); then its switches and its view (its LANES), then its
    // modulators and macros, then its sends, as whole state writes them (TracksArea.h: writeTrackLeavesIn(); ModulationArea.h:
    // writeModulationIn(); SendsArea.h: writeSendsIn()). Any thread: made on the history's thread (work()).
    // A comparison finds a clip by its key, in no order among the rest (Schema.h: CLIP is in no order group), and a modulator
    // and a send by its ID, in the order of its kind.
    juce::ValueTree copyTrack (const SongState& state, TrackID id)
    {
        const auto* track = state.tracks.find (id);

        if (track == nullptr)
            return {};

        auto copy = writeTrackElement (*track);
        const auto onMaster = track->isMaster();

        if (const auto* clips = onMaster ? nullptr : state.clips.tracks.find (id.getRaw()))
            for (const auto clip : *clips)
                if (const auto* value = state.clips.find (clip))
                    copy.appendChild (writeClipElement (*value, state.notes.find (clip)), nullptr);

        // Its devices and containers after its clips, each whole as whole state writes it.
        if (const auto* list = state.devices.findList (makeChannelPlace (id, onMaster)))
            for (const auto device : *list)
                if (auto element = writeDeviceTree (state, device); element.isValid())
                    copy.appendChild (element, nullptr);

        writeTrackLeavesIn (copy, state); // its switches and its view, its LANES after its devices (TracksArea.h)
        writeModulationIn (copy, state);
        writeSendsIn (copy, state);
        return copy;
    }

    // The song's own parts (its properties, tempo map, sections, chord lane and MIDI links: all that the song's own line
    // compares, SongDiff.h: findSongChange()) and its tracks' order and names: each track with its ID, kind and name in its
    // folder, and the master's ID. What the song's own line and the lines' names and order are worked out from, without
    // copying any track's parts. Each of the song's own parts whole, from a state (SongState.h: writeSongPart()), where the
    // state holds it. The tracks' three fields read from the state's tracks in the song's order (each folder before the
    // tracks in it, top to bottom), never a track copied. Each stub holds them as the writer writes them (TracksValue.h:
    // writeTrackElement()): the ID a number, the kind its word, the name where it has one. Any thread: made on the history's
    // thread (work()).
    juce::ValueTree makeSkeleton (const SongState& state)
    {
        juce::ValueTree own (schema::PHAZITE);

        // Its own properties and its count of IDs, as whole state writes them, without the elements of its own (its LASTKEY,
        // PIANOROLL and MERGING), which the line compares nothing of.
        writeSongOwnPropertiesInto (own, state.songOwn, state.songStanding, state.songOutside);
        own.setProperty (schema::nextID, (juce::int64) state.nextID, nullptr);

        for (const auto* type : { &schema::TEMPOMAP, &schema::SECTIONS, &schema::CHORDLANE, &schema::MIDILINKS })
            if (auto part = writeSongPart (state, *type); part.isValid())
                own.appendChild (part, nullptr);

        // The kinds' words as values, made once: a text made for each track would allocate.
        static const juce::var trackWord (schema::trackIsTrack), folderWord (schema::trackIsFolder), returnWord (schema::trackIsReturn);
        juce::ValueTree list (schema::TRACKS);
        const auto& tracks = state.tracks;

        // Each list's stubs, a folder's stub holding those of the tracks in it, in each list's order.
        const std::function<void (juce::ValueTree&, TrackID)> addList = [&tracks, &addList] (juce::ValueTree& parent, TrackID folder)
        {
            const auto* ids = tracks.findList (folder);

            if (ids == nullptr)
                return;

            for (const auto id : *ids)
            {
                const auto* value = tracks.tracks.find (id.getRaw());

                if (value == nullptr)
                    continue;

                juce::ValueTree stub (schema::TRACK);
                stub.setProperty (schema::trackID, (juce::int64) id.getRaw(), nullptr);
                stub.setProperty (schema::trackKind, value->isFolder() ? folderWord : value->isReturn() ? returnWord : trackWord, nullptr);

                if (value->name.has_value() && value->name->isNotEmpty())
                    stub.setProperty (schema::trackName, *value->name, nullptr);

                addList (stub, id);
                parent.appendChild (stub, nullptr);
            }
        };

        addList (list, {});
        own.appendChild (list, nullptr);

        if (const auto master = tracks.master.id; master.isValid())
            own.appendChild (juce::ValueTree (schema::MASTER, { { schema::trackID, (juce::int64) master.getRaw() } }), nullptr);

        return own;
    }

    // An automation curve by its key, whole, as a state holds it (Automation.h: copyWholeCurve() of a song): none for one the
    // state hasn't. Any thread.
    juce::ValueTree copyCurve (const SongState& state, const juce::String& key)
    {
        const auto parameter = readCurveKeyText (key);
        const auto* curve = parameter.has_value() ? state.curves.find (*parameter) : nullptr;

        if (curve == nullptr)
            return {};

        juce::ValueTree automation (schema::AUTOMATION);
        writeCurveElement (*parameter, *curve, automation);
        auto element = automation.getChild (0);
        automation.removeChild (element, nullptr);
        return element;
    }

    // Each track's name as the song shows it ("Bass", "Track 3", "Folder"), in one walk.
    std::map<std::uint64_t, juce::String> nameTracks (const juce::ValueTree& song)
    {
        std::map<std::uint64_t, juce::String> names;
        auto number = 1;

        for (const auto& track : getTrackElements (song))
        {
            const auto isFolder = track[schema::trackKind].toString() == schema::trackIsFolder;
            const auto name = track[schema::trackName].toString();
            names[readID (track[schema::trackID])] = name.isNotEmpty() ? name
                                                     : isFolder        ? juce::String ("Folder")
                                                                       : "Track " + juce::String (number);

            if (! isFolder)
                ++number;
        }

        return names;
    }
} // namespace

struct ChangesWorker::State
{
    juce::ValueTree reference; // the version's song, never changed
    PluginParameterValues referenceParameters;
    bool knownTheSame = false; // the song was the reference as it was given
    bool indexed = false;
    std::atomic<bool> dropped { false }; // another reference (or none) since: what a round would find is dropped, so it stops

    // The reference's parts by their IDs and keys, and what automation names in each of its tracks.
    std::map<std::uint64_t, juce::ValueTree> referenceTracks; // the master's too
    std::map<juce::String, juce::ValueTree> referenceCurves;
    std::map<std::uint64_t, std::set<std::uint64_t>> automatableBefore;

    // What the rounds found: the lines of the tracks and the master that changed (named as they are assembled), the curves
    // that differ (by key, each its device), the song's own line; and the song as the last round knew it: its skeleton
    // (makeSkeleton(), or the whole song) and what automation names in each of its tracks.
    std::map<std::uint64_t, SongChange> lines;
    std::map<juce::String, std::uint64_t> differingCurves;
    SongChange songLine;
    juce::ValueTree skeleton;
    std::map<std::uint64_t, std::set<std::uint64_t>> automatableNow;
    std::uint64_t masterNow = 0; // the master's ID as the last round knew it, which its line is named by

    void index()
    {
        if (std::exchange (indexed, true))
            return;

        for (const auto& track : getTrackElements (reference))
        {
            const auto id = readID (track[schema::trackID]);
            referenceTracks[id] = track;
            automatableBefore[id] = getAutomatableIDs (track);
        }

        // The master's by its key, whatever its ID.
        if (const auto master = reference.getChildWithName (schema::MASTER); master.isValid())
        {
            referenceTracks[ChangesWorker::masterKey] = master;
            automatableBefore[ChangesWorker::masterKey] = getAutomatableIDs (master);
        }

        for (const auto& curve : reference.getChildWithName (schema::AUTOMATION))
            referenceCurves[schema::getElementKey (curve)] = curve;

        songLine.name = "Song";

        // The song as it was given: what it is now, until a round says otherwise.
        if (knownTheSame)
        {
            skeleton = reference;
            automatableNow = automatableBefore;
        }
    }
};

void ChangesWorker::setReference (const juce::ValueTree& song, PluginParameterValues parameters, bool withReported, bool knownTheSame)
{
    if (state != nullptr)
        state->dropped = true;

    state = std::make_shared<State>();
    state->reference = song;
    state->referenceParameters = std::move (parameters);
    state->knownTheSame = knownTheSame;
    ++epoch;
    working = false;
    tracks.clear();
    curves.clear();
    order = songOwn = false;
    reportedToo = withReported;

    // The song the same as it: no changes, known at once; else everything compared again.
    all = ! knownTheSame;
    hasResult = knownTheSame;
    changes.clear();
}

void ChangesWorker::clear()
{
    if (state != nullptr)
        state->dropped = true;

    state = nullptr;
    ++epoch;
    working = all = order = songOwn = hasResult = false;
    tracks.clear();
    curves.clear();
    changes.clear();
}

void ChangesWorker::markTrack (std::uint64_t id)
{
    if (state != nullptr && ! all)
        tracks.insert (id);
}

void ChangesWorker::markCurve (const juce::String& key)
{
    if (state != nullptr && ! all)
        curves.insert (key);
}

void ChangesWorker::markSong()
{
    songOwn = state != nullptr;
}

void ChangesWorker::markOrder()
{
    order = state != nullptr;
}

void ChangesWorker::markAll()
{
    if (state == nullptr)
        return;

    all = true;
    tracks.clear();
    curves.clear();
}

std::shared_ptr<ChangesWorker::Request> ChangesWorker::take (const Song& song, const KnobReader& readKnobs, const KnobReader& readOpened,
                                                             std::set<std::uint64_t> reported)
{
    if (state == nullptr || working || ! isDirty())
        return {};

    auto request = std::make_shared<Request>();
    request->state = state;
    request->epoch = epoch;
    request->number = ++number;
    request->withReported = reportedToo;
    request->master = getMasterID (song).getRaw();

    // The song as it is now, a state (SongState.h: getSongState(), a cheap copy of each area's value), which the history's
    // thread writes the trees of what changed from (work()): nothing copied here.
    request->songNow = getSongState (song);
    const auto& now = request->songNow;
    std::set<std::uint64_t> devices;

    if (all)
    {
        // Everything: the whole song, once, and every plug-in in it.
        request->all = true;

        for (const auto& entry : now.devices.devices)
            if (entry.second.form == DeviceValue::Form::plugin)
                devices.insert (entry.first);
    }
    else
    {
        // The song's own parts and its tracks' order and names (makeSkeleton()), where they changed; each track that changed,
        // its own parts alone, the master's by its key, with the plug-ins in it; each curve that changed, by its key.
        request->skeleton = order || songOwn;
        request->tracks = tracks;
        request->curves = curves;

        for (const auto id : tracks)
            findPluginDevices (now.devices, now.devices.findList (makeChannelPlace (TrackID::fromRaw (id), id == masterKey)), devices);
    }

    request->parameters = readKnobs (devices);
    request->opened = readOpened (devices);
    request->reported = std::move (reported);
    all = order = songOwn = false;
    tracks.clear();
    curves.clear();
    working = true;
    return request;
}

bool ChangesWorker::takeResult (const Result& result)
{
    if (state == nullptr || result.epoch != epoch)
        return false;

    working = false;
    hasResult = true;
    changes = result.changes;
    return true;
}

ChangesWorker::Result ChangesWorker::work (const Request& request)
{
    auto& s = *request.state;

    // Of a reference that was let go of since it was taken: nothing to work out, as its list is dropped.
    if (s.dropped.load())
        return { request.epoch, request.number, {} };

    s.index();
    s.masterNow = request.master;

    // The reference's knobs of plug-ins that loaded after it was read, as they opened (the state it opened from is the one
    // the reference holds), so that a knob turned after shows as a change.
    for (const auto& [device, knobs] : request.opened)
        if (s.referenceParameters.count (device) == 0)
            s.referenceParameters[device] = knobs;

    const PluginValues values { &s.referenceParameters, &request.parameters, request.withReported ? &request.reported : nullptr };

    const auto compareTrack = [&s, &values] (std::uint64_t id, const juce::ValueTree& now)
    {
        const auto found = s.referenceTracks.find (id);
        const auto before = found != s.referenceTracks.end() ? found->second : juce::ValueTree();

        if (now.isValid())
            s.automatableNow[id] = getAutomatableIDs (now);
        else
            s.automatableNow.erase (id);

        if (! before.isValid() && ! now.isValid())
        {
            s.lines.erase (id);
            return;
        }

        if (s.dropped.load()) // let go of meanwhile: the rest isn't worked out
            return;

        ++tracksCompared;

        // Named as the lines are assembled, from the song's tracks as they are now.
        if (auto change = findTrackChange (before, now, s.reference, {}, values);
            change.kind != SongChange::Kind::changed || ! change.what.isEmpty())
            s.lines[id] = std::move (change);
        else
            s.lines.erase (id);
    };

    const auto compareCurve = [&s] (const juce::String& key, const juce::ValueTree& now)
    {
        const auto found = s.referenceCurves.find (key);
        const auto before = found != s.referenceCurves.end() ? found->second : juce::ValueTree();

        if ((before.isValid() || now.isValid()) && (! before.isValid() || ! now.isValid() || ! holdTheSame (before, now)))
            s.differingCurves[key] = readID ((now.isValid() ? now : before)[schema::curveDevice]);
        else
            s.differingCurves.erase (key);
    };

    // The trees of what changed, written here from the state taken on the message thread (take()), each counted as a copy.
    const auto& taken = request.songNow;

    if (request.all)
    {
        // Every part again, from the whole song.
        const auto now = writeSongTree (taken);
        countCopied ((juce::int64) taken.tracks.tracks.size() + 1);
        s.lines.clear();
        s.differingCurves.clear();
        s.automatableNow.clear();
        s.skeleton = now;
        std::map<std::uint64_t, juce::ValueTree> tracksNow;

        for (const auto& track : getTrackElements (now))
            tracksNow[readID (track[schema::trackID])] = track;

        if (const auto master = now.getChildWithName (schema::MASTER); master.isValid())
            tracksNow[masterKey] = master;

        std::set<std::uint64_t> ids;

        for (const auto& [id, track] : s.referenceTracks)
            ids.insert (id);

        for (const auto& [id, track] : tracksNow)
            ids.insert (id);

        for (const auto id : ids)
        {
            const auto found = tracksNow.find (id);
            compareTrack (id, found != tracksNow.end() ? found->second : juce::ValueTree());
        }

        std::map<juce::String, juce::ValueTree> curvesNow;

        for (const auto& curve : now.getChildWithName (schema::AUTOMATION))
            curvesNow[schema::getElementKey (curve)] = curve;

        for (const auto& [key, curve] : s.referenceCurves)
            if (curvesNow.count (key) == 0)
                compareCurve (key, {});

        for (const auto& [key, curve] : curvesNow)
            compareCurve (key, curve);

        s.songLine = findSongChange (s.reference, now);
    }
    else
    {
        for (const auto id : request.tracks)
        {
            const auto master = taken.tracks.master.id;
            countCopied (1);
            compareTrack (id, copyTrack (taken, id == masterKey ? master : TrackID::fromRaw (id)));
        }

        for (const auto& key : request.curves)
            compareCurve (key, copyCurve (taken, key));

        if (request.skeleton)
        {
            s.skeleton = makeSkeleton (taken);
            s.songLine = findSongChange (s.reference, s.skeleton);
        }
    }

    // The lines in the song's order: its tracks as they are now, those it hasn't any more, the master, then the song's own;
    // a track's line names its automation where a curve of what it holds differs (on a track both songs have).
    std::set<std::uint64_t> automated;

    for (const auto& [key, device] : s.differingCurves)
        automated.insert (device);

    const auto names = nameTracks (s.skeleton);
    Result result { request.epoch, request.number, {} };

    const auto addLine = [&] (std::uint64_t id, const juce::String& name, bool isMaster)
    {
        const auto line = s.lines.find (id);
        const auto inBoth = s.referenceTracks.count (id) > 0 && (isMaster || names.count (id) > 0);
        auto touches = false;

        if (inBoth && ! automated.empty())
            for (const auto* ids : { &s.automatableBefore[id], &s.automatableNow[id] })
                touches =
                    touches || std::any_of (ids->begin(), ids->end(), [&automated] (auto device) { return automated.count (device) > 0; });

        if (line == s.lines.end() && ! touches)
            return;

        auto change = line != s.lines.end() ? line->second : SongChange {};

        if (line == s.lines.end())
        {
            change.where = isMaster ? SongChange::Where::master : SongChange::Where::track;
            change.trackID = id;
        }

        // The master's line named by its ID now (its key is none of its IDs).
        if (isMaster)
            change.trackID = s.masterNow;

        change.name = name;

        if (touches)
            change.what.addIfNotAlreadyThere ("automation");

        result.changes.push_back (std::move (change));
    };

    std::set<std::uint64_t> shown;

    for (const auto& track : getTrackElements (s.skeleton))
    {
        const auto id = readID (track[schema::trackID]);
        shown.insert (id);
        addLine (id, names.at (id), false);
    }

    for (const auto& track : getTrackElements (s.reference))
        if (const auto id = readID (track[schema::trackID]); shown.count (id) == 0)
            addLine (id, getTrackNameIn (s.reference, track), false);

    if (const auto master = s.reference.getChildWithName (schema::MASTER); master.isValid() && s.masterNow != 0)
        addLine (masterKey, "Master", true);

    if (! s.songLine.what.isEmpty())
        result.changes.push_back (s.songLine);

    return result;
}

juce::int64 ChangesWorker::countTracksCompared()
{
    return tracksCompared.load();
}

juce::int64 ChangesWorker::countTracksCopied()
{
    return tracksCopied.load();
}

juce::int64 ChangesWorker::countCopiedOnMessageThread()
{
    return copiedOnMessageThread.load();
}
} // namespace phazite

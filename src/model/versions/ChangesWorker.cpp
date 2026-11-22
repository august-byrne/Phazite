// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ChangesWorker.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/WholeState.h"

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    std::atomic<juce::int64> tracksCompared { 0 }, tracksCopied { 0 };

    // The plug-ins' devices in a part of the song, their containers' too.
    void findPluginDevices (const juce::ValueTree& element, std::set<std::uint64_t>& found)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::DEVICE) && child[schema::deviceType].toString() == schema::deviceIsPlugin)
                found.insert (readID (child[schema::deviceID]));

            findPluginDevices (child, found);
        }
    }

    // A track's own parts, or the master's: without the tracks in it (a folder's), which are parts of their own. Its clips
    // first, by their IDs (Clips.h), each whole with its notes (WholeState.h: copyWholeClip()), as the song holds them beside
    // its tree; then its other parts. A comparison finds a clip by its key, in no order among the rest (Schema.h: CLIP is in
    // no order group).
    juce::ValueTree copyTrack (const Song& song, const juce::ValueTree& track)
    {
        juce::ValueTree copy (track.getType());
        copy.copyPropertiesFrom (track, nullptr);

        if (track.hasType (schema::TRACK))
            for (const auto clip : getClips (song, getTrackID (track)))
                copy.appendChild (copyWholeClip (song, clip), nullptr);

        for (const auto& child : track)
            if (! child.hasType (schema::TRACK))
                copy.appendChild (copyWhole (song, child), nullptr);

        return copy;
    }

    // The song's own parts (its properties, tempo map, sections and chord lane) and its tracks' order and names: each track
    // with its ID, kind and name in its folder, and the master's ID. What the song's own line and the lines' names and order
    // are worked out from, without copying any track's parts. Each of the song's own parts whole (WholeState.h), as the song
    // holds it, beside its tree too (roadmap step S5.20: the tempo map is a value beside it, its TEMPOMAP an empty place that
    // copyWhole() fills).
    juce::ValueTree makeSkeleton (const Song& whole)
    {
        const auto song = whole.getState();
        juce::ValueTree own (song.getType());
        own.copyPropertiesFrom (song, nullptr);

        for (const auto* type : { &schema::TEMPOMAP, &schema::SECTIONS, &schema::CHORDLANE })
            if (const auto part = song.getChildWithName (*type); part.isValid())
                own.appendChild (copyWhole (whole, part), nullptr);

        const std::function<void (const juce::ValueTree&, juce::ValueTree)> addTracks =
            [&addTracks] (const juce::ValueTree& from, juce::ValueTree into)
        {
            for (const auto& child : from)
            {
                if (! child.hasType (schema::TRACK))
                    continue;

                juce::ValueTree stub (schema::TRACK);

                for (const auto* property : { &schema::trackID.getName(), &schema::trackKind.getName(), &schema::trackName.getName() })
                    if (child.hasProperty (*property))
                        stub.setProperty (*property, child[*property], nullptr);

                into.appendChild (stub, nullptr);
                addTracks (child, stub);
            }
        };

        juce::ValueTree list (schema::TRACKS);
        addTracks (song.getChildWithName (schema::TRACKS), list);
        own.appendChild (list, nullptr);

        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
            own.appendChild (juce::ValueTree (schema::MASTER, { { schema::trackID, master[schema::trackID] } }), nullptr);

        return own;
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

        if (const auto master = reference.getChildWithName (schema::MASTER); master.isValid())
        {
            const auto id = readID (master[schema::trackID]);
            referenceTracks[id] = master;
            automatableBefore[id] = getAutomatableIDs (master);
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
    const auto songState = song.getState();
    std::set<std::uint64_t> devices;

    if (all)
    {
        // Everything: the whole song, once.
        request->all = true;
        request->songNow = copyWholeSong (song);
        tracksCopied += (juce::int64) getTrackElements (songState).size() + 1;
        findPluginDevices (request->songNow, devices);
    }
    else
    {
        if (order || songOwn)
        {
            request->skeleton = true;
            request->songNow = makeSkeleton (song);
        }

        // Each track that changed, its own parts alone; none for one that is gone.
        const auto master = songState.getChildWithName (schema::MASTER);

        for (const auto id : tracks)
        {
            const auto element = master.isValid() && readID (master[schema::trackID]) == id ? master : song.getIndex().findTrack (id);
            const auto copy = element.isValid() ? copyTrack (song, element) : juce::ValueTree();
            ++tracksCopied;
            findPluginDevices (copy, devices);
            request->tracks[id] = copy;
        }

        // Each curve that changed, whole, as the song holds it (Automation.h: copyWholeCurve(), by the parameter its key
        // names); none for one that is gone.
        for (const auto& key : curves)
        {
            const auto parameter = readCurveKeyText (key);
            request->curves[key] = parameter.has_value() ? copyWholeCurve (song, *parameter) : juce::ValueTree();
        }
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

    if (request.all)
    {
        // Every part again, from the whole song.
        const auto& now = request.songNow;
        s.lines.clear();
        s.differingCurves.clear();
        s.automatableNow.clear();
        s.skeleton = now;
        std::map<std::uint64_t, juce::ValueTree> tracksNow;

        for (const auto& track : getTrackElements (now))
            tracksNow[readID (track[schema::trackID])] = track;

        if (const auto master = now.getChildWithName (schema::MASTER); master.isValid())
            tracksNow[readID (master[schema::trackID])] = master;

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
        for (const auto& [id, track] : request.tracks)
            compareTrack (id, track);

        for (const auto& [key, curve] : request.curves)
            compareCurve (key, curve);

        if (request.skeleton)
        {
            s.skeleton = request.songNow;
            s.songLine = findSongChange (s.reference, s.skeleton);
        }
    }

    // The lines in the song's order: its tracks as they are now, those it hasn't any more, the master, then the song's own;
    // a track's line names its automation where a curve of what it holds differs (on a track both songs have).
    std::set<std::uint64_t> automated;

    for (const auto& [key, device] : s.differingCurves)
        automated.insert (device);

    const auto names = nameTracks (s.skeleton);
    const auto masterNow = readID (s.skeleton.getChildWithName (schema::MASTER)[schema::trackID]);
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

    if (const auto master = s.reference.getChildWithName (schema::MASTER); master.isValid() && masterNow != 0)
        addLine (masterNow, "Master", true);

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
} // namespace phazite

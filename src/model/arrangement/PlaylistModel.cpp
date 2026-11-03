// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlaylistModel.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Import.h"
#include "internal/Sections.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/chords/internal/Chords.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <numeric>

namespace phazite
{
namespace
{
    SongSection makeSongSection (const Section& section)
    {
        return { section.id, section.name, section.range, section.colour, section.tip, section.energy };
    }

    // A track of the song's that holds clips: a track or a return, not a folder or the master.
    bool holdsClips (const Song& song, TrackID id)
    {
        const auto state = findTrackState (song, id);
        return state.hasType (schema::TRACK) && ! isFolderState (state);
    }

    // A track's row; its color is its row's in the palette unless it has its own. The master's is gray. Without the colors
    // of the folders it is in, which getPlaylistRows() knows from the folders before it.
    TrackRow makeRow (const Song& song, TrackID id, int index)
    {
        const auto state = findTrackState (song, id);
        TrackRow row;
        row.id = id;
        row.collapsed = isCollapsed (song, id); // the master's: its lanes are thin rows

        if (state.hasType (schema::MASTER))
        {
            row.name = "Master";
            row.colour = getTrackColours().back().colour;
            row.kind = TrackType::master;
            return row;
        }

        row.name = getTrackName (song, id);
        row.colour = getTrackColour (song, id, index);
        row.kind = isFolderState (state) ? TrackType::folder : (isReturnState (state) ? TrackType::returnTrack : TrackType::track);

        for (auto folder = state.getParent(); folder.hasType (schema::TRACK); folder = folder.getParent())
            ++row.depth;

        row.pinned = isPinned (song, id);
        row.savedHeight = getSavedTrackHeight (song, id);
        row.hasInstrument = ! isFolderState (state) && hasInstrument (song, id);
        row.frozen = state[schema::trackFrozen];
        return row;
    }

    ClipInfo makeInfo (const Song& song, const juce::ValueTree& clipState)
    {
        ClipInfo info;
        info.id = getClipID (clipState);
        info.track = getTrackID (clipState.getParent());
        info.kind = isAudioState (clipState) ? ClipType::audio : ClipType::midi;
        info.name = clipState[schema::clipName].toString();
        info.range = getClipRange (song, clipState);
        info.muted = clipState[schema::clipMute];
        info.linked = clipState.hasProperty (schema::clipLink);

        if (isAudioState (clipState))
        {
            // Its fades as they play, as parts of its length.
            const auto position = getPlayedPosition (song, clipState);

            if (const auto length = position.end - position.start; length > 0.0)
            {
                const auto [fadeIn, fadeOut] = getFittedFades (song, clipState);
                info.fadeIn = fadeIn / length;
                info.fadeOut = fadeOut / length;
            }

            info.fadeInCurve = getClipFadeCurve (clipState, true);
            info.fadeOutCurve = getClipFadeCurve (clipState, false);
            info.followsTempo = getClipTempo (song, info.id) > 0.0;

            if (const auto takes = getClipTakeCount (song, info.id); takes > 1)
            {
                info.takes = takes;
                info.take = getClipTake (song, info.id);
            }
        }

        return info;
    }

    // Whether two audio clips, the second starting and ending later, overlap only where the first fades out and the
    // second fades in: a crossfade. Their fades are parts of their lengths, so with a little leeway for rounding.
    bool isCrossfade (const ClipInfo& first, const ClipInfo& second)
    {
        if (first.kind != ClipType::audio || second.kind != ClipType::audio || second.range.getStart() <= first.range.getStart()
            || second.range.getEnd() <= first.range.getEnd())
            return false;

        const auto overlap = (first.range.getEnd() - second.range.getStart()).inBeats();
        const auto leeway = 1.0e-6 + overlap * 1.0e-3;
        return first.fadeOut * first.range.getLength().inBeats() + leeway >= overlap
               && second.fadeIn * second.range.getLength().inBeats() + leeway >= overlap;
    }
} // namespace

//==============================================================================
std::vector<TrackRow> getPlaylistRows (const ProjectContext& context)
{
    std::vector<TrackRow> rows;
    auto* song = findSong (context);

    if (song == nullptr)
        return rows;

    const auto ids = getTrackIDs (*song);
    std::vector<TrackRow> returns;
    std::vector<juce::Colour> folders; // the colors of the folders the row goes in, by depth: each folder comes before its tracks

    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (isHiddenInCollapsedFolder (*song, ids[i]))
            continue;

        auto row = makeRow (*song, ids[i], (int) i);
        folders.resize ((size_t) row.depth);
        row.folderColours = folders;

        if (row.kind == TrackType::folder)
            folders.push_back (row.colour);

        // A return holds no clips: its row shows only while it has automation lanes, after the tracks, where the master's shows
        // too (docs/design.md, section 6).
        if (row.kind != TrackType::returnTrack)
            rows.push_back (std::move (row));
        else if (! getLanes (*song, ids[i]).empty())
            returns.push_back (std::move (row));
    }

    rows.insert (rows.end(), returns.begin(), returns.end());
    return rows;
}

std::optional<TrackRow> getTrackRow (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (song == nullptr || ! findTrackState (*song, id).isValid())
        return {};

    const auto ids = getTrackIDs (*song);
    const auto indexOf = [&ids] (TrackID track) { return (int) (std::find (ids.begin(), ids.end(), track) - ids.begin()); };
    auto row = makeRow (*song, id, indexOf (id));

    for (auto folder = findTrackState (*song, id).getParent(); folder.hasType (schema::TRACK); folder = folder.getParent())
        row.folderColours.insert (row.folderColours.begin(), getTrackColour (*song, getTrackID (folder), indexOf (getTrackID (folder))));

    return row;
}

std::optional<TrackRow> getMasterRow (const ProjectContext& context)
{
    auto* song = findSong (context);
    const auto master = song != nullptr ? getMasterID (*song) : TrackID();

    if (! master.isValid() || getLanes (*song, master).empty())
        return {};

    return makeRow (*song, master, -1);
}

std::vector<TrackID> getAllPlaylistTracks (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getTrackIDs (*song) : std::vector<TrackID>();
}

bool isInside (const ProjectContext& context, TrackID track, TrackID folder)
{
    auto* song = findSong (context);

    if (song == nullptr || track == folder || ! findTrackState (*song, folder).isValid())
        return false;

    for (auto parent = findTrackState (*song, track).getParent(); parent.hasType (schema::TRACK); parent = parent.getParent())
        if (getTrackID (parent) == folder)
            return true;

    return false;
}

FolderSummary getFolderSummary (const ProjectContext& context, TrackID folder)
{
    FolderSummary summary;
    auto* song = findSong (context);

    if (song == nullptr || ! findTrackState (*song, folder).isValid())
        return summary;

    for (const auto id : getAllPlaylistTracks (context))
    {
        if (! isInside (context, id, folder) || ! canHoldClips (context, id))
            continue;

        const auto colour = getTrackRow (context, id).value_or (TrackRow()).colour;

        for (const auto& clip : getClips (context, id))
            summary.clips.push_back ({ clip.range, colour, summary.lanes, clip.muted });

        ++summary.lanes;
    }

    return summary;
}

void setCollapsed (ProjectContext& context, TrackID id, bool collapsed)
{
    if (auto* song = findSong (context))
        setCollapsed (*song, id, collapsed);
}

void setSavedTrackHeight (ProjectContext& context, TrackID id, int height)
{
    if (auto* song = findSong (context))
        setSavedTrackHeight (*song, id, height);
}

bool isAnyRowExpanded (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr && isAnyRowExpanded (*song);
}

void renameTrack (ProjectContext& context, TrackID id, const juce::String& name)
{
    if (auto* song = findSong (context))
        renameTrack (*song, id, name);
}

void setTrackColours (ProjectContext& context, const std::vector<TrackID>& ids, juce::Colour colour)
{
    if (auto* song = findSong (context))
        setTrackColours (*song, ids, colour);
}

bool canMoveTracksTo (const ProjectContext& context, const std::vector<TrackID>& moving, TrackID target, TrackDrop drop)
{
    auto* song = findSong (context);
    return song != nullptr && canMoveTracksTo (*song, moving, target, drop);
}

void moveTracksTo (ProjectContext& context, const std::vector<TrackID>& moving, TrackID target, TrackDrop drop)
{
    if (auto* song = findSong (context))
        moveTracksTo (*song, moving, target, drop);
}

TrackID addTrackAtEnd (ProjectContext& context, bool folder)
{
    auto* song = findSong (context);
    return song != nullptr ? addTrackAtEnd (*song, folder, &getSelection (context)) : TrackID();
}

//==============================================================================
std::vector<ClipInfo> getClips (const ProjectContext& context, TrackID id)
{
    std::vector<ClipInfo> clips;
    auto* song = findSong (context);

    if (song != nullptr && holdsClips (*song, id))
        for (const auto& clipState : getClipStates (findTrackState (*song, id)))
            clips.push_back (makeInfo (*song, clipState));

    // From the earliest, whatever order the track's element holds them in.
    std::stable_sort (clips.begin(), clips.end(),
                      [] (const ClipInfo& a, const ClipInfo& b) { return a.range.getStart() < b.range.getStart(); });
    return clips;
}

std::optional<ClipInfo> getClip (const ProjectContext& context, ClipID id)
{
    if (auto* song = findSong (context))
        if (const auto clipState = findClipState (*song, id); clipState.isValid())
            return makeInfo (*song, clipState);

    return {};
}

bool canHoldClips (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && holdsClips (*song, id);
}

std::vector<ClipSubRow> getClipSubRows (const std::vector<ClipInfo>& clips)
{
    const auto count = clips.size();
    std::vector<ClipSubRow> subRows (count);

    // The clips each one overlaps, which can't share its sub-row: those that start after it, up to its end. Clips that
    // overlap by less than a thousandth of a beat only touch: a loop as long as its beats ends there, however its
    // length from its audio rounds, and a copy of it placed on its end doesn't overlap it.
    constexpr auto touching = BeatDuration::fromBeats (1.0e-3);
    std::vector<size_t> byStart (count);
    std::iota (byStart.begin(), byStart.end(), size_t());
    std::stable_sort (byStart.begin(), byStart.end(),
                      [&clips] (size_t a, size_t b) { return clips[a].range.getStart() < clips[b].range.getStart(); });
    std::vector<std::vector<size_t>> overlapping (count);

    for (size_t i = 0; i < count; ++i)
    {
        const auto& first = clips[byStart[i]];
        const auto end = first.range.getEnd() - touching;

        for (auto j = i + 1; j < count && clips[byStart[j]].range.getStart() < end; ++j)
        {
            if (const auto& second = clips[byStart[j]]; ! second.range.isEmpty() && ! isCrossfade (first, second))
            {
                overlapping[byStart[i]].push_back (byStart[j]);
                overlapping[byStart[j]].push_back (byStart[i]);
            }
        }
    }

    // The oldest first, each in the highest sub-row that none of the clips it overlaps took before it.
    std::vector<size_t> byAge (count);
    std::iota (byAge.begin(), byAge.end(), size_t());
    std::sort (byAge.begin(), byAge.end(), [&clips] (size_t a, size_t b) { return clips[a].id < clips[b].id; });
    std::vector<int> index (count, -1);

    for (const auto i : byAge)
    {
        std::vector<bool> taken;

        for (const auto other : overlapping[i])
        {
            if (const auto otherIndex = index[other]; otherIndex >= 0)
            {
                taken.resize (std::max (taken.size(), (size_t) otherIndex + 1));
                taken[(size_t) otherIndex] = true;
            }
        }

        index[i] = (int) (std::find (taken.begin(), taken.end(), false) - taken.begin());
    }

    // Clips that overlap, directly or through others, share the row out into as many sub-rows as they take.
    std::vector<bool> reached (count, false);

    for (size_t i = 0; i < count; ++i)
    {
        if (reached[i])
            continue;

        std::vector<size_t> group { i };
        reached[i] = true;
        auto taken = 0;

        for (size_t member = 0; member < group.size(); ++member)
        {
            taken = std::max (taken, index[group[member]] + 1);

            for (const auto other : overlapping[group[member]])
            {
                if (! reached[other])
                {
                    reached[other] = true;
                    group.push_back (other);
                }
            }
        }

        for (const auto member : group)
            subRows[member] = { index[member], taken };
    }

    return subRows;
}

float getFadeGain (FadeCurve curve, float alpha)
{
    return engine::getFadeGain (toEngineFade (curve), alpha);
}

std::vector<NoteShape> getNoteShapes (const ProjectContext& context, ClipID id)
{
    std::vector<NoteShape> shapes;
    auto* song = findSong (context);
    const auto clipState = song != nullptr ? findClipState (*song, id) : juce::ValueTree();

    if (! isMidiState (clipState))
        return shapes;

    // The notes are in the clip's content, which can start before the clip does.
    const auto contentOffset = -getClipOffset (clipState).inBeats();

    for (const auto& note : clipState)
    {
        if (! note.hasType (schema::NOTE))
            continue;

        const auto start = contentOffset + (double) note[schema::noteStart];
        shapes.push_back ({ (int) note[schema::notePitch], start, start + (double) note[schema::noteLength] });
    }

    return shapes;
}

//==============================================================================
ClipID placeClip (ProjectContext& context, TrackID trackID, BeatPosition start, ClipID lastUsed)
{
    auto* song = findSong (context);
    return song != nullptr && holdsClips (*song, trackID) ? placeClip (*song, trackID, start, lastUsed) : ClipID();
}

void moveClip (ProjectContext& context, ClipID id, BeatPosition start, TrackID trackID)
{
    if (auto* song = findSong (context); song != nullptr && holdsClips (*song, trackID))
        moveClip (*song, id, start, trackID);
}

void trimClipStart (ProjectContext& context, ClipID id, BeatPosition start)
{
    if (auto* song = findSong (context))
        trimClipStart (*song, id, start);
}

void trimClipEnd (ProjectContext& context, ClipID id, BeatPosition end)
{
    if (auto* song = findSong (context))
        trimClipEnd (*song, id, end);
}

void fadeClipTo (ProjectContext& context, ClipID id, bool fadeIn, BeatPosition at)
{
    auto* song = findSong (context);
    const auto clipState = song != nullptr ? findClipState (*song, id) : juce::ValueTree();

    if (! isAudioState (clipState))
        return;

    const auto time = toSeconds (*song, at);
    const auto position = getPlayedPosition (*song, clipState);
    setClipFade (*song, id, fadeIn, fadeIn ? time - position.start : position.end - time);
}

void setFadeCurve (ProjectContext& context, ClipID id, bool fadeIn, FadeCurve curve)
{
    if (auto* song = findSong (context))
        setFadeCurve (*song, id, fadeIn, curve);
}

double getSlipOffset (const ProjectContext& context, ClipID id)
{
    auto* song = findSong (context);
    return song != nullptr && findClipState (*song, id).isValid() ? getClipStartOffset (*song, id) : 0.0;
}

void slipClip (ProjectContext& context, ClipID id, double startOffset, BeatPosition from, BeatPosition to)
{
    if (auto* song = findSong (context))
        slipClip (*song, id, startOffset, toSeconds (*song, to) - toSeconds (*song, from));
}

std::vector<ClipID> splitClips (ProjectContext& context, const std::vector<ClipID>& ids, BeatPosition at)
{
    auto* song = findSong (context);
    return song != nullptr ? splitClips (*song, ids, at) : std::vector<ClipID>();
}

std::vector<ClipID> copyClips (ProjectContext& context, const std::vector<ClipID>& ids, bool asAliases)
{
    auto* song = findSong (context);
    return song != nullptr ? copyClips (*song, ids, asAliases) : std::vector<ClipID>();
}

void deleteClips (ProjectContext& context, const std::vector<ClipID>& ids, bool closeGap)
{
    if (auto* song = findSong (context))
        deleteClips (*song, ids, closeGap);
}

void toggleClipsMuted (ProjectContext& context, const std::vector<ClipID>& ids)
{
    if (auto* song = findSong (context))
        toggleClipsMuted (*song, ids);
}

void nudgeClips (ProjectContext& context, const std::vector<ClipID>& ids, BeatDuration step)
{
    if (auto* song = findSong (context))
        nudgeClips (*song, ids, step);
}

void moveAutomationWithClips (ProjectContext& context, const std::vector<ClipPlace>& places, BeatDuration distance, bool copied)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return;

    std::vector<AutomationStretch> stretches;

    for (const auto& place : places)
        if (findTrackState (*song, place.track).isValid())
            stretches.push_back ({ place.track, place.range });

    moveAutomation (*song, stretches, distance, copied);
}

ClipID addMidiClipAt (ProjectContext& context, TrackID trackID, BeatPosition start)
{
    auto* song = findSong (context);
    return song != nullptr && holdsClips (*song, trackID) ? addMidiClipAt (*song, trackID, start, &getSelection (context)) : ClipID();
}

std::vector<CrossfadeSpot> getCrossfadeSpots (const ProjectContext& context, TrackID trackID, BeatDuration gap)
{
    std::vector<CrossfadeSpot> spots;
    auto* song = findSong (context);

    if (song == nullptr || ! holdsClips (*song, trackID))
        return spots;

    std::vector<juce::ValueTree> audio;

    for (const auto& clipState : getClipStates (findTrackState (*song, trackID)))
        if (isAudioState (clipState))
            audio.push_back (clipState);

    for (const auto& first : audio)
    {
        for (const auto& second : audio)
        {
            if (first == second)
                continue;

            // The second starts after the first, where it ends or inside it (a crossfade already), and ends later.
            const auto one = getPlayedPosition (*song, first), two = getPlayedPosition (*song, second);

            if (two.start <= one.start || two.end <= one.end || toBeats (*song, two.start) > toBeats (*song, one.end) + gap)
                continue;

            spots.push_back ({ getClipID (first), getClipID (second), toBeats (*song, (one.end + two.start) / 2.0) });
        }
    }

    return spots;
}

void setCrossfade (ProjectContext& context, const CrossfadeSpot& spot, BeatPosition pointer)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isAudioState (findClipState (*song, spot.first)) || ! isAudioState (findClipState (*song, spot.second)))
        return;

    // As long as twice the distance from the boundary to the pointer: the boundary stays in the middle.
    const auto boundary = toSeconds (*song, spot.boundary);
    const auto distance = toSeconds (*song, pointer) - boundary;
    setCrossfade (*song, spot.first, spot.second, boundary, 2.0 * std::abs (distance));
}

//==============================================================================
bool canImport (const ProjectContext& context, const juce::File& file)
{
    return findSong (context) != nullptr && getFileKind (file) != FileKind::other;
}

std::vector<ClipID> importFiles (ProjectContext& context, const juce::StringArray& paths, TrackID target, BeatPosition start)
{
    auto* song = findSong (context);
    return song != nullptr ? importFiles (*song, paths, target, start) : std::vector<ClipID>();
}

std::vector<SongSection> getSongSections (const ProjectContext& context)
{
    std::vector<SongSection> sections;

    if (auto* song = findSong (context))
        for (const auto& section : getSections (*song))
            sections.push_back (makeSongSection (section));

    return sections;
}

std::optional<SongSection> getSongSection (const ProjectContext& context, SectionID id)
{
    if (auto* song = findSong (context))
        if (const auto section = getSection (*song, id))
            return makeSongSection (*section);

    return {};
}

SectionID addSectionAt (ProjectContext& context, BeatPosition beat)
{
    auto* song = findSong (context);
    return song != nullptr ? addSectionAt (*song, beat, &getSelection (context)) : SectionID();
}

void renameSection (ProjectContext& context, SectionID id, const juce::String& name)
{
    if (auto* song = findSong (context))
        renameSection (*song, id, name);
}

void setSectionColour (ProjectContext& context, SectionID id, juce::Colour colour)
{
    if (auto* song = findSong (context))
        setSectionColour (*song, id, colour);
}

std::vector<BeatPosition> getSectionBoundaries (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getSectionBoundaries (*song) : std::vector<BeatPosition>();
}

void moveSection (ProjectContext& context, SectionID id, BeatPosition destination)
{
    if (auto* song = findSong (context))
        moveSection (*song, id, destination);
}

SectionID copySection (ProjectContext& context, SectionID id, BeatPosition destination, bool asAliases)
{
    auto* song = findSong (context);
    return song != nullptr ? copySection (*song, id, destination, asAliases) : SectionID();
}

void resizeSection (ProjectContext& context, SectionID id, BeatDuration newLength, bool moveRest)
{
    if (auto* song = findSong (context))
        resizeSection (*song, id, newLength, moveRest);
}

BeatPosition getPlayStartBeat (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getPlayStartBeat (*song) : BeatPosition();
}

BeatPosition getTimelineEnd (const ProjectContext& context)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return {};

    // The latest clip's end, a section's or a chord's.
    auto end = std::max (getClipsEnd (*song), getSectionsEnd (*song));

    for (const auto& chord : getLaneChords (*song))
        end = std::max (end, chord.range.getEnd());

    return end;
}
} // namespace phazite

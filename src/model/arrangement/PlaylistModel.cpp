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
#include "model/engine/internal/LiveInput.h"
#include "model/mirror/internal/Bridge.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <numeric>

namespace phazite
{
namespace
{
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::Clip* findClip (const ProjectContext& context, ClipID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findClipForID (*edit, toTracktion (id)) : nullptr;
    }

    juce::Array<te::Track*> findTracks (const ProjectContext& context, const std::vector<TrackID>& ids)
    {
        juce::Array<te::Track*> tracks;

        for (const auto id : ids)
            if (auto* track = findTrack (context, id))
                tracks.add (track);

        return tracks;
    }

    juce::Array<te::Clip*> findClips (const ProjectContext& context, const std::vector<ClipID>& ids)
    {
        juce::Array<te::Clip*> clips;

        for (const auto id : ids)
            if (auto* clip = findClip (context, id))
                clips.add (clip);

        return clips;
    }

    SongSection makeSongSection (const Section& section)
    {
        return { section.id, section.name, section.range, section.colour, section.tip, section.energy };
    }

    std::vector<ClipID> idsOf (const juce::Array<te::Clip*>& clips)
    {
        std::vector<ClipID> ids;

        for (auto* clip : clips)
            if (clip != nullptr)
                ids.push_back (getClipID (*clip));

        return ids;
    }

    // A track's row; its color is its row's in the palette unless it has its own. The master's is gray. Without the colors
    // of the folders it is in, which getPlaylistRows() knows from the folders before it.
    TrackRow makeRow (const te::Edit& edit, TrackID id, int index)
    {
        const auto& song = getSong (edit);
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

        if (auto* audio = dynamic_cast<te::AudioTrack*> (mirror::findTrack (edit, state)))
            row.hasInstrument = hasInstrument (*audio);

        row.frozen = state[schema::trackFrozen];

        return row;
    }

    ClipInfo makeInfo (te::Clip& clip)
    {
        ClipInfo info;
        info.id = getClipID (clip);
        info.track = clip.getTrack() != nullptr ? getTrackID (*clip.getTrack()) : TrackID();
        info.kind = dynamic_cast<te::AudioClipBase*> (&clip) != nullptr ? ClipType::audio : ClipType::midi;
        info.name = clip.getName();
        info.range = { fromTracktion (clip.getStartBeat()), fromTracktion (clip.getEndBeat()) };
        info.muted = clip.isMuted();
        info.linked = hasAliases (getSong (clip.edit), { getClipID (clip) });

        // The song's own place for one of its clips, without rounding through seconds, and its fades' shapes.
        const auto clipState = findClipState (clip);

        if (clipState.isValid())
            info.range = getClipRange (clip.edit, clipState);

        if (auto* audio = dynamic_cast<te::AudioClipBase*> (&clip))
        {
            if (const auto length = audio->getPosition().getLength().inSeconds(); length > 0.0)
            {
                info.fadeIn = audio->getFadeIn().inSeconds() / length;
                info.fadeOut = audio->getFadeOut().inSeconds() / length;
            }

            if (clipState.isValid())
            {
                info.fadeInCurve = getClipFadeCurve (clipState, true);
                info.fadeOutCurve = getClipFadeCurve (clipState, false);
            }
            info.followsTempo = getClipTempo (*audio) > 0.0;

            if (const auto takes = getClipTakeCount (*audio); takes > 1)
            {
                info.takes = takes;
                info.take = getClipTake (*audio);
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
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return rows;

    const auto& song = getSong (*edit);
    const auto ids = getTrackIDs (song);
    std::vector<TrackRow> returns;
    std::vector<juce::Colour> folders; // the colors of the folders the row goes in, by depth: each folder comes before its tracks

    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (isHiddenInCollapsedFolder (song, ids[i]))
            continue;

        auto row = makeRow (*edit, ids[i], (int) i);
        folders.resize ((size_t) row.depth);
        row.folderColours = folders;

        if (row.kind == TrackType::folder)
            folders.push_back (row.colour);

        // A return holds no clips: its row shows only while it has automation lanes, after the tracks, where the master's shows
        // too (docs/design.md, section 6).
        if (row.kind != TrackType::returnTrack)
            rows.push_back (std::move (row));
        else if (auto* track = findTrack (context, ids[i]); track != nullptr && ! getLanes (*track).empty())
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
    auto row = makeRow (*getEdit (context), id, indexOf (id));

    for (auto folder = findTrackState (*song, id).getParent(); folder.hasType (schema::TRACK); folder = folder.getParent())
        row.folderColours.insert (row.folderColours.begin(), getTrackColour (*song, getTrackID (folder), indexOf (getTrackID (folder))));

    return row;
}

std::optional<TrackRow> getMasterRow (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    auto* master = edit != nullptr ? edit->getMasterTrack() : nullptr;

    if (master == nullptr || getLanes (*master).empty())
        return {};

    return makeRow (*edit, getTrackID (*master), -1);
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
    auto* edit = getEdit (context);
    return edit != nullptr && isAnyRowExpanded (*edit);
}

void renameTrack (ProjectContext& context, TrackID id, const juce::String& name)
{
    if (auto* track = findTrack (context, id))
        renameTrack (*track, name);
}

void setTrackColours (ProjectContext& context, const std::vector<TrackID>& ids, juce::Colour colour)
{
    if (const auto tracks = findTracks (context, ids); ! tracks.isEmpty())
        setTrackColours (tracks, colour);
}

bool canMoveTracksTo (const ProjectContext& context, const std::vector<TrackID>& moving, TrackID target, TrackDrop drop)
{
    const auto tracks = findTracks (context, moving);
    auto* to = findTrack (context, target);
    return ! tracks.isEmpty() && to != nullptr && canMoveTracksTo (tracks, *to, drop);
}

void moveTracksTo (ProjectContext& context, const std::vector<TrackID>& moving, TrackID target, TrackDrop drop)
{
    if (const auto tracks = findTracks (context, moving); ! tracks.isEmpty())
        if (auto* to = findTrack (context, target))
            moveTracksTo (tracks, *to, drop);
}

TrackID addTrackAtEnd (ProjectContext& context, bool folder)
{
    auto* edit = getEdit (context);
    auto* added = edit != nullptr ? addTrackAtEnd (*edit, folder, &getSelection (context)) : nullptr;
    return added != nullptr ? getTrackID (*added) : TrackID();
}

//==============================================================================
std::vector<ClipInfo> getClips (const ProjectContext& context, TrackID id)
{
    std::vector<ClipInfo> clips;

    if (auto* track = dynamic_cast<te::ClipTrack*> (findTrack (context, id)))
        for (auto* clip : track->getClips())
            clips.push_back (makeInfo (*clip));

    // Tracktion keeps a track's clips in the order of the project's data, which moving a clip doesn't sort.
    std::stable_sort (clips.begin(), clips.end(),
                      [] (const ClipInfo& a, const ClipInfo& b) { return a.range.getStart() < b.range.getStart(); });
    return clips;
}

std::optional<ClipInfo> getClip (const ProjectContext& context, ClipID id)
{
    if (auto* clip = findClip (context, id))
        return makeInfo (*clip);

    return {};
}

bool canHoldClips (const ProjectContext& context, TrackID id)
{
    return dynamic_cast<te::ClipTrack*> (findTrack (context, id)) != nullptr;
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
    auto* midi = dynamic_cast<te::MidiClip*> (findClip (context, id));

    if (midi == nullptr)
        return shapes;

    // The notes are in the clip's content, which can start before the clip does.
    const auto contentOffset = (midi->getContentStartBeat() - midi->getStartBeat()).inBeats();

    for (auto* note : midi->getSequence().getNotes())
    {
        const auto start = contentOffset + note->getStartBeat().inBeats();
        shapes.push_back ({ note->getNoteNumber(), start, start + note->getLengthBeats().inBeats() });
    }

    return shapes;
}

//==============================================================================
ClipID placeClip (ProjectContext& context, TrackID trackID, BeatPosition start, ClipID lastUsed)
{
    auto* track = dynamic_cast<te::ClipTrack*> (findTrack (context, trackID));
    auto* placed = track != nullptr ? placeClip (*track, toTracktion (start), findClip (context, lastUsed)) : nullptr;
    return placed != nullptr ? getClipID (*placed) : ClipID();
}

void moveClip (ProjectContext& context, ClipID id, BeatPosition start, TrackID trackID)
{
    auto* clip = findClip (context, id);
    auto* track = dynamic_cast<te::ClipTrack*> (findTrack (context, trackID));

    if (clip != nullptr && track != nullptr)
        moveClip (*clip, toTracktion (start), *track);
}

void trimClipStart (ProjectContext& context, ClipID id, BeatPosition start)
{
    if (auto* clip = findClip (context, id))
        trimClipStart (*clip, toTracktion (start));
}

void trimClipEnd (ProjectContext& context, ClipID id, BeatPosition end)
{
    if (auto* clip = findClip (context, id))
        trimClipEnd (*clip, toTracktion (end));
}

void fadeClipTo (ProjectContext& context, ClipID id, bool fadeIn, BeatPosition at)
{
    auto* audio = dynamic_cast<te::AudioClipBase*> (findClip (context, id));

    if (audio == nullptr)
        return;

    const auto time = audio->edit.tempoSequence.toTime (toTracktion (at));
    const auto position = audio->getPosition();
    setClipFade (*audio, fadeIn, fadeIn ? time - position.getStart() : position.getEnd() - time);
}

void setFadeCurve (ProjectContext& context, ClipID id, bool fadeIn, FadeCurve curve)
{
    if (auto* clip = findClip (context, id))
        setFadeCurve (*clip, fadeIn, curve);
}

double getSlipOffset (const ProjectContext& context, ClipID id)
{
    auto* clip = findClip (context, id);
    return clip != nullptr ? clip->getPosition().getOffset().inSeconds() : 0.0;
}

void slipClip (ProjectContext& context, ClipID id, double startOffset, BeatPosition from, BeatPosition to)
{
    if (auto* clip = findClip (context, id))
    {
        const auto& tempo = clip->edit.tempoSequence;
        slipClip (*clip, te::TimeDuration::fromSeconds (startOffset), tempo.toTime (toTracktion (to)) - tempo.toTime (toTracktion (from)));
    }
}

std::vector<ClipID> splitClips (ProjectContext& context, const std::vector<ClipID>& ids, BeatPosition at)
{
    const auto clips = findClips (context, ids);
    return clips.isEmpty() ? std::vector<ClipID>() : idsOf (splitClips (clips, toTracktion (at)));
}

std::vector<ClipID> copyClips (ProjectContext& context, const std::vector<ClipID>& ids, bool asAliases)
{
    const auto clips = findClips (context, ids);
    return clips.isEmpty() ? std::vector<ClipID>() : idsOf (copyClips (clips, asAliases));
}

void deleteClips (ProjectContext& context, const std::vector<ClipID>& ids, bool closeGap)
{
    if (const auto clips = findClips (context, ids); ! clips.isEmpty())
        deleteClips (clips, closeGap);
}

void toggleClipsMuted (ProjectContext& context, const std::vector<ClipID>& ids)
{
    if (auto* song = findSong (context))
        toggleClipsMuted (*song, ids);
}

void nudgeClips (ProjectContext& context, const std::vector<ClipID>& ids, BeatDuration step)
{
    if (const auto clips = findClips (context, ids); ! clips.isEmpty())
        nudgeClips (clips, toTracktion (step));
}

void moveAutomationWithClips (ProjectContext& context, const std::vector<ClipPlace>& places, BeatDuration distance, bool copied)
{
    std::vector<AutomationStretch> stretches;

    for (const auto& place : places)
        if (auto* track = findTrack (context, place.track))
            stretches.push_back ({ track, place.range });

    moveAutomation (stretches, distance, copied);
}

ClipID addMidiClipAt (ProjectContext& context, TrackID trackID, BeatPosition start)
{
    auto* track = dynamic_cast<te::ClipTrack*> (findTrack (context, trackID));
    auto* added = track != nullptr ? addMidiClipAt (*track, toTracktion (start), &getSelection (context)) : nullptr;
    return added != nullptr ? getClipID (*added) : ClipID();
}

std::vector<CrossfadeSpot> getCrossfadeSpots (const ProjectContext& context, TrackID trackID, BeatDuration gap)
{
    std::vector<CrossfadeSpot> spots;
    auto* track = dynamic_cast<te::ClipTrack*> (findTrack (context, trackID));

    if (track == nullptr)
        return spots;

    const auto& tempo = track->edit.tempoSequence;

    for (auto* a : track->getClips())
    {
        for (auto* b : track->getClips())
        {
            auto* first = dynamic_cast<te::AudioClipBase*> (a);
            auto* second = dynamic_cast<te::AudioClipBase*> (b);

            if (first == nullptr || second == nullptr || first == second)
                continue;

            // The second starts after the first, where it ends or inside it (a crossfade already), and ends later.
            const auto one = first->getEditTimeRange(), two = second->getEditTimeRange();

            if (two.getStart() <= one.getStart() || two.getEnd() <= one.getEnd()
                || tempo.toBeats (two.getStart()) > tempo.toBeats (one.getEnd()) + toTracktion (gap))
                continue;

            const auto boundary = te::TimePosition::fromSeconds ((one.getEnd().inSeconds() + two.getStart().inSeconds()) / 2.0);
            spots.push_back ({ getClipID (*first), getClipID (*second), fromTracktion (tempo.toBeats (boundary)) });
        }
    }

    return spots;
}

void setCrossfade (ProjectContext& context, const CrossfadeSpot& spot, BeatPosition pointer)
{
    auto* first = dynamic_cast<te::AudioClipBase*> (findClip (context, spot.first));
    auto* second = dynamic_cast<te::AudioClipBase*> (findClip (context, spot.second));

    if (first == nullptr || second == nullptr)
        return;

    // As long as twice the distance from the boundary to the pointer: the boundary stays in the middle.
    const auto& tempo = first->edit.tempoSequence;
    const auto boundary = tempo.toTime (toTracktion (spot.boundary));
    const auto distance = tempo.toTime (toTracktion (pointer)) - boundary;
    setCrossfade (*first, *second, boundary, te::TimeDuration::fromSeconds (2.0 * std::abs (distance.inSeconds())));
}

//==============================================================================
bool canImport (const ProjectContext& context, const juce::File& file)
{
    auto* edit = getEdit (context);
    return edit != nullptr && getFileKind (file) != FileKind::other;
}

std::vector<ClipID> importFiles (ProjectContext& context, const juce::StringArray& paths, TrackID target, BeatPosition start)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? idsOf (importFiles (*edit, paths, findTrack (context, target), toTracktion (start))) : std::vector<ClipID>();
}

std::vector<SongSection> getSongSections (const ProjectContext& context)
{
    std::vector<SongSection> sections;

    if (auto* edit = getEdit (context))
        for (const auto& section : getSections (getSong (*edit)))
            sections.push_back (makeSongSection (section));

    return sections;
}

std::optional<SongSection> getSongSection (const ProjectContext& context, SectionID id)
{
    if (auto* edit = getEdit (context))
        if (const auto section = getSection (getSong (*edit), id))
            return makeSongSection (*section);

    return {};
}

SectionID addSectionAt (ProjectContext& context, BeatPosition beat)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? addSectionAt (getSong (*edit), beat, &getSelection (context)) : SectionID();
}

void renameSection (ProjectContext& context, SectionID id, const juce::String& name)
{
    if (auto* edit = getEdit (context))
        renameSection (getSong (*edit), id, name);
}

void setSectionColour (ProjectContext& context, SectionID id, juce::Colour colour)
{
    if (auto* edit = getEdit (context))
        setSectionColour (getSong (*edit), id, colour);
}

std::vector<BeatPosition> getSectionBoundaries (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? getSectionBoundaries (getSong (*edit)) : std::vector<BeatPosition>();
}

void moveSection (ProjectContext& context, SectionID id, BeatPosition destination)
{
    if (auto* edit = getEdit (context))
        moveSection (*edit, id, destination);
}

SectionID copySection (ProjectContext& context, SectionID id, BeatPosition destination, bool asAliases)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? copySection (*edit, id, destination, asAliases) : SectionID();
}

void resizeSection (ProjectContext& context, SectionID id, BeatDuration newLength, bool moveRest)
{
    if (auto* edit = getEdit (context))
        resizeSection (*edit, id, newLength, moveRest);
}

BeatPosition getPlayStartBeat (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? fromTracktion (edit->tempoSequence.toBeats (getPlayStart (*edit))) : BeatPosition();
}

BeatPosition getTimelineEnd (const ProjectContext& context)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return {};

    auto end =
        std::max (fromTracktion (edit->tempoSequence.toBeats (te::toPosition (edit->getLength()))), getSectionsEnd (getSong (*edit)));

    for (const auto& chord : getLaneChords (getSong (*edit)))
        end = std::max (end, chord.range.getEnd());

    return end;
}
} // namespace phazite

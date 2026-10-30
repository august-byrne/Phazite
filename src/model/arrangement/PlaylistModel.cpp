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
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

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

    FadeShape toFadeShape (te::AudioFadeCurve::Type type)
    {
        switch (type)
        {
            case te::AudioFadeCurve::convex:
                return FadeShape::convex;
            case te::AudioFadeCurve::concave:
                return FadeShape::concave;
            case te::AudioFadeCurve::sCurve:
                return FadeShape::sCurve;
            case te::AudioFadeCurve::linear:
            default:
                return FadeShape::linear;
        }
    }

    te::AudioFadeCurve::Type toFadeType (FadeShape shape)
    {
        switch (shape)
        {
            case FadeShape::convex:
                return te::AudioFadeCurve::convex;
            case FadeShape::concave:
                return te::AudioFadeCurve::concave;
            case FadeShape::sCurve:
                return te::AudioFadeCurve::sCurve;
            case FadeShape::linear:
            default:
                return te::AudioFadeCurve::linear;
        }
    }

    // A track's row; its color is its row's in the palette unless it has its own. The master's is gray.
    TrackRow makeRow (te::Track& track, int index)
    {
        TrackRow row;
        row.id = getTrackID (track);
        row.collapsed = isCollapsed (track); // the master's: its lanes are thin rows

        if (track.isMasterTrack())
        {
            row.name = "Master";
            row.colour = getTrackColours().back().colour;
            row.kind = TrackType::master;
            return row;
        }

        const auto state = findTrackState (track);
        row.name = getTrackName (track);
        row.colour = getTrackColour (track, index);
        row.kind = isFolderState (state) ? TrackType::folder : (isReturnState (state) ? TrackType::returnTrack : TrackType::track);

        for (auto folder = state.getParent(); folder.hasType (schema::TRACK); folder = folder.getParent())
            ++row.depth;

        row.pinned = isPinned (track);
        row.savedHeight = getSavedTrackHeight (track);

        if (auto* audio = dynamic_cast<te::AudioTrack*> (&track))
        {
            row.hasInstrument = hasInstrument (*audio);
            row.frozen = audio->isFrozen (te::Track::individualFreeze);
        }

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
        info.linked = clip.isLinked();

        if (auto* audio = dynamic_cast<te::AudioClipBase*> (&clip))
        {
            if (const auto length = audio->getPosition().getLength().inSeconds(); length > 0.0)
            {
                info.fadeIn = audio->getFadeIn().inSeconds() / length;
                info.fadeOut = audio->getFadeOut().inSeconds() / length;
            }

            info.fadeInShape = toFadeShape (audio->getFadeInType());
            info.fadeOutShape = toFadeShape (audio->getFadeOutType());
            info.followsTempo = getClipTempo (*audio) > 0.0;

            if (const auto takes = (int) audio->state.getProperty (schema::phaziteTakes, 0); takes > 1)
            {
                info.takes = takes;
                info.take = (int) audio->state.getProperty (schema::phaziteTake, 1);
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

    const auto tracks = getPlaylistTracks (*edit);

    for (int i = 0; i < tracks.size(); ++i)
        if (! isHiddenInCollapsedFolder (*tracks[i]))
            rows.push_back (makeRow (*tracks[i], i));

    return rows;
}

std::optional<TrackRow> getTrackRow (const ProjectContext& context, TrackID id)
{
    auto* edit = getEdit (context);
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return {};

    return makeRow (*track, getPlaylistTracks (*edit).indexOf (track));
}

std::optional<TrackRow> getMasterRow (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    auto* master = edit != nullptr ? edit->getMasterTrack() : nullptr;

    if (master == nullptr || getLanes (*master).empty())
        return {};

    return makeRow (*master, -1);
}

std::vector<TrackID> getAllPlaylistTracks (const ProjectContext& context)
{
    std::vector<TrackID> ids;

    if (auto* edit = getEdit (context))
        for (auto* track : getPlaylistTracks (*edit))
            ids.push_back (getTrackID (*track));

    return ids;
}

bool isInside (const ProjectContext& context, TrackID track, TrackID folder)
{
    auto* inner = findTrack (context, track);
    auto* outer = findTrack (context, folder);
    return inner != nullptr && outer != nullptr && inner != outer && inner->isAChildOf (*outer);
}

void setCollapsed (ProjectContext& context, TrackID id, bool collapsed)
{
    if (auto* track = findTrack (context, id))
        setCollapsed (*track, collapsed);
}

void setSavedTrackHeight (ProjectContext& context, TrackID id, int height)
{
    if (auto* track = findTrack (context, id))
        setSavedTrackHeight (*track, height);
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

void moveTracksAfter (ProjectContext& context, const std::vector<TrackID>& moving, TrackID previous)
{
    if (const auto tracks = findTracks (context, moving); ! tracks.isEmpty())
        moveTracksAfter (tracks, findTrack (context, previous));
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

    // The clips each one overlaps, which can't share its sub-row: those that start after it, up to its end.
    std::vector<size_t> byStart (count);
    std::iota (byStart.begin(), byStart.end(), size_t());
    std::stable_sort (byStart.begin(), byStart.end(),
                      [&clips] (size_t a, size_t b) { return clips[a].range.getStart() < clips[b].range.getStart(); });
    std::vector<std::vector<size_t>> overlapping (count);

    for (size_t i = 0; i < count; ++i)
    {
        const auto& first = clips[byStart[i]];

        for (auto j = i + 1; j < count && clips[byStart[j]].range.getStart() < first.range.getEnd(); ++j)
        {
            if (const auto& second = clips[byStart[j]]; first.range.overlaps (second.range) && ! isCrossfade (first, second))
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

float getFadeGain (FadeShape shape, float alpha)
{
    return te::AudioFadeCurve::alphaToGainForType (toFadeType (shape), alpha);
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
    if (const auto clips = findClips (context, ids); ! clips.isEmpty())
        toggleClipsMuted (clips);
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
    return edit != nullptr && getFileKind (edit->engine, file) != FileKind::other;
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
        for (const auto& section : getSections (*edit))
            sections.push_back (makeSongSection (section));

    return sections;
}

std::optional<SongSection> getSongSection (const ProjectContext& context, SectionID id)
{
    if (auto* edit = getEdit (context))
        if (const auto section = getSection (*edit, id))
            return makeSongSection (*section);

    return {};
}

SectionID addSectionAt (ProjectContext& context, BeatPosition beat)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? addSectionAt (*edit, beat, &getSelection (context)) : SectionID();
}

void renameSection (ProjectContext& context, SectionID id, const juce::String& name)
{
    if (auto* edit = getEdit (context))
        renameSection (*edit, id, name);
}

void setSectionColour (ProjectContext& context, SectionID id, juce::Colour colour)
{
    if (auto* edit = getEdit (context))
        setSectionColour (*edit, id, colour);
}

std::vector<BeatPosition> getSectionBoundaries (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? getSectionBoundaries (*edit) : std::vector<BeatPosition>();
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

    auto end = std::max (fromTracktion (edit->tempoSequence.toBeats (te::toPosition (edit->getLength()))), getSectionsEnd (*edit));

    for (const auto& chord : getLaneChords (*edit))
        end = std::max (end, chord.range.getEnd());

    return end;
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlaylistModel.h"
#include "Clips.h"
#include "Import.h"
#include "Sections.h"
#include "Tracks.h"
#include "model/engine/LiveInput.h"
#include "model/mixer/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/transport/Playback.h"

namespace phazite
{
namespace
{
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = context.getEdit();
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::Clip* findClip (const ProjectContext& context, ClipID id)
    {
        auto* edit = context.getEdit();
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

    te::ArrangerClip* findSection (const ProjectContext& context, SectionID id)
    {
        auto* edit = context.getEdit();
        return edit != nullptr && id.isValid() ? dynamic_cast<te::ArrangerClip*> (te::findClipForID (*edit, toTracktion (id))) : nullptr;
    }

    SongSection makeSection (te::ArrangerClip& section)
    {
        return { getSectionID (section),     section.getName(),       fromTracktion (section.getEditBeatRange()),
                 getSectionColour (section), getSectionTip (section), getSectionEnergy (section) };
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

    // A track's row; its color is its row's in the palette unless it has its own.
    TrackRow makeRow (te::Track& track, int index)
    {
        TrackRow row;
        row.id = getTrackID (track);
        row.name = track.getName();
        row.colour = getTrackColour (track, index);
        row.kind = track.isFolderTrack() ? TrackType::folder : (getReturnBus (track) >= 0 ? TrackType::returnTrack : TrackType::track);
        row.depth = track.getTrackDepth();
        row.pinned = isPinned (track);
        row.collapsed = isCollapsed (track);
        row.savedHeight = getSavedTrackHeight (track);

        if (auto* audio = dynamic_cast<te::AudioTrack*> (&track))
            row.hasInstrument = hasInstrument (*audio);

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
        }

        return info;
    }
} // namespace

//==============================================================================
std::vector<TrackRow> getPlaylistRows (const ProjectContext& context)
{
    std::vector<TrackRow> rows;
    auto* edit = context.getEdit();

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
    auto* edit = context.getEdit();
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return {};

    return makeRow (*track, getPlaylistTracks (*edit).indexOf (track));
}

std::vector<TrackID> getAllPlaylistTracks (const ProjectContext& context)
{
    std::vector<TrackID> ids;

    if (auto* edit = context.getEdit())
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
    auto* edit = context.getEdit();
    auto* added = edit != nullptr ? addTrackAtEnd (*edit, folder, &context.getSelection()) : nullptr;
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

ClipID addMidiClipAt (ProjectContext& context, TrackID trackID, BeatPosition start)
{
    auto* track = dynamic_cast<te::ClipTrack*> (findTrack (context, trackID));
    auto* added = track != nullptr ? addMidiClipAt (*track, toTracktion (start), &context.getSelection()) : nullptr;
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
    auto* edit = context.getEdit();
    return edit != nullptr && getFileKind (edit->engine, file) != FileKind::other;
}

std::vector<ClipID> importFiles (ProjectContext& context, const juce::StringArray& paths, TrackID target, BeatPosition start)
{
    auto* edit = context.getEdit();
    return edit != nullptr ? idsOf (importFiles (*edit, paths, findTrack (context, target), toTracktion (start))) : std::vector<ClipID>();
}

std::vector<SongSection> getSongSections (const ProjectContext& context)
{
    std::vector<SongSection> sections;

    if (auto* edit = context.getEdit())
        for (auto* section : getSections (*edit))
            sections.push_back (makeSection (*section));

    return sections;
}

std::optional<SongSection> getSongSection (const ProjectContext& context, SectionID id)
{
    if (auto* section = findSection (context, id))
        return makeSection (*section);

    return {};
}

SectionID addSectionAt (ProjectContext& context, BeatPosition beat)
{
    auto* edit = context.getEdit();
    auto* added = edit != nullptr ? addSectionAt (*edit, toTracktion (beat), &context.getSelection()) : nullptr;
    return added != nullptr ? getSectionID (*added) : SectionID();
}

void renameSection (ProjectContext& context, SectionID id, const juce::String& name)
{
    if (auto* section = findSection (context, id))
        renameSection (*section, name);
}

void setSectionColour (ProjectContext& context, SectionID id, juce::Colour colour)
{
    if (auto* section = findSection (context, id))
        setSectionColour (*section, colour);
}

std::vector<BeatPosition> getSectionBoundaries (const ProjectContext& context)
{
    std::vector<BeatPosition> boundaries;

    if (auto* edit = context.getEdit())
        for (const auto boundary : getSectionBoundaries (*edit))
            boundaries.push_back (fromTracktion (boundary));

    return boundaries;
}

void moveSection (ProjectContext& context, SectionID id, BeatPosition destination)
{
    if (auto* section = findSection (context, id))
        moveSection (*section, toTracktion (destination));
}

SectionID copySection (ProjectContext& context, SectionID id, BeatPosition destination, bool asAliases)
{
    auto* section = findSection (context, id);
    auto* copy = section != nullptr ? copySection (*section, toTracktion (destination), asAliases) : nullptr;
    return copy != nullptr ? getSectionID (*copy) : SectionID();
}

void resizeSection (ProjectContext& context, SectionID id, BeatDuration newLength, bool moveRest)
{
    if (auto* section = findSection (context, id))
        resizeSection (*section, toTracktion (newLength), moveRest);
}

BeatPosition getPlayStartBeat (const ProjectContext& context)
{
    auto* edit = context.getEdit();
    return edit != nullptr ? fromTracktion (edit->tempoSequence.toBeats (getPlayStart (*edit))) : BeatPosition();
}

BeatPosition getTimelineEnd (const ProjectContext& context)
{
    auto* edit = context.getEdit();
    return edit != nullptr ? fromTracktion (edit->tempoSequence.toBeats (te::toPosition (edit->getLength()))) : BeatPosition();
}
} // namespace phazite

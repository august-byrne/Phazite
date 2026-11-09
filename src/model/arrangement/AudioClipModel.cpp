// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioClipModel.h"
#include "internal/AudioClips.h"
#include "internal/ClipRenders.h"
#include "internal/Clips.h"
#include "internal/Tracks.h"
#include "internal/Transients.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace phazite
{
namespace
{
    // An audio clip's element in the open project's song, or invalid.
    juce::ValueTree findAudioClip (const ProjectContext& context, ClipID id)
    {
        auto* song = findSong (context);
        const auto clipState = song != nullptr ? findClipState (*song, id) : juce::ValueTree();
        return isAudioState (clipState) ? clipState : juce::ValueTree();
    }

    // A track that can hold clips of either kind (not a folder or the master), or invalid.
    juce::ValueTree findTrack (const ProjectContext& context, TrackID id)
    {
        auto* song = findSong (context);
        const auto state = song != nullptr ? findTrackState (*song, id) : juce::ValueTree();
        return state.hasType (schema::TRACK) && ! isFolderState (state) ? state : juce::ValueTree();
    }

    bool holdsAudio (const juce::ValueTree& trackState)
    {
        const auto clips = getClipStates (trackState);
        return std::any_of (clips.begin(), clips.end(), isAudioState);
    }

    // A change to an audio clip, as an undo step of its own with that name.
    template <typename Change>
    void changeClip (ProjectContext& context, ClipID id, const juce::String& name, Change&& change)
    {
        if (findAudioClip (context, id).isValid())
        {
            auto& song = *findSong (context);
            const UndoStep step (song, name);
            change (song);
        }
    }
} // namespace

std::optional<AudioClipInfo> getAudioClip (const ProjectContext& context, ClipID id)
{
    const auto clipState = findAudioClip (context, id);
    const auto shown = getClip (context, id);

    if (! clipState.isValid() || ! shown.has_value())
        return {};

    const auto& song = *findSong (context);
    AudioClipInfo info;
    info.clip = *shown;

    // The file as it was recorded, not the render that plays it stretched, pitched or reversed.
    const auto original = getClipFile (song, clipState);
    const auto file = getAudioFileInfo (original);
    info.fileName = original.getFileName();
    info.sampleRate = file.sampleRate;
    info.channels = file.numChannels;
    info.seconds = file.getSeconds();

    const auto audio = getAudioFileRange (song, clipState);
    info.content = { toBeats (song, audio.start), toBeats (song, audio.end) };

    info.gain = getClipGain (song, id);
    info.pitch = getClipPitch (song, id);
    info.reversed = isClipReversed (song, id);
    info.tempo = getClipTempo (song, id);
    info.followsTempo = info.tempo > 0.0;

    // A render not made yet is being prepared (ClipRenders.h).
    if (file.sampleRate <= 0.0)
        info.state = AudioState::missing;
    else if (isRendered (song, id) && ! getAudioFileInfo (getPlayedFile (song, clipState)).isValid())
        info.state = AudioState::preparing;

    return info;
}

std::optional<ClipID> getAudioClipNearest (const ProjectContext& context, TrackID id, BeatPosition beat)
{
    const auto track = findTrack (context, id);

    if (! track.isValid())
        return {};

    const auto& song = *findSong (context);
    std::optional<ClipID> nearest;
    auto nearestDistance = std::numeric_limits<double>::max();

    for (const auto& clipState : getClipStates (track))
    {
        if (! isAudioState (clipState))
            continue;

        const auto range = getClipRange (song, clipState);
        const auto distance = range.contains (beat)
                                  ? 0.0
                                  : std::min (std::abs ((range.getStart() - beat).inBeats()), std::abs ((range.getEnd() - beat).inBeats()));

        if (distance < nearestDistance)
        {
            nearest = getClipID (clipState);
            nearestDistance = distance;
        }
    }

    return nearest;
}

std::optional<ClipType> getClipTypeToEdit (const ProjectContext& context)
{
    const auto& selection = context.getSongSelection();
    std::optional<ClipType> type;

    for (const auto clip : selection.getClips())
    {
        if (const auto info = getClip (context, clip))
        {
            if (info->kind == ClipType::midi)
                return ClipType::midi;

            type = ClipType::audio;
        }
    }

    if (type.has_value())
        return type;

    if (const auto tracks = selection.getTracks(); ! tracks.empty())
        if (const auto track = findTrack (context, tracks.front()); track.isValid())
            return holdsAudio (track) ? ClipType::audio : ClipType::midi;

    return {};
}

std::optional<ClipID> getAudioClipToEdit (const ProjectContext& context, BeatPosition beat)
{
    const auto& selection = context.getSongSelection();
    std::optional<ClipID> firstAudio;

    for (const auto clip : selection.getClips())
    {
        if (const auto info = getClip (context, clip))
        {
            if (info->kind == ClipType::midi)
                return {};

            if (! firstAudio.has_value())
                firstAudio = clip;
        }
    }

    if (firstAudio.has_value())
        return firstAudio;

    if (const auto tracks = selection.getTracks(); ! tracks.empty())
        return getAudioClipNearest (context, tracks.front(), beat);

    return {};
}

//==============================================================================
void setClipGain (ProjectContext& context, ClipID id, float decibels)
{
    changeClip (context, id, "Change Clip Gain", [id, decibels] (Song& song) { setClipGain (song, id, decibels); });
}

void normalizeClip (ProjectContext& context, ClipID id)
{
    changeClip (context, id, "Normalize Clip", [id] (Song& song) { normalizeClips (song, { id }); });
}

void setClipPitch (ProjectContext& context, ClipID id, float semitones)
{
    changeClip (context, id, "Change Clip Pitch", [id, semitones] (Song& song) { setClipPitch (song, id, semitones); });
}

void setClipReversed (ProjectContext& context, ClipID id, bool reversed)
{
    changeClip (context, id, reversed ? "Reverse Clip" : "Play Clip Forwards",
                [id, reversed] (Song& song) { setClipReversed (song, id, reversed); });
}

void setClipFollowsTempo (ProjectContext& context, ClipID id, bool follows)
{
    changeClip (context, id, follows ? "Follow Tempo" : "Stop Following Tempo",
                [id, follows] (Song& song) { setClipFollowsTempo (song, id, follows); });
}

void setClipTempo (ProjectContext& context, ClipID id, double bpm)
{
    changeClip (context, id, "Change Clip Tempo", [id, bpm] (Song& song) { setClipTempo (song, id, bpm); });
}

//==============================================================================
std::vector<WarpMarker> getWarpMarkers (const ProjectContext& context, ClipID id)
{
    return findAudioClip (context, id).isValid() ? getWarpMarkers (*findSong (context), id) : std::vector<WarpMarker>();
}

double getAudioTimeAt (const ProjectContext& context, ClipID id, BeatPosition beat)
{
    return findAudioClip (context, id).isValid() ? getFileTimeAt (*findSong (context), id, beat) : 0.0;
}

bool addWarpMarker (ProjectContext& context, ClipID id, double time)
{
    const auto clipState = findAudioClip (context, id);
    auto* song = findSong (context);

    // None where it can't be, so that no empty step is left.
    if (! clipState.isValid() || time < 0.0 || time > getClipFileLength (*song, clipState))
        return false;

    for (const auto& marker : getWarpMarkers (*song, id))
        if (std::abs (marker.time - time) < minWarpGap)
            return false;

    auto added = false;
    changeClip (context, id, "Add Warp Marker", [id, time, &added] (Song& changed) { added = addWarpMarker (changed, id, time); });
    return added;
}

void moveWarpMarker (ProjectContext& context, ClipID id, double time, BeatPosition beat)
{
    changeClip (context, id, "Move Warp Marker", [id, time, beat] (Song& song) { moveWarpMarker (song, id, time, beat); });
}

void removeWarpMarker (ProjectContext& context, ClipID id, double time)
{
    changeClip (context, id, "Remove Warp Marker", [id, time] (Song& song) { removeWarpMarker (song, id, time); });
}

void removeWarpMarkers (ProjectContext& context, ClipID id)
{
    if (! getWarpMarkers (context, id).empty())
        changeClip (context, id, "Remove Warp Markers", [id] (Song& song) { removeWarpMarkers (song, id); });
}

std::optional<std::vector<WarpMarker>> getTransients (const ProjectContext& context, ClipID id)
{
    const auto clipState = findAudioClip (context, id);

    if (! clipState.isValid())
        return std::vector<WarpMarker>();

    const auto& song = *findSong (context);
    const auto hits = getTransientFinder (context).find (getClipFile (song, clipState));

    if (! hits.has_value())
        return std::nullopt;

    std::vector<WarpMarker> shown;
    const auto beats = getBeatsOfFileTimes (song, id, *hits);

    for (size_t i = 0; i < hits->size(); ++i)
        shown.push_back ({ (*hits)[i], beats[i] });

    return shown;
}

void quantizeAudio (ProjectContext& context, const std::vector<ClipID>& clips, BeatDuration grid, double strength)
{
    if (! canQuantizeAudio (context, clips) || grid.inBeats() <= 0.0)
        return;

    // Each clip's hits, found now if they haven't been: the command can't wait for them.
    auto& song = *findSong (context);
    auto& finder = getTransientFinder (context);
    const UndoStep step (song, "Quantize Audio");

    for (const auto clip : clips)
        if (const auto clipState = findAudioClip (context, clip); clipState.isValid())
            quantizeClipAudio (song, clip, finder.findNow (getClipFile (song, clipState)), grid, strength);
}

bool canQuantizeAudio (const ProjectContext& context, const std::vector<ClipID>& clips)
{
    auto* song = findSong (context);
    return song != nullptr && hasAudioClips (*song, clips);
}

ClipRenderProgress getClipRenderProgress (const ProjectContext& context)
{
    if (const auto* renders = getClipRenders (context))
    {
        const auto progress = renders->getProgress();
        return { progress.renders, progress.done };
    }

    return {};
}

std::map<ClipID, double> getClipsBeingRendered (const ProjectContext& context)
{
    std::map<ClipID, double> clips;
    const auto* renders = getClipRenders (context);
    auto* song = findSong (context);

    if (renders == nullptr || song == nullptr || renders->getProgress().renders == 0)
        return clips;

    for (const auto clip : getAudioClips (*song))
        if (isRendered (*song, clip))
            if (const auto progress = renders->getProgressOf (getPlayedFile (*song, findClipState (*song, clip))))
                clips[clip] = *progress;

    return clips;
}

namespace
{
    constexpr double shortestClip = 1.0 / 16.0; // a 64th note, in beats

    const char* getUndoName (ClipProperty property)
    {
        switch (property)
        {
            case ClipProperty::start:
                return "Move Clips";
            case ClipProperty::length:
                return "Change Clip Length";
            case ClipProperty::gain:
                return "Change Clip Gain";
            case ClipProperty::pitch:
            default:
                return "Change Clip Pitch";
        }
    }

    // A clip's value of a property, or nothing for one it doesn't have (a MIDI clip's gain and pitch).
    std::optional<double> getProperty (const ProjectContext& context, const ClipInfo& clip, ClipProperty property)
    {
        if (property == ClipProperty::start)
            return clip.range.getStart().inBeats();

        if (property == ClipProperty::length)
            return clip.range.getLength().inBeats();

        if (const auto audio = getAudioClip (context, clip.id))
            return property == ClipProperty::gain ? audio->gain : audio->pitch;

        return {};
    }

    // Each clip that has the property given the value worked out for it from its own and the earliest start, as one step.
    void setEachClip (ProjectContext& context, const std::vector<ClipID>& ids, ClipProperty property,
                      const std::function<double (double earliest, double own)>& valueOf)
    {
        auto* song = findSong (context);
        std::vector<std::pair<ClipInfo, double>> clips;
        auto earliest = std::numeric_limits<double>::max();

        for (const auto id : ids)
        {
            if (const auto clip = getClip (context, id))
            {
                if (const auto own = getProperty (context, *clip, property))
                {
                    clips.emplace_back (*clip, *own);
                    earliest = std::min (earliest, clip->range.getStart().inBeats());
                }
            }
        }

        if (song == nullptr || clips.empty())
            return;

        const UndoStep step (*song, getUndoName (property));

        for (const auto& [clip, own] : clips)
        {
            const auto value = valueOf (earliest, own);

            switch (property)
            {
                case ClipProperty::start:
                    moveClip (context, clip.id, BeatPosition::fromBeats (std::max (0.0, value)), clip.track);
                    break;
                case ClipProperty::length:
                    trimClipEnd (context, clip.id, clip.range.getStart() + BeatDuration::fromBeats (std::max (shortestClip, value)));
                    break;
                case ClipProperty::gain:
                    setClipGain (context, clip.id, (float) value);
                    break;
                case ClipProperty::pitch:
                    setClipPitch (context, clip.id, (float) value);
                    break;
            }
        }
    }
} // namespace

void changeClips (ProjectContext& context, const std::vector<ClipID>& ids, ClipProperty property, double amount)
{
    // Moved together, as far as the earliest can go.
    setEachClip (context, ids, property, [property, amount] (double earliest, double own)
                 { return own + (property == ClipProperty::start ? std::max (-earliest, amount) : amount); });
}

void setClipsTo (ProjectContext& context, const std::vector<ClipID>& ids, ClipProperty property, double value)
{
    setEachClip (context, ids, property, [property, value] (double earliest, double own)
                 { return property == ClipProperty::start ? own + value - earliest : value; });
}
} // namespace phazite

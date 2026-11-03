// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioClipModel.h"
#include "internal/AudioClips.h"
#include "internal/ClipRenders.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace phazite
{
namespace
{
    te::AudioClipBase* findAudioClip (const ProjectContext& context, ClipID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::AudioClipBase*> (te::findClipForID (*edit, toTracktion (id))) : nullptr;
    }

    // A track that can hold clips of either kind (not a folder).
    te::AudioTrack* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::AudioTrack*> (te::findTrackForID (*edit, toTracktion (id))) : nullptr;
    }

    bool holdsAudio (const te::AudioTrack& track)
    {
        for (auto* clip : track.getClips())
            if (dynamic_cast<te::AudioClipBase*> (clip) != nullptr)
                return true;

        return false;
    }

    // A change to an audio clip, as an undo step of its own with that name.
    template <typename Change>
    void changeClip (ProjectContext& context, ClipID id, const juce::String& name, Change&& change)
    {
        if (auto* clip = findAudioClip (context, id))
        {
            const UndoStep step (clip->edit, name);
            change (*clip);
        }
    }
} // namespace

std::optional<AudioClipInfo> getAudioClip (const ProjectContext& context, ClipID id)
{
    auto* clip = findAudioClip (context, id);
    const auto shown = getClip (context, id);

    if (clip == nullptr || ! shown.has_value())
        return {};

    AudioClipInfo info;
    info.clip = *shown;

    // The file as it was recorded, not the render that plays it stretched, pitched or reversed.
    const auto original = getClipFile (*clip);
    const auto file = getAudioFileInfo (original);
    info.fileName = original.getFileName();
    info.sampleRate = file.sampleRate;
    info.channels = file.numChannels;
    info.seconds = file.getSeconds();

    const auto& tempo = clip->edit.tempoSequence;
    const auto audio = getAudioTimeRange (*clip);
    info.content = { fromTracktion (tempo.toBeats (audio.getStart())), fromTracktion (tempo.toBeats (audio.getEnd())) };

    info.gain = getClipGain (*clip);
    info.pitch = getClipPitch (*clip);
    info.reversed = isClipReversed (*clip);
    info.tempo = getClipTempo (*clip);
    info.followsTempo = info.tempo > 0.0;

    if (file.sampleRate <= 0.0)
        info.state = AudioState::missing;
    else if (isRendered (*clip) && ! clip->getAudioFile().isValid())
        info.state = AudioState::preparing;

    return info;
}

std::optional<ClipID> getAudioClipNearest (const ProjectContext& context, TrackID id, BeatPosition beat)
{
    auto* track = findTrack (context, id);

    if (track == nullptr)
        return {};

    std::optional<ClipID> nearest;
    auto nearestDistance = std::numeric_limits<double>::max();
    const auto at = toTracktion (beat);

    for (auto* clip : track->getClips())
    {
        if (dynamic_cast<te::AudioClipBase*> (clip) == nullptr)
            continue;

        const auto range = clip->getEditBeatRange();
        const auto distance =
            range.contains (at) ? 0.0 : std::min (std::abs ((range.getStart() - at).inBeats()), std::abs ((range.getEnd() - at).inBeats()));

        if (distance < nearestDistance)
        {
            nearest = getClipID (*clip);
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
        if (auto* track = findTrack (context, tracks.front()))
            return holdsAudio (*track) ? ClipType::audio : ClipType::midi;

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
    changeClip (context, id, "Change Clip Gain", [decibels] (te::AudioClipBase& clip) { setClipGain (clip, decibels); });
}

void setClipPitch (ProjectContext& context, ClipID id, float semitones)
{
    changeClip (context, id, "Change Clip Pitch", [semitones] (te::AudioClipBase& clip) { setClipPitch (clip, semitones); });
}

void setClipReversed (ProjectContext& context, ClipID id, bool reversed)
{
    changeClip (context, id, reversed ? "Reverse Clip" : "Play Clip Forwards",
                [reversed] (te::AudioClipBase& clip) { setClipReversed (clip, reversed); });
}

void setClipFollowsTempo (ProjectContext& context, ClipID id, bool follows)
{
    changeClip (context, id, follows ? "Follow Tempo" : "Stop Following Tempo",
                [follows] (te::AudioClipBase& clip) { setClipFollowsTempo (clip, follows); });
}

void setClipTempo (ProjectContext& context, ClipID id, double bpm)
{
    changeClip (context, id, "Change Clip Tempo", [bpm] (te::AudioClipBase& clip) { setClipTempo (clip, bpm); });
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
    auto* edit = getEdit (context);

    if (renders == nullptr || edit == nullptr || renders->getProgress().renders == 0)
        return clips;

    for (auto* clip : getAudioClips (*edit))
        if (isRendered (*clip))
            if (const auto progress = renders->getProgressOf (clip->getSourceFileReference().getFile()))
                clips[getClipID (*clip)] = *progress;

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
        auto* edit = getEdit (context);
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

        if (edit == nullptr || clips.empty())
            return;

        const UndoStep step (*edit, getUndoName (property));

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

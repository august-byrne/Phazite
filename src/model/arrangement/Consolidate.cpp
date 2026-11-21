// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Consolidate.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Selection.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AudioFiles.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <map>

namespace phazite
{
namespace
{
    bool overlaps (BeatRange a, BeatRange b)
    {
        return a.getStart() < b.getEnd() && b.getStart() < a.getEnd();
    }

    // The clips of a track that play in a range: not muted, and overlapping it.
    std::vector<juce::ValueTree> getPlayingIn (const Song& song, TrackID track, BeatRange range)
    {
        std::vector<juce::ValueTree> playing;

        for (const auto& clipState : getClipStates (findTrackState (song, track)))
            if (! (bool) clipState[schema::clipMute] && overlaps (getClipRange (song, clipState), range))
                playing.push_back (clipState);

        return playing;
    }

    // The name every one of the clips is given, or none.
    juce::String getSharedName (const std::vector<juce::ValueTree>& clips)
    {
        if (clips.empty())
            return {};

        const auto name = clips.front()[schema::clipName].toString();

        for (const auto& clipState : clips)
            if (clipState[schema::clipName].toString() != name)
                return {};

        return name;
    }
} // namespace

std::vector<ConsolidatePart> getConsolidateParts (const Song& song, const Selection& selection, const RangeSelection& rangeSelection)
{
    // Each track's range: the time range selected across tracks, or its selected clips' span.
    std::map<TrackID, BeatRange> ranges;

    if (! rangeSelection.isEmpty())
    {
        for (const auto track : rangeSelection.getTracks())
            ranges[track] = rangeSelection.getRange();
    }
    else
    {
        for (const auto clip : getSelectedClips (selection))
        {
            const auto track = getClipTrack (song, clip);
            const auto clipRange = getClipRange (song, clip);

            if (const auto found = ranges.find (track); found == ranges.end())
                ranges.emplace (track, clipRange);
            else
                found->second = found->second.getUnionWith (clipRange);
        }
    }

    std::vector<ConsolidatePart> parts;

    for (const auto& trackState : getTrackStates (song))
    {
        const auto track = getTrackID (trackState);
        const auto found = ranges.find (track);

        if (found == ranges.end() || found->second.isEmpty() || isFolderState (trackState) || isReturnState (trackState))
            continue;

        ConsolidatePart part { track, found->second };

        for (const auto& clipState : getPlayingIn (song, track, part.range))
            (isAudioState (clipState) ? part.audio : part.midi) = true;

        if (part.audio || part.midi)
            parts.push_back (part);
    }

    return parts;
}

std::vector<ClipID> consolidatePart (Song& song, const ConsolidatePart& part, const juce::File& renderedAudio)
{
    std::vector<ClipID> made;
    const auto playing = getPlayingIn (song, part.track, part.range);
    std::vector<juce::ValueTree> midiClips, audioClips;

    for (const auto& clipState : playing)
        (isAudioState (clipState) ? audioClips : midiClips).push_back (clipState);

    // What the MIDI clips play in the range, before they go: each note as it plays, cut at its clip's and the range's edges,
    // and the controller events (a sustain pedal, the pitch wheel) there, counted from the range's start.
    std::vector<NoteData> notes;
    std::vector<ControlValue> controls; // at their song beats, where the joined clip plays them

    for (const auto& clipState : midiClips)
    {
        const auto clipRange = getClipRange (song, clipState);
        // The range's own function, not a BeatRange made of std::max() and std::min() of what two getters return: the
        // MSVC of CI's Windows release build made that one empty (its end the range's start), and the joined clip lost
        // its notes there and nowhere else (known bug 204).
        const auto played = clipRange.getIntersectionWith (part.range);

        for (auto note : getClipNotes (song, getClipID (clipState)))
        {
            const auto from = std::max (note.start, played.getStart()), to = std::min (note.getEnd(), played.getEnd());

            if (to <= from)
                continue;

            note.start = from;
            note.length = to - from;
            note.id = {};
            notes.push_back (note);
        }

        for (const auto& control : getClipControls (song, getClipID (clipState)))
            if (control.beat >= played.getStart() && control.beat < played.getEnd())
                controls.push_back (control);
    }

    const auto midiName = getSharedName (midiClips), audioName = getSharedName (audioClips);
    const auto allFollow = std::all_of (audioClips.begin(), audioClips.end(), [&song] (const juce::ValueTree& clipState)
                                        { return getClipTempo (song, getClipID (clipState)) > 0.0; });

    // Everything there goes, and the joined clips take its place.
    clearClipsIn (song, part.track, part.range);

    if (part.midi)
    {
        if (const auto clip = insertMidiClip (song, part.track, part.range, nullptr, midiName); clip.isValid())
        {
            for (auto& note : notes)
                note.clip = clip;

            setClipNotes (song, clip, notes);
            addControls (song, clip, controls); // after its notes, counted from its start, which is the range's
            made.push_back (clip);
        }
    }

    if (part.audio && renderedAudio.existsAsFile())
    {
        const auto seconds = getAudioFileInfo (renderedAudio).getSeconds();
        const auto clipState = makeAudioClipState (song, audioName, renderedAudio, part.range.getStart(), { 0.0, seconds });

        if (const auto clip = placeClipState (song, clipState, part.track); clip.isValid())
        {
            // At the tempo it was rendered at, it follows the song's as the clips it joins did.
            if (allFollow && getTempoMap (song).isSteadyBetween (part.range.getStart().inBeats(), part.range.getEnd().inBeats()))
                setClipFollowsTempo (song, clip, true);

            made.push_back (clip);
        }
    }

    return made;
}
} // namespace phazite

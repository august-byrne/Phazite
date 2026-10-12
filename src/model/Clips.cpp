// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Clips.h"
#include "Tracks.h"

#include <algorithm>

namespace phazite
{
namespace
{
    const auto minimumClipLength = te::BeatDuration::fromBeats (1.0 / 64.0);
} // namespace

juce::Array<te::Clip*> getSelectedClips (const te::SelectionManager& selection)
{
    auto clips = selection.getItemsOfType<te::Clip>();
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return {};

    const auto tracks = getPlaylistTracks (*edit);
    std::stable_sort (clips.begin(), clips.end(),
                      [&tracks] (te::Clip* a, te::Clip* b)
                      {
                          const auto rowA = tracks.indexOf (a->getTrack()), rowB = tracks.indexOf (b->getTrack());
                          return rowA != rowB ? rowA < rowB : a->getPosition().getStart() < b->getPosition().getStart();
                      });
    return clips;
}

juce::Colour getClipColour (const te::Clip& clip, juce::Colour trackColour)
{
    return clip.state.hasProperty (te::IDs::colour) ? clip.getColour() : trackColour;
}

juce::Array<te::Clip*> copyClips (const juce::Array<te::Clip*>& clips, bool asAliases)
{
    juce::Array<te::Clip*> copies;

    for (auto* clip : clips)
    {
        // Linked clips share a link group; a copy that isn't an alias leaves its original's.
        if (asAliases && ! clip->isLinked())
            clip->setLinkGroupID (juce::Uuid().toString());

        if (auto copy = te::duplicateClip (*clip))
        {
            if (! asAliases)
                copy->setLinkGroupID ({});

            copies.add (copy.get());
        }
    }

    return copies;
}

void moveClip (te::Clip& clip, te::BeatPosition start, te::ClipTrack& track)
{
    const auto time = clip.edit.tempoSequence.toTime (te::BeatPosition::fromBeats (std::max (0.0, start.inBeats())));

    if (clip.getClipTrack() != &track)
        clip.moveTo (track);

    clip.setStart (time, false, true);
}

void trimClipStart (te::Clip& clip, te::BeatPosition beat)
{
    const auto& tempo = clip.edit.tempoSequence;
    const auto earliest = std::max (te::TimePosition(), clip.getPosition().getStartOfSource());
    const auto latest = tempo.toTime (clip.getEndBeat() - minimumClipLength);

    if (latest < earliest)
        return;

    clip.setStart (juce::jlimit (earliest, latest, tempo.toTime (beat)), true, false);
}

void trimClipEnd (te::Clip& clip, te::BeatPosition beat)
{
    const auto& tempo = clip.edit.tempoSequence;
    const auto earliest = tempo.toTime (clip.getStartBeat() + minimumClipLength);
    const auto latest = clip.getPosition().getStart() + clip.getMaximumLength();

    if (latest < earliest)
        return;

    clip.setEnd (juce::jlimit (earliest, latest, tempo.toTime (beat)), true);
}

void setClipFade (te::AudioClipBase& clip, bool fadeIn, te::TimeDuration length)
{
    const auto limited = juce::jlimit (te::TimeDuration(), clip.getPosition().getLength(), length);

    if (fadeIn)
        clip.setFadeIn (limited);
    else
        clip.setFadeOut (limited);
}

te::MidiClip* insertMidiClip (te::ClipTrack& track, te::BeatRange range, te::SelectionManager* selection)
{
    const auto clip = track.insertMIDIClip (track.getName(), track.edit.tempoSequence.toTime (range), selection);
    return clip.get();
}
} // namespace phazite

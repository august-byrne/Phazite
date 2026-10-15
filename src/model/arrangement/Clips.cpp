// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Clips.h"
#include "Tracks.h"
#include "model/base/Toggles.h"

#include <algorithm>
#include <vector>

namespace phazite
{
namespace
{
    const auto minimumClipLength = te::BeatDuration::fromBeats (1.0 / 64.0);

    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);

    // A copy of the part of a clip that lies in a range, where that part is. The copy is cut at the range's edges as
    // Split cuts clips (an audio clip loses its fades where it is cut); the clip itself stays whole.
    te::Clip* copyPartOfClip (te::Clip& clip, te::BeatRange range)
    {
        const auto copies = copyClips ({ &clip }, false);

        if (copies.isEmpty())
            return nullptr;

        auto* part = copies.getFirst();

        if (const auto rest = splitClips ({ part }, range.getStart()); ! rest.isEmpty())
        {
            part->removeFromParent(); // what lies before the range
            part = rest.getFirst();
        }

        for (auto* after : splitClips ({ part }, range.getEnd()))
            after->removeFromParent();

        return part;
    }
} // namespace

juce::Array<te::Clip*> getSelectedClips (const te::SelectionManager& selection)
{
    auto clips = selection.getItemsOfType<te::Clip>();
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return {};

    // Sections are arranger clips, with edits of their own (Sections.h).
    clips.removeIf ([] (te::Clip* clip) { return dynamic_cast<te::ArrangerClip*> (clip) != nullptr; });

    const auto tracks = getPlaylistTracks (*edit);
    std::stable_sort (clips.begin(), clips.end(),
                      [&tracks] (te::Clip* a, te::Clip* b)
                      {
                          const auto rowA = tracks.indexOf (a->getTrack()), rowB = tracks.indexOf (b->getTrack());
                          return rowA != rowB ? rowA < rowB : a->getPosition().getStart() < b->getPosition().getStart();
                      });
    return clips;
}

bool hasSelectedClips (const te::SelectionManager& selection)
{
    return ! getSelectedClips (selection).isEmpty();
}

bool hasAliases (const juce::Array<te::Clip*>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return clip->isLinked(); });
}

bool hasAudioClips (const juce::Array<te::Clip*>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return dynamic_cast<te::AudioClipBase*> (clip) != nullptr; });
}

bool hasFadeShape (const juce::Array<te::Clip*>& clips, te::AudioFadeCurve::Type type)
{
    auto any = false;

    for (auto* clip : clips)
    {
        if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
        {
            if (audio->getFadeInType() != type || audio->getFadeOutType() != type)
                return false;

            any = true;
        }
    }

    return any;
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

void setCrossfade (te::AudioClipBase& first, te::AudioClipBase& second, te::TimePosition boundary, te::TimeDuration length)
{
    // How far each can reach over the boundary: the first as far as its audio goes on, the second as far
    // back as its audio starts.
    const auto firstReach = first.getPosition().getStart() + first.getMaximumLength() - boundary;
    const auto secondReach = boundary - std::max (te::TimePosition(), second.getPosition().getStartOfSource());
    const auto half = std::max (te::TimeDuration(), std::min ({ length / 2.0, firstReach, secondReach }));

    // Tracktion plays both where they overlap; the fades make that a crossfade.
    first.setEnd (boundary + half, true);
    second.setStart (boundary - half, true, false);
    first.setFadeOut (half * 2.0);
    second.setFadeIn (half * 2.0);
}

void setFadeShape (const juce::Array<te::Clip*>& clips, te::AudioFadeCurve::Type shape)
{
    for (auto* clip : clips)
    {
        if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
        {
            audio->setFadeInType (shape);
            audio->setFadeOutType (shape);
        }
    }
}

te::MidiClip* insertMidiClip (te::ClipTrack& track, te::BeatRange range, te::SelectionManager* selection)
{
    const auto clip = track.insertMIDIClip (track.getName(), track.edit.tempoSequence.toTime (range), selection);
    return clip.get();
}

//==============================================================================
juce::Array<te::Clip*> splitClips (const juce::Array<te::Clip*>& clips, te::BeatPosition beat)
{
    juce::Array<te::Clip*> parts;

    for (auto* clip : clips)
    {
        const auto time = clip->edit.tempoSequence.toTime (beat);
        const auto range = clip->getEditTimeRange();

        if (time > range.getStart() && time < range.getEnd())
            if (auto* track = clip->getClipTrack())
                if (auto* part = track->splitClip (*clip, time))
                    parts.add (part);
    }

    return parts;
}

void splitClipsAtEdges (const juce::Array<te::Track*>& tracks, te::BeatRange range)
{
    juce::Array<te::Clip*> clips;

    for (auto* track : tracks)
        if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
            clips.addArray (clipTrack->getClips());

    // The right-hand parts of the first split can reach past the end too.
    const auto parts = splitClips (clips, range.getStart());
    auto all = clips;
    all.addArray (parts);
    splitClips (all, range.getEnd());
}

juce::Array<te::Clip*> duplicateClips (const juce::Array<te::Clip*>& clips, bool asAliases, te::SelectionManager* selection)
{
    if (clips.isEmpty())
        return {};

    auto start = clips.getFirst()->getStartBeat(), end = clips.getFirst()->getEndBeat();

    for (auto* clip : clips)
    {
        start = std::min (start, clip->getStartBeat());
        end = std::max (end, clip->getEndBeat());
    }

    if (selection != nullptr)
        selection->deselectAll();

    const auto copies = copyClips (clips, asAliases); // each starts where its original is

    for (auto* copy : copies)
    {
        if (auto* track = copy->getClipTrack())
            moveClip (*copy, copy->getStartBeat() + (end - start), *track);

        if (selection != nullptr)
            selection->addToSelection (copy);
    }

    return copies;
}

void makeClipsUnique (const juce::Array<te::Clip*>& clips)
{
    for (auto* clip : clips)
    {
        const auto group = clip->getLinkGroupID();

        if (group.isEmpty())
            continue;

        clip->setLinkGroupID ({});

        if (const auto rest = clip->edit.findClipsInLinkGroup (group); rest.size() == 1)
            rest.getFirst()->setLinkGroupID ({});
    }
}

bool areAllClipsMuted (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (clips, [] (te::Clip* clip) { return clip->isMuted(); });
}

void toggleClipsMuted (const juce::Array<te::Clip*>& clips)
{
    toggleAll (clips, [] (te::Clip* clip) { return clip->isMuted(); }, [] (te::Clip* clip, bool on) { clip->setMuted (on); });
}

void nudgeClips (const juce::Array<te::Clip*>& clips, te::BeatDuration amount)
{
    // The earliest clip decides how far back they can go.
    for (auto* clip : clips)
        amount = std::max (amount, te::BeatPosition() - clip->getStartBeat());

    for (auto* clip : clips)
        if (auto* track = clip->getClipTrack())
            moveClip (*clip, clip->getStartBeat() + amount, *track);
}

void deleteClips (const juce::Array<te::Clip*>& clips, bool closeGap)
{
    // Latest first, so that closing one gap doesn't move a clip still to be deleted.
    auto sorted = clips;
    std::sort (sorted.begin(), sorted.end(),
               [] (te::Clip* a, te::Clip* b) { return a->getPosition().getStart() > b->getPosition().getStart(); });

    for (auto* clip : sorted)
    {
        const auto removed = clip->getPosition();
        auto* track = clip->getClipTrack();
        clip->removeFromParent();

        if (closeGap && track != nullptr)
            for (auto* other : juce::Array<te::Clip*> (track->getClips())) // a copy: moving clips can reorder the list
                if (other->getPosition().getStart() >= removed.getStart())
                    other->setStart (std::max (removed.getStart(), other->getPosition().getStart() - removed.getLength()), false, true);
    }
}

void deleteTimeRange (te::Edit& edit, te::BeatRange range, const juce::Array<te::Track*>& tracks, bool closeGap)
{
    const auto time = edit.tempoSequence.toTime (range);

    for (auto* track : tracks)
    {
        auto* clipTrack = dynamic_cast<te::ClipTrack*> (track);

        if (clipTrack == nullptr)
            continue;

        for (auto* clip : juce::Array<te::Clip*> (clipTrack->getClips())) // a copy: deleting changes the list
            if (clip->getEditTimeRange().overlaps (time))
                te::deleteRegionOfClip (*clip, time);

        if (closeGap)
            for (auto* clip : juce::Array<te::Clip*> (clipTrack->getClips()))
                if (clip->getPosition().getStart() >= time.getEnd())
                    clip->setStart (clip->getPosition().getStart() - time.getLength(), false, true);
    }
}

te::BeatRange duplicateTimeRange (te::Edit& edit, te::BeatRange range, const juce::Array<te::Track*>& tracks)
{
    const auto length = range.getLength();
    const te::BeatRange copyRange (range.getEnd(), range.getEnd() + length);

    if (length <= tinyBeats)
        return copyRange;

    // The part of each clip that lies in the range, copied where it is, before anything moves.
    std::vector<te::Clip::Ptr> parts;

    for (auto* track : tracks)
        if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
            for (auto* clip : juce::Array<te::Clip*> (clipTrack->getClips())) // a copy: the parts join the list
                if (clip->getStartBeat() < range.getEnd() - tinyBeats && clip->getEndBeat() > range.getStart() + tinyBeats)
                    if (auto* part = copyPartOfClip (*clip, range))
                        parts.emplace_back (part);

    // Each part goes a range's length later, in place of what lies there. What is in the way goes first, so that no
    // part cuts another.
    for (const auto& part : parts)
        if (auto* track = part->getTrack())
            deleteTimeRange (edit, { part->getStartBeat() + length, part->getEndBeat() + length }, { track }, false);

    for (const auto& part : parts)
        if (auto* track = part->getClipTrack())
            moveClip (*part, part->getStartBeat() + length, *track);

    return copyRange;
}
} // namespace phazite

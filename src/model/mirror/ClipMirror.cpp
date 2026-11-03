// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Bridge.h"

#include <set>
#include <unordered_map>

namespace phazite::mirror
{
namespace
{
    // Where a clip of the song is in Tracktion's time: from its start to its end, with its content starting its offset
    // before it.
    te::ClipPosition getPosition (const te::Edit& edit, const juce::ValueTree& clipState)
    {
        const auto& tempo = edit.tempoSequence;
        const auto range = getClipRange (clipState);
        const auto start = tempo.toTime (toTracktion (range.getStart()));
        const auto contentStart = tempo.toTime (toTracktion (getContentStart (clipState)));
        return { { start, tempo.toTime (toTracktion (range.getEnd())) }, start - contentStart };
    }

    // Tracktion's notes of a clip as the song's: each with its ID, where it is, its pitch, velocity and mute. Tracktion's
    // that the song's clip hasn't go, and so do any without an ID of the song's, or with one another note has.
    void reconcileNotes (te::MidiClip& clip, const juce::ValueTree& clipState)
    {
        auto* undo = &clip.edit.getUndoManager();
        auto& sequence = clip.getSequence();
        std::unordered_map<juce::int64, te::MidiNote*> byID;
        juce::Array<te::MidiNote*> gone;

        for (auto* note : sequence.getNotes())
            if (const auto id = (juce::int64) note->state[schema::phaziteNoteID]; id <= 0 || ! byID.emplace (id, note).second)
                gone.add (note);

        std::set<juce::int64> kept;

        for (const auto& noteState : clipState)
        {
            if (! noteState.hasType (schema::NOTE))
                continue;

            const auto id = (juce::int64) noteState[schema::noteID];
            const auto pitch = (int) noteState[schema::notePitch];
            const auto start = te::BeatPosition::fromBeats ((double) noteState[schema::noteStart]);
            const auto length = te::BeatDuration::fromBeats ((double) noteState[schema::noteLength]);
            const auto velocity = (int) noteState[schema::noteVelocity];
            kept.insert (id);
            te::MidiNote* note = nullptr;

            if (const auto found = byID.find (id); found != byID.end())
            {
                note = found->second;

                if (note->getNoteNumber() != pitch)
                    note->setNoteNumber (pitch, undo);

                if (note->getStartBeat() != start || note->getLengthBeats() != length)
                    note->setStartAndLength (start, length, undo);

                if (note->getVelocity() != velocity)
                    note->setVelocity (velocity, undo);
            }
            else
            {
                note = sequence.addNote (pitch, start, length, velocity, 0, undo);

                if (note == nullptr)
                    continue;

                note->state.setProperty (schema::phaziteNoteID, id, undo);
            }

            if (note->isMute() != (bool) noteState[schema::noteMute])
                note->setMute (noteState[schema::noteMute], undo);

            // Kept on Tracktion's note for the model's reading of it (getNoteData()); Tracktion makes nothing of them.
            for (const auto& flag : { schema::noteSlide, schema::notePortamento })
                if ((bool) note->state[flag] != (bool) noteState[flag])
                    note->state.setProperty (flag, (bool) noteState[flag], undo);
        }

        for (const auto& [id, note] : byID)
            if (! kept.contains (id))
                gone.add (note);

        for (auto* note : gone)
            sequence.removeNote (*note, undo);
    }

    // Tracktion's controller events of a clip as the song's, all of them again where they differ.
    void reconcileControls (te::MidiClip& clip, const juce::ValueTree& clipState)
    {
        if (haveSameControls (clip, clipState))
            return;

        auto* undo = &clip.edit.getUndoManager();
        auto& sequence = clip.getSequence();
        sequence.removeAllControllers (undo);

        for (const auto& child : clipState)
        {
            if (child.hasType (schema::CONTROL))
            {
                const auto event = toTracktionControl (child);
                sequence.addControllerEvent (event.beat, event.type, event.value, event.metadata, undo);
            }
        }
    }

    // A new clip's state for Tracktion, of a type, where it plays.
    juce::ValueTree makeTracktionClip (const juce::Identifier& type, te::EditItemID id, const juce::String& name, te::ClipPosition position)
    {
        juce::ValueTree created (type);
        created.setProperty (te::IDs::name, name, nullptr);
        created.setProperty (te::IDs::start, position.getStart().inSeconds(), nullptr);
        created.setProperty (te::IDs::length, position.getLength().inSeconds(), nullptr);
        created.setProperty (te::IDs::offset, position.getOffset().inSeconds(), nullptr);
        id.writeID (created, nullptr);
        return created;
    }

    // What every clip of Tracktion's gets from the song's: its track and place, its name and mute; in no link group of
    // Tracktion's (the song keeps its aliases itself), and not looping, which is none of Phazite's.
    void reconcilePlace (te::Clip& clip, const juce::ValueTree& clipState, te::ClipTrack& track, te::ClipPosition position)
    {
        if (clip.getClipTrack() != &track)
            clip.moveTo (track);

        if (const auto now = clip.getPosition(); now.time != position.time || now.offset != position.offset)
            clip.setPosition (position);

        if (const auto name = clipState[schema::clipName].toString(); clip.getName() != name)
            clip.setName (name);

        if (clip.isMuted() != (bool) clipState[schema::clipMute])
            clip.setMuted (clipState[schema::clipMute]);

        if (clip.isLinked())
            clip.setLinkGroupID ({});

        if (clip.isLooping())
            clip.disableLooping();
    }

    void reconcileMidiClip (te::Edit& edit, const juce::ValueTree& clipState, te::ClipTrack& track)
    {
        const auto id = toTracktion (getClipID (clipState));
        const auto position = getPosition (edit, clipState);
        auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (edit, id));

        if (clip == nullptr)
        {
            // As Tracktion's insertMIDIClip() makes one.
            if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (&track); audioTrack != nullptr && ! audioTrack->containsAnyMIDIClips())
                audioTrack->setVerticalScaleToDefault();

            const auto created = makeTracktionClip (te::IDs::MIDICLIP, id, clipState[schema::clipName].toString(), position);
            clip = dynamic_cast<te::MidiClip*> (te::insertClipWithState (track, created));

            if (clip == nullptr)
                return;
        }

        reconcilePlace (*clip, clipState, track, position);
        reconcileNotes (*clip, clipState);
        reconcileControls (*clip, clipState);
    }

    // The song's fades of an audio clip as Tracktion plays them: scaled down together where they would overrun it, as
    // Tracktion's own getters give them, so that Tracktion never scales its own outside the undo history.
    std::pair<double, double> getFittedFades (const juce::ValueTree& clipState, double length)
    {
        auto in = juce::jlimit (0.0, length, getClipFade (clipState, true));
        auto out = juce::jlimit (0.0, length, getClipFade (clipState, false));

        if (in + out > length && in + out > 0.0)
        {
            const auto scale = length / (in + out);
            in *= scale;
            out *= scale;
        }

        return { in, out };
    }

    void reconcileAudioClip (te::Edit& edit, const juce::ValueTree& clipState, te::ClipTrack& track)
    {
        const auto id = toTracktion (getClipID (clipState));
        const auto position = getPlayedPosition (edit, clipState);
        const auto played = getPlayedFile (edit, clipState);
        auto* clip = dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, id));

        if (clip == nullptr)
        {
            auto created = makeTracktionClip (te::IDs::AUDIOCLIP, id, clipState[schema::clipName].toString(), position);
            created.setProperty (te::IDs::source, te::SourceFileReference::findPathFromFile (edit, played, true), nullptr);
            clip = dynamic_cast<te::AudioClipBase*> (te::insertClipWithState (track, created));

            if (clip == nullptr)
                return;
        }

        if (auto& source = clip->getSourceFileReference(); source.getFile() != played)
            source.source = te::SourceFileReference::findPathFromFile (edit, played, true);

        reconcilePlace (*clip, clipState, track, position);
        auto* undo = &edit.getUndoManager();

        if (const auto gain = (float) clipState.getProperty (schema::clipGain, 0.0f); ! juce::exactlyEqual (clip->getGainDB(), gain))
            clip->setGainDB (gain);

        // Written as they are: they fit, so Tracktion's setters, which fit each against the other's old length, aren't needed.
        const auto [fadeIn, fadeOut] = getFittedFades (clipState, position.getLength().inSeconds());

        if (! juce::exactlyEqual ((double) clip->state.getProperty (te::IDs::fadeIn, 0.0), fadeIn))
            clip->state.setProperty (te::IDs::fadeIn, fadeIn, undo);

        if (! juce::exactlyEqual ((double) clip->state.getProperty (te::IDs::fadeOut, 0.0), fadeOut))
            clip->state.setProperty (te::IDs::fadeOut, fadeOut, undo);

        if (const auto shape = getNearestFadeType (getClipFadeCurve (clipState, true)); clip->getFadeInType() != shape)
            clip->setFadeInType (shape);

        if (const auto shape = getNearestFadeType (getClipFadeCurve (clipState, false)); clip->getFadeOutType() != shape)
            clip->setFadeOutType (shape);
    }

    void reconcileClip (te::Edit& edit, const juce::ValueTree& clipState, te::ClipTrack& track)
    {
        if (isAudioState (clipState))
            reconcileAudioClip (edit, clipState, track);
        else
            reconcileMidiClip (edit, clipState, track);
    }

    te::ClipTrack* findTrackOf (te::Edit& edit, const juce::ValueTree& clipState)
    {
        return dynamic_cast<te::ClipTrack*> (findTrack (edit, clipState.getParent()));
    }

    // A track's elements in the order Tracktion keeps them (ClipOwner's sortClips()): the rest first, then its automation,
    // its clips by where they start, its devices, its output and its LFOs. Put so in the change's step: Tracktion would
    // sort clips added or moved a moment later, as an undo step of its own (an Edit built from the song would have one
    // as the project opens).
    void putInOrder (te::ClipTrack& track)
    {
        struct Order
        {
            static int getPriority (const juce::ValueTree& element)
            {
                if (element.hasType (te::IDs::AUTOMATIONTRACK))
                    return 0;

                if (te::Clip::isClipState (element))
                    return 1;

                if (element.hasType (te::IDs::PLUGIN))
                    return 2;

                if (element.hasType (te::IDs::OUTPUTDEVICES))
                    return 3;

                return element.hasType (te::IDs::LFOS) ? 4 : -1;
            }

            int compareElements (const juce::ValueTree& first, const juce::ValueTree& second) const noexcept
            {
                if (const auto priority = getPriority (first) - getPriority (second); priority != 0)
                    return priority;

                if (te::Clip::isClipState (first) && te::Clip::isClipState (second))
                {
                    const auto a = (double) first[te::IDs::start], b = (double) second[te::IDs::start];
                    return a < b ? -1 : (b < a ? 1 : 0);
                }

                return 0;
            }
        };

        Order order;
        track.state.sort (order, &track.edit.getUndoManager(), true);
    }
} // namespace

void reconcileClips (te::Edit& edit)
{
    if (! findTrackList (edit).isValid())
        return;

    std::set<te::EditItemID> kept;

    for (const auto& clipState : getClipStates (edit))
    {
        kept.insert (toTracktion (getClipID (clipState)));

        if (auto* track = findTrackOf (edit, clipState))
            reconcileClip (edit, clipState, *track);
    }

    // Tracktion's clips the song hasn't go.
    juce::Array<te::Clip::Ptr> gone;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (! kept.contains (clip->itemID))
                gone.add (clip);

    for (auto& clip : gone)
        clip->removeFromParent();

    for (auto* track : te::getAudioTracks (edit))
        putInOrder (*track);
}

void reconcileClip (te::Edit& edit, ClipID id)
{
    if (const auto clipState = findClipState (edit, id); clipState.isValid())
    {
        if (auto* track = findTrackOf (edit, clipState))
        {
            reconcileClip (edit, clipState, *track);
            putInOrder (*track);
        }
    }
    else if (auto* clip = te::findClipForID (edit, toTracktion (id)))
    {
        clip->removeFromParent();
    }
}
} // namespace phazite::mirror

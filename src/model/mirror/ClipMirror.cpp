// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"

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

    void reconcileClip (te::Edit& edit, const juce::ValueTree& clipState, te::ClipTrack& track)
    {
        const auto id = toTracktion (getClipID (clipState));
        const auto position = getPosition (edit, clipState);
        const auto name = clipState[schema::clipName].toString();
        auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (edit, id));

        if (clip == nullptr)
        {
            // As Tracktion's insertMIDIClip() makes one.
            if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (&track); audioTrack != nullptr && ! audioTrack->containsAnyMIDIClips())
                audioTrack->setVerticalScaleToDefault();

            juce::ValueTree created (te::IDs::MIDICLIP);
            created.setProperty (te::IDs::name, name, nullptr);
            created.setProperty (te::IDs::start, position.getStart().inSeconds(), nullptr);
            created.setProperty (te::IDs::length, position.getLength().inSeconds(), nullptr);
            created.setProperty (te::IDs::offset, position.getOffset().inSeconds(), nullptr);
            id.writeID (created, nullptr);
            clip = dynamic_cast<te::MidiClip*> (te::insertClipWithState (track, created));

            if (clip == nullptr)
                return;
        }
        else if (clip->getClipTrack() != &track)
        {
            clip->moveTo (track);
        }

        if (const auto now = clip->getPosition(); now.time != position.time || now.offset != position.offset)
            clip->setPosition (position);

        if (clip->getName() != name)
            clip->setName (name);

        if (clip->isMuted() != (bool) clipState[schema::clipMute])
            clip->setMuted (clipState[schema::clipMute]);

        if (clip->isLinked())
            clip->setLinkGroupID ({});

        // A clip of the song's plays its notes once: looping is none of Phazite's.
        if (clip->isLooping())
            clip->disableLooping();

        reconcileNotes (*clip, clipState);
        reconcileControls (*clip, clipState);
    }

    te::ClipTrack* findTrackOf (te::Edit& edit, const juce::ValueTree& clipState)
    {
        return dynamic_cast<te::ClipTrack*> (te::findTrackForID (edit, toTracktion (getTrackID (clipState.getParent()))));
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

    // Tracktion's MIDI clips the song hasn't go.
    juce::Array<te::Clip::Ptr> gone;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (dynamic_cast<te::MidiClip*> (clip) != nullptr && ! kept.contains (clip->itemID))
                gone.add (clip);

    for (auto& clip : gone)
        clip->removeFromParent();
}

void reconcileClip (te::Edit& edit, ClipID id)
{
    if (const auto clipState = findClipState (edit, id); clipState.isValid())
    {
        if (auto* track = findTrackOf (edit, clipState))
            reconcileClip (edit, clipState, *track);
    }
    else if (auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (edit, toTracktion (id))))
    {
        clip->removeFromParent();
    }
}
} // namespace phazite::mirror

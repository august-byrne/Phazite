// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NotesModel.h"
#include "internal/Notes.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <unordered_map>

namespace phazite
{
namespace
{
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::AudioTrack* findAudioTrack (const ProjectContext& context, TrackID id)
    {
        return dynamic_cast<te::AudioTrack*> (findTrack (context, id));
    }

    te::MidiClip* findMidiClip (const ProjectContext& context, ClipID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, toTracktion (id))) : nullptr;
    }

    juce::Array<te::MidiClip*> findMidiClips (const ProjectContext& context, const std::vector<ClipID>& ids)
    {
        juce::Array<te::MidiClip*> clips;

        for (const auto id : ids)
            if (auto* clip = findMidiClip (context, id))
                clips.add (clip);

        return clips;
    }

    // The notes named, found once per clip: each clip's notes by ID.
    std::vector<ClipNote> findNotes (const ProjectContext& context, const std::vector<NoteRef>& refs)
    {
        std::vector<ClipNote> notes;
        std::unordered_map<ClipID, std::unordered_map<NoteID, te::MidiNote*>> byClip;

        for (const auto& ref : refs)
        {
            auto* clip = findMidiClip (context, ref.clip);

            if (clip == nullptr)
                continue;

            auto [found, isNew] = byClip.try_emplace (ref.clip);

            if (isNew)
                for (auto* note : clip->getSequence().getNotes())
                    found->second.emplace (getNoteID (*note), note);

            if (const auto note = found->second.find (ref.note); note != found->second.end())
                notes.push_back ({ clip, note->second });
        }

        return notes;
    }

    std::vector<NoteRef> refsOf (const std::vector<ClipNote>& notes)
    {
        std::vector<NoteRef> refs;

        for (const auto& n : notes)
            refs.push_back (getNoteRef (*n.clip, *n.note));

        return refs;
    }

    NoteClip makeNoteClip (const te::MidiClip& clip)
    {
        auto* track = clip.getTrack();
        const auto row = track != nullptr ? getPlaylistTracks (clip.edit).indexOf (track) : 0;

        return { getClipID (clip),
                 track != nullptr ? getTrackID (*track) : TrackID(),
                 clip.getName(),
                 fromTracktion (clip.getEditBeatRange()),
                 fromTracktion (clip.getContentStartBeat()),
                 track != nullptr ? getTrackColour (*track, row) : juce::Colours::grey };
    }
} // namespace

//==============================================================================
std::vector<NoteClip> getNoteClips (const ProjectContext& context, const std::vector<ClipID>& ids)
{
    std::vector<NoteClip> clips;

    for (const auto id : ids)
        if (auto* clip = findMidiClip (context, id))
            clips.push_back (makeNoteClip (*clip));

    return clips;
}

std::optional<NoteClip> getNoteClip (const ProjectContext& context, ClipID id)
{
    if (auto* clip = findMidiClip (context, id))
        return makeNoteClip (*clip);

    return {};
}

bool canHoldNotes (const ProjectContext& context, TrackID id)
{
    return findAudioTrack (context, id) != nullptr;
}

bool hasAudioClips (const ProjectContext& context, TrackID id)
{
    if (auto* track = findAudioTrack (context, id))
        for (auto* clip : track->getClips())
            if (dynamic_cast<te::AudioClipBase*> (clip) != nullptr)
                return true;

    return false;
}

std::optional<ClipID> getMidiClipNearest (const ProjectContext& context, TrackID id, BeatPosition beat)
{
    auto* track = findAudioTrack (context, id);

    if (track == nullptr)
        return {};

    std::optional<ClipID> nearest;
    auto nearestDistance = std::numeric_limits<double>::max();
    const auto at = toTracktion (beat);

    for (auto* clip : track->getClips())
    {
        if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
        {
            const auto range = midi->getEditBeatRange();
            const auto distance = range.contains (at)
                                      ? 0.0
                                      : std::min (std::abs ((range.getStart() - at).inBeats()), std::abs ((range.getEnd() - at).inBeats()));

            if (distance < nearestDistance)
            {
                nearest = getClipID (*midi);
                nearestDistance = distance;
            }
        }
    }

    return nearest;
}

ClipID addClipForNotes (ProjectContext& context, TrackID id, BeatPosition start)
{
    auto* track = findAudioTrack (context, id);
    auto* clip = track != nullptr ? insertBarLongMidiClip (*track, toTracktion (start), nullptr) : nullptr;
    return clip != nullptr ? getClipID (*clip) : ClipID();
}

std::optional<double> getLinkedPlayBeat (const ProjectContext& context, const std::vector<ClipID>& ids, double songBeat)
{
    return getLinkedPlayBeat (findMidiClips (context, ids), songBeat);
}

//==============================================================================
std::vector<NoteData> getNotes (const ProjectContext& context, const std::vector<ClipID>& ids)
{
    std::vector<NoteData> notes;

    for (auto* clip : findMidiClips (context, ids))
        for (auto* note : clip->getSequence().getNotes())
            notes.push_back (getNoteData (*clip, *note));

    return notes;
}

std::optional<NoteData> getNote (const ProjectContext& context, NoteRef ref)
{
    auto* clip = findMidiClip (context, ref.clip);
    auto* note = clip != nullptr ? findNote (*clip, ref.note) : nullptr;

    if (note == nullptr)
        return {};

    return getNoteData (*clip, *note);
}

std::vector<NoteData> getNotesOfOtherTracks (const ProjectContext& context, const std::vector<TrackID>& shown, BeatRange range)
{
    std::vector<NoteData> notes;
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return notes;

    const auto visible = toTracktion (range);

    for (auto* track : te::getAudioTracks (*edit))
    {
        if (std::find (shown.begin(), shown.end(), getTrackID (*track)) != shown.end())
            continue;

        for (auto* clip : track->getClips())
            if (auto* midi = dynamic_cast<te::MidiClip*> (clip); midi != nullptr && midi->getEditBeatRange().overlaps (visible))
                for (auto* note : midi->getSequence().getNotes())
                    notes.push_back (getNoteData (*midi, *note));
    }

    return notes;
}

NoteRef addNote (ProjectContext& context, ClipID id, int pitch, BeatPosition start, BeatDuration length, int velocity)
{
    auto* clip = findMidiClip (context, id);
    auto* note = clip != nullptr ? addNote (*clip, pitch, toTracktion (start), toTracktion (length), velocity) : nullptr;
    return note != nullptr ? getNoteRef (*clip, *note) : NoteRef();
}

void moveNote (ProjectContext& context, NoteRef ref, BeatPosition start, int pitch)
{
    for (const auto& n : findNotes (context, { ref }))
        moveNote (*n.clip, *n.note, toTracktion (start), pitch);
}

void setNoteLength (ProjectContext& context, NoteRef ref, BeatDuration length)
{
    for (const auto& n : findNotes (context, { ref }))
        setNoteLength (*n.clip, *n.note, toTracktion (length));
}

void deleteNotes (ProjectContext& context, const std::vector<NoteRef>& refs)
{
    deleteNotes (findNotes (context, refs));
}

void toggleNotesMuted (ProjectContext& context, const std::vector<NoteRef>& refs)
{
    toggleNotesMuted (findNotes (context, refs));
}

void setNoteVelocities (ProjectContext& context, const std::vector<NoteRef>& refs, int velocity)
{
    setNoteVelocities (findNotes (context, refs), velocity);
}

void nudgeNotes (ProjectContext& context, const std::vector<NoteRef>& refs, BeatDuration amount)
{
    if (const auto notes = findNotes (context, refs); ! notes.empty())
        nudgeNotes (notes, toTracktion (amount));
}

void setNotes (ProjectContext& context, const std::vector<NoteData>& notes)
{
    std::vector<NoteRef> refs;

    for (const auto& note : notes)
        refs.push_back (note.getRef());

    // Found once, in the order given (those that are gone left out), then each set as given.
    auto data = notes.begin();

    for (const auto& n : findNotes (context, refs))
    {
        const auto ref = getNoteRef (*n.clip, *n.note);

        while (data != notes.end() && data->getRef() != ref)
            ++data;

        if (data == notes.end())
            break;

        moveNote (*n.clip, *n.note, toTracktion (data->start), data->pitch);
        setNoteLength (*n.clip, *n.note, toTracktion (data->length));
        ++data;
    }
}

std::vector<NoteRef> replaceNotes (ProjectContext& context, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? refsOf (replaceNotes (*edit, findNotes (context, old), replacements)) : std::vector<NoteRef>();
}

std::vector<NoteRef> replaceNotes (ProjectContext& context, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements,
                                   const juce::String& undoName)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? refsOf (replaceNotes (*edit, findNotes (context, old), replacements, undoName)) : std::vector<NoteRef>();
}

void changeNoteVelocities (ProjectContext& context, const std::vector<NoteRef>& refs, int change)
{
    changeNoteVelocities (findNotes (context, refs), change);
}

//==============================================================================
std::optional<Key> getTrackKey (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    return track != nullptr ? getTrackKey (*track) : std::nullopt;
}

void chooseTrackKey (ProjectContext& context, TrackID id, const std::optional<Key>& key)
{
    if (auto* track = findTrack (context, id))
        chooseTrackKey (*track, key);
}

void useKeyOnEveryTrack (ProjectContext& context, const std::optional<Key>& key)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return;

    const UndoStep step (*edit, "Use This Key on Every Track");
    setTrackKeys (getPlaylistTracks (*edit), key);
}

//==============================================================================
bool isDrumTrack (const ProjectContext& context, TrackID id)
{
    auto* track = findAudioTrack (context, id);
    return track != nullptr && isDrumTrack (*track);
}

void setDrumTrack (ProjectContext& context, TrackID id, bool on)
{
    if (auto* track = findAudioTrack (context, id))
        setDrumTrack (*track, on);
}

std::optional<std::vector<DrumRow>> getSamplerSounds (const ProjectContext& context, TrackID id)
{
    auto* track = findAudioTrack (context, id);
    return track != nullptr ? getSamplerSounds (*track) : std::nullopt;
}

std::vector<int> getNoteNumbersPlayed (const ProjectContext& context, const std::vector<ClipID>& ids)
{
    return getNoteNumbersPlayed (findMidiClips (context, ids));
}

std::vector<int> getDrumRowOrder (const ProjectContext& context, TrackID id)
{
    auto* track = findAudioTrack (context, id);
    return track != nullptr ? getDrumRowOrder (*track) : std::vector<int>();
}

void setDrumRowOrder (ProjectContext& context, TrackID id, const std::vector<int>& notes)
{
    if (auto* track = findAudioTrack (context, id))
        setDrumRowOrder (*track, notes);
}

std::vector<int> getHiddenDrumRows (const ProjectContext& context, TrackID id)
{
    auto* track = findAudioTrack (context, id);
    return track != nullptr ? getHiddenDrumRows (*track) : std::vector<int>();
}

void hideDrumRow (ProjectContext& context, TrackID id, int note)
{
    if (auto* track = findAudioTrack (context, id))
        hideDrumRow (*track, note);
}

void showAllDrumRows (ProjectContext& context, TrackID id)
{
    if (auto* track = findAudioTrack (context, id))
        showAllDrumRows (*track);
}

//==============================================================================
TrackID auditionNotes (ProjectContext& context, ClipID id, const std::vector<int>& pitches, int velocity)
{
    auto* clip = findMidiClip (context, id);
    auto* track = clip != nullptr ? dynamic_cast<te::AudioTrack*> (clip->getTrack()) : nullptr;

    if (track == nullptr || ! hasInstrument (*track))
        return {};

    // Guide notes go into the track as live MIDI does, so the playback graph has to be running.
    clip->edit.getTransport().ensureContextAllocated();
    const auto channel = clip->getMidiChannel().isValid() ? clip->getMidiChannel() : te::MidiChannel (1);

    for (const auto pitch : pitches)
        track->playGuideNote (pitch, channel, juce::jlimit (1, 127, velocity), false, true, false);

    return getTrackID (*track);
}

void stopAuditioning (ProjectContext& context, TrackID id)
{
    if (auto* track = findAudioTrack (context, id))
        track->turnOffGuideNotes();
}

//==============================================================================
void addNoteCommands (CommandRegistry& registry, ProjectContext& context, std::function<double()> getGridStepInBeats)
{
    addNoteCommands (registry, getSelection (context), std::move (getGridStepInBeats));
}
} // namespace phazite

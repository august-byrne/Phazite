// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NotesModel.h"
#include "internal/Notes.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace phazite
{
namespace
{
    // A MIDI clip's element in the open project's song, or invalid.
    juce::ValueTree findMidiClip (const ProjectContext& context, ClipID id)
    {
        auto* song = findSong (context);
        const auto clipState = song != nullptr ? findClipState (*song, id) : juce::ValueTree();
        return isMidiState (clipState) ? clipState : juce::ValueTree();
    }

    // A track of the open project's that can hold notes: a track or a return, not a folder or the master.
    bool canHold (const Song& song, TrackID id)
    {
        const auto state = findTrackState (song, id);
        return state.hasType (schema::TRACK) && ! isFolderState (state);
    }

    NoteClip makeNoteClip (const Song& song, const juce::ValueTree& clipState)
    {
        const auto track = getTrackID (clipState.getParent());
        const auto tracks = getTrackIDs (song);
        const auto row = (int) (std::find (tracks.begin(), tracks.end(), track) - tracks.begin());
        return { getClipID (clipState),
                 track,
                 clipState[schema::clipName].toString(),
                 getClipRange (clipState),
                 getContentStart (clipState),
                 getTrackColour (song, track, row) };
    }
} // namespace

//==============================================================================
std::vector<NoteClip> getNoteClips (const ProjectContext& context, const std::vector<ClipID>& ids)
{
    std::vector<NoteClip> clips;

    for (const auto id : ids)
        if (const auto clipState = findMidiClip (context, id); clipState.isValid())
            clips.push_back (makeNoteClip (*findSong (context), clipState));

    return clips;
}

std::optional<NoteClip> getNoteClip (const ProjectContext& context, ClipID id)
{
    if (const auto clipState = findMidiClip (context, id); clipState.isValid())
        return makeNoteClip (*findSong (context), clipState);

    return {};
}

bool canHoldNotes (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id);
}

bool hasAudioClips (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id) && hasAudioClips (*song, getClips (*song, id));
}

std::optional<ClipID> getMidiClipNearest (const ProjectContext& context, TrackID id, BeatPosition beat)
{
    auto* song = findSong (context);

    if (song == nullptr || ! canHold (*song, id))
        return {};

    std::optional<ClipID> nearest;
    auto nearestDistance = std::numeric_limits<double>::max();

    for (const auto& clipState : getClipStates (findTrackState (*song, id)))
    {
        if (! isMidiState (clipState))
            continue;

        const auto range = getClipRange (clipState);
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

ClipID addClipForNotes (ProjectContext& context, TrackID id, BeatPosition start)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id) ? insertBarLongMidiClip (*song, id, start, nullptr) : ClipID();
}

std::optional<double> getLinkedPlayBeat (const ProjectContext& context, const std::vector<ClipID>& ids, double songBeat)
{
    auto* song = findSong (context);
    return song != nullptr ? getLinkedPlayBeat (*song, ids, songBeat) : std::nullopt;
}

//==============================================================================
std::vector<NoteData> getNotes (const ProjectContext& context, const std::vector<ClipID>& ids)
{
    std::vector<NoteData> notes;

    if (auto* song = findSong (context))
        for (const auto id : ids)
            for (const auto& note : getClipNotes (*song, id))
                notes.push_back (note);

    return notes;
}

std::optional<NoteData> getNote (const ProjectContext& context, NoteRef ref)
{
    auto* song = findSong (context);
    return song != nullptr ? getNoteData (*song, ref) : std::nullopt;
}

std::vector<NoteData> getNotesOfOtherTracks (const ProjectContext& context, const std::vector<TrackID>& shown, BeatRange range)
{
    std::vector<NoteData> notes;
    auto* song = findSong (context);

    if (song == nullptr)
        return notes;

    for (const auto track : getTrackIDs (*song))
    {
        if (std::find (shown.begin(), shown.end(), track) != shown.end())
            continue;

        for (const auto& clipState : getClipStates (findTrackState (*song, track)))
            if (isMidiState (clipState) && getClipRange (clipState).overlaps (range))
                for (const auto& note : getClipNotes (*song, getClipID (clipState)))
                    notes.push_back (note);
    }

    return notes;
}

NoteRef addNote (ProjectContext& context, ClipID id, int pitch, BeatPosition start, BeatDuration length, int velocity)
{
    auto* song = findSong (context);
    return song != nullptr ? addNote (*song, id, pitch, start, length, velocity) : NoteRef();
}

void moveNote (ProjectContext& context, NoteRef ref, BeatPosition start, int pitch)
{
    if (auto* song = findSong (context))
        moveNote (*song, ref, start, pitch);
}

void setNoteLength (ProjectContext& context, NoteRef ref, BeatDuration length)
{
    if (auto* song = findSong (context))
        setNoteLength (*song, ref, length);
}

void deleteNotes (ProjectContext& context, const std::vector<NoteRef>& refs)
{
    if (auto* song = findSong (context))
        deleteNotes (*song, refs);
}

void toggleNotesMuted (ProjectContext& context, const std::vector<NoteRef>& refs)
{
    if (auto* song = findSong (context))
        toggleNotesMuted (*song, refs);
}

void setNoteVelocities (ProjectContext& context, const std::vector<NoteRef>& refs, int velocity)
{
    if (auto* song = findSong (context))
        setNoteVelocities (*song, refs, velocity);
}

void nudgeNotes (ProjectContext& context, const std::vector<NoteRef>& refs, BeatDuration amount)
{
    if (auto* song = findSong (context))
        nudgeNotes (*song, refs, amount);
}

namespace
{
    constexpr double shortestNote = 1.0 / 16.0; // a 64th note, in beats

    const char* getUndoName (NoteProperty property)
    {
        switch (property)
        {
            case NoteProperty::start:
                return "Move Notes";
            case NoteProperty::length:
                return "Change Note Length";
            case NoteProperty::pitch:
                return "Transpose Notes";
            case NoteProperty::velocity:
            default:
                return "Change Velocity";
        }
    }

    double getProperty (const NoteData& note, NoteProperty property)
    {
        switch (property)
        {
            case NoteProperty::start:
                return note.start.inBeats();
            case NoteProperty::length:
                return note.length.inBeats();
            case NoteProperty::pitch:
                return note.pitch;
            case NoteProperty::velocity:
            default:
                return note.velocity;
        }
    }

    // The lowest and highest of the notes' values of a property.
    using Range = std::pair<double, double>;

    Range getRange (const std::vector<NoteData>& notes, NoteProperty property)
    {
        auto lowest = getProperty (notes.front(), property), highest = lowest;

        for (const auto& note : notes)
        {
            lowest = std::min (lowest, getProperty (note, property));
            highest = std::max (highest, getProperty (note, property));
        }

        return { lowest, highest };
    }

    // The notes, as they are now; each given the value worked out for it from its own and the notes' range, kept in range,
    // as one step.
    void setEach (ProjectContext& context, const std::vector<NoteRef>& refs, NoteProperty property,
                  const std::function<double (Range, double)>& valueOf)
    {
        auto* song = findSong (context);
        std::vector<NoteData> notes;

        for (const auto ref : refs)
            if (const auto note = getNote (context, ref))
                notes.push_back (*note);

        if (song == nullptr || notes.empty())
            return;

        const UndoStep step (*song, getUndoName (property));
        const auto range = getRange (notes, property);
        auto changed = notes;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto value = valueOf (range, getProperty (notes[i], property));
            auto& note = changed[i];

            switch (property)
            {
                case NoteProperty::start:
                    note.start = BeatPosition::fromBeats (std::max (0.0, value));
                    break;
                case NoteProperty::length:
                    note.length = BeatDuration::fromBeats (std::max (shortestNote, value));
                    break;
                case NoteProperty::pitch:
                    note.pitch = juce::jlimit (0, 127, (int) std::lround (value));
                    break;
                case NoteProperty::velocity:
                    note.velocity = juce::jlimit (1, 127, (int) std::lround (value));
                    break;
            }
        }

        if (property != NoteProperty::velocity)
        {
            setNotes (context, changed);
            return;
        }

        for (const auto& note : changed)
            setNoteVelocities (context, { note.getRef() }, note.velocity);
    }
} // namespace

void changeNotes (ProjectContext& context, const std::vector<NoteRef>& refs, NoteProperty property, double amount)
{
    // Starts and pitches together, as far as the earliest, the lowest and the highest can go.
    const auto together = property == NoteProperty::start || property == NoteProperty::pitch;
    const auto top = property == NoteProperty::pitch ? 127.0 : std::numeric_limits<double>::max();

    setEach (context, refs, property, [together, top, amount] (Range range, double own)
             { return own + (together ? juce::jlimit (-range.first, top - range.second, amount) : amount); });
}

void setNotesTo (ProjectContext& context, const std::vector<NoteRef>& refs, NoteProperty property, double value)
{
    const auto each = property == NoteProperty::length || property == NoteProperty::velocity;
    setEach (context, refs, property, [each, value] (Range range, double own) { return each ? value : own + value - range.first; });
}

void setNotes (ProjectContext& context, const std::vector<NoteData>& notes)
{
    // Each as given, those that are gone left out.
    if (auto* song = findSong (context))
    {
        for (const auto& note : notes)
        {
            if (! findNoteState (*song, note.getRef()).isValid())
                continue;

            moveNote (*song, note.getRef(), note.start, note.pitch);
            setNoteLength (*song, note.getRef(), note.length);
        }
    }
}

std::vector<NoteRef> replaceNotes (ProjectContext& context, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements)
{
    auto* song = findSong (context);
    return song != nullptr ? replaceNotes (*song, old, replacements) : std::vector<NoteRef>();
}

std::vector<NoteRef> replaceNotes (ProjectContext& context, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements,
                                   const juce::String& undoName)
{
    auto* song = findSong (context);
    return song != nullptr ? replaceNotes (*song, old, replacements, undoName) : std::vector<NoteRef>();
}

void changeNoteVelocities (ProjectContext& context, const std::vector<NoteRef>& refs, int change)
{
    if (auto* song = findSong (context))
        changeNoteVelocities (*song, refs, change);
}

//==============================================================================
std::optional<Key> getTrackKey (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr ? getTrackKey (*song, id) : std::nullopt;
}

void chooseTrackKey (ProjectContext& context, TrackID id, const std::optional<Key>& key)
{
    if (auto* song = findSong (context); song != nullptr && findTrackState (*song, id).isValid())
        chooseTrackKey (*song, id, key);
}

void useKeyOnEveryTrack (ProjectContext& context, const std::optional<Key>& key)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return;

    const UndoStep step (*song, "Use This Key on Every Track");
    setTrackKeys (*song, getTrackIDs (*song), key);
}

//==============================================================================
bool isDrumTrack (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id) && isDrumTrack (*song, id);
}

void setDrumTrack (ProjectContext& context, TrackID id, bool on)
{
    if (auto* song = findSong (context); song != nullptr && canHold (*song, id))
        setDrumTrack (*song, id, on);
}

std::optional<std::vector<DrumRow>> getSamplerSounds (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id) ? getSamplerSounds (*song, id) : std::nullopt;
}

std::vector<int> getNoteNumbersPlayed (const ProjectContext& context, const std::vector<ClipID>& ids)
{
    auto* song = findSong (context);
    return song != nullptr ? getNoteNumbersPlayed (*song, ids) : std::vector<int>();
}

std::vector<int> getDrumRowOrder (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id) ? getDrumRowOrder (*song, id) : std::vector<int>();
}

void setDrumRowOrder (ProjectContext& context, TrackID id, const std::vector<int>& notes)
{
    if (auto* song = findSong (context); song != nullptr && canHold (*song, id))
        setDrumRowOrder (*song, id, notes);
}

std::vector<int> getHiddenDrumRows (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && canHold (*song, id) ? getHiddenDrumRows (*song, id) : std::vector<int>();
}

void hideDrumRow (ProjectContext& context, TrackID id, int note)
{
    if (auto* song = findSong (context); song != nullptr && canHold (*song, id))
        hideDrumRow (*song, id, note);
}

void showAllDrumRows (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context); song != nullptr && canHold (*song, id))
        showAllDrumRows (*song, id);
}

//==============================================================================
TrackID auditionNotes (ProjectContext& context, ClipID id, const std::vector<int>& pitches, int velocity)
{
    auto* song = findSong (context);
    const auto track = song != nullptr ? getClipTrack (*song, id) : TrackID();

    if (! track.isValid() || ! findMidiClip (context, id).isValid() || ! hasInstrument (*song, track))
        return {};

    // Into the track, as live MIDI goes, through Phazite's engine while it plays the project.
    auto* session = findPlayingSession (*song);

    if (session == nullptr)
        return {};

    session->playGuideNotes ((juce::int64) track.getRaw(), pitches, velocity);
    return track;
}

void stopAuditioning (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context))
        if (auto* session = findSession (*song))
            session->stopGuideNotes ((juce::int64) id.getRaw());
}

//==============================================================================
void addNoteCommands (CommandRegistry& registry, ProjectContext& context, std::function<double()> getGridStepInBeats)
{
    addNoteCommands (registry, getSelection (context), std::move (getGridStepInBeats));
}
} // namespace phazite

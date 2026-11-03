// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Notes.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumSamplerPlugin.h"
#include "model/mirror/internal/Mirror.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace phazite
{
namespace
{
    // The scales Phazite offers, in the order its menus show them: the names projects save them by and menus show, and the
    // semitones of their notes above the root.
    struct SavedScale
    {
        Scale scale;
        const char* name;
        const char* shownName;
        std::array<int, 7> steps;
    };

    // clang-format off
    constexpr SavedScale savedScales[] = {
        { Scale::major,         "major",         "Major",          { 0, 2, 4, 5, 7, 9, 11 } },
        { Scale::minor,         "minor",         "Minor",          { 0, 2, 3, 5, 7, 8, 10 } },
        { Scale::dorian,        "dorian",        "Dorian",         { 0, 2, 3, 5, 7, 9, 10 } },
        { Scale::phrygian,      "phrygian",      "Phrygian",       { 0, 1, 3, 5, 7, 8, 10 } },
        { Scale::lydian,        "lydian",        "Lydian",         { 0, 2, 4, 6, 7, 9, 11 } },
        { Scale::mixolydian,    "mixolydian",    "Mixolydian",     { 0, 2, 4, 5, 7, 9, 10 } },
        { Scale::locrian,       "locrian",       "Locrian",        { 0, 1, 3, 5, 6, 8, 10 } },
        { Scale::harmonicMinor, "harmonicMinor", "Harmonic Minor", { 0, 2, 3, 5, 7, 8, 11 } },
        { Scale::melodicMinor,  "melodicMinor",  "Melodic Minor",  { 0, 2, 3, 5, 7, 9, 11 } }
    };
    // clang-format on

    const SavedScale& findSavedScale (Scale scale)
    {
        for (const auto& saved : savedScales)
            if (saved.scale == scale)
                return saved;

        return savedScales[0];
    }

    // The chords the piano roll stamps, with their notes' semitones above the root.
    struct ChordStamp
    {
        ChordType type;
        std::vector<int> intervals;
        const char* name;
    };

    // clang-format off
    const ChordStamp chords[] = { { ChordType::major,                 { 0, 4, 7 },          "Major" },
                                  { ChordType::minor,                 { 0, 3, 7 },          "Minor" },
                                  { ChordType::dominantSeventh,       { 0, 4, 7, 10 },      "7" },
                                  { ChordType::majorSeventh,          { 0, 4, 7, 11 },      "Maj7" },
                                  { ChordType::minorSeventh,          { 0, 3, 7, 10 },      "m7" },
                                  { ChordType::suspendedSecond,       { 0, 2, 7 },          "Sus2" },
                                  { ChordType::suspendedFourth,       { 0, 5, 7 },          "Sus4" },
                                  { ChordType::diminished,            { 0, 3, 6 },          "Dim" },
                                  { ChordType::augmented,             { 0, 4, 8 },          "Aug" },
                                  { ChordType::majorSixth,            { 0, 4, 7, 9 },       "6" },
                                  { ChordType::minorSixth,            { 0, 3, 7, 9 },       "m6" },
                                  { ChordType::dominantNinth,         { 0, 4, 7, 10, 14 },  "9" },
                                  { ChordType::majorNinth,            { 0, 4, 7, 11, 14 },  "Maj9" },
                                  { ChordType::power,                 { 0, 7 },             "Power" },
                                  { ChordType::halfDiminishedSeventh, { 0, 3, 6, 10 },      "m7b5" },
                                  { ChordType::diminishedSeventh,     { 0, 3, 6, 9 },       "Dim7" },
                                  { ChordType::minorNinth,            { 0, 3, 7, 10, 14 },  "m9" } };
    // clang-format on

    // A key, on a track's element or on LASTKEY in the song's Phazite state (model/base/internal/Schema.h). A key exists exactly when
    // its root does.
    std::optional<Key> readKey (const juce::ValueTree& state)
    {
        if (! state.hasProperty (schema::keyRoot))
            return std::nullopt;

        return Key { juce::jlimit (0, 11, (int) state[schema::keyRoot]),
                     findScale (state[schema::keyScale].toString()).value_or (Scale::major) };
    }

    void writeKey (juce::ValueTree state, const std::optional<Key>& key, juce::UndoManager* undo)
    {
        if (! key.has_value())
        {
            state.removeProperty (schema::keyRoot, undo);
            state.removeProperty (schema::keyScale, undo);
            return;
        }

        state.setProperty (schema::keyRoot, key->root, undo);
        state.setProperty (schema::keyScale, getSavedScaleName (key->scale), undo);
    }

    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);
    const auto tinyTime = te::TimeDuration::fromSeconds (1.0e-6);
    constexpr int maxPiecesPerNote = 256; // chopping into tinier pieces than this makes no musical sense

    juce::UndoManager* getUndoManager (te::MidiClip& clip)
    {
        return &getSong (clip.edit).getUndoManager();
    }

    // Where a clip's notes count from in the song (its content start), and where it is, as the song has them.
    te::BeatPosition getContentStartOf (const te::MidiClip& clip)
    {
        if (const auto clipState = findClipState (clip); clipState.isValid())
            return toTracktion (getContentStart (clipState));

        return clip.getContentStartBeat();
    }

    te::BeatPosition getStartOf (const te::MidiClip& clip)
    {
        if (const auto clipState = findClipState (clip); clipState.isValid())
            return toTracktion (getClipRange (clipState).getStart());

        return clip.getStartBeat();
    }

    // A song beat as a position among the clip's notes, which count from its content's start.
    te::BeatPosition toClipBeat (const te::MidiClip& clip, te::BeatPosition songBeat)
    {
        return te::BeatPosition::fromBeats ((songBeat - getContentStartOf (clip)).inBeats());
    }

    //==============================================================================
    // The song's notes: each a NOTE in its clip's element (Schema.h), and in each alias's, with the same ID.

    juce::ValueTree findNoteState (const juce::ValueTree& clipState, NoteID id)
    {
        for (const auto& child : clipState)
            if (child.hasType (schema::NOTE) && (juce::int64) child[schema::noteID] == (juce::int64) id.getRaw())
                return child;

        return {};
    }

    void reconcileAll (te::Edit& edit, const std::vector<juce::ValueTree>& clipStates)
    {
        for (const auto& clipState : clipStates)
            mirror::reconcileClip (edit, getClipID (clipState));
    }

    // Changes the note with an ID in a clip, and in each of its aliases, which hold the same notes; and Tracktion's
    // notes follow.
    void changeNote (te::MidiClip& clip, NoteID id, const std::function<void (juce::ValueTree&, juce::UndoManager*)>& change)
    {
        const auto clipState = findClipState (clip);

        if (! clipState.isValid())
            return;

        const auto aliases = getAliasStates (clipState);

        for (const auto& alias : aliases)
            if (auto note = findNoteState (alias, id); note.isValid())
                change (note, getUndoManager (clip));

        reconcileAll (clip.edit, aliases);
    }

    // A new note's ID, the song's (Song::makeID()): one no note has had before.
    juce::int64 makeNoteID (te::Edit& edit)
    {
        return (juce::int64) getSong (edit).makeID();
    }

    // What a note is besides its place, pitch and velocity: muted, a slide note or a portamento note, each written on its
    // element only while it is (Schema.h).
    struct NoteFlags
    {
        bool muted = false, slide = false, portamento = false;
    };

    NoteFlags getFlags (const NoteData& data)
    {
        return { data.muted, data.slide, data.portamento };
    }

    void writeFlags (juce::ValueTree& note, NoteFlags flags)
    {
        for (const auto& [property, on] : { std::pair { &schema::noteMute, flags.muted }, std::pair { &schema::noteSlide, flags.slide },
                                            std::pair { &schema::notePortamento, flags.portamento } })
            if (on)
                note.setProperty (*property, true, nullptr);
    }

    // Adds a note to a clip and to each of its aliases, with the ID wanted if no note of the clip has it, or else a new
    // one; and Tracktion's notes follow. Returns its ID, or none if the clip isn't the song's.
    NoteID addNoteState (te::MidiClip& clip, NoteID wanted, int pitch, te::BeatPosition start, te::BeatDuration length, int velocity,
                         NoteFlags flags)
    {
        const auto clipState = findClipState (clip);

        if (! clipState.isValid())
            return {};

        const auto id =
            wanted.isValid() && ! findNoteState (clipState, wanted).isValid() ? (juce::int64) wanted.getRaw() : makeNoteID (clip.edit);

        const auto aliases = getAliasStates (clipState);

        for (auto alias : aliases)
        {
            juce::ValueTree note (schema::NOTE);
            note.setProperty (schema::noteID, id, nullptr);
            note.setProperty (schema::notePitch, pitch, nullptr);
            note.setProperty (schema::noteStart, start.inBeats(), nullptr);
            note.setProperty (schema::noteLength, length.inBeats(), nullptr);
            note.setProperty (schema::noteVelocity, velocity, nullptr);
            writeFlags (note, flags);
            alias.appendChild (note, getUndoManager (clip));
        }

        reconcileAll (clip.edit, aliases);
        return NoteID::fromRaw ((std::uint64_t) id);
    }

    void removeNoteState (te::MidiClip& clip, NoteID id)
    {
        const auto clipState = findClipState (clip);

        if (! clipState.isValid())
            return;

        const auto aliases = getAliasStates (clipState);

        for (auto alias : aliases)
            if (const auto note = findNoteState (alias, id); note.isValid())
                alias.removeChild (note, getUndoManager (clip));

        reconcileAll (clip.edit, aliases);
    }

    // The notes, each once: of a note in a clip and its copies in the clip's aliases, the first one, since an edit to it is
    // made to each of them.
    std::vector<ClipNote> withoutAliasCopies (const std::vector<ClipNote>& notes)
    {
        std::vector<ClipNote> unique;
        std::set<std::pair<juce::String, juce::int64>> seen;

        for (const auto& n : notes)
        {
            if (n.clip == nullptr || n.note == nullptr)
                continue;

            const auto group = findClipState (*n.clip)[schema::clipLink].toString();
            const auto key = group.isNotEmpty() ? group : "clip " + juce::String ((juce::int64) getClipID (*n.clip).getRaw());

            if (seen.insert ({ key, (juce::int64) getNoteID (*n.note).getRaw() }).second)
                unique.push_back (n);
        }

        return unique;
    }

    // Where a clip can grow to without covering another clip on its track: the start of the first clip that reaches past
    // its end, or its own end if another clip already covers that. Nothing if nothing on the track reaches past its end.
    std::optional<te::TimePosition> findRoomEnd (const te::Clip& clip)
    {
        auto* track = clip.getClipTrack();

        if (track == nullptr)
            return std::nullopt;

        const auto end = clip.getPosition().getEnd();
        std::optional<te::TimePosition> roomEnd;

        for (auto* other : track->getClips())
        {
            if (other == &clip || other->getPosition().getEnd() <= end + tinyTime)
                continue;

            const auto covered = std::max (other->getPosition().getStart(), end);
            roomEnd = roomEnd.has_value() ? std::min (*roomEnd, covered) : covered;
        }

        return roomEnd;
    }

    // Makes the clip end at the end of the bar a song beat is in, if the clip ends before the beat, but only into the
    // empty space after it: never past the start of the next clip on its track, so that a note edit never makes one clip
    // cover another.
    void growToHold (te::MidiClip& clip, te::BeatPosition end)
    {
        auto clipState = findClipState (clip);

        if (! clipState.isValid())
            return;

        const auto range = getClipRange (clipState);

        if (end <= toTracktion (range.getEnd()) + tinyBeats)
            return;

        const auto& tempo = clip.edit.tempoSequence;
        const auto bars = getBars (getSong (clip.edit));
        auto newEnd = tempo.toTime (toTracktion (bars.getBarStart (bars.getBarAt (fromTracktion (end - tinyBeats)) + 1)));

        if (const auto roomEnd = findRoomEnd (clip))
            newEnd = std::min (newEnd, *roomEnd);

        if (newEnd > clip.getPosition().getEnd() + tinyTime)
            setClipPlace (clip.edit, clipState, range.getStart(), fromTracktion (tempo.toBeats (newEnd)) - range.getStart(),
                          getClipOffset (clipState));
    }

    int clampPitch (int pitch)
    {
        return juce::jlimit (0, 127, pitch);
    }

    // Integer division and remainder that round down, so that they also work below 0.
    int floorDivide (int a, int b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    int floorModulo (int a, int b)
    {
        return a - b * floorDivide (a, b);
    }

    std::vector<int> parseNotes (const juce::var& value)
    {
        std::vector<int> notes;

        for (const auto& token : juce::StringArray::fromTokens (value.toString(), ",", {}))
            if (token.trim().isNotEmpty())
                notes.push_back (token.getIntValue());

        return notes;
    }

    juce::String joinNotes (const std::vector<int>& notes)
    {
        juce::StringArray tokens;

        for (const auto note : notes)
            tokens.add (juce::String (note));

        return tokens.joinIntoString (",");
    }
} // namespace

//==============================================================================
te::BeatPosition getNoteStart (const te::MidiClip& clip, const te::MidiNote& note)
{
    return getContentStartOf (clip) + te::toDuration (note.getStartBeat());
}

NoteData getNoteData (const te::MidiClip& clip, const te::MidiNote& note)
{
    // Its slide and portamento as the mirror keeps them on Tracktion's note (ClipMirror.cpp).
    return { getClipID (clip),
             getNoteID (note),
             note.getNoteNumber(),
             fromTracktion (getNoteStart (clip, note)),
             fromTracktion (note.getLengthBeats()),
             note.getVelocity(),
             note.isMute(),
             (bool) note.state[schema::noteSlide],
             (bool) note.state[schema::notePortamento] };
}

std::vector<NoteData> getNoteData (const std::vector<ClipNote>& notes)
{
    std::vector<NoteData> data;

    for (const auto& n : notes)
        if (n.clip != nullptr && n.note != nullptr)
            data.push_back (getNoteData (*n.clip, *n.note));

    return data;
}

BeatDuration getMinimumNoteLength()
{
    return BeatDuration::fromBeats (1.0 / 64.0);
}

//==============================================================================
NoteID getNoteID (const te::MidiNote& note)
{
    return NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, static_cast<juce::int64> (note.state[schema::phaziteNoteID])));
}

te::MidiNote* findNote (const te::MidiClip& clip, NoteID id)
{
    if (id.isValid())
        for (auto* note : clip.getSequence().getNotes())
            if (getNoteID (*note) == id)
                return note;

    return nullptr;
}

NoteRef getNoteRef (const te::MidiClip& clip, const te::MidiNote& note)
{
    return { getClipID (clip), getNoteID (note) };
}

//==============================================================================
std::vector<ClipNote> getSelectedNotes (const Selection& selection)
{
    std::vector<ClipNote> notes;
    auto* edit = selection.getEdit();

    if (edit == nullptr || selection.getNotes().empty())
        return notes;

    // Each note clip's notes by ID, found once: those of the selected notes still there, in the order they were selected.
    std::unordered_map<ClipID, std::unordered_map<NoteID, te::MidiNote*>> byClip;

    for (const auto& ref : selection.getNotes())
    {
        if (std::find (selection.getNoteClips().begin(), selection.getNoteClips().end(), ref.clip) == selection.getNoteClips().end())
            continue;

        auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, toTracktion (ref.clip)));

        if (clip == nullptr)
            continue;

        auto& notesOfClip = byClip[ref.clip];

        if (notesOfClip.empty())
            for (auto* note : clip->getSequence().getNotes())
                notesOfClip.emplace (getNoteID (*note), note);

        if (const auto found = notesOfClip.find (ref.note); found != notesOfClip.end())
            notes.push_back ({ clip, found->second });
    }

    return notes;
}

void selectNotes (Selection& selection, const std::vector<ClipNote>& notes)
{
    std::vector<NoteRef> refs;

    for (const auto& n : notes)
        refs.push_back (getNoteRef (*n.clip, *n.note));

    selection.selectNotes (refs);
}

bool hasSelectedNotes (const Selection& selection)
{
    return ! getSelectedNotes (selection).empty();
}

//==============================================================================
namespace
{
    // Adds a note at a song beat, as addNote() says, with its flags, with the ID wanted if no note of the clip has it.
    te::MidiNote* addNoteTo (te::MidiClip& clip, NoteID wanted, int pitch, te::BeatPosition start, te::BeatDuration length, int velocity,
                             NoteFlags flags)
    {
        start = std::max (start, getStartOf (clip));
        length = std::max (length, toTracktion (getMinimumNoteLength()));
        growToHold (clip, start + length);
        const auto id =
            addNoteState (clip, wanted, clampPitch (pitch), toClipBeat (clip, start), length, juce::jlimit (1, 127, velocity), flags);
        return id.isValid() ? findNote (clip, id) : nullptr;
    }
} // namespace

te::MidiNote* addNote (te::MidiClip& clip, int pitch, te::BeatPosition start, te::BeatDuration length, int velocity)
{
    return addNoteTo (clip, {}, pitch, start, length, velocity, {});
}

void moveNote (te::MidiClip& clip, te::MidiNote& note, te::BeatPosition start, int pitch)
{
    start = std::max (start, getStartOf (clip));
    growToHold (clip, start + note.getLengthBeats());
    const auto clipBeat = toClipBeat (clip, start);
    changeNote (clip, getNoteID (note),
                [clipBeat, pitch] (juce::ValueTree& n, juce::UndoManager* undo)
                {
                    n.setProperty (schema::noteStart, clipBeat.inBeats(), undo);
                    n.setProperty (schema::notePitch, clampPitch (pitch), undo);
                });
}

void setNoteLength (te::MidiClip& clip, te::MidiNote& note, te::BeatDuration length)
{
    length = std::max (length, toTracktion (getMinimumNoteLength()));
    growToHold (clip, getNoteStart (clip, note) + length);
    changeNote (clip, getNoteID (note),
                [length] (juce::ValueTree& n, juce::UndoManager* undo) { n.setProperty (schema::noteLength, length.inBeats(), undo); });
}

void deleteNotes (const std::vector<ClipNote>& notes)
{
    // Each by its ID, all found before any goes; a note's copies in its clip's aliases go with it.
    std::vector<std::pair<te::MidiClip*, NoteID>> ids;

    for (const auto& n : withoutAliasCopies (notes))
        ids.emplace_back (n.clip, getNoteID (*n.note));

    for (const auto& [clip, id] : ids)
        removeNoteState (*clip, id);
}

void transposeNotes (const std::vector<ClipNote>& notes, int semitones)
{
    const auto unique = withoutAliasCopies (notes);

    // As far as the highest or lowest note can go, so that chords keep their shape.
    for (const auto& n : unique)
        semitones = juce::jlimit (-n.note->getNoteNumber(), 127 - n.note->getNoteNumber(), semitones);

    if (semitones == 0)
        return;

    for (const auto& n : unique)
    {
        const auto pitch = n.note->getNoteNumber() + semitones;
        changeNote (*n.clip, getNoteID (*n.note),
                    [pitch] (juce::ValueTree& note, juce::UndoManager* undo) { note.setProperty (schema::notePitch, pitch, undo); });
    }
}

void nudgeNotes (const std::vector<ClipNote>& notes, te::BeatDuration amount)
{
    const auto unique = withoutAliasCopies (notes);

    // The note nearest its clip's start decides how far back they can go.
    for (const auto& n : unique)
        amount = std::max (amount, getStartOf (*n.clip) - getNoteStart (*n.clip, *n.note));

    for (const auto& n : unique)
        moveNote (*n.clip, *n.note, getNoteStart (*n.clip, *n.note) + amount, n.note->getNoteNumber());
}

void changeNoteLengths (const std::vector<ClipNote>& notes, te::BeatDuration change)
{
    for (const auto& n : withoutAliasCopies (notes))
        setNoteLength (*n.clip, *n.note, n.note->getLengthBeats() + change);
}

bool areAllNotesMuted (const std::vector<ClipNote>& notes)
{
    return areAllOn (notes, [] (const ClipNote& n) { return n.note->isMute(); });
}

void toggleNotesMuted (const std::vector<ClipNote>& notes)
{
    toggleAll (
        withoutAliasCopies (notes), [] (const ClipNote& n) { return n.note->isMute(); },
        [] (const ClipNote& n, bool on)
        {
            changeNote (*n.clip, getNoteID (*n.note),
                        [on] (juce::ValueTree& note, juce::UndoManager* undo)
                        {
                            if (on)
                                note.setProperty (schema::noteMute, true, undo);
                            else
                                note.removeProperty (schema::noteMute, undo);
                        });
        });
}

namespace
{
    bool hasFlag (const ClipNote& n, const juce::Identifier& flag)
    {
        return (bool) n.note->state[flag];
    }

    void toggleNoteFlags (const std::vector<ClipNote>& notes, const juce::Identifier& flag)
    {
        toggleAll (
            withoutAliasCopies (notes), [&flag] (const ClipNote& n) { return hasFlag (n, flag); },
            [&flag] (const ClipNote& n, bool on)
            {
                changeNote (*n.clip, getNoteID (*n.note),
                            [on, &flag] (juce::ValueTree& note, juce::UndoManager* undo)
                            {
                                if (on)
                                    note.setProperty (flag, true, undo);
                                else
                                    note.removeProperty (flag, undo);
                            });
            });
    }
} // namespace

bool areAllNotesSlides (const std::vector<ClipNote>& notes)
{
    return areAllOn (notes, [] (const ClipNote& n) { return hasFlag (n, schema::noteSlide); });
}

void toggleNotesSlide (const std::vector<ClipNote>& notes)
{
    toggleNoteFlags (notes, schema::noteSlide);
}

bool areAllNotesPortamento (const std::vector<ClipNote>& notes)
{
    return areAllOn (notes, [] (const ClipNote& n) { return hasFlag (n, schema::notePortamento); });
}

void toggleNotesPortamento (const std::vector<ClipNote>& notes)
{
    toggleNoteFlags (notes, schema::notePortamento);
}

void setNoteVelocities (const std::vector<ClipNote>& notes, int velocity)
{
    const auto clamped = juce::jlimit (1, 127, velocity);

    for (const auto& n : withoutAliasCopies (notes))
        changeNote (*n.clip, getNoteID (*n.note),
                    [clamped] (juce::ValueTree& note, juce::UndoManager* undo) { note.setProperty (schema::noteVelocity, clamped, undo); });
}

void changeNoteVelocities (const std::vector<ClipNote>& notes, int change)
{
    if (notes.empty())
        return;

    const UndoStep step (notes.front().clip->edit, "Change Velocity");

    for (const auto& n : withoutAliasCopies (notes))
        setNoteVelocities ({ n }, n.note->getVelocity() + change);
}

std::vector<ClipNote> duplicateNotes (const std::vector<ClipNote>& notes)
{
    const auto unique = withoutAliasCopies (notes);

    if (unique.empty())
        return {};

    // The copies go right after the whole selection.
    auto start = getNoteStart (*unique.front().clip, *unique.front().note), end = start;

    for (const auto& n : unique)
    {
        start = std::min (start, getNoteStart (*n.clip, *n.note));
        end = std::max (end, getNoteStart (*n.clip, *n.note) + n.note->getLengthBeats());
    }

    auto copies = getNoteData (unique);

    for (auto& copy : copies)
        copy.start = copy.start + fromTracktion (end - start);

    return replaceNotes (unique.front().clip->edit, {}, copies);
}

void setClipNotes (te::MidiClip& clip, const std::vector<NoteData>& notes)
{
    const auto clipState = findClipState (clip);

    if (! clipState.isValid())
        return;

    auto* undo = getUndoManager (clip);
    const auto contentStart = getContentStartOf (clip);
    const auto aliases = getAliasStates (clipState);
    std::vector<juce::int64> ids;

    for (size_t i = 0; i < notes.size(); ++i)
        ids.push_back (makeNoteID (clip.edit));

    for (auto alias : aliases)
    {
        for (int i = alias.getNumChildren(); --i >= 0;)
            if (alias.getChild (i).hasType (schema::NOTE))
                alias.removeChild (i, undo);

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& data = notes[i];
            juce::ValueTree note (schema::NOTE);
            note.setProperty (schema::noteID, ids[i], nullptr);
            note.setProperty (schema::notePitch, clampPitch (data.pitch), nullptr);
            note.setProperty (schema::noteStart, (toTracktion (data.start) - contentStart).inBeats(), nullptr);
            note.setProperty (schema::noteLength, data.length.inBeats(), nullptr);
            note.setProperty (schema::noteVelocity, juce::jlimit (1, 127, data.velocity), nullptr);
            writeFlags (note, getFlags (data));
            alias.appendChild (note, undo);
        }
    }

    reconcileAll (clip.edit, aliases);
}

std::vector<ClipNote> replaceNotes (te::Edit& edit, const std::vector<ClipNote>& old, const std::vector<NoteData>& replacements)
{
    deleteNotes (old);
    std::vector<ClipNote> added;
    te::MidiClip* clip = nullptr;

    for (const auto& data : replacements)
    {
        if (clip == nullptr || getClipID (*clip) != data.clip)
            clip = data.clip.isValid() ? dynamic_cast<te::MidiClip*> (te::findClipForID (edit, toTracktion (data.clip))) : nullptr;

        if (clip == nullptr)
            continue;

        // A note that replaces one of the old ones (a quantized or transposed note, a chopped note's first piece) keeps
        // that one's ID, if no other note of the clip has it now; a copy, whose original is still there, gets its own.
        if (auto* note =
                addNoteTo (*clip, data.id, data.pitch, toTracktion (data.start), toTracktion (data.length), data.velocity, getFlags (data)))
            added.push_back ({ clip, note });
    }

    return added;
}

std::vector<ClipNote> replaceNotes (te::Edit& edit, const std::vector<ClipNote>& old, const std::vector<NoteData>& replacements,
                                    const juce::String& undoName)
{
    if (old.empty() && replacements.empty())
        return {};

    const UndoStep step (edit, undoName);
    return replaceNotes (edit, old, replacements);
}

std::vector<ClipNote> replaceSelectedNotes (Selection& selection,
                                            const std::function<std::vector<NoteData> (std::vector<NoteData>)>& transform)
{
    const auto notes = withoutAliasCopies (getSelectedNotes (selection));

    if (notes.empty())
        return {};

    auto added = replaceNotes (notes.front().clip->edit, notes, transform (getNoteData (notes)));
    selectNotes (selection, added);
    return added;
}

//==============================================================================
std::vector<NoteData> quantize (std::vector<NoteData> notes, const QuantizeSettings& settings)
{
    const auto grid = settings.gridBeats;
    const auto strength = (double) juce::jlimit (0.0f, 1.0f, settings.strength);
    const auto swing = (double) juce::jlimit (0.0f, 1.0f, settings.swing);

    if (grid <= 0.0)
        return notes;

    // The nearest grid line, with every other one late by up to a third of a step: at full swing, the second
    // of each pair of steps lands two-thirds of the way through the pair, as in a triplet.
    const auto quantizeBeat = [=] (double beat)
    {
        const auto index = std::round (beat / grid);
        auto target = index * grid;

        if (std::fmod (std::abs (index), 2.0) > 0.5)
            target += swing * grid / 3.0;

        return std::max (0.0, beat + (target - beat) * strength);
    };

    for (auto& note : notes)
    {
        const auto end = note.getEnd().inBeats();
        note.start = BeatPosition::fromBeats (quantizeBeat (note.start.inBeats()));

        if (settings.ends)
            note.length = std::max (getMinimumNoteLength(), BeatDuration::fromBeats (quantizeBeat (end) - note.start.inBeats()));
    }

    return notes;
}

std::vector<NoteData> legato (std::vector<NoteData> notes, double gapBeats)
{
    // The starts in each clip, in order; a note lasts until the next start after its own.
    std::map<ClipID, std::vector<double>> starts;

    for (const auto& note : notes)
        starts[note.clip].push_back (note.start.inBeats());

    for (auto& [clip, beats] : starts)
        std::sort (beats.begin(), beats.end());

    for (auto& note : notes)
    {
        const auto& clipStarts = starts[note.clip];
        const auto next = std::upper_bound (clipStarts.begin(), clipStarts.end(), note.start.inBeats() + 1.0e-6);

        if (next != clipStarts.end())
            note.length = std::max (getMinimumNoteLength(), BeatDuration::fromBeats (*next - note.start.inBeats() - gapBeats));
    }

    return notes;
}

std::vector<NoteData> chop (std::vector<NoteData> notes, double pieceBeats)
{
    if (pieceBeats <= getMinimumNoteLength().inBeats() / 2.0)
        return notes;

    std::vector<NoteData> pieces;

    for (const auto& note : notes)
    {
        const auto length = note.length.inBeats();
        auto offset = 0.0;

        for (int i = 0; offset < length - 1.0e-6 && i < maxPiecesPerNote; ++i, offset += pieceBeats)
        {
            auto piece = note; // the first piece replaces the note (replaceNotes() keeps its ID), and the rest are new
            piece.start = note.start + BeatDuration::fromBeats (offset);
            piece.length = BeatDuration::fromBeats (std::min (pieceBeats, length - offset));
            pieces.push_back (piece);
        }
    }

    return pieces;
}

//==============================================================================
juce::String getScaleName (Scale scale)
{
    return findSavedScale (scale).shownName;
}

juce::String describeKey (const Key& key)
{
    return juce::MidiMessage::getMidiNoteName (key.root, true, false, 0) + " " + getScaleName (key.scale).toLowerCase();
}

std::vector<int> getScaleSteps (Scale scale)
{
    const auto& steps = findSavedScale (scale).steps;
    return { steps.begin(), steps.end() };
}

bool isInKey (int pitch, const Key& key)
{
    const auto steps = getScaleSteps (key.scale);
    return std::find (steps.begin(), steps.end(), floorModulo (pitch - key.root, 12)) != steps.end();
}

const std::vector<Scale>& getKeyScales()
{
    static const auto scales = []
    {
        std::vector<Scale> all;

        for (const auto& saved : savedScales)
            all.push_back (saved.scale);

        return all;
    }();

    return scales;
}

juce::String getSavedScaleName (Scale scale)
{
    return findSavedScale (scale).name;
}

std::optional<Scale> findScale (const juce::String& savedName)
{
    for (const auto& saved : savedScales)
        if (savedName == saved.name)
            return saved.scale;

    return std::nullopt;
}

std::optional<Key> getTrackKey (const te::Track& track)
{
    return readKey (findTrackState (track));
}

void setTrackKey (te::Track& track, const std::optional<Key>& key)
{
    writeKey (findTrackState (track), key, &getSong (track.edit).getUndoManager());

    // What new tracks get; not an edit to undo.
    if (key.has_value())
        writeKey (getSong (track.edit).getState().getOrCreateChildWithName (schema::LASTKEY, nullptr), key, nullptr);
}

void setTrackKeys (const juce::Array<te::Track*>& tracks, const std::optional<Key>& key)
{
    for (auto* track : tracks)
        setTrackKey (*track, key);
}

void chooseTrackKey (te::Track& track, const std::optional<Key>& key)
{
    const UndoStep step (track.edit, key.has_value() ? "Set Key" : "Turn Key Off");
    setTrackKey (track, key);
}

void applyLastKey (te::Track& track)
{
    if (const auto last = readKey (findPhaziteState (track.edit).getChildWithName (schema::LASTKEY)))
        writeKey (findTrackState (track), last, nullptr);
}

void setLastKey (te::Edit& edit, const Key& key)
{
    writeKey (getSong (edit).getState().getOrCreateChildWithName (schema::LASTKEY, nullptr), key, nullptr);
}

int snapToKey (int pitch, const Key& key)
{
    for (int distance = 0; distance < 12; ++distance)
    {
        if (isInKey (pitch - distance, key))
            return pitch - distance;

        if (isInKey (pitch + distance, key))
            return pitch + distance;
    }

    return pitch;
}

std::vector<NoteData> transpose (std::vector<NoteData> notes, int steps, const std::optional<Key>& diatonicIn)
{
    for (auto& note : notes)
    {
        if (! diatonicIn.has_value())
        {
            note.pitch = clampPitch (note.pitch + steps);
            continue;
        }

        // Count scale steps: from the note's degree, octaves wrap around the scale.
        const auto& key = *diatonicIn;
        const auto scale = getScaleSteps (key.scale);
        const auto size = (int) scale.size();
        const auto inKey = snapToKey (note.pitch, key);
        const auto fromRoot = inKey - key.root;
        const auto degree = (int) (std::find (scale.begin(), scale.end(), floorModulo (fromRoot, 12)) - scale.begin());
        const auto target = degree + steps;
        const auto octave = floorDivide (fromRoot, 12) + floorDivide (target, size);
        note.pitch = clampPitch (key.root + 12 * octave + scale[(size_t) floorModulo (target, size)]);
    }

    return notes;
}

//==============================================================================
const std::vector<ChordShape>& getChordShapes()
{
    static const auto shapes = []
    {
        std::vector<ChordShape> all;

        for (const auto& chord : chords)
            all.push_back ({ chord.name, chord.type });

        return all;
    }();

    return shapes;
}

std::vector<int> getChordIntervals (ChordType type)
{
    for (const auto& chord : chords)
        if (chord.type == type)
            return chord.intervals;

    return { 0, 4, 7 };
}

std::vector<int> getChordPitches (int root, ChordType type)
{
    std::vector<int> pitches;

    for (const auto step : getChordIntervals (type))
        if (juce::isPositiveAndBelow (root + step, 128))
            pitches.push_back (root + step);

    return pitches;
}

//==============================================================================
juce::String getDrumName (int note)
{
    // Producers' names for the drums used most, and General MIDI's for the rest.
    struct Name
    {
        int note;
        const char* name;
    };

    static constexpr Name shortNames[] = { { 35, "Kick 2" },   { 36, "Kick" },    { 37, "Rim" },        { 38, "Snare" },
                                           { 39, "Clap" },     { 40, "Snare 2" }, { 42, "Closed Hat" }, { 44, "Pedal Hat" },
                                           { 46, "Open Hat" }, { 49, "Crash" },   { 51, "Ride" } };

    for (const auto& entry : shortNames)
        if (entry.note == note)
            return entry.name;

    if (note >= 35 && note <= 81)
        return juce::MidiMessage::getRhythmInstrumentName (note);

    return juce::MidiMessage::getMidiNoteName (note, true, true, 4);
}

bool isDrumTrack (const te::AudioTrack& track)
{
    if (findTrackState (track).hasProperty (schema::drumMode))
        return findTrackState (track)[schema::drumMode];

    for (auto* plugin : track.pluginList)
        if (isInstrument (*plugin))
            return dynamic_cast<DrumSamplerPlugin*> (plugin) != nullptr;

    return false;
}

void setDrumTrack (te::AudioTrack& track, bool isDrums)
{
    findTrackState (track).setProperty (schema::drumMode, isDrums, nullptr);
}

std::optional<std::vector<DrumRow>> getSamplerSounds (const te::AudioTrack& track)
{
    // A row for each of the drum sampler's pads.
    for (auto* plugin : track.pluginList)
    {
        if (const auto* drums = dynamic_cast<const DrumSamplerPlugin*> (plugin))
        {
            std::vector<DrumRow> pads;

            for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
                pads.push_back ({ devices::DrumSampler::firstNote + pad, drums->getPadName (pad) });

            return pads;
        }
    }

    return std::nullopt;
}

std::vector<int> getNoteNumbersPlayed (const juce::Array<te::MidiClip*>& clips)
{
    std::set<int> played;

    for (auto* clip : clips)
        for (auto* note : clip->getSequence().getNotes())
            played.insert (note->getNoteNumber());

    return std::vector<int> (played.begin(), played.end());
}

std::vector<int> getDrumRowOrder (const te::AudioTrack& track)
{
    return parseNotes (findTrackState (track)[schema::drumOrder]);
}

void setDrumRowOrder (te::AudioTrack& track, const std::vector<int>& notes)
{
    findTrackState (track).setProperty (schema::drumOrder, joinNotes (notes), nullptr);
}

std::vector<int> getHiddenDrumRows (const te::AudioTrack& track)
{
    return parseNotes (findTrackState (track)[schema::drumHidden]);
}

void hideDrumRow (te::AudioTrack& track, int note)
{
    auto hidden = getHiddenDrumRows (track);

    if (std::find (hidden.begin(), hidden.end(), note) == hidden.end())
        hidden.push_back (note);

    findTrackState (track).setProperty (schema::drumHidden, joinNotes (hidden), nullptr);
}

void showAllDrumRows (te::AudioTrack& track)
{
    findTrackState (track).removeProperty (schema::drumHidden, nullptr);
}

//==============================================================================
void addNoteCommands (CommandRegistry& registry, Selection& selection, std::function<double()> getGridStepInBeats)
{
    const auto undoTarget = undoTargetOf (selection);

    struct Simple
    {
        const char* id;
        const char* name;
        const char* description;
        juce::StringArray synonyms;
        std::function<void (const std::vector<ClipNote>&)> action;
    };

    const auto gridStep = [getGridStepInBeats] { return te::BeatDuration::fromBeats (getGridStepInBeats()); };

    for (auto& simple :
         { Simple { "note.transposeUp",
                    "Transpose Up",
                    "Moves the selected notes up a semitone.",
                    { "pitch up", "semitone", "raise" },
                    [] (const std::vector<ClipNote>& notes) { transposeNotes (notes, 1); } },
           Simple { "note.transposeDown",
                    "Transpose Down",
                    "Moves the selected notes down a semitone.",
                    { "pitch down", "semitone", "lower" },
                    [] (const std::vector<ClipNote>& notes) { transposeNotes (notes, -1); } },
           Simple { "note.octaveUp",
                    "Octave Up",
                    "Moves the selected notes up an octave.",
                    { "transpose", "pitch up", "raise" },
                    [] (const std::vector<ClipNote>& notes) { transposeNotes (notes, 12); } },
           Simple { "note.octaveDown",
                    "Octave Down",
                    "Moves the selected notes down an octave.",
                    { "transpose", "pitch down", "lower" },
                    [] (const std::vector<ClipNote>& notes) { transposeNotes (notes, -12); } },
           Simple { "note.lengthen",
                    "Lengthen Notes",
                    "Makes the selected notes a step of the grid longer.",
                    { "longer", "extend", "length" },
                    [gridStep] (const std::vector<ClipNote>& notes) { changeNoteLengths (notes, gridStep()); } },
           Simple { "note.shorten",
                    "Shorten Notes",
                    "Makes the selected notes a step of the grid shorter.",
                    { "shorter", "length" },
                    [gridStep] (const std::vector<ClipNote>& notes) { changeNoteLengths (notes, te::BeatDuration() - gridStep()); } } })
    {
        registry.add ({ .id = simple.id,
                        .name = simple.name,
                        .category = "Note",
                        .description = simple.description,
                        .synonyms = simple.synonyms,
                        .perform = [&selection, action = simple.action] { action (getSelectedNotes (selection)); },
                        .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                        .undoTarget = undoTarget,
                        .keyPriority = Command::notePriority,
                        .repeatsWhileHeld = true });
    }

    registry.add ({ .id = "note.legato",
                    .name = "Legato",
                    .category = "Note",
                    .description = "Makes each selected note last until the next one starts.",
                    .synonyms = { "connect", "glue", "smooth", "length" },
                    .perform = [&selection]
                    { replaceSelectedNotes (selection, [] (std::vector<NoteData> notes) { return legato (std::move (notes)); }); },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority });

    // Slide and portamento notes, as FL Studio has them.
    registry.add ({ .id = "note.slide",
                    .name = "Slide Note",
                    .category = "Note",
                    .description = "Makes the selected notes slide notes, which play nothing themselves but glide the notes sounding as "
                                   "they start to their pitch over their length, as an 808 glides; or plain notes again. Phazite's own "
                                   "instruments play them.",
                    .synonyms = { "glide", "808", "pitch slide", "bend" },
                    .perform = [&selection] { toggleNotesSlide (getSelectedNotes (selection)); },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .isTicked = [&selection] { return areAllNotesSlides (getSelectedNotes (selection)); },
                    .keyPriority = Command::notePriority });

    registry.add ({ .id = "note.portamento",
                    .name = "Portamento",
                    .category = "Note",
                    .description = "Makes the selected notes glide in from the note played before each, or start on their own pitch "
                                   "again. Phazite's own instruments play the glide, over their Glide time.",
                    .synonyms = { "porta", "glide", "legato", "slide in" },
                    .perform = [&selection] { toggleNotesPortamento (getSelectedNotes (selection)); },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .isTicked = [&selection] { return areAllNotesPortamento (getSelectedNotes (selection)); },
                    .keyPriority = Command::notePriority });

    registry.add ({ .id = "note.chop",
                    .name = "Chop",
                    .category = "Note",
                    .description = "Cuts the selected notes into pieces a step of the grid long.",
                    .synonyms = { "slice", "cut", "split", "stutter" },
                    .perform =
                        [&selection, getGridStepInBeats]
                    {
                        replaceSelectedNotes (selection, [&getGridStepInBeats] (std::vector<NoteData> notes)
                                              { return chop (std::move (notes), getGridStepInBeats()); });
                    },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority });
}
} // namespace phazite

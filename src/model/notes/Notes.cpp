// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Notes.h"
#include "model/base/Schema.h"
#include "model/base/Toggles.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/Undo.h"
#include "model/devices/DrumSamplerPlugin.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace phazite
{
namespace
{
    // The scales Phazite offers, in the order its menus show them, with Tracktion's and the names projects save them by.
    struct SavedScale
    {
        Scale scale;
        te::Scale::ScaleType type;
        const char* name;
    };

    constexpr SavedScale savedScales[] = { { Scale::major, te::Scale::major, "major" },
                                           { Scale::minor, te::Scale::minor, "minor" },
                                           { Scale::dorian, te::Scale::dorian, "dorian" },
                                           { Scale::phrygian, te::Scale::phrygian, "phrygian" },
                                           { Scale::lydian, te::Scale::lydian, "lydian" },
                                           { Scale::mixolydian, te::Scale::mixolydian, "mixolydian" },
                                           { Scale::locrian, te::Scale::locrian, "locrian" },
                                           { Scale::harmonicMinor, te::Scale::harmonicMinor, "harmonicMinor" },
                                           { Scale::melodicMinor, te::Scale::melodicMinor, "melodicMinor" } };

    const SavedScale& findSavedScale (Scale scale)
    {
        for (const auto& saved : savedScales)
            if (saved.scale == scale)
                return saved;

        return savedScales[0];
    }

    // The chords the piano roll stamps, with Tracktion's.
    struct ChordStamp
    {
        ChordType type;
        te::Chord::ChordType tracktionType;
        const char* name;
    };

    constexpr ChordStamp chords[] = { { ChordType::major, te::Chord::majorTriad, "Major" },
                                      { ChordType::minor, te::Chord::minorTriad, "Minor" },
                                      { ChordType::dominantSeventh, te::Chord::dominatSeventhChord, "7" },
                                      { ChordType::majorSeventh, te::Chord::majorSeventhChord, "Maj7" },
                                      { ChordType::minorSeventh, te::Chord::minorSeventhChord, "m7" },
                                      { ChordType::suspendedSecond, te::Chord::suspendedSecond, "Sus2" },
                                      { ChordType::suspendedFourth, te::Chord::suspendedFourth, "Sus4" },
                                      { ChordType::diminished, te::Chord::diminishedTriad, "Dim" },
                                      { ChordType::augmented, te::Chord::augmentedTriad, "Aug" },
                                      { ChordType::majorSixth, te::Chord::majorSixthChord, "6" },
                                      { ChordType::minorSixth, te::Chord::minorSixthChord, "m6" },
                                      { ChordType::dominantNinth, te::Chord::dominantNinthChord, "9" },
                                      { ChordType::majorNinth, te::Chord::majorNinthChord, "Maj9" },
                                      { ChordType::power, te::Chord::powerChord, "Power" } };

    // A key, on a track's state or on LASTKEY in the song's Phazite state (model/base/Schema.h). A key exists exactly when
    // its root does.
    std::optional<Key> readKey (const juce::ValueTree& state)
    {
        if (! state.hasProperty (schema::phaziteKeyRoot))
            return std::nullopt;

        return Key { juce::jlimit (0, 11, (int) state[schema::phaziteKeyRoot]),
                     findScale (state[schema::phaziteKeyScale].toString()).value_or (Scale::major) };
    }

    void writeKey (juce::ValueTree state, const std::optional<Key>& key, juce::UndoManager* undo)
    {
        if (! key.has_value())
        {
            state.removeProperty (schema::phaziteKeyRoot, undo);
            state.removeProperty (schema::phaziteKeyScale, undo);
            return;
        }

        state.setProperty (schema::phaziteKeyRoot, key->root, undo);
        state.setProperty (schema::phaziteKeyScale, getSavedScaleName (key->scale), undo);
    }

    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);
    const auto tinyTime = te::TimeDuration::fromSeconds (1.0e-6);
    constexpr int maxPiecesPerNote = 256; // chopping into tinier pieces than this makes no musical sense

    juce::UndoManager* getUndoManager (te::MidiClip& clip)
    {
        return &clip.edit.getUndoManager();
    }

    // A song beat as a position among the clip's notes, which count from its content's start.
    te::BeatPosition toClipBeat (const te::MidiClip& clip, te::BeatPosition songBeat)
    {
        return te::BeatPosition::fromBeats ((songBeat - clip.getContentStartBeat()).inBeats());
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
    // cover another. A looped clip doesn't grow: more of it would only repeat its loop.
    void growToHold (te::MidiClip& clip, te::BeatPosition end)
    {
        if (clip.isLooping() || end <= clip.getEndBeat() + tinyBeats)
            return;

        const auto& tempo = clip.edit.tempoSequence;
        const auto bar = tempo.toBarsAndBeats (tempo.toTime (end - tinyBeats)).bars;
        auto newEnd = tempo.toTime (tempo.toBeats (te::tempo::BarsAndBeats { bar + 1, {}, 0 }));

        if (const auto roomEnd = findRoomEnd (clip))
            newEnd = std::min (newEnd, *roomEnd);

        if (newEnd > clip.getPosition().getEnd() + tinyTime)
            clip.setEnd (newEnd, true);
    }

    int clampPitch (int pitch)
    {
        return juce::jlimit (0, 127, pitch);
    }

    std::vector<int> getScaleSteps (Scale scale)
    {
        std::vector<int> steps;

        for (const auto step : te::Scale (toTracktion (scale)).getSteps())
            steps.push_back (step);

        return steps;
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

    te::SamplerPlugin* findSampler (const te::AudioTrack& track)
    {
        for (auto* plugin : track.pluginList)
            if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (plugin))
                return sampler;

        return nullptr;
    }
} // namespace

//==============================================================================
te::BeatPosition getNoteStart (const te::MidiClip& clip, const te::MidiNote& note)
{
    return clip.getContentStartBeat() + te::toDuration (note.getStartBeat());
}

NoteData getNoteData (const te::MidiClip& clip, const te::MidiNote& note)
{
    return { getClipID (clip),
             getNoteID (note),
             note.getNoteNumber(),
             fromTracktion (getNoteStart (clip, note)),
             fromTracktion (note.getLengthBeats()),
             note.getVelocity(),
             note.isMute() };
}

te::Scale::ScaleType toTracktion (Scale scale)
{
    return findSavedScale (scale).type;
}

Scale toScale (te::Scale::ScaleType type)
{
    if (type == te::Scale::ionian)
        return Scale::major; // the same notes
    if (type == te::Scale::aeolian)
        return Scale::minor;

    for (const auto& saved : savedScales)
        if (saved.type == type)
            return saved.scale;

    return Scale::major;
}

te::Chord::ChordType toTracktion (ChordType type)
{
    for (const auto& chord : chords)
        if (chord.type == type)
            return chord.tracktionType;

    return te::Chord::majorTriad;
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
namespace
{
    juce::int64 readNoteID (const juce::ValueTree& note)
    {
        return std::max<juce::int64> (0, static_cast<juce::int64> (note[schema::phaziteNoteID]));
    }

    // The highest note ID in a tree, or 0.
    juce::int64 findHighestNoteID (const juce::ValueTree& tree)
    {
        juce::int64 highest = 0;

        for (const auto& child : tree)
            highest = std::max (highest, child.hasType (te::IDs::NOTE) ? readNoteID (child) : findHighestNoteID (child));

        return highest;
    }

    // Among a tree's notes (a sequence's children), gives each that has no ID, or one that a note before it has, the next
    // ID above the highest among them, in their order; and the same in the trees below it. A clip and its aliases hold
    // the same notes in the same order, so they get the same IDs, as Tracktion's copying of one alias to the others
    // keeps them.
    void assignMissingNoteIDs (const juce::ValueTree& tree)
    {
        std::vector<juce::int64> ids;

        for (const auto& child : tree)
        {
            if (child.hasType (te::IDs::NOTE))
                ids.push_back (readNoteID (child));
            else
                assignMissingNoteIDs (child);
        }

        if (ids.empty())
            return;

        std::sort (ids.begin(), ids.end());

        if (ids.front() > 0 && std::adjacent_find (ids.begin(), ids.end()) == ids.end())
            return; // as usual, each has one of its own

        auto next = ids.back() + 1;
        std::set<juce::int64> used;

        for (auto child : tree)
            if (child.hasType (te::IDs::NOTE))
                if (const auto id = readNoteID (child); id == 0 || ! used.insert (id).second)
                    child.setProperty (schema::phaziteNoteID, next++, nullptr);
    }
} // namespace

NoteID getNoteID (const te::MidiNote& note)
{
    return NoteID::fromRaw ((std::uint64_t) readNoteID (note.state));
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

void assignNoteIDs (te::Edit& edit)
{
    assignMissingNoteIDs (edit.state);
}

NoteIDKeeper::NoteIDKeeper (te::Edit& edit)
    : state (edit.state)
{
    assignNoteIDs (edit);
    next = findHighestNoteID (state) + 1;
    state.addListener (this);
}

NoteIDKeeper::~NoteIDKeeper()
{
    state.removeListener (this);
}

void NoteIDKeeper::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    // A note added with its ID (put back by an undo, or copied with its clip's other notes) keeps it.
    if (child.hasType (te::IDs::NOTE))
    {
        if (const auto id = readNoteID (child); id > 0)
            next = std::max (next, id + 1);
        else
            child.setProperty (schema::phaziteNoteID, next++, nullptr);

        return;
    }

    // A clip, or a track, with its notes: those that have no ID yet are numbered in their clip (a MIDI file's, say).
    if (child.getNumChildren() > 0)
    {
        assignMissingNoteIDs (child);
        next = std::max (next, findHighestNoteID (child) + 1);
    }
}

//==============================================================================
te::SelectedMidiEvents* getSelectedMidiEvents (const te::SelectionManager& selection)
{
    return selection.getFirstItemOfType<te::SelectedMidiEvents>();
}

std::vector<ClipNote> getSelectedNotes (const te::SelectionManager& selection)
{
    std::vector<ClipNote> notes;

    if (auto* events = getSelectedMidiEvents (selection))
        for (auto* note : events->getSelectedNotes())
            if (auto* clip = events->clipForEvent (note))
                notes.push_back ({ clip, note });

    return notes;
}

bool hasSelectedNotes (const te::SelectionManager& selection)
{
    return ! getSelectedNotes (selection).empty();
}

juce::Array<te::MidiNote*> toMidiNotes (const std::vector<ClipNote>& notes)
{
    juce::Array<te::MidiNote*> midiNotes;

    for (const auto& n : notes)
        midiNotes.add (n.note);

    return midiNotes;
}

//==============================================================================
te::MidiNote* addNote (te::MidiClip& clip, int pitch, te::BeatPosition start, te::BeatDuration length, int velocity)
{
    start = std::max (start, clip.getStartBeat());
    length = std::max (length, toTracktion (getMinimumNoteLength()));
    growToHold (clip, start + length);
    return clip.getSequence().addNote (clampPitch (pitch), toClipBeat (clip, start), length, juce::jlimit (1, 127, velocity), 0,
                                       getUndoManager (clip));
}

void moveNote (te::MidiClip& clip, te::MidiNote& note, te::BeatPosition start, int pitch)
{
    start = std::max (start, clip.getStartBeat());
    growToHold (clip, start + note.getLengthBeats());

    auto* undoManager = getUndoManager (clip);
    note.setStartAndLength (toClipBeat (clip, start), note.getLengthBeats(), undoManager);
    note.setNoteNumber (clampPitch (pitch), undoManager);
}

void setNoteLength (te::MidiClip& clip, te::MidiNote& note, te::BeatDuration length)
{
    length = std::max (length, toTracktion (getMinimumNoteLength()));
    growToHold (clip, getNoteStart (clip, note) + length);
    note.setStartAndLength (note.getStartBeat(), length, getUndoManager (clip));
}

void deleteNotes (const std::vector<ClipNote>& notes)
{
    for (const auto& n : notes)
        if (n.clip != nullptr && n.note != nullptr)
            n.clip->getSequence().removeNote (*n.note, getUndoManager (*n.clip));
}

void transposeNotes (const std::vector<ClipNote>& notes, int semitones)
{
    // As far as the highest or lowest note can go, so that chords keep their shape.
    for (const auto& n : notes)
        semitones = juce::jlimit (-n.note->getNoteNumber(), 127 - n.note->getNoteNumber(), semitones);

    if (semitones == 0)
        return;

    for (const auto& n : notes)
        n.note->setNoteNumber (n.note->getNoteNumber() + semitones, getUndoManager (*n.clip));
}

void nudgeNotes (const std::vector<ClipNote>& notes, te::BeatDuration amount)
{
    // The note nearest its clip's start decides how far back they can go.
    for (const auto& n : notes)
        amount = std::max (amount, n.clip->getStartBeat() - getNoteStart (*n.clip, *n.note));

    for (const auto& n : notes)
        moveNote (*n.clip, *n.note, getNoteStart (*n.clip, *n.note) + amount, n.note->getNoteNumber());
}

void changeNoteLengths (const std::vector<ClipNote>& notes, te::BeatDuration change)
{
    for (const auto& n : notes)
        setNoteLength (*n.clip, *n.note, n.note->getLengthBeats() + change);
}

bool areAllNotesMuted (const std::vector<ClipNote>& notes)
{
    return areAllOn (notes, [] (const ClipNote& n) { return n.note->isMute(); });
}

void toggleNotesMuted (const std::vector<ClipNote>& notes)
{
    toggleAll (
        notes, [] (const ClipNote& n) { return n.note->isMute(); },
        [] (const ClipNote& n, bool on) { n.note->setMute (on, getUndoManager (*n.clip)); });
}

void setNoteVelocities (const std::vector<ClipNote>& notes, int velocity)
{
    for (const auto& n : notes)
        n.note->setVelocity (juce::jlimit (1, 127, velocity), getUndoManager (*n.clip));
}

void changeNoteVelocities (const std::vector<ClipNote>& notes, int change)
{
    if (notes.empty())
        return;

    const UndoStep step (notes.front().clip->edit, "Change Velocity");

    for (const auto& n : notes)
        setNoteVelocities ({ n }, n.note->getVelocity() + change);
}

std::vector<ClipNote> duplicateNotes (const std::vector<ClipNote>& notes)
{
    if (notes.empty())
        return {};

    // The copies go right after the whole selection.
    auto start = getNoteStart (*notes.front().clip, *notes.front().note), end = start;

    for (const auto& n : notes)
    {
        start = std::min (start, getNoteStart (*n.clip, *n.note));
        end = std::max (end, getNoteStart (*n.clip, *n.note) + n.note->getLengthBeats());
    }

    auto copies = getNoteData (notes);

    for (auto& copy : copies)
        copy.start = copy.start + fromTracktion (end - start);

    return replaceNotes (notes.front().clip->edit, {}, copies);
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

        if (auto* note = addNote (*clip, data.pitch, toTracktion (data.start), toTracktion (data.length), data.velocity))
        {
            if (data.muted)
                note->setMute (true, getUndoManager (*clip));

            // A note that replaces one of the old ones (a quantized or transposed note, a chopped note's first piece) keeps
            // that one's ID, if no other note of the clip has it now; a copy, whose original is still there, gets its own.
            if (data.id.isValid() && findNote (*clip, data.id) == nullptr)
                note->state.setProperty (schema::phaziteNoteID, (juce::int64) data.id.getRaw(), nullptr);

            added.push_back ({ clip, note });
        }
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

std::vector<ClipNote> replaceSelectedNotes (te::SelectionManager& selection,
                                            const std::function<std::vector<NoteData> (std::vector<NoteData>)>& transform)
{
    // The selection's notes object, before the edit replaces the notes in it.
    auto* events = getSelectedMidiEvents (selection);
    const auto notes = getSelectedNotes (selection);

    if (events == nullptr || notes.empty())
        return {};

    auto added = replaceNotes (notes.front().clip->edit, notes, transform (getNoteData (notes)));
    events->setSelected (selection, toMidiNotes (added), false);
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
Key getKeyAt (const te::Edit& edit, te::BeatPosition beat)
{
    const auto& pitch = edit.pitchSequence.getPitchAtBeat (beat);
    return { floorModulo (pitch.getPitch(), 12), toScale (pitch.getScale()) };
}

juce::String getScaleName (Scale scale)
{
    return te::Scale::getNameForType (toTracktion (scale));
}

juce::String describeKey (const Key& key)
{
    return juce::MidiMessage::getMidiNoteName (key.root, true, false, 0) + " " + getScaleName (key.scale).toLowerCase();
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
    return readKey (track.state);
}

void setTrackKey (te::Track& track, const std::optional<Key>& key)
{
    writeKey (track.state, key, &track.edit.getUndoManager());

    // What new tracks get; not an edit to undo.
    if (key.has_value())
        writeKey (getPhaziteState (track.edit).getOrCreateChildWithName (schema::LASTKEY, nullptr), key, nullptr);
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
        writeKey (track.state, last, nullptr);
}

void setLastKey (te::Edit& edit, const Key& key)
{
    writeKey (getPhaziteState (edit).getOrCreateChildWithName (schema::LASTKEY, nullptr), key, nullptr);
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

std::vector<int> getChordPitches (int root, ChordType type)
{
    std::vector<int> pitches;

    for (const auto step : te::Chord (toTracktion (type)).getSteps())
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
    if (track.state.hasProperty (schema::phaziteDrumMode))
        return track.state[schema::phaziteDrumMode];

    for (auto* plugin : track.pluginList)
        if (plugin->isSynth())
            return dynamic_cast<te::SamplerPlugin*> (plugin) != nullptr || dynamic_cast<DrumSamplerPlugin*> (plugin) != nullptr;

    return false;
}

void setDrumTrack (te::AudioTrack& track, bool isDrums)
{
    track.state.setProperty (schema::phaziteDrumMode, isDrums, nullptr);
}

std::optional<std::vector<DrumRow>> getSamplerSounds (const te::AudioTrack& track)
{
    // Phazite's drum sampler: a row for each pad.
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

    auto* sampler = findSampler (track);

    if (sampler == nullptr)
        return std::nullopt;

    std::vector<DrumRow> sounds;

    for (int i = 0; i < sampler->getNumSounds(); ++i)
    {
        const auto note = sampler->getKeyNote (i);

        if (std::none_of (sounds.begin(), sounds.end(), [note] (const DrumRow& row) { return row.note == note; }))
            sounds.push_back ({ note, sampler->getSoundName (i) });
    }

    return sounds;
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
    return parseNotes (track.state[schema::phaziteDrumOrder]);
}

void setDrumRowOrder (te::AudioTrack& track, const std::vector<int>& notes)
{
    track.state.setProperty (schema::phaziteDrumOrder, joinNotes (notes), nullptr);
}

std::vector<int> getHiddenDrumRows (const te::AudioTrack& track)
{
    return parseNotes (track.state[schema::phaziteDrumHidden]);
}

void hideDrumRow (te::AudioTrack& track, int note)
{
    auto hidden = getHiddenDrumRows (track);

    if (std::find (hidden.begin(), hidden.end(), note) == hidden.end())
        hidden.push_back (note);

    track.state.setProperty (schema::phaziteDrumHidden, joinNotes (hidden), nullptr);
}

void showAllDrumRows (te::AudioTrack& track)
{
    track.state.removeProperty (schema::phaziteDrumHidden, nullptr);
}

//==============================================================================
void addNoteCommands (CommandRegistry& registry, te::SelectionManager& selection, std::function<double()> getGridStepInBeats)
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

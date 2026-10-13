// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Notes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace phazite
{
namespace
{
    const juce::Identifier drumModeProperty { "phaziteDrumMode" }, drumOrderProperty { "phaziteDrumOrder" },
        drumHiddenProperty { "phaziteDrumHidden" };

    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);
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

    // Makes the clip end at the end of the bar a song beat is in, if the clip ends before the beat.
    void growToHold (te::MidiClip& clip, te::BeatPosition end)
    {
        if (end <= clip.getEndBeat() + tinyBeats)
            return;

        const auto& tempo = clip.edit.tempoSequence;
        const auto bar = tempo.toBarsAndBeats (tempo.toTime (end - tinyBeats)).bars;
        clip.setEnd (tempo.toTime (tempo.toBeats (te::tempo::BarsAndBeats { bar + 1, {}, 0 })), true);
    }

    int clampPitch (int pitch)
    {
        return juce::jlimit (0, 127, pitch);
    }

    std::vector<int> getScaleSteps (te::Scale::ScaleType type)
    {
        std::vector<int> steps;

        for (const auto step : te::Scale (type).getSteps())
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

NoteData getNoteData (te::MidiClip& clip, const te::MidiNote& note)
{
    return { &clip, note.getNoteNumber(), getNoteStart (clip, note), note.getLengthBeats(), note.getVelocity(), note.isMute() };
}

std::vector<NoteData> getNoteData (const std::vector<ClipNote>& notes)
{
    std::vector<NoteData> data;

    for (const auto& n : notes)
        if (n.clip != nullptr && n.note != nullptr)
            data.push_back (getNoteData (*n.clip, *n.note));

    return data;
}

te::BeatDuration getMinimumNoteLength()
{
    return te::BeatDuration::fromBeats (1.0 / 64.0);
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

//==============================================================================
te::MidiNote* addNote (te::MidiClip& clip, int pitch, te::BeatPosition start, te::BeatDuration length, int velocity)
{
    start = std::max (start, clip.getStartBeat());
    length = std::max (length, getMinimumNoteLength());
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
    length = std::max (length, getMinimumNoteLength());
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
    return ! notes.empty() && std::all_of (notes.begin(), notes.end(), [] (const ClipNote& n) { return n.note->isMute(); });
}

void toggleNotesMuted (const std::vector<ClipNote>& notes)
{
    const auto mute = ! areAllNotesMuted (notes);

    for (const auto& n : notes)
        n.note->setMute (mute, getUndoManager (*n.clip));
}

void setNoteVelocities (const std::vector<ClipNote>& notes, int velocity)
{
    for (const auto& n : notes)
        n.note->setVelocity (juce::jlimit (1, 127, velocity), getUndoManager (*n.clip));
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
        copy.start = copy.start + (end - start);

    return replaceNotes ({}, copies);
}

std::vector<ClipNote> replaceNotes (const std::vector<ClipNote>& old, const std::vector<NoteData>& replacements)
{
    deleteNotes (old);
    std::vector<ClipNote> added;

    for (const auto& data : replacements)
    {
        if (data.clip == nullptr)
            continue;

        if (auto* note = addNote (*data.clip, data.pitch, data.start, data.length, data.velocity))
        {
            if (data.muted)
                note->setMute (true, getUndoManager (*data.clip));

            added.push_back ({ data.clip, note });
        }
    }

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
        note.start = te::BeatPosition::fromBeats (quantizeBeat (note.start.inBeats()));

        if (settings.ends)
            note.length = std::max (getMinimumNoteLength(), te::BeatDuration::fromBeats (quantizeBeat (end) - note.start.inBeats()));
    }

    return notes;
}

std::vector<NoteData> legato (std::vector<NoteData> notes, double gapBeats)
{
    // The starts in each clip, in order; a note lasts until the next start after its own.
    std::map<te::MidiClip*, std::vector<double>> starts;

    for (const auto& note : notes)
        starts[note.clip].push_back (note.start.inBeats());

    for (auto& [clip, beats] : starts)
        std::sort (beats.begin(), beats.end());

    for (auto& note : notes)
    {
        const auto& clipStarts = starts[note.clip];
        const auto next = std::upper_bound (clipStarts.begin(), clipStarts.end(), note.start.inBeats() + 1.0e-6);

        if (next != clipStarts.end())
            note.length = std::max (getMinimumNoteLength(), te::BeatDuration::fromBeats (*next - note.start.inBeats() - gapBeats));
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
            auto piece = note;
            piece.start = note.start + te::BeatDuration::fromBeats (offset);
            piece.length = te::BeatDuration::fromBeats (std::min (pieceBeats, length - offset));
            pieces.push_back (piece);
        }
    }

    return pieces;
}

//==============================================================================
Key getKeyAt (const te::Edit& edit, te::BeatPosition beat)
{
    const auto& pitch = edit.pitchSequence.getPitchAtBeat (beat);
    return { floorModulo (pitch.getPitch(), 12), pitch.getScale() };
}

juce::String describeKey (const Key& key)
{
    return juce::MidiMessage::getMidiNoteName (key.root, true, false, 0) + " " + te::Scale::getNameForType (key.scale).toLowerCase();
}

bool isInKey (int pitch, const Key& key)
{
    const auto steps = getScaleSteps (key.scale);
    return std::find (steps.begin(), steps.end(), floorModulo (pitch - key.root, 12)) != steps.end();
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
    static const std::vector<ChordShape> shapes { { "Major", te::Chord::majorTriad },      { "Minor", te::Chord::minorTriad },
                                                  { "7", te::Chord::dominatSeventhChord }, { "Maj7", te::Chord::majorSeventhChord },
                                                  { "m7", te::Chord::minorSeventhChord },  { "Sus2", te::Chord::suspendedSecond },
                                                  { "Sus4", te::Chord::suspendedFourth },  { "Dim", te::Chord::diminishedTriad },
                                                  { "Aug", te::Chord::augmentedTriad },    { "6", te::Chord::majorSixthChord },
                                                  { "m6", te::Chord::minorSixthChord },    { "9", te::Chord::dominantNinthChord },
                                                  { "Maj9", te::Chord::majorNinthChord },  { "Power", te::Chord::powerChord } };
    return shapes;
}

std::vector<int> getChordPitches (int root, te::Chord::ChordType type)
{
    std::vector<int> pitches;

    for (const auto step : te::Chord (type).getSteps())
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
    if (track.state.hasProperty (drumModeProperty))
        return track.state[drumModeProperty];

    for (auto* plugin : track.pluginList)
        if (plugin->isSynth())
            return dynamic_cast<te::SamplerPlugin*> (plugin) != nullptr;

    return false;
}

void setDrumTrack (te::AudioTrack& track, bool isDrums)
{
    track.state.setProperty (drumModeProperty, isDrums, nullptr);
}

std::vector<DrumRow> getDrumRows (const te::AudioTrack& track, const juce::Array<te::MidiClip*>& clips)
{
    std::vector<DrumRow> rows;
    const auto contains = [&rows] (int note)
    { return std::any_of (rows.begin(), rows.end(), [note] (const DrumRow& row) { return row.note == note; }); };

    // The sampler's pads, or else the usual drums.
    if (auto* sampler = findSampler (track))
    {
        for (int i = 0; i < sampler->getNumSounds(); ++i)
            if (const auto note = sampler->getKeyNote (i); ! contains (note))
                rows.push_back ({ note, sampler->getSoundName (i) });
    }
    else
    {
        for (const auto note : { 36, 37, 38, 39, 42, 46, 41, 45, 48, 49, 51 })
            rows.push_back ({ note, getDrumName (note) });
    }

    // Any other notes the clips play.
    std::set<int> used;

    for (auto* clip : clips)
        for (auto* note : clip->getSequence().getNotes())
            used.insert (note->getNoteNumber());

    for (const auto note : used)
        if (! contains (note))
            rows.push_back ({ note, getDrumName (note) });

    // The track's order first; rows it doesn't mention keep theirs, after them.
    const auto order = parseNotes (track.state[drumOrderProperty]);
    const auto rank = [&order] (int note)
    {
        const auto found = std::find (order.begin(), order.end(), note);
        return found != order.end() ? (int) (found - order.begin()) : (int) order.size();
    };

    std::stable_sort (rows.begin(), rows.end(), [&rank] (const DrumRow& a, const DrumRow& b) { return rank (a.note) < rank (b.note); });

    const auto hidden = parseNotes (track.state[drumHiddenProperty]);
    rows.erase (std::remove_if (rows.begin(), rows.end(), [&hidden] (const DrumRow& row)
                                { return std::find (hidden.begin(), hidden.end(), row.note) != hidden.end(); }),
                rows.end());
    return rows;
}

void moveDrumRow (te::AudioTrack& track, const std::vector<DrumRow>& shown, int note, int newIndex)
{
    std::vector<int> order;

    for (const auto& row : shown)
        if (row.note != note)
            order.push_back (row.note);

    order.insert (order.begin() + juce::jlimit (0, (int) order.size(), newIndex), note);
    track.state.setProperty (drumOrderProperty, joinNotes (order), nullptr);
}

void hideDrumRow (te::AudioTrack& track, int note)
{
    auto hidden = parseNotes (track.state[drumHiddenProperty]);

    if (std::find (hidden.begin(), hidden.end(), note) == hidden.end())
        hidden.push_back (note);

    track.state.setProperty (drumHiddenProperty, joinNotes (hidden), nullptr);
}

void showAllDrumRows (te::AudioTrack& track)
{
    track.state.removeProperty (drumHiddenProperty, nullptr);
}

//==============================================================================
void addNoteCommands (CommandRegistry& registry, te::SelectionManager& selection, std::function<double()> getGridStepInBeats)
{
    const auto getEdit = [&selection] { return selection.getEdit(); };
    const auto hasNotes = [&selection] { return ! getSelectedNotes (selection).empty(); };

    // The notes' new versions replace them and are selected in their place.
    const auto replaceSelected = [&selection] (std::function<std::vector<NoteData> (std::vector<NoteData>)> transform)
    {
        return [&selection, transform]
        {
            auto* events = getSelectedMidiEvents (selection);
            const auto notes = getSelectedNotes (selection);

            if (events == nullptr || notes.empty())
                return;

            juce::Array<te::MidiNote*> added;

            for (const auto& n : replaceNotes (notes, transform (getNoteData (notes))))
                added.add (n.note);

            events->setSelected (selection, added, false);
        };
    };

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
                        .isEnabled = hasNotes,
                        .undoTarget = getEdit,
                        .keyPriority = Command::notePriority });
    }

    registry.add ({ .id = "note.legato",
                    .name = "Legato",
                    .category = "Note",
                    .description = "Makes each selected note last until the next one starts.",
                    .synonyms = { "connect", "glue", "smooth", "length" },
                    .perform = replaceSelected ([] (std::vector<NoteData> notes) { return legato (std::move (notes)); }),
                    .isEnabled = hasNotes,
                    .undoTarget = getEdit,
                    .keyPriority = Command::notePriority });

    registry.add ({ .id = "note.chop",
                    .name = "Chop",
                    .category = "Note",
                    .description = "Cuts the selected notes into pieces a step of the grid long.",
                    .synonyms = { "slice", "cut", "split", "stutter" },
                    .perform = replaceSelected ([getGridStepInBeats] (std::vector<NoteData> notes)
                                                { return chop (std::move (notes), getGridStepInBeats()); }),
                    .isEnabled = hasNotes,
                    .undoTarget = getEdit,
                    .keyPriority = Command::notePriority });
}
} // namespace phazite

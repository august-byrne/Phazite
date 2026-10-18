// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChordModel.h"
#include "Progressions.h"
#include "internal/Chords.h"
#include "model/arrangement/internal/Sections.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    constexpr double tiny = 1.0e-6;

    te::MidiClip* findMidiClip (const ProjectContext& context, ClipID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, toTracktion (id))) : nullptr;
    }

    LaneChord toLaneChord (const PlacedChord& chord)
    {
        return { chord.range, chord.numeral, chord.key, getChordNameIn (chord.numeral, chord.key),
                 getPitchClassesIn (chord.numeral, chord.key) };
    }

    // Runs a change of the lane as an undo step of its own.
    template <typename Change>
    void editLane (ProjectContext& context, const char* stepName, Change&& change)
    {
        if (auto* edit = getEdit (context))
        {
            const UndoStep step (*edit, stepName);
            changeLane (*edit, [&] { change (*edit); });
        }
    }

    void addChoice (std::vector<Numeral>& choices, const Numeral& numeral, const Key& key)
    {
        // Each chord once, however it is written.
        const auto root = getRootIn (numeral, key);

        for (const auto& choice : choices)
            if (getRootIn (choice, key) == root && choice.type == numeral.type)
                return;

        choices.push_back (numeral);
    }

    Numeral makeNumeral (int degree, int accidental, ChordType type)
    {
        return { degree, accidental, type, 0 };
    }
} // namespace

//==============================================================================
std::vector<LaneChord> getLaneChords (const ProjectContext& context)
{
    std::vector<LaneChord> chords;

    if (auto* edit = getEdit (context))
        for (const auto& chord : getLaneChords (*edit))
            chords.push_back (toLaneChord (chord));

    return chords;
}

std::optional<LaneChord> getLaneChordAt (const ProjectContext& context, BeatPosition where)
{
    for (const auto& chord : getLaneChords (context))
        if (chord.range.contains (where))
            return chord;

    return std::nullopt;
}

bool hasLaneChords (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr && ! getLaneChords (*edit).empty();
}

std::vector<LaneKeyChange> getLaneKeyChanges (const ProjectContext& context)
{
    std::vector<LaneKeyChange> changes;

    if (auto* edit = getEdit (context))
        for (const auto& key : getLaneKeys (*edit))
            changes.push_back ({ key.start, key.key });
    else
        changes.push_back ({});

    return changes;
}

Key getLaneKeyAt (const ProjectContext& context, BeatPosition where)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? getLaneKeyAt (*edit, where) : Key {};
}

//==============================================================================
void applyProgression (ProjectContext& context, const Progression& progression, BeatRange range)
{
    if (range.isEmpty())
        return;

    editLane (context, "Apply Chord Progression", [&] (te::Edit& edit) { putProgression (edit, progression, range); });
}

void addLaneChord (ProjectContext& context, BeatRange range, const Numeral& numeral)
{
    if (range.isEmpty())
        return;

    editLane (context, "Add Chord", [&] (te::Edit& edit) { putLaneChord (edit, range, numeral); });
}

void changeLaneChord (ProjectContext& context, BeatPosition chordStart, const Numeral& numeral)
{
    if (auto* edit = getEdit (context); edit != nullptr && hasLaneChordAt (*edit, chordStart))
        editLane (context, "Change Chord", [&] (te::Edit& e) { setLaneChordNumeral (e, chordStart, numeral); });
}

void deleteLaneChord (ProjectContext& context, BeatPosition chordStart)
{
    if (auto* edit = getEdit (context); edit != nullptr && hasLaneChordAt (*edit, chordStart))
        editLane (context, "Delete Chord", [&] (te::Edit& e) { removeLaneChord (e, chordStart); });
}

void setLaneChordEnd (ProjectContext& context, BeatPosition chordStart, BeatPosition end)
{
    if (auto* edit = getEdit (context);
        edit != nullptr && hasLaneChordAt (*edit, chordStart) && end > chordStart + BeatDuration::fromBeats (tiny))
        editLane (context, "Change Chord Length", [&] (te::Edit& e) { setLaneChordEnd (e, chordStart, end); });
}

void setLaneKey (ProjectContext& context, BeatPosition from, const Key& key)
{
    editLane (context, "Change Key", [&] (te::Edit& edit) { setLaneKey (edit, from, key); });
}

void removeLaneKeyChange (ProjectContext& context, BeatPosition at)
{
    if (at > BeatPosition::fromBeats (1.0e-4))
        editLane (context, "Remove Key Change", [&] (te::Edit& edit) { removeLaneKeyChange (edit, at); });
}

std::vector<Numeral> getChordChoices (const ProjectContext& context, BeatPosition where)
{
    const auto key = getLaneKeyAt (context, where);
    auto choices = getDiatonicChords (key);

    if (hasMajorThird (key))
    {
        // Borrowed from the parallel minor, then the dominants of ii, iii, V and vi.
        for (const auto& numeral :
             { makeNumeral (4, 0, ChordType::minor), makeNumeral (7, -1, ChordType::major), makeNumeral (6, -1, ChordType::major),
               makeNumeral (3, -1, ChordType::major), makeNumeral (6, 0, ChordType::major), makeNumeral (7, 0, ChordType::major),
               makeNumeral (2, 0, ChordType::major), makeNumeral (3, 0, ChordType::major) })
            addChoice (choices, numeral, key);
    }
    else
    {
        // Borrowed from the parallel major and the harmonic minor, and the chord a semitone above the root.
        for (const auto& numeral : { makeNumeral (5, 0, ChordType::major), makeNumeral (4, 0, ChordType::major),
                                     makeNumeral (1, 0, ChordType::major), makeNumeral (2, -1, ChordType::major) })
            addChoice (choices, numeral, key);
    }

    return choices;
}

//==============================================================================
std::optional<BeatRange> getSelectedSectionsRange (const ProjectContext& context)
{
    if (getEdit (context) == nullptr)
        return std::nullopt;

    const auto sections = getSelectedSections (getSelection (context));

    if (sections.isEmpty())
        return std::nullopt;

    auto start = fromTracktion (sections.getFirst()->getStartBeat()), end = fromTracktion (sections.getFirst()->getEndBeat());

    for (auto* section : sections)
    {
        start = std::min (start, fromTracktion (section->getStartBeat()));
        end = std::max (end, fromTracktion (section->getEndBeat()));
    }

    return BeatRange (start, end);
}

std::optional<BeatRange> getSelectedTimeRange (const ProjectContext& context)
{
    const auto& range = context.getSongRange();

    if (getEdit (context) == nullptr || range.isEmpty())
        return std::nullopt;

    return range.getRange();
}

std::optional<BeatRange> getWholeSongRange (const ProjectContext& context)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return std::nullopt;

    const auto end = fromTracktion (edit->tempoSequence.toBeats (te::toPosition (edit->getLength())));

    if (end <= BeatPosition::fromBeats (tiny))
        return std::nullopt;

    return BeatRange (BeatPosition(), end);
}

std::optional<BeatRange> getChordPartRange (const ProjectContext& context)
{
    if (const auto range = getSelectedTimeRange (context))
        return range;

    if (const auto range = getSelectedSectionsRange (context))
        return range;

    const auto chords = getLaneChords (context);

    if (chords.empty())
        return std::nullopt;

    return BeatRange (chords.front().range.getStart(), chords.back().range.getEnd());
}

BeatDuration getProgressionLength (const ProjectContext& context, const Progression& progression, BeatPosition from)
{
    auto* edit = getEdit (context);
    auto beat = from;

    for (const auto& chord : progression.chords)
    {
        const auto beatsPerBar = edit != nullptr ? edit->tempoSequence.getTimeSigAt (toTracktion (beat)).numerator.get() : 4;
        beat += BeatDuration::fromBeats (chord.bars * std::max (1, beatsPerBar));
    }

    return beat - from;
}

//==============================================================================
std::optional<ClipID> makeChordPart (ProjectContext& context, PartStyle style, BeatRange range)
{
    auto* edit = getEdit (context);

    if (edit == nullptr || range.isEmpty())
        return std::nullopt;

    const UndoStep step (*edit, getMakePartName (getPartKind (style)));

    if (auto* clip = addChordPart (*edit, style, range, &getSelection (context)))
        return getClipID (*clip);

    return std::nullopt;
}

std::optional<ClipID> makeChordPartHere (ProjectContext& context, PartStyle style)
{
    if (const auto range = getChordPartRange (context); range.has_value() && canMakeChordPart (context))
        return makeChordPart (context, style, *range);

    return std::nullopt;
}

bool canMakeChordPart (const ProjectContext& context)
{
    const auto range = getChordPartRange (context);

    if (! range.has_value())
        return false;

    for (const auto& chord : getLaneChords (context))
        if (chord.range.overlaps (*range))
            return true;

    return false;
}

juce::String getMakePartName (PartKind kind)
{
    switch (kind)
    {
        case PartKind::bass:
            return "Make Bass Line";
        case PartKind::arpeggio:
            return "Make Arpeggio";
        case PartKind::chords:
        default:
            return "Make Chord Part";
    }
}

std::optional<PartStyle> getChordPart (const ProjectContext& context, ClipID id)
{
    auto* clip = findMidiClip (context, id);
    return clip != nullptr ? getChordPartStyle (*clip) : std::nullopt;
}

bool followsChords (const ProjectContext& context, ClipID id)
{
    auto* clip = findMidiClip (context, id);
    return clip != nullptr && followsChords (*clip);
}

void setFollowsChords (ProjectContext& context, ClipID id, bool follow)
{
    auto* clip = findMidiClip (context, id);

    if (clip == nullptr || ! getChordPartStyle (*clip).has_value() || followsChords (*clip) == follow)
        return;

    const UndoStep step (clip->edit, follow ? "Follow the Chords" : "Stop Following the Chords");
    setChordPart (*clip, follow ? getChordPartStyle (*clip) : std::nullopt);
}

//==============================================================================
bool writeProgressionPreview (const ProjectContext& context, const Progression& progression, BeatPosition from, const juce::File& file)
{
    // The chords as they would go into the song from the beat, held, with their roots under them.
    std::vector<PlacedChord> chords;
    const auto key = getLaneKeyAt (context, from);
    const auto progressionKey = getKeyForProgression (key, progression.scale);
    auto* edit = getEdit (context);
    auto beat = BeatPosition();

    for (const auto& chord : progression.chords)
    {
        const auto beatsPerBar =
            edit != nullptr ? edit->tempoSequence.getTimeSigAt (toTracktion (from + (beat - BeatPosition()))).numerator.get() : 4;
        const auto length = BeatDuration::fromBeats (chord.bars * std::max (1, beatsPerBar));
        chords.push_back ({ { beat, length }, chord.numeral, progressionKey });
        beat += length;
    }

    const auto range = BeatRange (BeatPosition(), beat);
    auto notes = makePartNotes (chords, PartStyle::sustainedTriads, range, {});

    for (const auto& note : makePartNotes (chords, PartStyle::bassRoots, range, {}))
        notes.push_back (note);

    constexpr int ticksPerBeat = 960;
    juce::MidiMessageSequence sequence;

    for (const auto& note : notes)
    {
        sequence.addEvent (juce::MidiMessage::noteOn (1, note.pitch, (juce::uint8) note.velocity), note.start.inBeats() * ticksPerBeat);
        sequence.addEvent (juce::MidiMessage::noteOff (1, note.pitch), note.getEnd().inBeats() * ticksPerBeat);
    }

    sequence.addEvent (juce::MidiMessage::endOfTrack(), range.getEnd().inBeats() * ticksPerBeat);
    sequence.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerBeat);
    midi.addTrack (sequence);

    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && midi.writeTo (stream);
}

//==============================================================================
void addChordCommands (CommandRegistry& registry, ProjectContext& context)
{
    const auto undoTarget = [&context] { return context.getUndoManager(); };

    registry.add ({ .id = "chords.makeChordPart",
                    .name = "Make Chord Part",
                    .category = "Chords",
                    .description = "Adds a track that plays the chord lane's chords, held, over the selected range or sections, "
                                   "or all of the chords. It changes with the chords until you edit its notes.",
                    .synonyms = { "pad", "chords", "harmony", "generate", "accompaniment", "keys" },
                    .perform = [&context] { makeChordPartHere (context, PartStyle::sustainedTriads); },
                    .isEnabled = [&context] { return canMakeChordPart (context); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "chords.makeBassLine",
                    .name = "Make Bass Line",
                    .category = "Chords",
                    .description = "Adds a track that plays the chord lane's roots as a bass line, over the selected range or "
                                   "sections, or all of the chords. It changes with the chords until you edit its notes.",
                    .synonyms = { "bass", "bassline", "roots", "generate", "low end" },
                    .perform = [&context] { makeChordPartHere (context, PartStyle::bassRoots); },
                    .isEnabled = [&context] { return canMakeChordPart (context); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "chords.makeArpeggio",
                    .name = "Make Arpeggio",
                    .category = "Chords",
                    .description = "Adds a track that plays the chord lane's chords one note at a time, in eighth notes, over the "
                                   "selected range or sections, or all of the chords. It changes with the chords until you edit "
                                   "its notes.",
                    .synonyms = { "arp", "arpeggiator", "broken chords", "generate", "melody" },
                    .perform = [&context] { makeChordPartHere (context, PartStyle::arpeggioUp); },
                    .isEnabled = [&context] { return canMakeChordPart (context); },
                    .undoTarget = undoTarget });
}
} // namespace phazite

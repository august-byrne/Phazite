// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChordModel.h"
#include "Progressions.h"
#include "internal/Chords.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite
{
namespace
{
    constexpr double tiny = 1.0e-6;

    LaneChord toLaneChord (const PlacedChord& chord)
    {
        return { chord.range, chord.numeral, chord.key, getChordNameIn (chord.numeral, chord.key),
                 getPitchClassesIn (chord.numeral, chord.key) };
    }

    // Runs a change of the lane as an undo step of its own.
    template <typename Change>
    void editLane (ProjectContext& context, const char* stepName, Change&& change)
    {
        if (auto* song = findSong (context))
        {
            const UndoStep step (*song, stepName);
            change (*song);
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

    if (auto* song = findSong (context))
        for (const auto& chord : getLaneChords (*song))
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
    auto* song = findSong (context);
    return song != nullptr && ! getLaneChords (*song).empty();
}

std::vector<LaneKeyChange> getLaneKeyChanges (const ProjectContext& context)
{
    std::vector<LaneKeyChange> changes;

    if (auto* song = findSong (context))
        for (const auto& key : getLaneKeys (*song))
            changes.push_back ({ key.start, key.key });
    else
        changes.push_back ({});

    return changes;
}

Key getLaneKeyAt (const ProjectContext& context, BeatPosition where)
{
    auto* song = findSong (context);
    return song != nullptr ? getLaneKeyAt (*song, where) : Key {};
}

//==============================================================================
void applyProgression (ProjectContext& context, const Progression& progression, BeatRange range)
{
    if (range.isEmpty())
        return;

    editLane (context, "Apply Chord Progression", [&] (Song& song) { putProgression (song, progression, range); });
}

void addLaneChord (ProjectContext& context, BeatRange range, const Numeral& numeral)
{
    if (range.isEmpty())
        return;

    editLane (context, "Add Chord", [&] (Song& song) { putLaneChord (song, range, numeral); });
}

void changeLaneChord (ProjectContext& context, BeatPosition chordStart, const Numeral& numeral)
{
    if (auto* played = findSong (context); played != nullptr && hasLaneChordAt (*played, chordStart))
        editLane (context, "Change Chord", [&] (Song& song) { setLaneChordNumeral (song, chordStart, numeral); });
}

void deleteLaneChord (ProjectContext& context, BeatPosition chordStart)
{
    if (auto* played = findSong (context); played != nullptr && hasLaneChordAt (*played, chordStart))
        editLane (context, "Delete Chord", [&] (Song& song) { removeLaneChord (song, chordStart); });
}

void deleteAllLaneChords (ProjectContext& context)
{
    if (! hasLaneChords (context))
        return;

    editLane (context, "Delete All Chords",
              [] (Song& song)
              {
                  for (const auto& chord : getLaneChords (song))
                      removeLaneChord (song, chord.range.getStart());

                  for (const auto& key : getLaneKeys (song))
                      if (key.start > BeatPosition())
                          removeLaneKeyChange (song, key.start);
              });
}

void setLaneChordEnd (ProjectContext& context, BeatPosition chordStart, BeatPosition end)
{
    if (auto* played = findSong (context);
        played != nullptr && hasLaneChordAt (*played, chordStart) && end > chordStart + BeatDuration::fromBeats (tiny))
        editLane (context, "Change Chord Length", [&] (Song& song) { setLaneChordEnd (song, chordStart, end); });
}

void setLaneKey (ProjectContext& context, BeatPosition from, const Key& key)
{
    editLane (context, "Change Key", [&] (Song& song) { setLaneKey (song, from, key); });
}

void removeLaneKeyChange (ProjectContext& context, BeatPosition at)
{
    if (at > BeatPosition::fromBeats (1.0e-4))
        editLane (context, "Remove Key Change", [&] (Song& song) { removeLaneKeyChange (song, at); });
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
    if (findSong (context) == nullptr)
        return std::nullopt;

    const auto sections = getSelectedSections (getSelection (context));

    if (sections.empty())
        return std::nullopt;

    return getSectionsRange (sections);
}

std::optional<BeatRange> getSelectedTimeRange (const ProjectContext& context)
{
    const auto& range = context.getSongRange();

    if (findSong (context) == nullptr || range.isEmpty())
        return std::nullopt;

    return range.getRange();
}

std::optional<BeatRange> getWholeSongRange (const ProjectContext& context)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return std::nullopt;

    const auto end = std::max (getClipsEnd (*song), getSectionsEnd (*song));

    if (end <= BeatPosition::fromBeats (tiny))
        return std::nullopt;

    return BeatRange (BeatPosition(), end);
}

BeatDuration getProgressionLength (const ProjectContext& context, const Progression& progression, BeatPosition from)
{
    auto* song = findSong (context);
    auto beat = from;

    for (const auto& chord : progression.chords)
    {
        const auto beatsPerBar = song != nullptr ? getBars (*song).getBeatsPerBar (beat) : 4;
        beat += BeatDuration::fromBeats (chord.bars * std::max (1, beatsPerBar));
    }

    return beat - from;
}

//==============================================================================
bool writeProgressionPreview (const ProjectContext& context, const Progression& progression, BeatPosition from, const juce::File& file)
{
    // The chords as they would go into the song from the beat, held, with their roots under them.
    std::vector<PlacedChord> chords;
    const auto key = getLaneKeyAt (context, from);
    const auto progressionKey = getKeyForProgression (key, progression.scale);
    auto* song = findSong (context);
    auto beat = BeatPosition();

    for (const auto& chord : progression.chords)
    {
        const auto beatsPerBar = song != nullptr ? getBars (*song).getBeatsPerBar (from + (beat - BeatPosition())) : 4;
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
} // namespace phazite

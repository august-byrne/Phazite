// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChordValues.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    // The UTF-8 of a degree sign and a slashed o, for diminished and half-diminished numerals.
    const juce::String degreeSign = juce::CharPointer_UTF8 ("\xc2\xb0");
    const juce::String slashedO = juce::CharPointer_UTF8 ("\xc3\xb8");

    int mod12 (int value)
    {
        return ((value % 12) + 12) % 12;
    }

    // A chord kind's names, and how a numeral writes it: in lower case when its third is minor, with a suffix.
    struct Kind
    {
        ChordType type;
        const char* savedName;
        const char* symbol; // after the root's letter
        bool minorThird;
        const char* suffix;  // after the numeral, in ASCII; "o" and "0" stand for the degree sign and the slashed o
        const char* letters; // how many letters above the root's each note's is, as getChordIntervals() lists them
    };

    // clang-format off
    constexpr Kind kinds[] = { { ChordType::major,                 "major",                 "",     false, "",     "024" },
                               { ChordType::minor,                 "minor",                 "m",    true,  "",     "024" },
                               { ChordType::dominantSeventh,       "dominantSeventh",       "7",    false, "7",    "0246" },
                               { ChordType::majorSeventh,          "majorSeventh",          "maj7", false, "maj7", "0246" },
                               { ChordType::minorSeventh,          "minorSeventh",          "m7",   true,  "7",    "0246" },
                               { ChordType::suspendedSecond,       "suspendedSecond",       "sus2", false, "sus2", "014" },
                               { ChordType::suspendedFourth,       "suspendedFourth",       "sus4", false, "sus4", "034" },
                               { ChordType::diminished,            "diminished",            "dim",  true,  "o",    "024" },
                               { ChordType::augmented,             "augmented",             "aug",  false, "+",    "024" },
                               { ChordType::majorSixth,            "majorSixth",            "6",    false, "6",    "0245" },
                               { ChordType::minorSixth,            "minorSixth",            "m6",   true,  "6",    "0245" },
                               { ChordType::dominantNinth,         "dominantNinth",         "9",    false, "9",    "02461" },
                               { ChordType::majorNinth,            "majorNinth",            "maj9", false, "maj9", "02461" },
                               { ChordType::power,                 "power",                 "5",    false, "5",    "04" },
                               { ChordType::halfDiminishedSeventh, "halfDiminishedSeventh", "m7b5", true,  "07",   "0246" },
                               { ChordType::diminishedSeventh,     "diminishedSeventh",     "dim7", true,  "o7",   "0246" },
                               { ChordType::minorNinth,            "minorNinth",            "m9",   true,  "9",    "02461" } };
    // clang-format on

    const Kind& findKind (ChordType type)
    {
        for (const auto& kind : kinds)
            if (kind.type == type)
                return kind;

        return kinds[0];
    }

    constexpr const char* romans[] = { "i", "ii", "iii", "iv", "v", "vi", "vii" };

    // What a numeral's suffix makes of a chord written in upper case (a major third) or lower case (a minor one).
    std::optional<ChordType> readKind (const juce::String& suffix, bool upper)
    {
        // The kinds without a third are the same either way.
        if (suffix == "sus2")
            return ChordType::suspendedSecond;

        if (suffix == "sus4")
            return ChordType::suspendedFourth;

        if (suffix == "5")
            return ChordType::power;

        if (upper)
        {
            if (suffix.isEmpty())
                return ChordType::major;
            if (suffix == "7")
                return ChordType::dominantSeventh;
            if (suffix == "maj7")
                return ChordType::majorSeventh;
            if (suffix == "6")
                return ChordType::majorSixth;
            if (suffix == "9")
                return ChordType::dominantNinth;
            if (suffix == "maj9")
                return ChordType::majorNinth;
            if (suffix == "+" || suffix == "aug")
                return ChordType::augmented;

            return std::nullopt;
        }

        if (suffix.isEmpty())
            return ChordType::minor;
        if (suffix == "7")
            return ChordType::minorSeventh;
        if (suffix == "6")
            return ChordType::minorSixth;
        if (suffix == "9")
            return ChordType::minorNinth;
        if (suffix == "o" || suffix == "dim" || suffix == degreeSign)
            return ChordType::diminished;
        if (suffix == "o7" || suffix == "dim7" || suffix == degreeSign + "7")
            return ChordType::diminishedSeventh;
        if (suffix == "m7b5" || suffix == slashedO + "7" || suffix == slashedO)
            return ChordType::halfDiminishedSeventh;

        return std::nullopt;
    }

    // The major key with a key's notes: its parent, whose scale the key's is a mode of (C major for D dorian or A minor).
    // Harmonic and melodic minor count as minor.
    int getParentMajor (const Key& key)
    {
        switch (key.scale)
        {
            case Scale::major:
                return key.root;
            case Scale::dorian:
                return mod12 (key.root - 2);
            case Scale::phrygian:
                return mod12 (key.root - 4);
            case Scale::lydian:
                return mod12 (key.root - 5);
            case Scale::mixolydian:
                return mod12 (key.root - 7);
            case Scale::locrian:
                return mod12 (key.root - 11);
            case Scale::minor:
            case Scale::harmonicMinor:
            case Scale::melodicMinor:
            default:
                return mod12 (key.root + 3);
        }
    }

    // Notes are spelled by letter: a chord's root with the letter of its step of the scale (so the leading note of D
    // harmonic minor is C#, not Db), and the key's root as its parent major's signature has it, with flats for F, Bb, Eb,
    // Ab and Db major.
    constexpr const char* letterNames = "CDEFGAB";
    constexpr int letterPitches[] = { 0, 2, 4, 5, 7, 9, 11 };

    int getRootLetter (const Key& key)
    {
        const auto parent = getParentMajor (key);
        const auto flats = parent == 5 || parent == 10 || parent == 3 || parent == 8 || parent == 1;
        constexpr int sharpLetters[] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 }; // C C# D D# E F F# G G# A A# B
        constexpr int flatLetters[] = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };  // C Db D Eb E F Gb G Ab A Bb B
        return (flats ? flatLetters : sharpLetters)[mod12 (key.root)];
    }

    juce::String spell (int letter, int pitchClass)
    {
        letter = ((letter % 7) + 7) % 7;
        auto difference = mod12 (pitchClass - letterPitches[letter]);

        if (difference > 6)
            difference -= 12;

        auto name = juce::String::charToString (letterNames[letter]);
        name << juce::String::repeatedString (difference > 0 ? "#" : "b", std::abs (difference));
        return name;
    }

    //==============================================================================
    struct Table
    {
        PartStyle style;
        PartKind kind;
        const char* savedName;
        const char* name;
    };

    // clang-format off
    constexpr Table partStyles[] = { { PartStyle::sustainedTriads,   PartKind::chords,   "sustainedTriads",   "Held triads" },
                                     { PartStyle::sustainedSevenths, PartKind::chords,   "sustainedSevenths", "Held sevenths" },
                                     { PartStyle::spreadChords,      PartKind::chords,   "spreadChords",      "Spread pads" },
                                     { PartStyle::pulsingChords,     PartKind::chords,   "pulsingChords",     "A chord on every beat" },
                                     { PartStyle::offbeatStabs,      PartKind::chords,   "offbeatStabs",      "Offbeat stabs" },
                                     { PartStyle::bassRoots,         PartKind::bass,     "bassRoots",         "Held roots" },
                                     { PartStyle::bassPulses,        PartKind::bass,     "bassPulses",        "Roots in eighth notes" },
                                     { PartStyle::bassRootFifth,     PartKind::bass,     "bassRootFifth",     "Root and fifth" },
                                     { PartStyle::bassOctaves,       PartKind::bass,     "bassOctaves",       "Octaves" },
                                     { PartStyle::arpeggioUp,        PartKind::arpeggio, "arpeggioUp",        "Up" },
                                     { PartStyle::arpeggioDown,      PartKind::arpeggio, "arpeggioDown",      "Down" },
                                     { PartStyle::arpeggioUpDown,    PartKind::arpeggio, "arpeggioUpDown",    "Up and down" } };
    // clang-format on

    const Table& findStyle (PartStyle style)
    {
        for (const auto& entry : partStyles)
            if (entry.style == style)
                return entry;

        return partStyles[0];
    }

    //==============================================================================
    // Voicing.

    constexpr int chordCentre = 62; // the middle of where chords are voiced: around middle C and the D above it
    constexpr int lowestChordNote = 48, highestChordNote = 76;
    constexpr int lowestBassNote = 36; // C2: bass roots go from here up an octave
    constexpr int lowestArpeggioNote = 60;

    // A chord's notes for a part: its pitch classes, from the bass note up, with a seventh added to a triad when sevenths
    // are wanted (the key's own, on a root of the key's scale, or else a minor seventh, a diminished one on a diminished
    // triad).
    std::vector<int> getPartPitchClasses (const PlacedChord& chord, bool sevenths)
    {
        auto classes = getPitchClassesIn (chord.numeral, chord.key);
        const auto intervals = getChordIntervals (chord.numeral.type);

        if (! sevenths || intervals.size() != 3)
            return classes;

        const auto root = getRootIn (chord.numeral, chord.key);
        auto seventh = chord.numeral.type == ChordType::diminished ? 9 : 10;

        if (chord.numeral.accidental == 0)
        {
            const auto steps = getScaleSteps (chord.key.scale);
            const auto degree = chord.numeral.degree - 1;
            seventh = mod12 (steps[(size_t) ((degree + 6) % 7)] - steps[(size_t) degree]);
        }

        classes.push_back (mod12 (root + seventh));
        return classes;
    }

    // Stacks pitch classes upwards from a lowest note: each the nearest note above the one before.
    std::vector<int> stackFrom (int lowest, const std::vector<int>& classes)
    {
        std::vector<int> pitches;

        for (const auto pitchClass : classes)
        {
            auto pitch = pitches.empty() ? lowest : pitches.back() + 1;

            while (mod12 (pitch) != pitchClass)
                ++pitch;

            pitches.push_back (pitch);
        }

        return pitches;
    }

    double getMean (const std::vector<int>& pitches)
    {
        auto sum = 0.0;

        for (const auto pitch : pitches)
            sum += pitch;

        return pitches.empty() ? (double) chordCentre : sum / (double) pitches.size();
    }

    // How far the voices move from one chord to the next: between notes in order when both have as many, else between
    // their middles; plus half how far the chord strays from middle C, so that a long progression doesn't creep away.
    double getMovement (const std::vector<int>& pitches, const std::vector<int>& previous)
    {
        auto movement = 0.0;

        if (previous.size() == pitches.size())
        {
            for (size_t i = 0; i < pitches.size(); ++i)
                movement += std::abs (pitches[i] - previous[i]);
        }
        else
        {
            movement = std::abs (getMean (pitches) - getMean (previous)) * (double) pitches.size();
        }

        return movement + std::abs (getMean (pitches) - (double) chordCentre) * 0.5;
    }

    // Close voicings: the chord's notes stacked within an octave and a bit, in its inversion, or, in root position, in
    // whichever inversion moves least from the chord before (the first, nearest middle C).
    std::vector<int> voiceClose (const PlacedChord& chord, const std::vector<int>& classes, const std::vector<int>& previous)
    {
        const auto rotations = chord.numeral.inversion == 0 ? classes.size() : (size_t) 1;
        std::vector<int> best;
        auto bestMovement = 1.0e9;

        for (size_t rotation = 0; rotation < rotations; ++rotation)
        {
            std::vector<int> order (classes.begin() + (std::ptrdiff_t) rotation, classes.end());
            order.insert (order.end(), classes.begin(), classes.begin() + (std::ptrdiff_t) rotation);

            for (int lowest = lowestChordNote; lowest < lowestChordNote + 24; ++lowest)
            {
                if (mod12 (lowest) != order.front())
                    continue;

                const auto pitches = stackFrom (lowest, order);

                if (pitches.back() > highestChordNote)
                    continue;

                const auto movement =
                    previous.empty() ? std::abs (getMean (pitches) - (double) chordCentre) : getMovement (pitches, previous);

                if (movement < bestMovement - 1.0e-9)
                {
                    best = pitches;
                    bestMovement = movement;
                }
            }
        }

        return best.empty() ? stackFrom (lowestChordNote + classes.front(), classes) : best;
    }

    // Open voicings: the bass note low with its fifth, the other notes an octave higher.
    std::vector<int> voiceSpread (const std::vector<int>& classes)
    {
        const auto bass = 48 + classes.front(); // C3 to B3
        std::vector<int> pitches { bass, bass + 7 };

        for (size_t i = 1; i < classes.size(); ++i)
        {
            auto pitch = bass + 12;

            while (mod12 (pitch) != classes[i])
                ++pitch;

            if (std::find (pitches.begin(), pitches.end(), pitch) == pitches.end())
                pitches.push_back (pitch);
        }

        std::sort (pitches.begin(), pitches.end());
        return pitches;
    }

    // A chord's fifth (the note 6, 7 or 8 semitones above its root, or a perfect fifth for a chord without one) as the
    // first such note above a bass note; the octave above when the bass note is the fifth itself.
    int getFifthAbove (const PlacedChord& chord, int bass)
    {
        auto interval = 7;

        for (const auto candidate : getChordIntervals (chord.numeral.type))
            if (candidate >= 6 && candidate <= 8)
                interval = candidate;

        const auto above = mod12 (getRootIn (chord.numeral, chord.key) + interval - bass);
        return bass + (above == 0 ? 12 : above);
    }

    void addNote (std::vector<NoteData>& notes, ClipID clip, int pitch, BeatPosition start, BeatPosition end, int velocity)
    {
        if (end <= start)
            return;

        NoteData note;
        note.clip = clip;
        note.pitch = juce::jlimit (0, 127, pitch);
        note.start = start;
        note.length = end - start;
        note.velocity = velocity;
        notes.push_back (note);
    }

    // Plays notes from a chord's start in steps: at each step that falls in [from, to), the notes play() gives for the
    // step's number, for a length (cut at to).
    template <typename PlayStep>
    void playSteps (BeatPosition chordStart, BeatPosition from, BeatPosition to, double step, double offset, double length, PlayStep&& play)
    {
        auto index = 0;

        for (auto beat = chordStart + BeatDuration::fromBeats (offset); beat < to; beat += BeatDuration::fromBeats (step), ++index)
            if (beat >= from)
                play (index, beat, std::min (to, beat + BeatDuration::fromBeats (length)));
    }
} // namespace

//==============================================================================
std::optional<Numeral> parseNumeral (const juce::String& text)
{
    auto rest = text.trim();
    Numeral numeral;

    if (const auto slash = rest.lastIndexOfChar ('/'); slash >= 0)
    {
        const auto inversion = rest.substring (slash + 1);

        if (inversion.length() != 1 || ! inversion.containsOnly ("0123"))
            return std::nullopt;

        numeral.inversion = inversion.getIntValue();
        rest = rest.substring (0, slash);
    }

    if (rest.startsWithChar ('b'))
    {
        numeral.accidental = -1;
        rest = rest.substring (1);
    }
    else if (rest.startsWithChar ('#'))
    {
        numeral.accidental = 1;
        rest = rest.substring (1);
    }

    auto end = 0;

    while (end < rest.length() && juce::String ("IVXivx").containsChar (rest[end]))
        ++end;

    const auto roman = rest.substring (0, end);
    const auto upper = roman == roman.toUpperCase(), lower = roman == roman.toLowerCase();

    if (roman.isEmpty() || upper == lower)
        return std::nullopt;

    const auto index = juce::StringArray (romans, 7).indexOf (roman.toLowerCase());

    if (index < 0)
        return std::nullopt;

    numeral.degree = index + 1;

    if (const auto type = readKind (rest.substring (end), upper))
        numeral.type = *type;
    else
        return std::nullopt;

    if (numeral.inversion >= (int) getChordIntervals (numeral.type).size())
        return std::nullopt;

    return numeral;
}

juce::String describeNumeral (const Numeral& numeral)
{
    const auto& kind = findKind (numeral.type);
    juce::String text (numeral.accidental < 0 ? "b" : numeral.accidental > 0 ? "#" : "");
    const juce::String roman (romans[(size_t) juce::jlimit (0, 6, numeral.degree - 1)]);
    text << (kind.minorThird ? roman : roman.toUpperCase());

    auto suffix = juce::String (kind.suffix);

    if (suffix.startsWithChar ('o'))
        suffix = degreeSign + suffix.substring (1);
    else if (suffix.startsWithChar ('0'))
        suffix = slashedO + suffix.substring (1);

    text << suffix;

    if (numeral.inversion > 0)
        text << "/" << numeral.inversion;

    return text;
}

int getRootIn (const Numeral& numeral, const Key& key)
{
    const auto steps = getScaleSteps (key.scale);
    return mod12 (key.root + steps[(size_t) juce::jlimit (0, 6, numeral.degree - 1)] + numeral.accidental);
}

std::vector<int> getPitchClassesIn (const Numeral& numeral, const Key& key)
{
    const auto root = getRootIn (numeral, key);
    std::vector<int> classes;

    for (const auto interval : getChordIntervals (numeral.type))
        classes.push_back (mod12 (root + interval));

    const auto inversion = (size_t) juce::jlimit (0, (int) classes.size() - 1, numeral.inversion);
    std::rotate (classes.begin(), classes.begin() + (std::ptrdiff_t) inversion, classes.end());
    return classes;
}

juce::String getChordNameIn (const Numeral& numeral, const Key& key)
{
    const auto& kind = findKind (numeral.type);
    const auto rootLetter = getRootLetter (key) + juce::jlimit (1, 7, numeral.degree) - 1;
    auto name = spell (rootLetter, getRootIn (numeral, key)) + kind.symbol;

    if (const auto letters = juce::String (kind.letters); numeral.inversion > 0 && numeral.inversion < letters.length())
        name << "/" << spell (rootLetter + (letters[numeral.inversion] - '0'), getPitchClassesIn (numeral, key).front());

    return name;
}

juce::String getChordSymbol (ChordType type)
{
    return findKind (type).symbol;
}

Numeral toNumeral (int root, ChordType type, int inversion, const Key& key)
{
    const auto steps = getScaleSteps (key.scale);
    const auto above = mod12 (root - key.root);
    Numeral numeral;
    numeral.type = type;
    numeral.inversion = inversion;

    // On a step of the scale; else the step above, lowered; else the step below, raised.
    for (int pass = 0; pass < 3; ++pass)
    {
        const auto accidental = pass == 0 ? 0 : pass == 1 ? -1 : 1;

        for (size_t step = 0; step < steps.size(); ++step)
        {
            if (mod12 (steps[step] + accidental) == above)
            {
                numeral.degree = (int) step + 1;
                numeral.accidental = accidental;
                return numeral;
            }
        }
    }

    return numeral;
}

std::vector<Numeral> getDiatonicChords (const Key& key)
{
    const auto steps = getScaleSteps (key.scale);
    std::vector<Numeral> chords;

    for (int degree = 0; degree < 7; ++degree)
    {
        const auto third = mod12 (steps[(size_t) ((degree + 2) % 7)] - steps[(size_t) degree]);
        const auto fifth = mod12 (steps[(size_t) ((degree + 4) % 7)] - steps[(size_t) degree]);
        auto type = ChordType::major;

        if (third == 3)
            type = fifth == 6 ? ChordType::diminished : ChordType::minor;
        else if (fifth == 8)
            type = ChordType::augmented;

        chords.push_back ({ degree + 1, 0, type, 0 });
    }

    return chords;
}

bool hasMajorThird (const Key& key)
{
    return key.scale == Scale::major || key.scale == Scale::lydian || key.scale == Scale::mixolydian;
}

Key getKeyForProgression (const Key& songKey, Scale progressionScale)
{
    const auto progressionHasMajorThird = hasMajorThird ({ 0, progressionScale });

    if (progressionHasMajorThird == hasMajorThird (songKey))
        return { songKey.root, progressionScale };

    // The key with the song's key's notes: its parent major, or that major's relative minor, three semitones under it.
    const auto parent = getParentMajor (songKey);
    return { progressionHasMajorThird ? parent : mod12 (parent + 9), progressionScale };
}

juce::String getSavedChordTypeName (ChordType type)
{
    return findKind (type).savedName;
}

std::optional<ChordType> findChordType (const juce::String& savedName)
{
    for (const auto& kind : kinds)
        if (savedName == kind.savedName)
            return kind.type;

    return std::nullopt;
}

//==============================================================================
const std::vector<PartStyle>& getPartStyles()
{
    static const auto styles = []
    {
        std::vector<PartStyle> all;

        for (const auto& entry : partStyles)
            all.push_back (entry.style);

        return all;
    }();

    return styles;
}

PartKind getPartKind (PartStyle style)
{
    return findStyle (style).kind;
}

juce::String getPartKindName (PartKind kind)
{
    switch (kind)
    {
        case PartKind::bass:
            return "Bass";
        case PartKind::arpeggio:
            return "Arpeggio";
        case PartKind::chords:
        default:
            return "Chords";
    }
}

juce::String getPartStyleName (PartStyle style)
{
    return findStyle (style).name;
}

juce::String getSavedPartStyleName (PartStyle style)
{
    return findStyle (style).savedName;
}

std::optional<PartStyle> findPartStyle (const juce::String& savedName)
{
    for (const auto& entry : partStyles)
        if (savedName == entry.savedName)
            return entry.style;

    return std::nullopt;
}

std::vector<NoteData> makePartNotes (const std::vector<PlacedChord>& chords, PartStyle style, BeatRange range, ClipID clip)
{
    std::vector<NoteData> notes;
    std::vector<int> previous; // the chord before, as voiced, so that the next moves little from it

    for (const auto& chord : chords)
    {
        const auto from = std::max (chord.range.getStart(), range.getStart());
        const auto to = std::min (chord.range.getEnd(), range.getEnd());

        if (to <= from)
            continue;

        const auto start = chord.range.getStart();
        const auto classes = getPartPitchClasses (chord, style == PartStyle::sustainedSevenths);

        switch (style)
        {
            case PartStyle::sustainedTriads:
            case PartStyle::sustainedSevenths:
            case PartStyle::spreadChords:
            {
                const auto pitches = style == PartStyle::spreadChords ? voiceSpread (classes) : voiceClose (chord, classes, previous);

                for (const auto pitch : pitches)
                    addNote (notes, clip, pitch, from, to, 88);

                previous = pitches;
                break;
            }

            case PartStyle::pulsingChords:
            case PartStyle::offbeatStabs:
            {
                const auto pitches = voiceClose (chord, classes, previous);
                const auto stabs = style == PartStyle::offbeatStabs;

                playSteps (start, from, to, 1.0, stabs ? 0.5 : 0.0, stabs ? 0.25 : 0.9,
                           [&] (int index, BeatPosition noteStart, BeatPosition noteEnd)
                           {
                               for (const auto pitch : pitches)
                                   addNote (notes, clip, pitch, noteStart, noteEnd, stabs ? 96 : (index % 4 == 0 ? 96 : 84));
                           });

                previous = pitches;
                break;
            }

            case PartStyle::bassRoots:
            case PartStyle::bassPulses:
            case PartStyle::bassRootFifth:
            case PartStyle::bassOctaves:
            {
                // The chord's lowest note (its root, or the bass note of an inversion), from C2 up an octave.
                const auto bass = lowestBassNote + classes.front();

                if (style == PartStyle::bassRoots)
                {
                    addNote (notes, clip, bass, from, to, 100);
                    break;
                }

                // The chord's own fifth (diminished, perfect or augmented), the first one above the bass note, or the octave
                // when the bass note is the fifth.
                const auto fifth = getFifthAbove (chord, bass);
                const auto quarter = style == PartStyle::bassRootFifth;
                playSteps (start, from, to, quarter ? 1.0 : 0.5, 0.0, quarter ? 0.9 : 0.45,
                           [&] (int index, BeatPosition noteStart, BeatPosition noteEnd)
                           {
                               auto pitch = bass;

                               if (style == PartStyle::bassRootFifth && index % 2 == 1)
                                   pitch = fifth;
                               else if (style == PartStyle::bassOctaves && index % 2 == 1)
                                   pitch = bass + 12;

                               addNote (notes, clip, pitch, noteStart, noteEnd, index % 2 == 0 ? 100 : 88);
                           });
                break;
            }

            case PartStyle::arpeggioUp:
            case PartStyle::arpeggioDown:
            case PartStyle::arpeggioUpDown:
            default:
            {
                // The chord's notes from middle C up, and its lowest note an octave higher at the top.
                auto pitches = stackFrom (lowestArpeggioNote + classes.front(), classes);
                pitches.push_back (pitches.front() + 12);

                std::vector<int> order (pitches);

                if (style == PartStyle::arpeggioDown)
                    std::reverse (order.begin(), order.end());
                else if (style == PartStyle::arpeggioUpDown)
                    order.insert (order.end(), pitches.rbegin() + 1, pitches.rend() - 1);

                playSteps (start, from, to, 0.5, 0.0, 0.45, [&] (int index, BeatPosition noteStart, BeatPosition noteEnd)
                           { addNote (notes, clip, order[(size_t) index % order.size()], noteStart, noteEnd, index % 2 == 0 ? 96 : 84); });
                break;
            }
        }
    }

    return notes;
}
} // namespace phazite

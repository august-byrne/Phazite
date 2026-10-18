// SPDX-License-Identifier: AGPL-3.0-or-later
// Chord progressions to try (docs/design.md, section 14; roadmap step 2.7): common progressions, each with a name, its
// numerals, a feel, and the moods and genres it suits, written for a major or a minor key. They are a data file
// (resources/progressions/progressions.json), so that a new one needs no code. The chord lane plays one in the song's
// key (ChordModel.h). No Tracktion here.
#pragma once

#include "model/chords/ChordValues.h"

#include <optional>
#include <vector>

namespace phazite
{
struct Progression
{
    struct Chord
    {
        Numeral numeral {};
        double bars = 1.0; // how long it lasts: a bar, or two, or half of one
    };

    juce::String id, name, feel;
    Scale scale = Scale::major; // the key it is written for: major or minor
    juce::StringArray moods, genres;
    std::vector<Chord> chords;

    double getLengthInBars() const;
    juce::String describeNumerals() const; // "I - V - vi - IV"

    // Reads a progression from its entry in the file: its "chords" are numerals separated by spaces, each with "*" and
    // a number of bars if it lasts more or less than one ("I*2 IV V*0.5 V7*0.5"). Nothing if it has no id, name or
    // chords, or a chord that isn't a numeral.
    static std::optional<Progression> fromJSON (const juce::var&);
};

// The progressions that ship with Phazite, in the file's order.
const std::vector<Progression>& getBuiltInProgressions();
const Progression* findProgression (const juce::String& id);

// The moods and genres the progressions are tagged with, each once, in the order they first come in the file.
juce::StringArray getProgressionMoods();
juce::StringArray getProgressionGenres();

// The progressions with a mood and a genre (an empty one means any), in the file's order.
std::vector<const Progression*> findProgressions (const juce::String& mood, const juce::String& genre);

// A progression's chords as they play in a key ("C", "G", "Am", "F"), where the key's third is major or minor as the
// progression's is, or else its relative key's (getKeyForProgression()).
juce::StringArray getChordNamesIn (const Progression&, const Key&);
} // namespace phazite

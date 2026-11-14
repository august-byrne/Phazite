// SPDX-License-Identifier: AGPL-3.0-or-later
// A small mutation fuzzer that runs in every test run: valid files (the golden files, presets that ship, what Phazite
// writes) damaged in many ways, each fed to a reader, which must refuse it or read it into something that holds together.
// It never crashes, hangs, asserts (TestMain.cpp fails a test whose code makes JUCE assert), reads out of bounds or
// allocates without bound, which the test process shows by dying, by a failed check, or under the sanitizers.
//
// Each case has a seed of its own (from its name) and runs a fixed number of rounds, round r from the seed plus r, so that
// a failure happens again exactly: it prints the case, the seed and the round, and writes what was fed to the reader into
// the golden tests' actual folder (GoldenFiles.h: getActualFolder()). The environment variables:
//   PHAZITE_FUZZ_ROUNDS   rounds for each case, in place of its own (a nightly run: 20000)
//   PHAZITE_FUZZ_ROUND    one round alone, to run again the one that failed
// A case's rounds are its own count times nothing else, so that the default run stays well under a minute in Debug.
#pragma once

#include "TestHelpers.h"

#include <functional>
#include <string>
#include <vector>

namespace phazite::test::fuzz
{
// What each round damages: bytes alone (any file), or, as well, what text holds (XML's elements and attributes, JSON's
// objects and arrays), which reaches the readers past their parsers.
enum class Shape
{
    bytes,
    xml,
    json
};

// The ways a file is damaged, chosen by a random number generator of the round's seed.
class Mutator
{
public:
    explicit Mutator (juce::int64 seed)
        : random (seed)
    {
    }

    juce::MemoryBlock mutate (const juce::MemoryBlock&, Shape);

    // One way each; mutate() takes one to three of them in a row.
    juce::MemoryBlock mutateBytes (const juce::MemoryBlock&);
    juce::String mutateXml (const juce::String&);
    juce::String mutateJson (const juce::String&);

    // A number taken to an edge: 0, -1, the largest and smallest of each integer type, a double's extremes, not a number and
    // infinity (as text).
    juce::String makeExtremeNumber();

    // Text a reader might trip on: very long, a path outside, a network path, characters XML can't hold.
    juce::String makeAwkwardText();

    juce::Random random;
};

// The rounds a case runs: its own, or PHAZITE_FUZZ_ROUNDS.
int getRounds (int ownRounds);

// What a reader made of one input: empty if it held (refused the input, or read it into something consistent), else what
// went wrong.
using Reader = std::function<std::string (const juce::MemoryBlock&)>;

// Runs a case: for each round, one of the seeds, damaged, fed to the reader. A failure fails the test with the case's name,
// its seed, the round and what went wrong, and writes what was fed into the actual folder. Returns how many inputs the
// reader refused or took, for the case to say.
struct Totals
{
    int rounds = 0, failures = 0;
};

Totals run (const juce::String& name, const std::vector<juce::MemoryBlock>& seeds, Shape, int ownRounds, const Reader&);

// The same, for a case that damages what it feeds itself (one file of several, one piece of a song's), from the round's
// mutator: each round returns what went wrong, or nothing, and sets what it fed, which a failure writes.
using Round = std::function<std::string (Mutator&, juce::MemoryBlock& fed)>;
Totals runRounds (const juce::String& name, int ownRounds, const Round&);

// The first problems of a list, as one line; none for none.
std::string joinProblems (const std::vector<std::string>&);

juce::MemoryBlock toBlock (const juce::String&);
} // namespace phazite::test::fuzz

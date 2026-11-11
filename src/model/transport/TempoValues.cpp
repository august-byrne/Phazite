// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TempoValues.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace phazite
{
namespace
{
    constexpr double tiny = 1.0e-9; // in beats or bars, for rounding errors
} // namespace

bool isValid (TimeSignature timeSignature)
{
    const auto denominator = timeSignature.denominator;
    return timeSignature.numerator >= 1 && timeSignature.numerator <= 32
           && (denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16);
}

//==============================================================================
Bars::Bars()
    : runs { Run {} }
{
}

Bars::Bars (const std::vector<TimeSignatureChange>& changes)
{
    for (const auto& change : changes)
    {
        const auto barLength = change.signature.getBarLength().inBeats();

        // The first is at the song's start, whatever beat it says.
        if (runs.empty())
        {
            runs.push_back ({ 0.0, 0, change.signature, barLength });
            continue;
        }

        const auto& last = runs.back();
        const auto beat = change.beat.inBeats();

        // Two at one beat: the later one counts.
        if (beat <= last.startBeat + tiny)
        {
            runs.back().signature = change.signature;
            runs.back().barLength = barLength;
            continue;
        }

        // One part of the way through a bar ends that bar there.
        const auto barsBefore = (int) std::ceil ((beat - last.startBeat) / last.barLength - tiny);
        runs.push_back ({ beat, last.firstBar + barsBefore, change.signature, barLength });
    }

    if (runs.empty())
        runs.push_back ({});
}

const Bars::Run& Bars::findRunAtBeat (double beat) const
{
    auto run = runs.begin();

    while (std::next (run) != runs.end() && std::next (run)->startBeat <= beat + tiny)
        ++run;

    return *run;
}

const Bars::Run& Bars::findRunOfBar (int bar) const
{
    auto run = runs.begin();

    while (std::next (run) != runs.end() && std::next (run)->firstBar <= bar)
        ++run;

    return *run;
}

double Bars::getBarsAt (BeatPosition beat) const
{
    const auto& run = findRunAtBeat (beat.inBeats());
    return run.firstBar + (beat.inBeats() - run.startBeat) / run.barLength;
}

int Bars::getBarAt (BeatPosition beat) const
{
    return (int) std::floor (getBarsAt (beat) + tiny);
}

BeatPosition Bars::getBarStart (int bar) const
{
    const auto& run = findRunOfBar (bar);
    return BeatPosition::fromBeats (run.startBeat + (double) (bar - run.firstBar) * run.barLength);
}

BeatDuration Bars::getBarLength (BeatPosition beat) const
{
    return BeatDuration::fromBeats (findRunAtBeat (beat.inBeats()).barLength);
}

BeatDuration Bars::getBeatLength (BeatPosition beat) const
{
    return findRunAtBeat (beat.inBeats()).signature.getBeatLength();
}
} // namespace phazite

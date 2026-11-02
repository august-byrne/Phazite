// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TempoMap.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
TempoMap::TempoMap()
    : TempoMap ({}, {}, 44100.0)
{
}

TempoMap::TempoMap (std::vector<Tempo> tempos, std::vector<TimeSignature> timeSignatures, double rate)
    : signatures (std::move (timeSignatures)),
      sampleRate (rate)
{
    const auto byBeat = [] (const auto& a, const auto& b) { return a.beat < b.beat; };
    std::stable_sort (tempos.begin(), tempos.end(), byBeat);
    std::stable_sort (signatures.begin(), signatures.end(), byBeat);

    if (tempos.empty() || tempos.front().beat > 0.0)
        tempos.insert (tempos.begin(), Tempo {});

    if (signatures.empty() || signatures.front().beat > 0.0)
        signatures.insert (signatures.begin(), TimeSignature {});

    tempos.front().beat = 0.0;
    signatures.front().beat = 0.0;

    // A segment from each beat where the tempo or the time signature changes.
    std::vector<double> starts;

    for (const auto& tempo : tempos)
        starts.push_back (tempo.beat);

    for (const auto& signature : signatures)
        starts.push_back (signature.beat);

    std::sort (starts.begin(), starts.end());
    starts.erase (std::unique (starts.begin(), starts.end()), starts.end());

    size_t tempo = 0, signature = 0;
    auto sample = 0.0;

    for (const auto start : starts)
    {
        while (tempo + 1 < tempos.size() && tempos[tempo + 1].beat <= start)
            ++tempo;

        while (signature + 1 < signatures.size() && signatures[signature + 1].beat <= start)
            ++signature;

        if (! segments.empty())
        {
            const auto& last = segments.back();
            sample = last.sample + (start - last.beat) * last.samplesPerBeat;
        }

        const auto bpm = tempos[tempo].bpm;
        const auto denominator = std::max (1, signatures[signature].denominator);
        segments.push_back ({ start, sample, sampleRate * 240.0 / (bpm * denominator), bpm, (int) signature });
    }
}

const TempoMap::Segment& TempoMap::findByBeat (double beat) const noexcept
{
    const auto after =
        std::upper_bound (segments.begin() + 1, segments.end(), beat, [] (double b, const Segment& segment) { return b < segment.beat; });
    return *(after - 1);
}

const TempoMap::Segment& TempoMap::findBySample (double sample) const noexcept
{
    const auto after = std::upper_bound (segments.begin() + 1, segments.end(), sample,
                                         [] (double s, const Segment& segment) { return s < segment.sample; });
    return *(after - 1);
}

double TempoMap::toSamples (double beat) const noexcept
{
    const auto& segment = findByBeat (beat);
    return segment.sample + (beat - segment.beat) * segment.samplesPerBeat;
}

double TempoMap::toBeats (double sample) const noexcept
{
    const auto& segment = findBySample (sample);
    return segment.beat + (sample - segment.sample) / segment.samplesPerBeat;
}

double TempoMap::getSamplesPerBeat (double beat) const noexcept
{
    return findByBeat (beat).samplesPerBeat;
}

double TempoMap::getTempoAt (double beat) const noexcept
{
    return findByBeat (beat).bpm;
}

TempoMap::TimeSignature TempoMap::getTimeSignatureAt (double beat) const noexcept
{
    return signatures[(size_t) findByBeat (beat).signature];
}

double TempoMap::getBarStart (double beat) const noexcept
{
    const auto signature = getTimeSignatureAt (beat);
    const auto beatsPerBar = (double) std::max (1, signature.numerator);
    return signature.beat + std::floor ((beat - signature.beat) / beatsPerBar) * beatsPerBar;
}
} // namespace phazite::engine

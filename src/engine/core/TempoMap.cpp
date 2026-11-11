// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TempoMap.h"
#include "engine/automation/Curve.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phazite::engine
{
namespace
{
    constexpr double flat = 1.0e-12;          // a tempo that changes less than this a beat stays as it is
    constexpr double pieceBeats = 1.0 / 16.0; // a bent line plays as straight pieces this long at most
    constexpr int minimumPieces = 16;         // and at least so many of them

    bool isBent (float curve)
    {
        return std::abs (curve) > 1.0e-6f && std::abs (curve) < 1.0f;
    }

    // Over a straight line of tempo, so many beats on from where it starts last so many samples: at a tempo of bpm there,
    // changing by bpmPerBeat a beat, a beat lasting scale / tempo samples. Before the song's start (in its first segment
    // only), its first tempo goes on as it is.
    double samplesAfter (double bpm, double bpmPerBeat, double scale, double beats)
    {
        if (beats <= 0.0 || std::abs (bpmPerBeat) < flat)
            return beats * scale / bpm;

        return scale / bpmPerBeat * std::log1p (bpmPerBeat * beats / bpm);
    }

    double beatsAfter (double bpm, double bpmPerBeat, double scale, double samples)
    {
        if (samples <= 0.0 || std::abs (bpmPerBeat) < flat)
            return samples * bpm / scale;

        return bpm / bpmPerBeat * std::expm1 (bpmPerBeat * samples / scale);
    }
} // namespace

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

    // A tempo's line to the next, as automation plays it: its tempo at a beat along it.
    const auto tempoAlong = [&tempos] (size_t index, double beat)
    {
        const auto& from = tempos[index];

        if (index + 1 >= tempos.size())
            return from.bpm;

        const auto& to = tempos[index + 1];
        const auto length = to.beat - from.beat;

        if (length <= 0.0)
            return to.bpm;

        return from.bpm + getCurveProgress ((beat - from.beat) / length, (double) from.curve) * (to.bpm - from.bpm);
    };

    // A segment from each beat where the tempo or the time signature changes, and along a bent line every so often, so that
    // over each the tempo is a straight line, flat or not.
    std::vector<double> starts;

    for (size_t i = 0; i < tempos.size(); ++i)
    {
        starts.push_back (tempos[i].beat);

        if (i + 1 < tempos.size() && isBent (tempos[i].curve))
        {
            const auto from = tempos[i].beat, to = tempos[i + 1].beat;
            const auto pieces = std::max (minimumPieces, (int) std::ceil ((to - from) / pieceBeats));

            for (int piece = 1; piece < pieces; ++piece)
                starts.push_back (from + (to - from) * piece / pieces);
        }
    }

    for (const auto& signature : signatures)
        starts.push_back (signature.beat);

    std::sort (starts.begin(), starts.end());
    starts.erase (std::unique (starts.begin(), starts.end()), starts.end());

    size_t tempo = 0, signature = 0;
    auto sample = 0.0;

    for (size_t s = 0; s < starts.size(); ++s)
    {
        const auto start = starts[s];

        while (tempo + 1 < tempos.size() && tempos[tempo + 1].beat <= start)
            ++tempo;

        while (signature + 1 < signatures.size() && signatures[signature + 1].beat <= start)
            ++signature;

        if (! segments.empty())
        {
            const auto& last = segments.back();
            sample = last.sample + samplesAfter (last.bpm, last.bpmPerBeat, last.scale, start - last.beat);
        }

        // The tempo over the segment: held, jumped to at once, or a line to where the next segment starts (the next tempo,
        // or the next piece of a bent line).
        const auto& from = tempos[tempo];
        auto bpm = from.bpm, bpmPerBeat = 0.0;

        if (tempo + 1 < tempos.size())
        {
            const auto& to = tempos[tempo + 1];
            const auto curve = std::clamp ((double) from.curve, -1.0, 1.0);

            if (curve <= -1.0)
            {
                bpm = to.bpm;
            }
            else if (curve < 1.0)
            {
                const auto end = s + 1 < starts.size() ? std::min (starts[s + 1], to.beat) : to.beat;
                bpm = tempoAlong (tempo, start);

                if (end > start)
                    bpmPerBeat = (tempoAlong (tempo, end) - bpm) / (end - start);
            }
        }

        segments.push_back ({ start, sample, bpm, bpmPerBeat, sampleRate * 60.0, (int) signature });
    }

    // Each time signature's first bar: one that starts part of the way through a bar of the one before ends that bar there,
    // and of two at one beat the later counts, as the song's bars have them (model/transport/TempoValues.h: Bars).
    firstBars.assign (signatures.size(), 0.0);

    for (size_t i = 1; i < signatures.size(); ++i)
    {
        const auto& last = signatures[i - 1];
        const auto barsBefore = std::ceil ((signatures[i].beat - last.beat) / last.getBarLength() - 1.0e-9);
        firstBars[i] = firstBars[i - 1] + std::max (0.0, barsBefore);
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
    return segment.sample + samplesAfter (segment.bpm, segment.bpmPerBeat, segment.scale, beat - segment.beat);
}

double TempoMap::toBeats (double sample) const noexcept
{
    const auto& segment = findBySample (sample);
    return segment.beat + beatsAfter (segment.bpm, segment.bpmPerBeat, segment.scale, sample - segment.sample);
}

double TempoMap::getSamplesPerQuarterNote (double beat) const noexcept
{
    return findByBeat (beat).scale / getTempoAt (beat);
}

double TempoMap::getTempoAt (double beat) const noexcept
{
    const auto& segment = findByBeat (beat);
    return beat <= segment.beat ? segment.bpm : segment.bpm + segment.bpmPerBeat * (beat - segment.beat);
}

TempoMap::TimeSignature TempoMap::getTimeSignatureAt (double beat) const noexcept
{
    return signatures[(size_t) findByBeat (beat).signature];
}

double TempoMap::getBarLength (double beat) const noexcept
{
    return getTimeSignatureAt (beat).getBarLength();
}

double TempoMap::getBeatLength (double beat) const noexcept
{
    return getTimeSignatureAt (beat).getBeatLength();
}

double TempoMap::getBarStart (double beat) const noexcept
{
    const auto signature = getTimeSignatureAt (beat);
    const auto bar = signature.getBarLength();
    return signature.beat + std::floor ((beat - signature.beat) / bar) * bar;
}

double TempoMap::getBarsAt (double beat) const noexcept
{
    const auto index = (size_t) findByBeat (beat).signature;
    const auto& signature = signatures[index];
    return firstBars[index] + (beat - signature.beat) / signature.getBarLength();
}

double TempoMap::getBeatAtOrAfter (double beat) const noexcept
{
    const auto index = (size_t) findByBeat (beat).signature;
    const auto& signature = signatures[index];
    const auto length = signature.getBeatLength();
    const auto next = signature.beat + std::ceil ((beat - signature.beat) / length - 1.0e-9) * length;
    return index + 1 < signatures.size() ? std::min (next, signatures[index + 1].beat) : next;
}

double TempoMap::getNextBeat (double beat) const noexcept
{
    // The one after the beat it is on (or the last one before it), each later than the last however far into the song.
    const auto index = (size_t) findByBeat (beat).signature;
    const auto& signature = signatures[index];
    const auto length = signature.getBeatLength();
    const auto next = signature.beat + (std::floor ((beat - signature.beat) / length + 1.0e-9) + 1.0) * length;
    return index + 1 < signatures.size() ? std::min (next, signatures[index + 1].beat) : next;
}

bool TempoMap::isSteadyBetween (double fromBeat, double toBeat) const noexcept
{
    const auto low = std::min (fromBeat, toBeat), high = std::max (fromBeat, toBeat);
    const auto tempo = getTempoAt (low);

    for (size_t i = 0; i < segments.size(); ++i)
    {
        const auto& segment = segments[i];
        const auto end = i + 1 < segments.size() ? segments[i + 1].beat : std::numeric_limits<double>::infinity();
        const auto from = std::max (low, segment.beat), to = std::min (high, end);

        if (to < from)
            continue;

        if (std::abs (segment.bpm + segment.bpmPerBeat * (from - segment.beat) - tempo) > 1.0e-9
            || (to > from && std::abs (segment.bpmPerBeat) >= flat))
            return false;
    }

    return true;
}
} // namespace phazite::engine

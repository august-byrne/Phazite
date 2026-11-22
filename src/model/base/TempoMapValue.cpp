// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/Schema.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace phazite
{
namespace
{
    // A tempo's line to the next where it has none: a step.
    constexpr float stepCurve = 1.0f;

    TempoEntry readTempo (const juce::ValueTree& tempo)
    {
        return { BeatPosition::fromBeats (tempo[schema::mapBeat]), (double) tempo.getProperty (schema::bpm, 120.0),
                 juce::jlimit (-1.0f, 1.0f, (float) tempo.getProperty (schema::tempoCurve, stepCurve)) };
    }

    SignatureEntry readSignature (const juce::ValueTree& signature)
    {
        return { BeatPosition::fromBeats (signature[schema::mapBeat]), (int) signature.getProperty (schema::numerator, 4),
                 (int) signature.getProperty (schema::denominator, 4) };
    }

    // A kind's entries, read in the element's order and sorted stably by beat, so that two at one beat stay in the element's
    // order: straight into the value where the element holds them in order already (as Phazite writes them), else sorted.
    // What the readers of the song's tempo map ask at every call while the tree holds it (Tempo.h: getTempoMapValue()), so
    // no more is made than the value.
    template <typename Entry, typename Read>
    immer::flex_vector<Entry> readKind (const juce::ValueTree& tempoMap, const juce::Identifier& type, Read&& read)
    {
        auto entries = immer::flex_vector<Entry>().transient();
        std::optional<BeatPosition> last;
        auto inOrder = true;

        for (const auto& child : tempoMap)
        {
            if (child.hasType (type))
            {
                const auto entry = read (child);
                inOrder = inOrder && ! (last.has_value() && entry.beat < *last);
                last = entry.beat;
                entries.push_back (entry);
            }
        }

        if (! last.has_value())
            entries.push_back (Entry {});

        auto held = entries.persistent();

        if (inOrder)
            return held;

        std::vector<Entry> sorted (held.begin(), held.end());
        std::stable_sort (sorted.begin(), sorted.end(), [] (const Entry& a, const Entry& b) { return a.beat < b.beat; });
        return immer::flex_vector<Entry> (sorted.begin(), sorted.end());
    }

    // An entry moved to where its beat puts it among the others of its kind, as its element would be read with that beat (the
    // entries in their file's order, sorted by beat, those at one beat staying as they were): moved later, before any already
    // at its new beat; moved earlier, after them; at its own beat still, where it was.
    template <typename Entry>
    immer::flex_vector<Entry> placeByBeat (const immer::flex_vector<Entry>& entries, size_t index, const Entry& moved)
    {
        const auto was = entries[index].beat;

        if (moved.beat == was)
            return entries.set (index, moved);

        const auto others = entries.erase (index);
        const auto isBefore = [] (const Entry& a, const Entry& b) { return a.beat < b.beat; };
        const auto place = was < moved.beat ? std::lower_bound (others.begin(), others.end(), moved, isBefore)
                                            : std::upper_bound (others.begin(), others.end(), moved, isBefore);
        return others.insert ((size_t) std::distance (others.begin(), place), moved);
    }

    // The place of the first entry whose key (Schema.cpp's for a beat: the beat with six decimals) is the one given.
    template <typename Entry>
    std::optional<size_t> findByKey (const immer::flex_vector<Entry>& entries, const juce::String& key)
    {
        for (size_t i = 0; i < entries.size(); ++i)
            if (juce::String (entries[i].beat.inBeats(), 6) == key)
                return i;

        return std::nullopt;
    }

    BeatPosition readBeat (const juce::var& value)
    {
        return BeatPosition::fromBeats (value.isVoid() ? 0.0 : (double) value);
    }
} // namespace

bool TempoEntry::operator== (const TempoEntry& other) const noexcept
{
    return beat == other.beat && juce::exactlyEqual (bpm, other.bpm) && juce::exactlyEqual (curve, other.curve);
}

bool SignatureEntry::operator== (const SignatureEntry& other) const noexcept
{
    return beat == other.beat && numerator == other.numerator && denominator == other.denominator;
}

bool TempoMapValue::isSameAs (const TempoMapValue& other) const noexcept
{
    return tempos.identity() == other.tempos.identity() && signatures.identity() == other.signatures.identity();
}

bool TempoMapValue::operator== (const TempoMapValue& other) const
{
    return tempos == other.tempos && signatures == other.signatures;
}

TempoMapValue makeDefaultTempoMap()
{
    return { immer::flex_vector<TempoEntry> { TempoEntry {} }, immer::flex_vector<SignatureEntry> { SignatureEntry {} } };
}

TempoMapValue readTempoMapValue (const juce::ValueTree& tempoMap)
{
    return { readTempoEntries (tempoMap), readSignatureEntries (tempoMap) };
}

immer::flex_vector<TempoEntry> readTempoEntries (const juce::ValueTree& tempoMap)
{
    return readKind<TempoEntry> (tempoMap, schema::TEMPO, readTempo);
}

immer::flex_vector<SignatureEntry> readSignatureEntries (const juce::ValueTree& tempoMap)
{
    return readKind<SignatureEntry> (tempoMap, schema::TIMESIGNATURE, readSignature);
}

void writeTempoMap (const TempoMapValue& value, juce::ValueTree& tempoMap)
{
    jassert (tempoMap.hasType (schema::TEMPOMAP));

    immer::for_each (value.tempos,
                     [&tempoMap] (const TempoEntry& tempo)
                     {
                         juce::ValueTree element (schema::TEMPO, { { schema::mapBeat, tempo.beat.inBeats() }, { schema::bpm, tempo.bpm } });

                         if (! juce::exactlyEqual (tempo.curve, stepCurve))
                             element.setProperty (schema::tempoCurve, tempo.curve, nullptr);

                         tempoMap.appendChild (element, nullptr);
                     });

    immer::for_each (value.signatures,
                     [&tempoMap] (const SignatureEntry& signature)
                     {
                         tempoMap.appendChild (juce::ValueTree (schema::TIMESIGNATURE, { { schema::mapBeat, signature.beat.inBeats() },
                                                                                         { schema::numerator, signature.numerator },
                                                                                         { schema::denominator, signature.denominator } }),
                                               nullptr);
                     });
}

bool setTempoMapProperty (TempoMapValue& value, const juce::String& step, const juce::Identifier& property, const juce::var& given)
{
    const auto type = step.upToFirstOccurrenceOf (":", false, false);
    const auto key = step.fromFirstOccurrenceOf (":", false, false);

    if (type == schema::TEMPO.toString())
    {
        const auto index = findByKey (value.tempos, key);

        if (! index.has_value())
            return false;

        auto entry = value.tempos[*index];

        if (property == schema::mapBeat)
        {
            entry.beat = readBeat (given);
            value.tempos = placeByBeat (value.tempos, *index, entry);
            return true;
        }

        if (property == schema::bpm)
            entry.bpm = given.isVoid() ? 120.0 : (double) given;
        else if (property == schema::tempoCurve)
            entry.curve = juce::jlimit (-1.0f, 1.0f, given.isVoid() ? stepCurve : (float) given);
        else
            return false;

        value.tempos = value.tempos.set (*index, entry);
        return true;
    }

    if (type == schema::TIMESIGNATURE.toString())
    {
        const auto index = findByKey (value.signatures, key);

        if (! index.has_value())
            return false;

        auto entry = value.signatures[*index];

        if (property == schema::mapBeat)
        {
            entry.beat = readBeat (given);
            value.signatures = placeByBeat (value.signatures, *index, entry);
            return true;
        }

        if (property == schema::numerator)
            entry.numerator = given.isVoid() ? 4 : (int) given;
        else if (property == schema::denominator)
            entry.denominator = given.isVoid() ? 4 : (int) given;
        else
            return false;

        value.signatures = value.signatures.set (*index, entry);
        return true;
    }

    return false;
}
} // namespace phazite

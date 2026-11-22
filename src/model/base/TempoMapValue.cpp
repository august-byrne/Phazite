// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/TempoMapValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <vector>

namespace phazite
{
namespace
{
    // A tempo's line to the next where it has none: a step.
    constexpr float stepCurve = 1.0f;

    // A property's number as the song reads one (SongText.h: readValueNumber(), the loader's reading, so that a text reads as
    // the same number wherever it is read), or what the value holds where the element has none, or what it has is no finite
    // number.
    double readOr (const juce::var& held, double absent)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? *number : absent;
    }

    TempoEntry readTempo (const juce::ValueTree& tempo)
    {
        const TempoEntry none;
        return { BeatPosition::fromBeats (readOr (tempo[schema::mapBeat], 0.0)), readOr (tempo[schema::bpm], none.bpm),
                 juce::jlimit (-1.0f, 1.0f, (float) readOr (tempo[schema::tempoCurve], stepCurve)) };
    }

    SignatureEntry readSignature (const juce::ValueTree& signature)
    {
        const SignatureEntry none;
        return { BeatPosition::fromBeats (readOr (signature[schema::mapBeat], 0.0)),
                 toWhole<int> (readOr (signature[schema::numerator], none.numerator)),
                 toWhole<int> (readOr (signature[schema::denominator], none.denominator)) };
    }

    // A kind's entries, read in the element's order and sorted stably by beat, so that two at one beat stay in the element's
    // order: straight into the value where the element holds them in order already (as Phazite writes them), else sorted.
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

    // An entry moved to where its beat puts it among the others of its kind (TempoMapValue.h: placeByBeat()).
    template <typename Entry>
    immer::flex_vector<Entry> placeEntryByBeat (const immer::flex_vector<Entry>& entries, size_t index, const Entry& moved, size_t& placed)
    {
        const auto was = entries[index].beat;

        if (moved.beat == was)
        {
            placed = index;
            return entries.set (index, moved);
        }

        const auto others = entries.erase (index);
        const auto isBefore = [] (const Entry& a, const Entry& b) { return a.beat < b.beat; };
        const auto place = was < moved.beat ? std::lower_bound (others.begin(), others.end(), moved, isBefore)
                                            : std::upper_bound (others.begin(), others.end(), moved, isBefore);
        placed = (size_t) std::distance (others.begin(), place);
        return others.insert (placed, moved);
    }

    template <typename Entry>
    immer::flex_vector<Entry> placeEntryByBeat (const immer::flex_vector<Entry>& entries, size_t index, const Entry& moved)
    {
        size_t placed = 0;
        return placeEntryByBeat (entries, index, moved, placed);
    }

    // An entry added after those of its kind at its beat, as an element appended to the map is read.
    template <typename Entry>
    immer::flex_vector<Entry> insertByBeat (const immer::flex_vector<Entry>& entries, const Entry& added)
    {
        const auto place =
            std::upper_bound (entries.begin(), entries.end(), added, [] (const Entry& a, const Entry& b) { return a.beat < b.beat; });
        return entries.insert ((size_t) std::distance (entries.begin(), place), added);
    }

    //==============================================================================
    // A number kept within what its description gives (Schema.h): within its range, made whole where it is a whole one, a
    // power of two where it is one of those.
    double keepWithin (double number, const schema::PropertyDescription& property)
    {
        const auto& range = property.getRange();

        if (! range.has_value())
            return number;

        auto kept = juce::jlimit (range->low, range->high, range->whole ? std::round (number) : number);

        if (range->powersOfTwo)
            kept = std::exp2 (std::round (std::log2 (kept)));

        return kept;
    }

    // A property as the song reads it (readValueNumber()): what isn't there, or isn't a finite number, as its description's
    // fallback (else as what the value holds where its element has none: a curve's 1), the rest kept within the description.
    double readKept (const juce::var& held, const schema::PropertyDescription& property, double absent)
    {
        const auto number = readValueNumber (held);

        if (! number.has_value() || ! std::isfinite (*number))
        {
            const auto& range = property.getRange();
            return range.has_value() && range->fallback.has_value() ? *range->fallback : absent;
        }

        return keepWithin (*number, property);
    }

    TempoEntry readHeldTempo (const juce::ValueTree& tempo)
    {
        const TempoEntry none;
        return { BeatPosition::fromBeats (readKept (tempo[schema::mapBeat], schema::mapBeat, 0.0)),
                 readKept (tempo[schema::bpm], schema::bpm, none.bpm),
                 (float) readKept (tempo[schema::tempoCurve], schema::tempoCurve, none.curve) };
    }

    SignatureEntry readHeldSignature (const juce::ValueTree& signature)
    {
        const SignatureEntry none;
        return { BeatPosition::fromBeats (readKept (signature[schema::mapBeat], schema::mapBeat, 0.0)),
                 (int) readKept (signature[schema::numerator], schema::numerator, none.numerator),
                 (int) readKept (signature[schema::denominator], schema::denominator, none.denominator) };
    }

    bool isKept (double number, const schema::PropertyDescription& property)
    {
        return std::isfinite (number) && juce::exactlyEqual (keepWithin (number, property), number);
    }

    template <typename Entry>
    bool isInOrder (const immer::flex_vector<Entry>& entries)
    {
        return std::is_sorted (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.beat < b.beat; });
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
        return BeatPosition::fromBeats (readOr (value, 0.0));
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

TempoMapValue readHeldTempoMap (const juce::ValueTree& tempoMap)
{
    return { readKind<TempoEntry> (tempoMap, schema::TEMPO, readHeldTempo),
             readKind<SignatureEntry> (tempoMap, schema::TIMESIGNATURE, readHeldSignature) };
}

TempoMapValue withEntryTaken (const TempoMapValue& value, const juce::ValueTree& entry)
{
    if (entry.hasType (schema::TEMPO))
        return { insertByBeat (value.tempos, readHeldTempo (entry)), value.signatures };

    if (entry.hasType (schema::TIMESIGNATURE))
        return { value.tempos, insertByBeat (value.signatures, readHeldSignature (entry)) };

    return value;
}

bool isHeldAsTheSongHoldsIt (const TempoMapValue& value)
{
    if (value.tempos.empty() || value.signatures.empty() || ! isInOrder (value.tempos) || ! isInOrder (value.signatures))
        return false;

    const auto tempoIsKept = [] (const TempoEntry& tempo)
    {
        return isKept (tempo.beat.inBeats(), schema::mapBeat) && isKept (tempo.bpm, schema::bpm)
               && isKept ((double) tempo.curve, schema::tempoCurve);
    };

    const auto signatureIsKept = [] (const SignatureEntry& signature)
    {
        return isKept (signature.beat.inBeats(), schema::mapBeat) && isKept ((double) signature.numerator, schema::numerator)
               && isKept ((double) signature.denominator, schema::denominator);
    };

    return std::all_of (value.tempos.begin(), value.tempos.end(), tempoIsKept)
           && std::all_of (value.signatures.begin(), value.signatures.end(), signatureIsKept);
}

immer::flex_vector<TempoEntry> placeByBeat (const immer::flex_vector<TempoEntry>& entries, size_t index, const TempoEntry& moved,
                                            size_t& place)
{
    return placeEntryByBeat (entries, index, moved, place);
}

immer::flex_vector<SignatureEntry> placeByBeat (const immer::flex_vector<SignatureEntry>& entries, size_t index,
                                                const SignatureEntry& moved, size_t& place)
{
    return placeEntryByBeat (entries, index, moved, place);
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
            value.tempos = placeEntryByBeat (value.tempos, *index, entry);
            return true;
        }

        if (property == schema::bpm)
            entry.bpm = readOr (given, TempoEntry {}.bpm);
        else if (property == schema::tempoCurve)
            entry.curve = juce::jlimit (-1.0f, 1.0f, (float) readOr (given, stepCurve));
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
            value.signatures = placeEntryByBeat (value.signatures, *index, entry);
            return true;
        }

        if (property == schema::numerator)
            entry.numerator = toWhole<int> (readOr (given, SignatureEntry {}.numerator));
        else if (property == schema::denominator)
            entry.denominator = toWhole<int> (readOr (given, SignatureEntry {}.denominator));
        else
            return false;

        value.signatures = value.signatures.set (*index, entry);
        return true;
    }

    return false;
}

namespace
{
    // One kind with the entry at a key made what was read of an element (none: taken out).
    template <typename Entry>
    immer::flex_vector<Entry> withEntryOfKind (const immer::flex_vector<Entry>& entries, const juce::String& key,
                                               const std::optional<Entry>& put)
    {
        const auto index = findByKey (entries, key);

        if (! put.has_value())
            return index.has_value() ? entries.erase (*index) : entries;

        return index.has_value() ? placeEntryByBeat (entries, *index, *put) : insertByBeat (entries, *put);
    }
} // namespace

std::optional<TempoMapValue> withEntryAt (const TempoMapValue& value, const juce::String& step, const juce::ValueTree& element)
{
    const auto type = step.upToFirstOccurrenceOf (":", false, false);
    const auto key = step.fromFirstOccurrenceOf (":", false, false);

    if (type == schema::TEMPO.toString() && (! element.isValid() || element.hasType (schema::TEMPO)))
    {
        const auto put = element.isValid() ? std::optional (readTempo (element)) : std::nullopt;
        return TempoMapValue { withEntryOfKind (value.tempos, key, put), value.signatures };
    }

    if (type == schema::TIMESIGNATURE.toString() && (! element.isValid() || element.hasType (schema::TIMESIGNATURE)))
    {
        const auto put = element.isValid() ? std::optional (readSignature (element)) : std::nullopt;
        return TempoMapValue { value.tempos, withEntryOfKind (value.signatures, key, put) };
    }

    return std::nullopt;
}

namespace
{
    template <typename Entry>
    std::optional<Entry> findEntry (const immer::flex_vector<Entry>& entries, const juce::String& key)
    {
        if (const auto index = findByKey (entries, key))
            return entries[*index];

        return std::nullopt;
    }
} // namespace

std::optional<TempoMapValue> withEntryAt (const TempoMapValue& value, const juce::String& step, const TempoMapValue& from)
{
    const auto type = step.upToFirstOccurrenceOf (":", false, false);
    const auto key = step.fromFirstOccurrenceOf (":", false, false);

    if (type == schema::TEMPO.toString())
        return TempoMapValue { withEntryOfKind (value.tempos, key, findEntry (from.tempos, key)), value.signatures };

    if (type == schema::TIMESIGNATURE.toString())
        return TempoMapValue { value.tempos, withEntryOfKind (value.signatures, key, findEntry (from.signatures, key)) };

    return std::nullopt;
}

bool holdsEntryAt (const TempoMapValue& value, const juce::String& step)
{
    const auto type = step.upToFirstOccurrenceOf (":", false, false);
    const auto key = step.fromFirstOccurrenceOf (":", false, false);

    if (type == schema::TEMPO.toString())
        return findByKey (value.tempos, key).has_value();

    if (type == schema::TIMESIGNATURE.toString())
        return findByKey (value.signatures, key).has_value();

    return false;
}
} // namespace phazite

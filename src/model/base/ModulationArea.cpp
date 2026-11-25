// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ModulationArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = SongValues::ModulationChange;
    using IDList = immer::flex_vector<ModulatorID>;

    bool holdSameValues (const ModulatorValue& a, const ModulatorValue& b)
    {
        if (a.values.size() != b.values.size())
            return false;

        for (const auto& [index, held] : a.values)
            if (const auto* other = b.values.find (index); other == nullptr || ! juce::exactlyEqual (held, *other))
                return false;

        return true;
    }

    bool holdSameSteps (const ModulatorValue& a, const ModulatorValue& b)
    {
        if (a.steps.size() != b.steps.size())
            return false;

        for (size_t i = 0; i < a.steps.size(); ++i)
            if (! juce::exactlyEqual (a.steps[i], b.steps[i]))
                return false;

        return true;
    }

    // What of a modulator or a macro differs between two of its values (the same ID).
    std::uint16_t findParts (const ModulatorValue& was, const ModulatorValue& now)
    {
        std::uint16_t parts = 0;
        const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

        if (! (was.place == now.place))
            add (Change::made);

        if (was.kind != now.kind)
            add (Change::kind);

        if (was.name != now.name)
            add (Change::name);

        if (was.off != now.off)
            add (Change::off);

        if (! holdSameValues (was, now))
        {
            add (Change::valuesSetByHand);

            if (was.values.empty() != now.values.empty())
                add (Change::valuesAppeared);
        }

        if (! holdSameSteps (was, now))
            add (Change::steps);

        if (! juce::exactlyEqual (was.value, now.value))
            add (Change::macroValue);

        if (! (was.modulations == now.modulations))
            add (Change::modulations);

        return parts;
    }

    // Two entries of the area alike without a look inside (ModulatorValue::isSameAs()), and two lists the same one.
    struct SameModulator
    {
        bool operator() (const std::pair<std::uint64_t, ModulatorValue>& a,
                         const std::pair<std::uint64_t, ModulatorValue>& b) const noexcept
        {
            return a.first == b.first && a.second.isSameAs (b.second);
        }
    };

    struct SameList
    {
        bool operator() (const std::pair<std::uint64_t, IDList>& a, const std::pair<std::uint64_t, IDList>& b) const noexcept
        {
            return a.first == b.first && a.second.identity() == b.second.identity();
        }
    };

    // Whether a list holds those of another that it still holds in another order.
    bool isReordered (const IDList& was, const IDList& now)
    {
        const auto holds = [] (const IDList& list, ModulatorID id) { return std::find (list.begin(), list.end(), id) != list.end(); };
        std::vector<ModulatorID> keptBefore, keptNow;

        for (const auto id : was)
            if (holds (now, id))
                keptBefore.push_back (id);

        for (const auto id : now)
            if (holds (was, id))
                keptNow.push_back (id);

        return keptBefore != keptNow;
    }

    // A list's owner as its modulators' places name it.
    std::optional<ModulatorPlace> findOwnerOf (const SongModulation& modulation, const IDList& list)
    {
        if (list.empty())
            return std::nullopt;

        const auto* first = modulation.find (list.front());
        return first != nullptr ? std::optional<ModulatorPlace> (first->place) : std::nullopt;
    }

    // An owner's list in two kept values, none for an owner without one.
    const IDList* findKeptList (const SongModulation& modulation, const juce::ValueTree& element)
    {
        if (element.hasType (schema::TRACK))
            return modulation.channels.find (readID (element[schema::trackID]));

        if (element.hasType (schema::MASTER))
            return modulation.channels.find (0);

        if (element.hasType (schema::CONTAINER))
            return modulation.containers.find (readID (element[schema::deviceID]));

        return nullptr;
    }

    void setArea (Song& song, ValueArea<SongModulation>& area, SongModulation next)
    {
        if (song.isWritingOutsideHistory())
            area.setOutsideHistory (std::move (next));
        else
            area.set (std::move (next));
    }
} // namespace

//==============================================================================
SongValues::ModulationTeller::ModulationTeller (ValueArea<SongModulation>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::ModulationTeller::~ModulationTeller()
{
    area.removeListener (this);
}

void SongValues::ModulationTeller::valueAreaChanged (const SongModulation& before, const SongModulation& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // Each value after, in a copy of the value told, which stays while the listeners are told (cheap: it shares what it holds).
    const auto now = after;
    std::vector<ModulationChange> changes;

    before.modulators.impl().template diff<SameModulator> (
        now.modulators.impl(),
        immer::make_differ (
            [&changes] (const std::pair<std::uint64_t, ModulatorValue>& added)
            { changes.push_back ({ .id = added.second.id, .after = added.second.place, .isThere = true, .parts = Change::made }); },
            [&changes] (const std::pair<std::uint64_t, ModulatorValue>& removed)
            { changes.push_back ({ .id = removed.second.id, .before = removed.second.place, .wasThere = true, .parts = Change::made }); },
            [&changes] (const std::pair<std::uint64_t, ModulatorValue>& was, const std::pair<std::uint64_t, ModulatorValue>& is)
            {
                if (const auto parts = findParts (was.second, is.second); parts != 0)
                    changes.push_back ({ .id = is.second.id,
                                         .before = was.second.place,
                                         .after = is.second.place,
                                         .wasThere = true,
                                         .isThere = true,
                                         .parts = parts });
            }));

    // Each owner whose list holds those it held before in another order.
    std::vector<ModulatorPlace> reordered;
    const auto ignore = [] (const std::pair<std::uint64_t, IDList>&) {};
    const auto compareLists = [&reordered, &now, &ignore] (const auto& was, const auto& is)
    {
        was.impl().template diff<SameList> (is.impl(),
                                            immer::make_differ (ignore, ignore,
                                                                [&reordered, &now] (const std::pair<std::uint64_t, IDList>& listBefore,
                                                                                    const std::pair<std::uint64_t, IDList>& listAfter)
                                                                {
                                                                    if (! isReordered (listBefore.second, listAfter.second))
                                                                        return;

                                                                    if (const auto owner = findOwnerOf (now, listAfter.second))
                                                                        reordered.push_back (*owner);
                                                                }));
    };

    compareLists (before.channels, now.channels);
    compareLists (before.containers, now.containers);

    if (changes.empty() && reordered.empty())
        return;

    std::sort (changes.begin(), changes.end(), [] (const ModulationChange& a, const ModulationChange& b) { return a.id < b.id; });

    for (auto& change : changes)
        change.now = change.isThere ? now.find (change.id) : nullptr;

    listeners.call ([&changes, &reordered] (SongValues::Listener& listener) { listener.modulationChanged (changes, reordered); });
}

//==============================================================================
void readHeldModulationIn (const juce::ValueTree& song, SongState& into)
{
    // Every modulator and macro the loader made consistent, as the song holds each, in the elements' order (each track's, the
    // master's, each channel's own then its containers'); what the loader left of each is what the song holds of it
    // (ModulationValue.h: keepModulatorAsHeld()), so reading it changes nothing of what the file held.
    into.modulation =
        readHeldModulation (song, [] ([[maybe_unused]] const juce::ValueTree& element)
                            { jassert (isWrittenAsHeld (element, readModulatorPlace (element))); }); // the loader keeps each as held
    jassert (holdsTogether (into.modulation));
}

//==============================================================================
bool SongValues::setModulator (Song& song, ModulatorValue value, int index)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (! value.id.isValid())
        return false;

    // One held as the other kind (a macro under a modulator's ID: a merge's) replaced whole, at the place given.
    const auto held = modulation.get();
    const auto id = value.id;
    const auto* was = held.find (id);
    const auto replaced = was != nullptr && was->isMacro() != value.isMacro();
    auto next = withHeldModulator (replaced ? withoutModulator (held, id) : held, std::move (value), index);

    if (const auto* now = next.find (id); was != nullptr && now != nullptr && ! replaced && *was == *now)
        return false;

    setArea (song, modulation, std::move (next));
    return true;
}

bool SongValues::eraseModulator (Song& song, ModulatorID id)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (modulation.get().find (id) == nullptr)
        return false;

    setArea (song, modulation, withoutModulator (modulation.get(), id));
    return true;
}

bool SongValues::eraseModulators (Song& song, const std::vector<ModulatorID>& ids)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    auto next = modulation.get();
    auto any = false;

    for (const auto id : ids)
    {
        if (next.find (id) == nullptr)
            continue;

        next = withoutModulator (next, id);
        any = true;
    }

    if (any)
        setArea (song, modulation, std::move (next));

    return any;
}

bool SongValues::setModulatorOrder (Song& song, const ModulatorPlace& owner, immer::flex_vector<ModulatorID> order)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (const auto* list = modulation.get().findList (owner); list == nullptr || *list == order)
        return false;

    auto next = withOwnerOrder (modulation.get(), owner, std::move (order));

    if (next.isSameAs (modulation.get()))
        return false;

    setArea (song, modulation, std::move (next));
    return true;
}

bool SongValues::setAllModulation (Song& song, SongModulation next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next.isSameAs (modulation.get()) || next == modulation.get())
        return false;

    jassert (holdsTogether (next)); // a pass's and an edit's of several, each held together as it was worked out
    setArea (song, modulation, std::move (next));
    return true;
}

//==============================================================================
bool leadsToModulation (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (parent.hasType (schema::PHAZITE) && child.hasType (schema::MASTER))
        return true;

    return (parent.hasType (schema::TRACK) || parent.hasType (schema::MASTER)) && child.hasType (schema::CONTAINER);
}

void writeModulationIn (juce::ValueTree& element, const SongState& kept)
{
    const auto* list = findKeptList (kept.modulation, element);

    if (list == nullptr)
        return;

    // After its other children: a track's sends come after them (SendsArea.h: writeSendsIn(), which whole state asks after).
    for (const auto id : *list)
        if (const auto* value = kept.modulation.find (id))
            element.appendChild (writeModulatorElement (*value), nullptr);
}

bool holdSameModulation (const SongState& a, const SongState& b)
{
    return a.modulation.isSameAs (b.modulation) || a.modulation == b.modulation;
}

bool holdSameModulator (const SongState& a, const SongState& b, ModulatorID id)
{
    const auto* first = a.modulation.find (id);
    const auto* second = b.modulation.find (id);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->isSameAs (*second) || *first == *second;
}

bool holdSameModulatorOrder (const SongState& a, const SongState& b, const ModulatorPlace& owner)
{
    const auto* first = a.modulation.findList (owner);
    const auto* second = b.modulation.findList (owner);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
}
} // namespace phazite

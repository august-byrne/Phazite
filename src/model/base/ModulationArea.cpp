// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ModulationArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = SongValues::ModulationChange;
    using IDList = immer::flex_vector<ModulatorID>;

    bool isModulatorElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO);
    }

    bool isFolder (const juce::ValueTree& track)
    {
        return track[schema::trackKind].toString() == schema::trackIsFolder;
    }

    // A channel of the song's, as readModulationValue() reads them: the MASTER of its PHAZITE element, a TRACK of its track
    // list, or a TRACK in a folder that is one.
    bool isSongChannel (const juce::ValueTree& element)
    {
        if (element.hasType (schema::MASTER))
            return element.getParent().hasType (schema::PHAZITE);

        if (! element.hasType (schema::TRACK))
            return false;

        const auto parent = element.getParent();

        if (parent.hasType (schema::TRACKS))
            return parent.getParent().hasType (schema::PHAZITE);

        return parent.hasType (schema::TRACK) && isFolder (parent) && isSongChannel (parent);
    }

    // What holds the song's modulators and macros: a channel of the song's, or a CONTAINER on one.
    bool isSongOwner (const juce::ValueTree& element)
    {
        return isSongChannel (element) || (element.hasType (schema::CONTAINER) && isSongChannel (element.getParent()));
    }

    // An owner's place as the song holds its modulators': a channel's, or a container's on its channel (ModulationValue.h:
    // readModulatorPlace(), of what is in it).
    ModulatorPlace readOwnerPlace (const juce::ValueTree& owner)
    {
        ModulatorPlace place;
        auto channel = owner;

        if (owner.hasType (schema::CONTAINER))
        {
            place.container = DeviceID::fromRaw (readID (owner[schema::deviceID]));
            channel = owner.getParent();
        }

        if (channel.hasType (schema::MASTER))
            place.onMaster = true;
        else if (channel.hasType (schema::TRACK))
            place.track = TrackID::fromRaw (readID (channel[schema::trackID]));

        return place;
    }

    // The list an owner's element names, without its parent (one taken out of the song): a container's by its ID, a
    // channel's by its track's, the master's by its type.
    ModulatorPlace readOwnerKey (const juce::ValueTree& owner)
    {
        ModulatorPlace key;

        if (owner.hasType (schema::CONTAINER))
            key.container = DeviceID::fromRaw (readID (owner[schema::deviceID]));
        else if (owner.hasType (schema::MASTER))
            key.onMaster = true;
        else
            key.track = TrackID::fromRaw (readID (owner[schema::trackID]));

        return key;
    }

    // Every MODULATOR and MACRO element in an element and what it holds, taken out: what the adoption leaves of a file's
    // anywhere in the song's tree once those of the song's owners are the area's. So the song's tree holds none.
    void dropModulatorElements (juce::ValueTree element)
    {
        for (auto i = element.getNumChildren(); --i >= 0;)
        {
            if (auto child = element.getChild (i); isModulatorElement (child))
                element.removeChild (i, nullptr);
            else
                dropModulatorElements (child);
        }
    }

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
SongValues::ModulationKeeper::ModulationKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

void SongValues::ModulationKeeper::remember (const juce::ValueTree& owner, SongModulation& next)
{
    // What it held now, in its list's order, in the place of what it may have been remembered with before (taken out, and
    // brought back by an undo since, which gave its modulators back itself).
    Remembered gone { owner, {} };

    if (const auto* list = next.findList (readOwnerKey (owner)))
    {
        const auto order = *list;

        for (const auto id : order)
        {
            if (const auto* value = next.find (id))
                gone.modulators.push_back (*value);

            next = withoutModulator (next, id);
        }
    }

    const auto found =
        std::find_if (remembered.begin(), remembered.end(), [&owner] (const Remembered& each) { return each.owner == owner; });

    if (found != remembered.end())
        remembered.erase (found);

    if (! gone.modulators.empty())
        remembered.push_back (std::move (gone));
}

bool SongValues::ModulationKeeper::giveBack (const juce::ValueTree& owner, const ModulatorPlace& place, SongModulation& next)
{
    const auto found =
        std::find_if (remembered.begin(), remembered.end(), [&owner] (const Remembered& each) { return each.owner == owner; });

    if (found == remembered.end())
        return false;

    const auto given = std::exchange (found->modulators, {});
    remembered.erase (found);

    for (auto value : given)
    {
        value.place = place;
        next = withHeldModulator (next, std::move (value));
    }

    return ! given.empty();
}

void SongValues::ModulationKeeper::sweep()
{
    if (remembered.size() < sweepAt)
        return;

    std::erase_if (remembered,
                   [this] (const Remembered& each) { return each.owner.getReferenceCount() <= 1 || each.owner.isAChildOf (state); });
    sweepAt = std::max<size_t> (64, remembered.size() * 2);
}

void SongValues::ModulationKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A lone one, added to an owner of the song's (d): refused in a step, read in and taken out outside the history.
    if (isModulatorElement (child))
    {
        if (! isSongOwner (parent))
            return;

        if (! song.isWritingOutsideHistory())
        {
            jassertfalse; // a modulator's or a macro's element on an owner of the song's: modulation's code adds one (Modulation.h)
            return;
        }

        if (auto value = readHeldModulator (child, readOwnerPlace (parent));
            value.has_value() && values.modulation.get().find (value->id) == nullptr)
        {
            ++values.modulationAdopted;
            values.modulation.setOutsideHistory (withHeldModulator (values.modulation.get(), std::move (*value)));
        }

        parent.removeChild (child, nullptr);
        return;
    }

    const auto isTrackList = child.hasType (schema::TRACKS) && parent == state;

    if (! isTrackList && ! isSongOwner (child))
        return;

    // A track (or the track list, the master, a container) put in the song: what each owner in it was remembered with given
    // back (a and b), and the elements each holds read in (c), in the elements' order, each owner's in its own.
    auto next = values.modulation.get();
    std::vector<juce::ValueTree> taken;
    auto changed = false;

    const std::function<void (const juce::ValueTree&)> takeIn = [this, &next, &taken, &changed, &takeIn] (const juce::ValueTree& owner)
    {
        if (owner.hasType (schema::TRACKS))
        {
            for (const auto& each : owner)
                if (each.hasType (schema::TRACK))
                    takeIn (each);

            return;
        }

        const auto place = readOwnerPlace (owner);
        changed = giveBack (owner, place, next) || changed;

        for (const auto& element : owner)
        {
            if (! isModulatorElement (element))
                continue;

            if (auto value = readHeldModulator (element, place); value.has_value() && next.find (value->id) == nullptr)
            {
                next = withHeldModulator (next, std::move (*value));
                ++values.modulationAdopted;
                changed = true;
            }

            taken.push_back (element);
        }

        if (owner.hasType (schema::CONTAINER))
            return;

        for (const auto& element : owner)
            if (element.hasType (schema::CONTAINER) || (element.hasType (schema::TRACK) && isFolder (owner)))
                takeIn (element);
    };

    takeIn (child);

    if (changed)
        values.setAllModulation (song, std::move (next));

    auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

    for (auto& element : taken)
        element.getParent().removeChild (element, undo);

    sweep();
}

void SongValues::ModulationKeeper::childRemoved (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A track (or the track list, the master) taken out of the song (a), or a container taken out of a channel of the
    // song's (b): what each owner in it holds remembered with its element, and erased.
    const auto isTrackList = child.hasType (schema::TRACKS) && parent == state;
    const auto isChannel =
        (child.hasType (schema::TRACK) && (parent.hasType (schema::TRACKS) ? parent.getParent() == state : isSongChannel (parent)))
        || (child.hasType (schema::MASTER) && parent == state);
    const auto isContainer = child.hasType (schema::CONTAINER) && isSongChannel (parent);

    if (! isTrackList && ! isChannel && ! isContainer)
        return;

    const auto before = values.modulation.get();
    auto next = before;

    const std::function<void (const juce::ValueTree&)> forget = [this, &next, &forget] (const juce::ValueTree& owner)
    {
        if (! owner.hasType (schema::TRACKS))
            remember (owner, next);

        if (owner.hasType (schema::CONTAINER))
            return;

        for (const auto& element : owner)
            if (element.hasType (schema::CONTAINER)
                || (element.hasType (schema::TRACK) && (owner.hasType (schema::TRACKS) || isFolder (owner))))
                forget (element);
    };

    forget (child);

    if (! next.isSameAs (before))
        values.setAllModulation (song, std::move (next));

    sweep();
}

void SongValues::ModulationKeeper::adopt (SongValues& into, Song& loaded)
{
    // Every modulator and macro the loader made consistent into the area, outside the history, as the song holds each, in the
    // elements' order (each track's, the master's, each channel's own then its containers'); what the loader left of each is
    // what the song holds of it (ModulationValue.h: keepModulatorAsHeld()), so the adoption changes nothing of what the file
    // held. Then every MODULATOR and MACRO element out of the tree, those anywhere else too, which are no modulators of the
    // song's (dropModulatorElements()).
    auto tree = loaded.getState();
    into.modulation.setOutsideHistory (
        readHeldModulation (tree, [] ([[maybe_unused]] const juce::ValueTree& element)
                            { jassert (isWrittenAsHeld (element, readModulatorPlace (element))); })); // the loader keeps each as held
    jassert (holdsTogether (into.modulation.get()));
    dropModulatorElements (tree);
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

    jassert (holdsTogether (next)); // a pass's, an edit's of several and the keeper's, each held together as it was worked out
    setArea (song, modulation, std::move (next));
    return true;
}

//==============================================================================
bool leadsToModulation (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if ((parent.hasType (schema::PHAZITE) || parent.hasType (schema::TRACKS) || parent.hasType (schema::TRACK))
        && (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK)))
        return true;

    if (parent.hasType (schema::PHAZITE) && child.hasType (schema::MASTER))
        return true;

    return (parent.hasType (schema::TRACK) || parent.hasType (schema::MASTER)) && child.hasType (schema::CONTAINER);
}

void writeModulationIn (juce::ValueTree& element, const ValuesKept& kept)
{
    const auto* list = findKeptList (kept.modulation, element);

    if (list == nullptr)
        return;

    // After its other children, but before a track's sends, which stay last.
    auto at = element.getNumChildren();

    for (int i = 0; i < element.getNumChildren(); ++i)
    {
        if (element.getChild (i).hasType (schema::SEND))
        {
            at = i;
            break;
        }
    }

    for (const auto id : *list)
        if (const auto* value = kept.modulation.find (id))
            element.addChild (writeModulatorElement (*value), at++, nullptr);
}

bool holdSameModulation (const ValuesKept& a, const ValuesKept& b)
{
    return a.modulation.isSameAs (b.modulation) || a.modulation == b.modulation;
}

bool holdSameModulator (const ValuesKept& a, const ValuesKept& b, ModulatorID id)
{
    const auto* first = a.modulation.find (id);
    const auto* second = b.modulation.find (id);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->isSameAs (*second) || *first == *second;
}

bool holdSameModulatorOrder (const ValuesKept& a, const ValuesKept& b, const ModulatorPlace& owner)
{
    const auto* first = a.modulation.findList (owner);
    const auto* second = b.modulation.findList (owner);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
}

bool holdSameModulationOf (const juce::ValueTree& element, const ValuesKept& kept, const juce::ValueTree& otherElement,
                           const ValuesKept& otherKept)
{
    const auto* list = findKeptList (kept.modulation, element);
    const auto* otherList = findKeptList (otherKept.modulation, otherElement);

    if (list == nullptr || otherList == nullptr)
        return list == otherList;

    if (! (*list == *otherList))
        return false;

    return std::all_of (list->begin(), list->end(),
                        [&kept, &otherKept] (ModulatorID id) { return holdSameModulator (kept, otherKept, id); });
}
} // namespace phazite

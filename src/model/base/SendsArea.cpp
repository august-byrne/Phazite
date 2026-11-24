// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SendsArea.h"
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
    using Change = SongValues::SendChange;
    using SendList = immer::flex_vector<DeviceID>;

    TrackID readTrackID (const juce::ValueTree& track)
    {
        return track.hasType (schema::TRACK) ? TrackID::fromRaw (readID (track[schema::trackID])) : TrackID();
    }

    // The tracks an element added to the song or taken out of it holds: itself, if a track; the tracks in it (a folder's),
    // however deep; the track list's. As readSendsValue() reads a song's sends: a track's TRACK children, folder or not.
    void collectTracks (const juce::ValueTree& element, std::vector<juce::ValueTree>& tracks)
    {
        if (element.hasType (schema::TRACK))
            tracks.push_back (element);

        if (element.hasType (schema::TRACK) || element.hasType (schema::TRACKS))
            for (const auto& child : element)
                if (child.hasType (schema::TRACK))
                    collectTracks (child, tracks);
    }

    // Every SEND element in an element and what it holds, taken out: what the adoption leaves of a file's anywhere in the
    // song's tree once those of the song's tracks are the area's. So the song's tree holds none.
    void dropSendElements (juce::ValueTree element)
    {
        for (auto i = element.getNumChildren(); --i >= 0;)
        {
            if (auto child = element.getChild (i); child.hasType (schema::SEND))
                element.removeChild (i, nullptr);
            else
                dropSendElements (child);
        }
    }

    // What of a send differs between two of its values (the same ID).
    std::uint8_t findParts (const SendValue& was, const SendValue& now)
    {
        std::uint8_t parts = 0;
        const auto add = [&parts] (std::uint8_t part) { parts = (std::uint8_t) (parts | part); };

        if (was.from != now.from)
            add (Change::made);

        if (was.to != now.to || was.preFader != now.preFader)
            add (Change::route);

        if (! juce::exactlyEqual (was.level, now.level))
            add (Change::level);

        return parts;
    }

    // Two entries of the area alike: one send's, the same value (a send's value is small, with no identity of its own to
    // compare), and two lists the same one.
    struct SameSend
    {
        bool operator() (const std::pair<std::uint64_t, SendValue>& a, const std::pair<std::uint64_t, SendValue>& b) const noexcept
        {
            return a.first == b.first && a.second == b.second;
        }
    };

    struct SameList
    {
        bool operator() (const std::pair<std::uint64_t, SendList>& a, const std::pair<std::uint64_t, SendList>& b) const noexcept
        {
            return a.first == b.first && a.second.identity() == b.second.identity();
        }
    };

    // Whether a list holds those of another that it still holds in another order.
    bool isReordered (const SendList& was, const SendList& now)
    {
        const auto holds = [] (const SendList& list, DeviceID id) { return std::find (list.begin(), list.end(), id) != list.end(); };
        std::vector<DeviceID> keptBefore, keptNow;

        for (const auto id : was)
            if (holds (now, id))
                keptBefore.push_back (id);

        for (const auto id : now)
            if (holds (was, id))
                keptNow.push_back (id);

        return keptBefore != keptNow;
    }

    void setArea (Song& song, ValueArea<SongSends>& area, SongSends next)
    {
        if (song.isWritingOutsideHistory())
            area.setOutsideHistory (std::move (next));
        else
            area.set (std::move (next));
    }

    // A track's list in kept values, none for a track without one (an element that is no TRACK: none).
    const SendList* findKeptList (const SongSends& sends, const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACK) ? sends.tracks.find (readID (element[schema::trackID])) : nullptr;
    }
} // namespace

//==============================================================================
SongValues::SendsTeller::SendsTeller (ValueArea<SongSends>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::SendsTeller::~SendsTeller()
{
    area.removeListener (this);
}

void SongValues::SendsTeller::valueAreaChanged (const SongSends& before, const SongSends& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // Each value after, in a copy of the value told, which stays while the listeners are told (cheap: it shares what it holds).
    const auto now = after;
    std::vector<SendChange> changes;

    const auto make = [] (DeviceID id, const SendValue* was, const SendValue* is, std::uint8_t parts)
    {
        SendChange change;
        change.id = id;
        change.parts = parts;

        if (was != nullptr)
        {
            change.fromBefore = was->from;
            change.toBefore = was->to;
        }

        if (is != nullptr)
        {
            change.fromAfter = is->from;
            change.toAfter = is->to;
        }

        return change;
    };

    before.sends.impl().template diff<SameSend> (
        now.sends.impl(), immer::make_differ ([&changes, &make] (const std::pair<std::uint64_t, SendValue>& added)
                                              { changes.push_back (make (added.second.id, nullptr, &added.second, Change::made)); },
                                              [&changes, &make] (const std::pair<std::uint64_t, SendValue>& removed)
                                              { changes.push_back (make (removed.second.id, &removed.second, nullptr, Change::made)); },
                                              [&changes, &make] (const std::pair<std::uint64_t, SendValue>& was,
                                                                 const std::pair<std::uint64_t, SendValue>& is)
                                              {
                                                  if (const auto parts = findParts (was.second, is.second); parts != 0)
                                                      changes.push_back (make (is.second.id, &was.second, &is.second, parts));
                                              }));

    // Each track whose list holds those it held before in another order.
    std::vector<TrackID> reordered;
    const auto ignore = [] (const std::pair<std::uint64_t, SendList>&) {};

    before.tracks.impl().template diff<SameList> (
        now.tracks.impl(),
        immer::make_differ (ignore, ignore,
                            [&reordered] (const std::pair<std::uint64_t, SendList>& was, const std::pair<std::uint64_t, SendList>& is)
                            {
                                if (isReordered (was.second, is.second))
                                    reordered.push_back (TrackID::fromRaw (is.first));
                            }));

    if (changes.empty() && reordered.empty())
        return;

    std::sort (changes.begin(), changes.end(), [] (const SendChange& a, const SendChange& b) { return a.id < b.id; });
    std::sort (reordered.begin(), reordered.end());

    for (auto& change : changes)
        change.now = change.isThere() ? now.find (change.id) : nullptr;

    listeners.call ([&changes, &reordered, &now] (SongValues::Listener& listener) { listener.sendsChanged (changes, reordered, now); });
}

//==============================================================================
SongValues::SendsKeeper::SendsKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

bool SongValues::SendsKeeper::isInTrackList (const juce::ValueTree& parent, const juce::ValueTree& child) const
{
    if (child.hasType (schema::TRACKS))
        return parent == state;

    for (auto element = parent; element.isValid(); element = element.getParent())
        if (element.hasType (schema::TRACKS))
            return element.getParent() == state;

    return false;
}

void SongValues::SendsKeeper::sweep()
{
    if (remembered.size() < sweepAt)
        return;

    std::erase_if (remembered,
                   [this] (const Remembered& each) { return each.track.getReferenceCount() <= 1 || each.track.isAChildOf (state); });
    sweepAt = std::max<size_t> (64, remembered.size() * 2);
}

void SongValues::SendsKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A lone one, added to a track of the song's (c): refused in a step, read in and taken out outside the history.
    if (child.hasType (schema::SEND))
    {
        if (! parent.hasType (schema::TRACK) || ! isInTrackList (parent, child))
            return;

        if (! song.isWritingOutsideHistory())
        {
            jassertfalse; // a send's element on a track of the song's: the sends' code adds one (model/mixer/internal/Mixer.h)
            return;
        }

        if (const auto from = readTrackID (parent); from.isValid())
        {
            if (auto value = readHeldSend (child, from); value.has_value() && values.sends.get().find (value->id) == nullptr)
            {
                ++values.sendsAdopted;
                values.sends.setOutsideHistory (withSend (values.sends.get(), std::move (*value)));
            }
        }

        parent.removeChild (child, nullptr);
        return;
    }

    if (! (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS)) || ! isInTrackList (parent, child))
        return;

    // A track (or what holds tracks) put in the song's track list: what each was remembered with given back (a), and the SEND
    // elements each holds read in (b), each track's in their order.
    std::vector<juce::ValueTree> trackElements;
    collectTracks (child, trackElements);
    auto next = values.sends.get();
    std::vector<juce::ValueTree> taken;
    auto changed = false;

    for (const auto& track : trackElements)
    {
        const auto from = readTrackID (track);

        if (const auto found =
                std::find_if (remembered.begin(), remembered.end(), [&track] (const Remembered& each) { return each.track == track; });
            found != remembered.end())
        {
            const auto given = std::exchange (found->sends, {});
            remembered.erase (found);

            for (auto value : given)
            {
                value.from = from;
                next = withSend (next, std::move (value));
                changed = true;
            }
        }

        for (const auto& element : track)
        {
            if (! element.hasType (schema::SEND))
                continue;

            if (auto value = from.isValid() ? readHeldSend (element, from) : std::nullopt;
                value.has_value() && next.find (value->id) == nullptr)
            {
                next = withSend (next, std::move (*value));
                ++values.sendsAdopted;
                changed = true;
            }

            taken.push_back (element);
        }
    }

    if (changed)
        values.setAllSends (song, std::move (next));

    auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

    for (auto& element : taken)
        element.getParent().removeChild (element, undo);

    sweep();
}

void SongValues::SendsKeeper::childRemoved (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (! (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS)) || ! isInTrackList (parent, child))
        return;

    // A track (or what holds tracks) taken out of the song's track list (a): each track's sends remembered with its element,
    // in the place of what it was remembered with before, or that forgotten where it holds none now; and erased.
    std::vector<juce::ValueTree> trackElements;
    collectTracks (child, trackElements);
    const auto before = values.sends.get();
    auto next = before;

    for (const auto& track : trackElements)
    {
        const auto found =
            std::find_if (remembered.begin(), remembered.end(), [&track] (const Remembered& each) { return each.track == track; });
        const auto* list = next.tracks.find (readTrackID (track).getRaw());

        if (list == nullptr)
        {
            if (found != remembered.end())
                remembered.erase (found);

            continue;
        }

        Remembered gone { track, {} };
        const auto order = *list;

        for (const auto id : order)
        {
            if (const auto* value = next.find (id))
                gone.sends.push_back (*value);

            next = withoutSend (next, id);
        }

        if (found != remembered.end())
            *found = std::move (gone);
        else
            remembered.push_back (std::move (gone));
    }

    if (! next.isSameAs (before))
        values.setAllSends (song, std::move (next));

    sweep();
}

void SongValues::SendsKeeper::adopt (SongValues& into, Song& loaded)
{
    // Every send the loader made consistent into the area, outside the history, as the song holds each, in the elements'
    // order (each track's, a folder's before those of the tracks in it, the first of an ID: readSendsValue()'s walk); what the
    // loader left of each is what the song holds of it (SendsValue.h: keepSendAsHeld()), so the adoption changes nothing of
    // what the file held. Then every SEND element out of the tree, those anywhere else too, which are no sends of the song's
    // (dropSendElements()).
    auto tree = loaded.getState();
    auto sends = into.sends.get();

    forEachTrackHeldSends (tree,
                           [&sends] (const juce::ValueTree&, TrackID from, const std::vector<HeldSendElement>& held)
                           {
                               for (const auto& each : held)
                               {
                                   jassert (isSendWrittenAsHeld (each.element)); // the loader keeps each as held

                                   if (auto value = readHeldSend (each.element, from))
                                       sends = withSend (sends, std::move (*value));
                               }
                           });

    jassert (holdsItsLists (sends));
    into.sends.setOutsideHistory (std::move (sends));
    dropSendElements (tree);
}

//==============================================================================
bool SongValues::setSend (Song& song, SendValue value, int index)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (! value.id.isValid())
        return false;

    if (const auto* was = sends.get().find (value.id); was != nullptr && *was == value)
        return false;

    setArea (song, sends, withSend (sends.get(), std::move (value), index));
    return true;
}

bool SongValues::eraseSend (Song& song, DeviceID id)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (sends.get().find (id) == nullptr)
        return false;

    setArea (song, sends, withoutSend (sends.get(), id));
    return true;
}

bool SongValues::eraseSends (Song& song, const std::vector<DeviceID>& ids)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    auto next = sends.get();
    auto any = false;

    for (const auto id : ids)
    {
        if (next.find (id) == nullptr)
            continue;

        next = withoutSend (next, id);
        any = true;
    }

    if (any)
        setArea (song, sends, std::move (next));

    return any;
}

bool SongValues::setTrackSendOrder (Song& song, TrackID track, immer::flex_vector<DeviceID> order)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    auto next = withTrackSendOrder (sends.get(), track, std::move (order));

    if (next.isSameAs (sends.get()))
        return false;

    setArea (song, sends, std::move (next));
    return true;
}

bool SongValues::setAllSends (Song& song, SongSends next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next.isSameAs (sends.get()) || next == sends.get())
        return false;

    jassert (holdsItsLists (next)); // a pass's, an edit's of several and the keeper's, each held together as it was worked out
    setArea (song, sends, std::move (next));
    return true;
}

//==============================================================================
int takeTrackSendsIn (SongSends& sends, const juce::ValueTree& track, TrackID from)
{
    auto taken = 0;

    for (const auto& element : track)
    {
        if (! element.hasType (schema::SEND))
            continue;

        if (auto value = readHeldSend (element, from); value.has_value() && sends.find (value->id) == nullptr)
        {
            sends = withSend (sends, std::move (*value));
            ++taken;
        }
    }

    return taken;
}

void writeSendsIn (juce::ValueTree& element, const ValuesKept& kept)
{
    if (const auto* list = findKeptList (kept.sends, element))
        for (const auto id : *list)
            if (const auto* value = kept.sends.find (id))
                element.appendChild (writeSendElement (*value), nullptr);
}

bool holdSameSends (const ValuesKept& a, const ValuesKept& b)
{
    return a.sends.isSameAs (b.sends) || a.sends == b.sends;
}

bool holdSameSend (const ValuesKept& a, const ValuesKept& b, DeviceID id)
{
    const auto* first = a.sends.find (id);
    const auto* second = b.sends.find (id);

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second;
}

bool holdSameTrackSendOrder (const ValuesKept& a, const ValuesKept& b, TrackID track)
{
    const auto* first = a.sends.tracks.find (track.getRaw());
    const auto* second = b.sends.tracks.find (track.getRaw());

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
}

bool holdSameSendsOf (const juce::ValueTree& element, const ValuesKept& kept, const juce::ValueTree& otherElement,
                      const ValuesKept& otherKept)
{
    const auto* list = findKeptList (kept.sends, element);
    const auto* otherList = findKeptList (otherKept.sends, otherElement);

    if (list == nullptr || otherList == nullptr)
        return list == otherList;

    if (! (*list == *otherList))
        return false;

    return std::all_of (list->begin(), list->end(), [&kept, &otherKept] (DeviceID id) { return holdSameSend (kept, otherKept, id); });
}
} // namespace phazite

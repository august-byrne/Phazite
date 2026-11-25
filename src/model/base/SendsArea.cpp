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
void readHeldSendsIn (const juce::ValueTree& song, SongState& into)
{
    // Every send the loader made consistent, as the song holds each, in the elements' order (each track's, a folder's before
    // those of the tracks in it, the first of an ID: readSendsValue()'s walk); what the loader left of each is what the song
    // holds of it (SendsValue.h: keepSendAsHeld()), so reading it changes nothing of what the file held.
    auto sends = into.sends;

    forEachTrackHeldSends (song,
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
    into.sends = std::move (sends);
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

    jassert (holdsItsLists (next)); // a pass's and an edit's of several, each held together as it was worked out
    setArea (song, sends, std::move (next));
    return true;
}

//==============================================================================
void writeSendsIn (juce::ValueTree& element, const SongState& kept)
{
    if (const auto* list = findKeptList (kept.sends, element))
        for (const auto id : *list)
            if (const auto* value = kept.sends.find (id))
                element.appendChild (writeSendElement (*value), nullptr);
}

bool holdSameSends (const SongState& a, const SongState& b)
{
    return a.sends.isSameAs (b.sends) || a.sends == b.sends;
}

bool holdSameSend (const SongState& a, const SongState& b, DeviceID id)
{
    const auto* first = a.sends.find (id);
    const auto* second = b.sends.find (id);

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second;
}

bool holdSameTrackSendOrder (const SongState& a, const SongState& b, TrackID track)
{
    const auto* first = a.sends.tracks.find (track.getRaw());
    const auto* second = b.sends.tracks.find (track.getRaw());

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
}
} // namespace phazite

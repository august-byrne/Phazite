// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SendsValue.h"
#include "Song.h"
#include "internal/IDsHeld.h"
#include "internal/Schema.h"
#include "internal/SongText.h"
#include "internal/SongValues.h"
#include "internal/TracksValue.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace phazite
{
bool SendValue::operator== (const SendValue& other) const noexcept
{
    return id == other.id && from == other.from && to == other.to && juce::exactlyEqual (level, other.level) && preFader == other.preFader;
}

bool SongSends::operator== (const SongSends& other) const
{
    return isSameAs (other) || (sends == other.sends && tracks == other.tracks);
}

//==============================================================================
namespace
{
    std::atomic<std::uint64_t> sendsRead { 0 }, sendLooks { 0 }; // countSendsRead(), countSendLooks(): counts alone

    using SendList = immer::flex_vector<DeviceID>;
    using SendLists = immer::map<std::uint64_t, SendList>;

    // The lists without a send under a track, the track's entry gone with its last send (the same lists for a track of none,
    // and for a send its list hasn't).
    SendLists withoutIn (const SendLists& lists, TrackID track, DeviceID send)
    {
        if (! track.isValid())
            return lists;

        if (const auto* list = lists.find (track.getRaw()))
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                if ((*list)[i] == send)
                {
                    const auto rest = list->erase (i);
                    return rest.empty() ? lists.erase (track.getRaw()) : lists.set (track.getRaw(), rest);
                }
            }
        }

        return lists;
    }

    // The lists with a send under a track, at a place (or after the others), none for a track of none.
    SendLists withIn (const SendLists& lists, TrackID track, DeviceID send, int index)
    {
        if (! track.isValid())
            return lists;

        const auto* found = lists.find (track.getRaw());
        auto list = found != nullptr ? *found : SendList();
        const auto place = index >= 0 ? std::min ((size_t) index, list.size()) : list.size();
        return lists.set (track.getRaw(), list.insert (place, send));
    }

    // Whether an ID is one the song can count on from (Schema.h: schema::maxID), as every item's is once the song is loaded.
    bool isHeldID (DeviceID id)
    {
        return id.isValid() && id.getRaw() < schema::maxID;
    }

    // An ID an element holds or names, as readID() reads one, a number or text: a number's own value, with no text made of it
    // (the sends' readers look through a track's sends by their IDs, as a control's handle checks its send at each tick).
    std::uint64_t readHeldID (const juce::var& held)
    {
        return held.isInt() || held.isInt64() ? (std::uint64_t) (juce::int64) held : readID (held);
    }

    // A send's level as its description reads it, as a float (the song's sends' levels are floats: Schema.h, sendLevel).
    float readLevel (const juce::var& held)
    {
        return (float) readDescribed (held, schema::sendLevel);
    }

    // Each property Phazite's writer writes of a send (writeSendElement()), given to `each` as its name and its value, in the
    // writer's order: the one list of them, which the writer and the loader's rule (keepSendAsHeld()) both go by. Nothing
    // allocated.
    template <typename Each>
    void forEachWrittenProperty (const SendValue& send, Each&& each)
    {
        each (schema::sendID.getName(), juce::var ((juce::int64) send.id.getRaw()));
        each (schema::sendTo.getName(), juce::var ((juce::int64) send.to.getRaw()));

        // None for 0 dB, as setSendLevel() keeps it (Mixer.cpp). The float the value holds, which a file's text holds as the
        // fewest digits that read back as it (SongText.h: writeValue()).
        if (! juce::exactlyEqual (send.level, 0.0f))
            each (schema::sendLevel.getName(), juce::var (send.level));

        if (send.preFader)
            each (schema::sendPreFader.getName(), juce::var (true));
    }

    // Whether an element holds what the writer writes of a send and nothing else: each property as the same text
    // (SongText.h: isSameValue()), no other, and no child. Nothing allocated.
    bool holdsAsWritten (const juce::ValueTree& element, const SendValue& held)
    {
        auto written = 0;
        auto same = element.getNumChildren() == 0;

        forEachWrittenProperty (held,
                                [&element, &written, &same] (const juce::Identifier& property, const juce::var& value)
                                {
                                    const auto* there = element.getPropertyPointer (property);
                                    same = same && there != nullptr && isSameValue (*there, value);
                                    ++written;
                                });

        return same && element.getNumProperties() == written;
    }

    // Each TRACK under an element (the track list, a folder), a folder before the tracks in it, as the song orders its tracks
    // (model/arrangement/internal/Tracks.h: getTrackOrder()).
    void forEachTrack (const juce::ValueTree& parent, const std::function<void (const juce::ValueTree&)>& visit)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
            {
                visit (child);
                forEachTrack (child, visit);
            }
        }
    }
} // namespace

SongSends withSend (const SongSends& sends, SendValue send, int index)
{
    jassert (send.id.isValid());
    auto next = sends;
    const auto* old = sends.find (send.id);

    if (old == nullptr || old->from != send.from)
    {
        if (old != nullptr)
            next.tracks = withoutIn (next.tracks, old->from, send.id);

        next.tracks = withIn (next.tracks, send.from, send.id, index);
    }

    const auto key = send.id.getRaw(); // read before the value is moved: a call's arguments come in no set order
    next.sends = next.sends.set (key, std::move (send));
    return next;
}

SongSends withoutSend (const SongSends& sends, DeviceID id)
{
    const auto* old = sends.find (id);

    if (old == nullptr)
        return sends;

    auto next = sends;
    next.tracks = withoutIn (next.tracks, old->from, id);
    next.sends = next.sends.erase (id.getRaw());
    return next;
}

SongSends withTrackSendOrder (const SongSends& sends, TrackID track, immer::flex_vector<DeviceID> order)
{
    const auto* list = sends.tracks.find (track.getRaw());
    const auto now = list != nullptr ? *list : SendList();
    std::vector<DeviceID> held (now.begin(), now.end()), given (order.begin(), order.end());
    std::sort (held.begin(), held.end());
    std::sort (given.begin(), given.end());

    if (held != given)
    {
        jassertfalse; // the same sends, in another order
        return sends;
    }

    if (order.empty() || std::equal (order.begin(), order.end(), now.begin(), now.end()))
        return sends; // the order it holds: the same value

    auto next = sends;
    next.tracks = next.tracks.set (track.getRaw(), std::move (order));
    return next;
}

bool holdsItsLists (const SongSends& sends)
{
    size_t listed = 0, onTracks = 0;

    for (const auto& [key, list] : sends.tracks)
    {
        if (list.empty())
            return false;

        std::vector<DeviceID> seen (list.begin(), list.end());
        std::sort (seen.begin(), seen.end());

        if (std::adjacent_find (seen.begin(), seen.end()) != seen.end())
            return false;

        for (const auto& id : seen)
            if (const auto* send = sends.find (id); send == nullptr || send->from.getRaw() != key)
                return false;

        listed += list.size();
    }

    for (const auto& [id, send] : sends.sends)
    {
        if (id != send.id.getRaw())
            return false;

        onTracks += send.from.isValid() ? size_t { 1 } : size_t { 0 };
    }

    return listed == onTracks;
}

//==============================================================================
bool setSendProperty (SendValue& send, const juce::Identifier& property, const juce::var& value)
{
    if (property == schema::sendID)
        send.id = DeviceID::fromRaw (readHeldID (value));
    else if (property == schema::sendTo)
        send.to = TrackID::fromRaw (readHeldID (value));
    else if (property == schema::sendLevel)
        send.level = readLevel (value);
    else if (property == schema::sendPreFader)
        send.preFader = (bool) value;
    else
        return false;

    return true;
}

SendValue readSendValue (const juce::ValueTree& element, TrackID from)
{
    SendValue send;
    send.from = from;

    for (const auto* property :
         { &schema::sendID.getName(), &schema::sendTo.getName(), &schema::sendLevel.getName(), &schema::sendPreFader.getName() })
        setSendProperty (send, *property, element[*property]);

    sendsRead.fetch_add (1, std::memory_order_relaxed);
    return send;
}

std::uint64_t countSendsRead() noexcept
{
    return sendsRead.load (std::memory_order_relaxed);
}

std::uint64_t countSendLooks() noexcept
{
    return sendLooks.load (std::memory_order_relaxed);
}

void countSendLook() noexcept
{
    sendLooks.fetch_add (1, std::memory_order_relaxed);
}

float keepSendLevel (float decibels)
{
    // Its description's reading of the float (within its range, 0 for no finite number), in ten-thousandths: rounded from the
    // float, so that a level set and the same level read from a file's text round alike.
    const auto rounded = std::round (readDescribed (juce::var ((double) decibels), schema::sendLevel) * 1.0e4) / 1.0e4;
    return std::abs (rounded) < 1.0e-4 ? 0.0f : (float) rounded;
}

std::optional<SendValue> readHeldSend (const juce::ValueTree& element, TrackID from)
{
    if (! element.hasType (schema::SEND))
        return std::nullopt;

    auto send = readSendValue (element, from);
    send.level = keepSendLevel (send.level); // as readSendValue() read it: its description's reading, as a float
    return isHeldID (send.id) ? std::optional<SendValue> (std::move (send)) : std::nullopt;
}

juce::ValueTree writeSendElement (const SendValue& send)
{
    juce::ValueTree element (schema::SEND);
    forEachWrittenProperty (send, [&element] (const juce::Identifier& property, const juce::var& value)
                            { element.setProperty (property, value, nullptr); });
    return element;
}

bool keepSendAsHeld (juce::ValueTree element)
{
    // Read as the song holds it (no allocation: its numbers, its switch), and written again only where it isn't so.
    const auto held = readHeldSend (element, {});

    if (! held.has_value() || holdsAsWritten (element, *held))
        return false;

    element.removeAllChildren (nullptr);
    element.removeAllProperties (nullptr);
    forEachWrittenProperty (*held, [&element] (const juce::Identifier& property, const juce::var& value)
                            { element.setProperty (property, value, nullptr); });
    return true;
}

bool isSendWrittenAsHeld (const juce::ValueTree& element)
{
    const auto held = readHeldSend (element, {});
    return held.has_value() && holdsAsWritten (element, *held);
}

std::vector<SendValue> readTrackSends (const juce::ValueTree& track)
{
    std::vector<SendValue> sends;

    if (! track.hasType (schema::TRACK))
        return sends;

    const auto from = TrackID::fromRaw (readHeldID (track[schema::trackID]));

    for (const auto& child : track)
        if (child.hasType (schema::SEND))
            sends.push_back (readSendValue (child, from));

    return sends;
}

std::optional<SendValue> findTrackSend (const juce::ValueTree& track, DeviceID id)
{
    if (! id.isValid() || ! track.hasType (schema::TRACK))
        return std::nullopt;

    for (const auto& child : track)
        if (child.hasType (schema::SEND) && readHeldID (child[schema::sendID]) == id.getRaw())
            return readSendValue (child, TrackID::fromRaw (readHeldID (track[schema::trackID])));

    return std::nullopt;
}

DeviceID readHeldSendID (const juce::ValueTree& sendElement)
{
    const auto id = DeviceID::fromRaw (readHeldID (sendElement[schema::sendID]));
    return sendElement.hasType (schema::SEND) && isHeldID (id) ? id : DeviceID();
}

void forEachTrackHeldSends (const juce::ValueTree& song,
                            const std::function<void (const juce::ValueTree&, TrackID, const std::vector<HeldSendElement>&)>& visit)
{
    std::unordered_set<std::uint64_t> seen;
    std::vector<HeldSendElement> held;

    forEachTrack (song.getChildWithName (schema::TRACKS),
                  [&seen, &held, &visit] (const juce::ValueTree& track)
                  {
                      const auto from = TrackID::fromRaw (readHeldID (track[schema::trackID]));

                      if (! from.isValid())
                          return;

                      held.clear();

                      for (const auto& child : track)
                          if (const auto id = readHeldSendID (child); id.isValid() && seen.insert (id.getRaw()).second)
                              held.push_back ({ .id = id, .element = child });

                      visit (track, from, held);
                  });
}

SongSends readSendsValue (const juce::ValueTree& song)
{
    auto sends = immer::map<std::uint64_t, SendValue>().transient();
    auto tracks = immer::map<std::uint64_t, SendList>().transient();

    forEachTrackHeldSends (song,
                           [&sends, &tracks] (const juce::ValueTree&, TrackID from, const std::vector<HeldSendElement>& held)
                           {
                               if (held.empty())
                                   return;

                               auto list = SendList().transient();

                               for (const auto& each : held)
                               {
                                   list.push_back (each.id);
                                   sends.set (each.id.getRaw(), readSendValue (each.element, from));
                               }

                               tracks.set (from.getRaw(), list.persistent());
                           });

    return { sends.persistent(), tracks.persistent() };
}

SongSends getSendsValue (const Song& song)
{
    countSendLook();
    return song.getValues()->sends.get();
}

bool holdsSend (const Song& song, DeviceID id)
{
    if (! id.isValid())
        return false;

    countSendLook();
    return song.getValues()->sends.get().find (id) != nullptr;
}

//==============================================================================
std::uint64_t findLargestIDIn (const SongSends& sends)
{
    std::uint64_t largest = 0;

    for (const auto& entry : sends.sends)
        largest = countHeldID (largest, entry.second.id.getRaw());

    return largest;
}

void forEachIDHeld (const SongSends& sends, const std::function<void (std::uint64_t)>& visit)
{
    for (const auto& entry : sends.sends)
        visit (entry.second.id.getRaw());
}

void forEachIDNamed (const SongSends& sends, const std::function<void (std::uint64_t)>& visit)
{
    for (const auto& entry : sends.sends)
        if (entry.second.to.isValid())
            visit (entry.second.to.getRaw());
}
} // namespace phazite

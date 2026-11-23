// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SendsArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = SongValues::SendChange;

    // A TRACK of the song's, as readSendsValue() reads their sends: one of its track list, or one in a track that is (a
    // folder's).
    bool isSongTrack (const juce::ValueTree& element)
    {
        if (! element.hasType (schema::TRACK))
            return false;

        const auto parent = element.getParent();

        if (parent.hasType (schema::TRACKS))
            return parent.getParent().hasType (schema::PHAZITE);

        return isSongTrack (parent);
    }

    // The song's track list, or what holds it.
    bool isSongTrackList (const juce::ValueTree& element)
    {
        return (element.hasType (schema::TRACKS) && element.getParent().hasType (schema::PHAZITE)) || element.hasType (schema::PHAZITE);
    }

    TrackID readTrackID (const juce::ValueTree& track)
    {
        return TrackID::fromRaw (readID (track[schema::trackID]));
    }

    DeviceID readSendID (const juce::ValueTree& send)
    {
        return DeviceID::fromRaw (readID (send[schema::sendID]));
    }

    // Whether an ID is one the song holds a send by (SendsValue.h: readHeldSend()).
    bool isHeldID (DeviceID id)
    {
        return id.isValid() && id.getRaw() < schema::maxID;
    }

    // The song's SEND an element is, or is in; an invalid tree for none. Looked for upwards only as far as what holds no send
    // inside it, as a fader's change is heard often.
    juce::ValueTree findSongSend (const juce::ValueTree& element)
    {
        for (auto each = element; each.isValid(); each = each.getParent())
        {
            if (each.hasType (schema::SEND))
                return isSongTrack (each.getParent()) ? each : juce::ValueTree();

            if (each.hasType (schema::TRACK) || each.hasType (schema::MASTER) || each.hasType (schema::CONTAINER)
                || each.hasType (schema::DEVICE) || each.hasType (schema::CHAIN) || each.hasType (schema::MODULATOR)
                || each.hasType (schema::MACRO) || each.hasType (schema::TRACKS) || each.hasType (schema::PHAZITE))
                return {};
        }

        return {};
    }

    // Whether a TRACK, or the track list, holds a SEND, itself or in a track in it (a folder's): what can change the song's
    // sends as it comes, goes, or is given another order.
    bool holdsSendsIn (const juce::ValueTree& element)
    {
        for (const auto& child : element)
            if (child.hasType (schema::SEND) || (child.hasType (schema::TRACK) && holdsSendsIn (child)))
                return true;

        return false;
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

    // A change of a send from one value to another (none for one that wasn't there, or isn't), its value after given as it is
    // told (tell()).
    Change makeChange (DeviceID id, const SendValue* was, const SendValue* now, std::uint8_t parts)
    {
        Change change;
        change.id = id;
        change.parts = parts;

        if (was != nullptr)
        {
            change.fromBefore = was->from;
            change.toBefore = was->to;
        }

        if (now != nullptr)
        {
            change.fromAfter = now->from;
            change.toAfter = now->to;
        }

        return change;
    }

    // Each TRACK under an element (the track list, a folder), a folder before the tracks in it, as readSendsValue() reads
    // them; until `each` returns true, whether it did.
    bool findTrack (const juce::ValueTree& parent, const std::function<bool (const juce::ValueTree&)>& each)
    {
        for (const auto& child : parent)
            if (child.hasType (schema::TRACK) && (each (child) || findTrack (child, each)))
                return true;

        return false;
    }
} // namespace

//==============================================================================
SongValues::SendsKeeper::SendsKeeper (SongValues& v, Song& s)
    : values (v),
      song (s)
{
    Heard heard;
    settle (heard); // what the song holds at the start, which no one is told
}

void SongValues::SendsKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    Heard heard;

    if (const auto send = findSongSend (tree); send.isValid())
    {
        // Given another ID: the song's sends found again. Anything else: itself.
        if (send == tree && property == schema::sendID)
            settle (heard);
        else
            reread (send, heard);
    }
    else if (property == schema::trackID && isSongTrack (tree) && holdsSendsIn (tree))
    {
        settle (heard); // a track given another ID (a merge's renumbering): its sends come to another track
    }

    tell (heard);
}

void SongValues::SendsKeeper::childAdded (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    Heard heard;

    if (child.hasType (schema::SEND) && isSongTrack (parent))
        settle (heard); // one added to its track
    else if (const auto send = findSongSend (parent); send.isValid())
        reread (send, heard); // something in one, which a send's readers don't read
    else if ((child.hasType (schema::TRACK) || child.hasType (schema::TRACKS)) && (isSongTrack (parent) || isSongTrackList (parent))
             && holdsSendsIn (child))
        settle (heard); // tracks that came with theirs

    tell (heard);
}

void SongValues::SendsKeeper::childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    Heard heard;

    if (child.hasType (schema::SEND) && isSongTrack (parent))
        settle (heard);
    else if (const auto send = findSongSend (parent); send.isValid())
        reread (send, heard);
    else if ((child.hasType (schema::TRACK) || child.hasType (schema::TRACKS)) && (isSongTrack (parent) || isSongTrackList (parent))
             && holdsSendsIn (child))
        settle (heard); // tracks that went with theirs

    tell (heard);
}

void SongValues::SendsKeeper::childOrderChanged (const juce::ValueTree& parent)
{
    Heard heard;

    // A track's children (its sends among them, or a folder's tracks), or the song's tracks, in another order.
    if ((isSongTrack (parent) || (parent.hasType (schema::TRACKS) && isSongTrackList (parent))) && holdsSendsIn (parent))
        settle (heard);

    tell (heard);
}

void SongValues::SendsKeeper::reread (const juce::ValueTree& send, Heard& heard)
{
    // Only the element the song reads its send from: another of its ID is read by no one (its ID didn't change: a change of it
    // finds the song's sends again).
    const auto id = readHeldSendID (send);
    const auto element = elements.find (id.getRaw());
    const auto* was = told.find (id);

    if (element == elements.end() || element->second != send || was == nullptr)
        return;

    auto now = readSendValue (send, was->from);

    if (const auto parts = findParts (*was, now); parts != 0)
    {
        heard.changes.push_back (makeChange (id, was, &now, parts));
        told.sends = told.sends.set (id.getRaw(), std::move (now));
    }
}

void SongValues::SendsKeeper::settle (Heard& heard)
{
    // The song's SEND elements as readSendsValue() finds them: each send's element and track, in the song's order, and each
    // track's list (a later track of the same ID holding sends replaces an earlier one's, as there).
    struct Found
    {
        DeviceID id {};
        juce::ValueTree element {};
        TrackID from {};
    };

    std::vector<Found> found;
    std::unordered_map<std::uint64_t, size_t> foundAt;
    std::map<std::uint64_t, std::vector<DeviceID>> lists;

    forEachTrackHeldSends (song.getState(),
                           [&found, &foundAt, &lists] (const juce::ValueTree&, TrackID from, const std::vector<HeldSendElement>& held)
                           {
                               if (held.empty())
                                   return;

                               auto& list = lists[from.getRaw()];
                               list.clear();

                               for (const auto& each : held)
                               {
                                   foundAt[each.id.getRaw()] = found.size();
                                   found.push_back ({ .id = each.id, .element = each.element, .from = from });
                                   list.push_back (each.id);
                               }
                           });

    auto next = told;

    // Those it held that it holds no longer: taken out.
    for (const auto& [key, was] : told.sends)
    {
        if (foundAt.count (key) > 0)
            continue;

        heard.changes.push_back (makeChange (was.id, &was, nullptr, Change::made));
        next.sends = next.sends.erase (key);
        elements.erase (key);
    }

    // Those that came, from nowhere or from another track, or are read from another element now: each read as it is now.
    for (const auto& each : found)
    {
        const auto key = each.id.getRaw();
        const auto* was = told.find (each.id);
        const auto element = elements.find (key);

        if (was != nullptr && was->from == each.from && element != elements.end() && element->second == each.element)
            continue;

        auto now = readSendValue (each.element, each.from);
        const auto parts = was != nullptr ? findParts (*was, now) : (std::uint8_t) Change::made;

        if (parts != 0)
            heard.changes.push_back (makeChange (each.id, was, &now, parts));

        elements[key] = each.element;
        next.sends = next.sends.set (key, std::move (now));
    }

    // Each track's list in its element's order; reordered where those it held before are in another order.
    for (const auto& [key, list] : told.tracks)
        if (lists.count (key) == 0)
            next.tracks = next.tracks.erase (key);

    for (const auto& [key, order] : lists)
    {
        const auto* was = told.tracks.find (key);

        if (was != nullptr && std::equal (was->begin(), was->end(), order.begin(), order.end()))
            continue;

        if (was != nullptr)
        {
            const std::vector<DeviceID> before (was->begin(), was->end());
            const auto isIn = [] (const std::vector<DeviceID>& ids, DeviceID id)
            { return std::find (ids.begin(), ids.end(), id) != ids.end(); };
            std::vector<DeviceID> keptBefore, keptNow;

            for (const auto id : before)
                if (isIn (order, id))
                    keptBefore.push_back (id);

            for (const auto id : order)
                if (isIn (before, id))
                    keptNow.push_back (id);

            if (keptBefore != keptNow)
                heard.reordered.push_back (TrackID::fromRaw (key));
        }

        next.tracks = next.tracks.set (key, immer::flex_vector<DeviceID> (order.begin(), order.end()));
    }

    told = std::move (next);
}

void SongValues::SendsKeeper::tell (Heard& heard) const
{
    if (heard.changes.empty() && heard.reordered.empty())
        return;

    // Each value after, in a copy of what the listeners are told now, which stays while they are told whatever one of them
    // changes meanwhile (cheap: it shares what it holds).
    const auto now = told;

    for (auto& change : heard.changes)
        change.now = change.isThere() ? now.find (change.id) : nullptr;

    values.listeners.call ([&heard] (SongValues::Listener& listener) { listener.sendsChanged (heard.changes, heard.reordered); });
}

//==============================================================================
bool isHeardAsSends (const juce::ValueTree& element)
{
    return findSongSend (element).isValid();
}

bool isHeardAsSends (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (child.hasType (schema::SEND) && isSongTrack (parent)) || isHeardAsSends (parent);
}

//==============================================================================
FoundSendElement findSendElement (const juce::ValueTree& song, DeviceID id)
{
    FoundSendElement found;

    if (! isHeldID (id))
        return found;

    findTrack (song.getChildWithName (schema::TRACKS),
               [&found, id] (const juce::ValueTree& track)
               {
                   for (const auto& child : track)
                   {
                       if (child.hasType (schema::SEND) && readSendID (child) == id)
                       {
                           found = { .element = child, .track = readTrackID (track) };
                           return true;
                       }
                   }

                   return false;
               });

    return found;
}

std::vector<DeviceID> readTrackSendOrder (const juce::ValueTree& song, TrackID track)
{
    std::vector<DeviceID> order;

    findTrack (song.getChildWithName (schema::TRACKS),
               [&order, track] (const juce::ValueTree& candidate)
               {
                   if (readTrackID (candidate) != track)
                       return false;

                   for (const auto& child : candidate)
                       if (const auto id = readSendID (child);
                           child.hasType (schema::SEND) && isHeldID (id) && std::find (order.begin(), order.end(), id) == order.end())
                           order.push_back (id);

                   return true;
               });

    return order;
}
} // namespace phazite

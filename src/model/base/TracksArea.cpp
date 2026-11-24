// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/TracksArea.h"
#include "Song.h"
#include "internal/ClipsArea.h"
#include "internal/DeviceStoreArea.h"
#include "internal/DevicesArea.h"
#include "internal/ModulationArea.h"
#include "internal/Schema.h"
#include "internal/SendsArea.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = TrackChange;
    using Field = TrackSwitchChange::Field;
    using IDList = immer::flex_vector<TrackID>;

    std::atomic<std::uint64_t> elementsWalked { 0 };      // countTrackElementsWalked()
    std::atomic<std::uint64_t> listEntriesCompared { 0 }; // countTrackListEntriesCompared()
    std::atomic<bool> checkingTrackOrder { false };       // SongValues::setCheckingTrackOrder()

    void countWalked()
    {
        elementsWalked.fetch_add (1, std::memory_order_relaxed);
    }

    void countCompared (size_t entries)
    {
        listEntriesCompared.fetch_add ((std::uint64_t) entries, std::memory_order_relaxed);
    }

    //==============================================================================
    // Where an element is: the song's root, its track list, its master, the master's LANES.
    bool isSongRoot (const juce::ValueTree& element)
    {
        return element.hasType (schema::PHAZITE) && ! element.getParent().isValid();
    }

    bool isSongTrackList (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACKS) && isSongRoot (element.getParent());
    }

    bool isSongMaster (const juce::ValueTree& element)
    {
        return element.hasType (schema::MASTER) && isSongRoot (element.getParent());
    }

    bool isMasterLanes (const juce::ValueTree& element)
    {
        return element.hasType (schema::LANES) && isSongMaster (element.getParent());
    }

    bool isLaneProperty (const juce::Identifier& property)
    {
        return property == schema::laneDevice.getName() || property == schema::laneParameter.getName()
               || property == schema::laneHeight.getName() || property == schema::laneCollapsed.getName();
    }

    // An ID as the song holds it (readHeldTrack()): none for one it can't count on from.
    std::uint64_t readHeldIDOf (const juce::var& held)
    {
        const auto id = readID (held);
        return id > 0 && id < schema::maxID ? id : 0;
    }

    //==============================================================================
    // The tracks of a list (a track list, a folder) read as the song holds them, each folder before the tracks in it, each
    // element counted (countTrackElementsWalked()): one whose ID the song can't count on, or that `take` refuses (of an ID
    // read already, its folder refused), left out with the tracks in it.
    using Take = std::function<bool (const juce::ValueTree& element, TrackValue value)>;

    void readTracksIn (const juce::ValueTree& list, const Take& take)
    {
        for (const auto& child : list)
        {
            if (! child.hasType (schema::TRACK))
                continue;

            countWalked();
            auto value = readHeldTrack (child);

            if (! value.has_value())
                continue;

            const auto folder = value->isFolder();

            if (take (child, std::move (*value)) && folder)
                readTracksIn (child, take);
        }
    }

    // A song's tracks, read once from its tree (readHeldTracks(), the adoption).
    SongTracks readHeldAll (const juce::ValueTree& song)
    {
        SongTracks tracks;

        readTracksIn (song.getChildWithName (schema::TRACKS),
                      [&tracks] (const juce::ValueTree&, TrackValue value)
                      {
                          if (tracks.tracks.find (value.id.getRaw()) != nullptr)
                              return false;

                          auto next = withTrack (tracks, std::move (value));

                          if (next.isSameAs (tracks))
                              return false; // refused: its folder isn't one

                          tracks = std::move (next);
                          return true;
                      });

        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
            if (auto value = readHeldTrack (master))
                tracks.master = std::move (*value);

        return tracks;
    }

    // Each TRACK an element is or holds (a folder's tracks, the track list's), and the MASTER, in the tree's order, a folder
    // before the tracks in it: what the switches' and the view's part of the keeper looks through.
    void forEachChannelIn (const juce::ValueTree& element, const std::function<void (juce::ValueTree)>& each)
    {
        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
            each (element);

        if (element.hasType (schema::TRACK) || element.hasType (schema::TRACKS))
            for (const auto& child : element)
                if (child.hasType (schema::TRACK))
                    forEachChannelIn (child, each);
    }

    // What an element of a channel holds of the switches and the view, read as the song holds them into the values given (a
    // TRACK's under its ID, the MASTER's under 0 and the master's row): the element is what the track holds, so its entries
    // replace those the values hold of its key. Nothing for a TRACK whose ID the song can't count on.
    void readLeavesOf (const juce::ValueTree& element, TrackSwitches& switches, TracksOutside& outside)
    {
        readHeldSwitchesOf (element, switches);

        if (element.hasType (schema::MASTER))
            outside = withMasterView (outside, readHeldViewOf (element));
        else if (const auto id = readHeldIDOf (element[schema::trackID]); id != 0)
            outside = withTrackView (outside, id, readHeldViewOf (element));
    }

    //==============================================================================
    // What of a track differs between two of its values (the same ID).
    std::uint16_t findParts (const TrackValue& was, const TrackValue& now)
    {
        std::uint16_t parts = 0;
        const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

        if (was.parent != now.parent)
            add (Change::place);

        if (was.kind != now.kind)
            add (Change::kind);

        if (was.bus != now.bus)
            add (Change::bus);

        if (was.fader != now.fader || was.vca != now.vca)
            add (Change::fader);

        if (was.name != now.name)
            add (Change::name);

        if (was.colour != now.colour)
            add (Change::colour);

        if (! juce::exactlyEqual (was.volume, now.volume) || ! juce::exactlyEqual (was.pan, now.pan))
            add (Change::mixer);

        if (was.key != now.key)
            add (Change::key);

        if (was.input != now.input || was.monitoring != now.monitoring)
            add (Change::recording);

        if (was.padDevice != now.padDevice || was.pad != now.pad)
            add (Change::pad);

        return parts;
    }

    // A track's place in its folder's list (the top list for none), or -1.
    int findIndex (const SongTracks& tracks, TrackID parent, TrackID id)
    {
        if (const auto* list = tracks.findList (parent))
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                if ((*list)[i] == id)
                {
                    countCompared (i + 1);
                    return (int) i;
                }
            }

            countCompared (list->size());
        }

        return -1;
    }

    // Whether those of one list that are in the other too are in the same order in both (one added or taken out moves no
    // other): each of `after` by its place in it, then those of `before` that `after` holds, their places there rising. Each
    // list's entries looked at once (a list holds each ID once), `after`'s places in one sorted vector (one allocation, however
    // long the list: a track moved in 200 allocates as in 20).
    bool keepOrder (const IDList& before, const IDList& after)
    {
        std::vector<std::pair<std::uint64_t, size_t>> places;
        places.reserve (after.size());

        for (const auto id : after)
            places.emplace_back (id.getRaw(), places.size());

        std::sort (places.begin(), places.end());
        countCompared (after.size() + before.size());
        std::optional<size_t> last;

        for (const auto id : before)
        {
            const auto found = std::lower_bound (places.begin(), places.end(), id.getRaw(),
                                                 [] (const auto& place, std::uint64_t raw) { return place.first < raw; });

            if (found != places.end() && found->first == id.getRaw())
            {
                if (last.has_value() && found->second < *last)
                    return false;

                last = found->second;
            }
        }

        return true;
    }

    // Each entry of a map of switches that changed, as the listener is told it: the master's mode under 0.
    template <typename Entry>
    void addSwitchChanges (std::vector<TrackSwitchChange>& changes, const std::vector<MapChange<std::uint64_t, Entry>>& found, Field field,
                           TrackID master)
    {
        for (const auto& each : found)
        {
            TrackSwitchChange change;
            change.field = field;
            change.onMaster = field == Field::automationMode && each.key == 0;
            change.track = change.onMaster ? master : TrackID::fromRaw (each.key);
            change.appeared = ! each.before.has_value() && each.after.has_value();
            change.went = each.before.has_value() && ! each.after.has_value();
            changes.push_back (change);
        }
    }

    // The tracks' area set, in the mode of the moment (SongValues.h: the setters).
    void setTracksArea (Song& song, ValueArea<SongTracks>& area, SongTracks next)
    {
        if (song.isWritingOutsideHistory())
            area.setOutsideHistory (std::move (next));
        else
            area.set (std::move (next));
    }

    // The TRACK elements a TRACK coming into the song is (itself first, then a folder's, each before those in it, depth first),
    // each with the folder it goes in: those the keeper's safety net reads (TracksKeeper).
    void collectComing (const juce::ValueTree& track, TrackID folder, std::vector<std::pair<juce::ValueTree, TrackID>>& coming)
    {
        coming.emplace_back (track, folder);
        const auto id = TrackID::fromRaw (readHeldIDOf (track[schema::trackID]));

        if (id.isValid() && track[schema::trackKind].toString() == schema::trackIsFolder) // one of no ID is left out with them
            for (const auto& child : track)
                if (child.hasType (schema::TRACK))
                    collectComing (child, id, coming);
    }
} // namespace

//==============================================================================
SongValues::TracksTeller::TracksTeller (ValueArea<SongTracks>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::TracksTeller::~TracksTeller()
{
    area.removeListener (this);
}

void SongValues::TracksTeller::valueAreaChanged (const SongTracks& before, const SongTracks& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // Copies of what the listeners are told, which stay while they are told whatever one of them changes meanwhile (cheap:
    // they share what they hold).
    const auto was = before;
    const auto now = after;
    std::vector<TrackChange> changes;
    std::vector<TrackID> reordered;

    const auto addChange = [&changes, &was, &now] (const TrackValue* old, const TrackValue* is)
    {
        TrackChange change;
        change.id = is != nullptr ? is->id : old->id;
        change.wasThere = old != nullptr;
        change.isThere = is != nullptr;
        change.parts = old != nullptr && is != nullptr ? findParts (*old, *is) : (std::uint16_t) Change::made;

        if (change.parts == 0)
            return;

        // Its places in its lists, which a search of each finds, for one that came, went or came to another folder alone: a
        // drag's event of its volume searches no list.
        const auto placed = change.has (Change::made) || change.has (Change::place);

        if (old != nullptr)
        {
            change.parentBefore = old->parent;

            if (placed)
                change.indexBefore = findIndex (was, old->parent, old->id);
        }

        if (is != nullptr)
        {
            change.parentAfter = is->parent;

            if (placed)
                change.indexAfter = findIndex (now, is->parent, is->id);
        }

        changes.push_back (change);
    };

    immer::diff (
        was.tracks, now.tracks, [&addChange] (const auto& entry) { addChange (nullptr, &entry.second); }, [&addChange] (const auto& entry)
        { addChange (&entry.second, nullptr); }, [&addChange] (const auto& old, const auto& is) { addChange (&old.second, &is.second); });

    // The master: put in anew whole (made), its ID given anew in place, or a field of it.
    {
        const auto putIn = was.masterPutIn != now.masterPutIn;
        TrackChange change;
        change.id = now.master.id;
        change.onMaster = true;
        change.wasThere = change.isThere = true;
        change.parts = (std::uint16_t) (findParts (was.master, now.master) | (putIn ? Change::made : 0)
                                        | (was.master.id != now.master.id ? Change::newID : 0));

        if (change.parts != 0)
            changes.push_back (change);
    }

    // Each list that holds those it held before in another order.
    immer::diff (
        was.lists, now.lists, [] (const auto&) {}, [] (const auto&) {},
        [&reordered] (const auto& old, const auto& is)
        {
            if (! keepOrder (old.second, is.second))
                reordered.push_back (TrackID::fromRaw (old.first));
        });

    for (auto& change : changes)
        change.now = change.onMaster ? &now.master : change.isThere ? now.tracks.find (change.id.getRaw()) : nullptr;

    if (! changes.empty() || ! reordered.empty())
        listeners.call ([&changes, &reordered, &now] (SongValues::Listener& listener)
                        { listener.tracksChanged (changes, reordered, now); });
}

//==============================================================================
SongValues::TracksKeeper::TracksKeeper (SongValues& v, Song& s)
    : values (v),
      song (s)
{
}

bool SongValues::TracksKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    const auto isTrack = child.hasType (schema::TRACK) && isSongTrackList (parent);
    const auto isPlace = (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER)) && parent == song.getState();

    if (! isTrack && ! isPlace)
        return false;

    // A place with nothing in it (no property, no child) is no track's.
    if (isPlace && child.getNumProperties() == 0 && child.getNumChildren() == 0)
        return true;

    // (a) in a step: refused, where it was put (W62).
    if (! song.isWritingOutsideHistory())
    {
        jassertfalse; // a track's element in the song's tree: the tracks' setters set one (model/arrangement/internal/Tracks.h)
        return true;
    }

    // (a) outside the history: each TRACK (a folder's too) read in with what it holds, or the MASTER.
    std::vector<std::pair<juce::ValueTree, TrackID>> coming;

    if (child.hasType (schema::TRACK))
        collectComing (child, {}, coming);
    else if (child.hasType (schema::TRACKS))
        for (const auto& each : child)
            if (each.hasType (schema::TRACK))
                collectComing (each, {}, coming);

    auto nextTracks = values.tracks.get();
    auto switches = values.trackSwitches.get();
    auto outside = values.tracksOutside.get();
    auto nextClips = values.clips.get();
    auto nextNotes = values.notes.get();
    auto nextDevices = values.devices.get();
    auto store = values.deviceStore.get();
    auto windows = values.devicesOutside.get();
    auto nextModulation = values.modulation.get();
    auto nextSends = values.sends.get();
    std::vector<std::uint64_t> refused;

    // What a channel's element holds, through each area's reader of a whole element: its devices, store, modulators, switches
    // and view each counted as that area's part of the keeper counts what it takes in (SongValues.h: countDevicesAdopted() and
    // those beside it), its clips and sends as the track (countTracksAdopted()).
    const auto takeHoldings = [&] (const juce::ValueTree& element, TrackID id, bool onMaster)
    {
        values.trackLeavesAdopted += holdsSwitchesIn (element) || holdsViewIn (element) ? 1 : 0;
        readLeavesOf (element, switches, outside);

        if (! onMaster)
            takeTrackClipsIn (nextClips, nextNotes, element, id);

        values.devicesAdopted += takeChannelDevicesIn (nextDevices, element, makeChannelPlace (id, onMaster));
        values.deviceStoreAdopted += takeChannelStoreIn (store, windows, element);
        values.modulationAdopted += takeChannelModulatorsIn (nextModulation, element);

        if (! onMaster)
            takeTrackSendsIn (nextSends, element, id);
    };

    for (const auto& [element, folder] : coming)
    {
        auto value = readHeldTrack (element);
        const auto id = value.has_value() ? value->id.getRaw() : 0;

        // One of an ID the song holds, of none, in a folder left out, or that the song can't hold, is left out with the tracks
        // in it.
        if (! value.has_value() || id == 0 || nextTracks.find (value->id) != nullptr
            || (folder.isValid() && std::find (refused.begin(), refused.end(), folder.getRaw()) != refused.end()))
        {
            refused.push_back (id);
            continue;
        }

        value->parent = folder;
        auto next = withTrack (nextTracks, std::move (*value), -1);

        if (next.isSameAs (nextTracks))
        {
            refused.push_back (id);
            continue;
        }

        nextTracks = std::move (next);
        takeHoldings (element, TrackID::fromRaw (id), false);
        ++values.tracksAdopted;
    }

    if (child.hasType (schema::MASTER))
    {
        if (auto value = readHeldTrack (child); value.has_value() && value->isMaster() && value->id.isValid())
        {
            value->parent = {};
            nextTracks.master = std::move (*value);
        }

        takeHoldings (child, nextTracks.master.id, true);
        ++values.tracksAdopted;
    }

    // Set: the tracks first, so that what the other areas hold names a track the song holds.
    values.tracks.setOutsideHistory (std::move (nextTracks));
    values.trackSwitches.setOutsideHistory (std::move (switches));
    values.tracksOutside.set (std::move (outside));
    values.clips.setOutsideHistory (std::move (nextClips));
    values.notes.setOutsideHistory (std::move (nextNotes));
    values.devices.setOutsideHistory (std::move (nextDevices));
    values.deviceStore.setOutsideHistory (std::move (store));
    values.devicesOutside.set (std::move (windows));
    values.modulation.setOutsideHistory (std::move (nextModulation));
    values.sends.setOutsideHistory (std::move (nextSends));

    // Taken out: a TRACK whole; what a place holds, the place staying where the song had none other.
    takingOut = child;

    if (child.hasType (schema::TRACK) || ! (parent.getChildWithName (child.getType()) == child))
    {
        parent.removeChild (child, nullptr);
    }
    else
    {
        child.removeAllChildren (nullptr);
        child.removeAllProperties (nullptr);
    }

    takingOut = {};
    return true;
}

bool SongValues::TracksKeeper::propertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! isSongMaster (tree))
        return false;

    // What the keeper takes away itself, and what an undo or a redo of a step puts back or takes away (a refused one's).
    if (takingOut.isValid() || song.getUndoManager().isPerformingUndoRedo() || ! tree.hasProperty (property))
        return true;

    // (a) in a step: refused, where it was written (W62).
    if (! song.isWritingOutsideHistory())
    {
        jassertfalse; // a property of the song's MASTER: the tracks' setters set the master (model/arrangement/internal/Tracks.h)
        return true;
    }

    // (a) outside the history: read in as the song holds it, into the master's value, its switches or its view, as a MASTER
    // element that holds it with what the song holds of the master reads (one Phazite doesn't know: dropped), and taken away.
    auto master = values.tracks.get().master;
    auto switches = values.trackSwitches.get();
    auto outside = values.tracksOutside.get();
    juce::ValueTree holder (schema::MASTER);
    writeMasterInto (holder, master);
    writeSwitchesInto (holder, switches);
    writeViewInto (holder, outside.master);
    holder.setProperty (property, tree[property], nullptr);

    if (auto value = readHeldTrack (holder); value.has_value() && value->isMaster())
    {
        auto next = values.tracks.get();
        next.master = std::move (*value);
        values.tracks.setOutsideHistory (std::move (next));
    }

    readHeldSwitchesOf (holder, switches);
    values.trackSwitches.setOutsideHistory (std::move (switches));
    values.tracksOutside.set (withMasterView (outside, readHeldViewOf (holder)));
    ++values.tracksAdopted;

    takingOut = tree;
    tree.removeProperty (property, nullptr);
    takingOut = {};
    return true;
}

void SongValues::TracksKeeper::adopt (SongValues&, Song& loaded)
{
    // Every TRACK out of the TRACKS, and every property and child out of the MASTER, their reader having read the song's tracks
    // into the area before (readHeldTracksIn(): SongValues.cpp, adopt()): what the other areas take of them taken before
    // (SongValues.cpp: adopt(), this last), what Phazite doesn't know of them dropped as the song opens.
    auto tree = loaded.getState();

    for (auto child : tree)
    {
        if (child.hasType (schema::TRACKS))
        {
            child.removeAllChildren (nullptr);
            child.removeAllProperties (nullptr);
        }
        else if (child.hasType (schema::MASTER))
        {
            child.removeAllChildren (nullptr);
            child.removeAllProperties (nullptr);
        }
    }
}

//==============================================================================
bool SongValues::setTrack (Song& song, TrackValue value, int index)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    const auto& held = tracks.get();

    if (value.isMaster())
    {
        value.parent = {};

        if (held.master == value)
            return false;
    }
    else if (const auto* was = held.tracks.find (value.id.getRaw()); was != nullptr && *was == value)
    {
        return false;
    }

    auto next = withTrack (held, std::move (value), index);

    if (next.isSameAs (held))
        return false; // refused (TracksValue.h: withTrack())

    setTracksArea (song, tracks, std::move (next));
    return true;
}

bool SongValues::eraseTrack (Song& song, TrackID id)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (tracks.get().tracks.find (id.getRaw()) == nullptr)
        return false;

    setTracksArea (song, tracks, withoutTrack (tracks.get(), id));
    return true;
}

bool SongValues::setTrackListOrder (Song& song, TrackID folder, immer::flex_vector<TrackID> order)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (const auto* list = tracks.get().findList (folder); list == nullptr || *list == order)
        return false;

    auto next = withListOrder (tracks.get(), folder, std::move (order));

    if (next.isSameAs (tracks.get()))
        return false;

    setTracksArea (song, tracks, std::move (next));
    return true;
}

bool SongValues::setAllTracks (Song& song, SongTracks next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    // A pass's and an edit's of several, held together as each was worked out: holdsItsLists() is the check's
    // (tests/SongCheck.cpp), not an assertion here, which would walk every track at every set. The master put in anew whole
    // is a change, though it is alike (SongTracks::masterPutIn).
    const auto& held = tracks.get();

    if (next.isSameAs (held) || (next == held && next.masterPutIn == held.masterPutIn))
        return false;

    setTracksArea (song, tracks, std::move (next));
    return true;
}

void SongValues::setCheckingTrackOrder (bool on) noexcept
{
    checkingTrackOrder = on;
}

bool SongValues::isCheckingTrackOrder() noexcept
{
    return checkingTrackOrder;
}

const TrackOrder& SongValues::getTrackOrder() const
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto& now = tracks.get();

    // While checking is on, the order kept compared with one made afresh: every one in a small song, a share in a big one.
    const auto checked = [&now] (const TrackOrder& kept) -> const TrackOrder&
    {
        constexpr size_t alwaysUpTo = 512, perAsk = 128;
        thread_local std::uint64_t asked = 0;

        if (checkingTrackOrder && (now.tracks.size() <= alwaysUpTo || ++asked % (now.tracks.size() / perAsk) == 0))
        {
            [[maybe_unused]] const auto fresh = makeTrackOrder (now);
            jassert (fresh.order == kept.order && fresh.places == kept.places); // the order kept and the tracks differ
        }

        return kept;
    };

    // The same lists, and the same folders among the tracks (one that came to be a folder, or stopped being one, numbers
    // the others otherwise): the order kept, looked at only where the map isn't the one it was made with.
    if (keptTrackOrder.has_value() && keptTrackOrder->lists.identity() == now.lists.identity())
    {
        if (keptTrackOrder->tracks.identity() == now.tracks.identity())
            return checked (keptTrackOrder->order);

        auto sameFolders = true;

        immer::diff (
            keptTrackOrder->tracks, now.tracks, [&sameFolders] (const auto&) { sameFolders = false; },
            [&sameFolders] (const auto&) { sameFolders = false; }, [&sameFolders] (const auto& was, const auto& is)
            { sameFolders = sameFolders && was.second.isFolder() == is.second.isFolder(); });

        if (sameFolders)
        {
            keptTrackOrder->tracks = now.tracks;
            return checked (keptTrackOrder->order);
        }
    }

    KeptTrackOrder kept;
    kept.tracks = now.tracks;
    kept.lists = now.lists;
    kept.order = makeTrackOrder (now);
    keptTrackOrder = std::move (kept);
    return keptTrackOrder->order;
}

//==============================================================================
SongValues::TrackSwitchesTeller::TrackSwitchesTeller (SongValues& v, juce::ListenerList<SongValues::Listener>& l)
    : values (v),
      area (v.trackSwitches),
      listeners (l)
{
    area.addListener (this);
}

SongValues::TrackSwitchesTeller::~TrackSwitchesTeller()
{
    area.removeListener (this);
}

void SongValues::TrackSwitchesTeller::valueAreaChanged (const TrackSwitches& before, const TrackSwitches& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // The switches after, in a copy of the value told, which stays while the listeners are told (cheap: it shares what it
    // holds); the master's mode named by the master's ID as the tracks' area holds it.
    const auto now = after;
    const auto found = findChanges (before, now);
    const auto master = values.tracks.get().master.id;
    std::vector<TrackSwitchChange> changes;
    addSwitchChanges (changes, found.mutes, Field::mute, master);
    addSwitchChanges (changes, found.solos, Field::solo, master);
    addSwitchChanges (changes, found.frozen, Field::frozen, master);
    addSwitchChanges (changes, found.modes, Field::automationMode, master);

    if (! changes.empty())
        listeners.call ([&changes, &now] (SongValues::Listener& listener) { listener.trackSwitchesChanged (changes, now); });
}

//==============================================================================
SongValues::TracksViewTeller::TracksViewTeller (SongValues& v, juce::ListenerList<SongValues::Listener>& l)
    : values (v),
      area (v.tracksOutside),
      listeners (l)
{
    area.addListener (this);
}

SongValues::TracksViewTeller::~TracksViewTeller()
{
    area.removeListener (this);
}

void SongValues::TracksViewTeller::outsideValueChanged (const TracksOutside& before, const TracksOutside& after)
{
    if (listeners.isEmpty())
        return;

    // Each track whose row changed that the song holds (one it doesn't hold is a row no view shows).
    const auto& held = values.tracks.get();
    std::vector<TrackID> shown;
    const auto addTrack = [&shown, &held] (std::uint64_t id)
    {
        if (held.tracks.find (id) != nullptr)
            shown.push_back (TrackID::fromRaw (id));
    };

    immer::diff (
        before.tracks, after.tracks, [&addTrack] (const auto& entry) { addTrack (entry.first); },
        [&addTrack] (const auto& entry) { addTrack (entry.first); }, [&addTrack] (const auto& was, const auto&) { addTrack (was.first); });

    const auto master = ! (before.master == after.master);

    if (! shown.empty() || master)
        listeners.call ([&shown, master] (SongValues::Listener& listener) { listener.tracksViewChanged (shown, master); });
}

//==============================================================================
SongValues::TrackLeavesKeeper::TrackLeavesKeeper (SongValues& v, Song& s)
    : values (v),
      song (s)
{
}

void SongValues::TrackLeavesKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A lone LANES added to the master, or a lone LANE to its LANES (b): refused in a step; read in outside the history, its
    // lanes after those the master has, and taken out.
    if ((child.hasType (schema::LANES) && isSongMaster (parent)) || (child.hasType (schema::LANE) && isMasterLanes (parent)))
    {
        if (! song.isWritingOutsideHistory())
        {
            jassertfalse; // a lane added to the song's tree: the lanes' functions change the lanes (Automation.h)
            return;
        }

        juce::ValueTree holder (schema::MASTER);
        juce::ValueTree lanes (schema::LANES);

        if (child.hasType (schema::LANES))
            lanes = child.createCopy();
        else
            lanes.appendChild (child.createCopy(), nullptr);

        holder.appendChild (lanes, nullptr);
        auto outside = values.tracksOutside.get();
        auto view = outside.master;
        view.lanes = view.lanes + readHeldLanesOf (holder);
        values.setTracksOutside (withMasterView (outside, std::move (view)));
        ++values.trackLeavesAdopted;
        parent.removeChild (child, nullptr);
    }
}

void SongValues::TrackLeavesKeeper::adopt (SongValues&, Song& loaded)
{
    // Every TRACK of the song's track list (a folder's tracks too) and the MASTER, each read as the song holds it before
    // (readHeldTrackLeavesIn(): SongValues.cpp, adopt()); then all of it taken out of them.
    std::vector<juce::ValueTree> channels;

    for (const auto& child : loaded.getState())
        if (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER))
            forEachChannelIn (child, [&channels] (juce::ValueTree element) { channels.push_back (element); });

    for (auto& element : channels)
    {
        jassert (! keepSwitchesAsHeld (element.createCopy()) && ! keepViewAsHeld (element.createCopy())); // the loader's
        takeSwitchesOutOf (element, nullptr);
        takeViewOutOf (element, nullptr);
    }
}

void readHeldTrackLeavesIn (const juce::ValueTree& song, SongState& into)
{
    // Every TRACK of the song's track list (a folder's tracks too) and the MASTER, each read as the song holds it (the first
    // of an ID, as the loader leaves each once), into the switches and the view.
    auto switches = into.trackSwitches;
    auto outside = into.tracksOutside;

    for (const auto& child : song)
        if (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER))
            forEachChannelIn (child, [&switches, &outside] (juce::ValueTree element) { readLeavesOf (element, switches, outside); });

    into.trackSwitches = std::move (switches);
    into.tracksOutside = std::move (outside);
}

void readHeldTracksIn (const juce::ValueTree& song, SongState& into)
{
    // The song's tracks, each read once as the song holds it (what the loader left of each is what the song holds of it:
    // TracksValue.h, keepTrackAsHeld()).
    auto held = readHeldAll (song);
    jassert (holdsItsLists (held));
    into.tracks = std::move (held);
}

//==============================================================================
bool SongValues::setSwitches (Song& song, TrackSwitches next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next.isSameAs (trackSwitches.get()) || next == trackSwitches.get())
        return false;

    if (song.isWritingOutsideHistory())
        trackSwitches.setOutsideHistory (std::move (next));
    else
        trackSwitches.set (std::move (next));

    return true;
}

bool SongValues::setSwitchesOutsideHistory (Song& song, TrackSwitches next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);
    juce::ignoreUnused (song);

    if (next.isSameAs (trackSwitches.get()) || next == trackSwitches.get())
        return false;

    trackSwitches.setOutsideHistory (std::move (next));
    return true;
}

bool SongValues::setTracksOutside (TracksOutside next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (next.isSameAs (tracksOutside.get()) || next == tracksOutside.get())
        return false;

    tracksOutside.set (std::move (next));
    return true;
}

//==============================================================================
bool leadsToTracks (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER));
}

void writeTrackLeavesIn (juce::ValueTree& element, const SongState& kept)
{
    // Its switches (its ID's entries; the master's mode under 0), then its view: its properties, and its LANES after the
    // children it holds now (its clips, its devices, a folder's tracks), before what the areas after it write (its modulators
    // and macros, its sends).
    if (element.hasType (schema::MASTER))
    {
        writeSwitchesInto (element, kept.trackSwitches);
        writeViewInto (element, kept.tracksOutside.master);
        return;
    }

    if (! element.hasType (schema::TRACK))
        return;

    // Its entries looked up by its ID as the one-field reader reads it (no text made of it): written only where it has one.
    const auto id = readTrackElementID (element).getRaw();
    const auto& switches = kept.trackSwitches;

    if (switches.mutes.find (id) != nullptr || switches.solos.find (id) != nullptr || switches.frozen.find (id) != nullptr
        || switches.modes.find (id) != nullptr)
        writeSwitchesInto (element, switches);

    if (const auto* view = kept.tracksOutside.tracks.find (id))
        writeViewInto (element, *view);
}

bool holdSameTrackSwitchesOf (const juce::ValueTree& element, const SongState& kept, const juce::ValueTree& otherElement,
                              const SongState& otherKept)
{
    if (element.hasType (schema::MASTER))
        return holdSameSwitchesOf (kept.trackSwitches, otherKept.trackSwitches, 0);

    if (! element.hasType (schema::TRACK))
        return true;

    const auto id = readTrackElementID (element).getRaw();
    return id == readTrackElementID (otherElement).getRaw() && holdSameSwitchesOf (kept.trackSwitches, otherKept.trackSwitches, id);
}

//==============================================================================
bool holdSameTracks (const SongState& a, const SongState& b)
{
    if (! a.tracks.has_value() || ! b.tracks.has_value())
        return a.tracks.has_value() == b.tracks.has_value();

    return a.tracks->isSameAs (*b.tracks) || *a.tracks == *b.tracks;
}

bool holdSameTrack (const SongState& a, const SongState& b, TrackID id)
{
    // Its value (its folder among it), and its switches' entries; one in neither the same.
    const auto* first = a.tracks.has_value() ? a.tracks->tracks.find (id.getRaw()) : nullptr;
    const auto* second = b.tracks.has_value() ? b.tracks->tracks.find (id.getRaw()) : nullptr;

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second && holdSameSwitchesOf (a.trackSwitches, b.trackSwitches, id.getRaw());
}

bool holdSameMaster (const SongState& a, const SongState& b)
{
    if (! a.tracks.has_value() || ! b.tracks.has_value())
        return a.tracks.has_value() == b.tracks.has_value();

    return a.tracks->master == b.tracks->master && holdSameSwitchesOf (a.trackSwitches, b.trackSwitches, 0);
}

bool holdSameTrackList (const SongState& a, const SongState& b, TrackID folder)
{
    const auto* first = a.tracks.has_value() ? a.tracks->findList (folder) : nullptr;
    const auto* second = b.tracks.has_value() ? b.tracks->findList (folder) : nullptr;

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
}

bool holdSameTracksOf (const juce::ValueTree& element, const SongState& kept, const juce::ValueTree& otherElement,
                       const SongState& otherKept)
{
    // The song's TRACKS and MASTER are its root's alone (a copy of one, given its tracks' own fields, has none).
    if (! kept.tracks.has_value() || ! otherKept.tracks.has_value() || element.getType() != otherElement.getType())
        return true;

    if (element.hasType (schema::TRACKS))
        return holdSameTracks (kept, otherKept);

    return ! element.hasType (schema::MASTER) || holdSameMaster (kept, otherKept);
}

juce::ValueTree withTracksOwn (const juce::ValueTree& place, const SongState& kept)
{
    // The tracks' own fields alone, as the song's tree held them before the tracks were values: a TRACK for each, nested in its
    // folder's, in each list's order, with its own properties (writeTrackElement()); the master's on the MASTER.
    if (! kept.tracks.has_value() || ! (place.hasType (schema::TRACKS) || place.hasType (schema::MASTER)))
        return place;

    auto copy = place.createCopy();
    const auto& tracks = *kept.tracks;

    if (place.hasType (schema::MASTER))
    {
        writeMasterInto (copy, tracks.master);
        return copy;
    }

    const std::function<void (juce::ValueTree&, TrackID)> addList = [&tracks, &addList] (juce::ValueTree& parent, TrackID owner)
    {
        if (const auto* list = tracks.findList (owner))
        {
            for (const auto id : *list)
            {
                if (const auto* value = tracks.tracks.find (id.getRaw()))
                {
                    auto element = writeTrackElement (*value);
                    addList (element, id);
                    parent.appendChild (element, nullptr);
                }
            }
        }
    };

    addList (copy, {});
    return copy;
}

//==============================================================================
bool isHeardAsTracks (const juce::ValueTree& element)
{
    // Children in another order: the places' (what the keeper takes in there), the master's lanes'.
    return isSongTrackList (element) || isSongMaster (element) || isMasterLanes (element);
}

bool isHeardAsTracks (const juce::ValueTree& element, const juce::Identifier& property)
{
    if (element.hasType (schema::LANE))
        return isLaneProperty (property) && isMasterLanes (element.getParent());

    return isSongMaster (element);
}

bool isHeardAsTracks (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! child.isValid())
        return isHeardAsTracks (parent);

    if (child.hasType (schema::TRACK))
        return isSongTrackList (parent);

    if (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER))
        return isSongRoot (parent);

    if (child.hasType (schema::LANES))
        return isSongMaster (parent);

    return child.hasType (schema::LANE) && isMasterLanes (parent);
}

//==============================================================================
SongTracks readHeldTracks (const juce::ValueTree& song)
{
    return readHeldAll (song);
}

std::uint64_t countTrackElementsWalked() noexcept
{
    return elementsWalked.load (std::memory_order_relaxed);
}

std::uint64_t countTrackListEntriesCompared() noexcept
{
    return listEntriesCompared.load (std::memory_order_relaxed);
}
} // namespace phazite

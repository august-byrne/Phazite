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

    // A song's tracks, read once from its tree (readHeldTracks()), as the song is made of its state.
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
    // before the tracks in it: what the switches and the view are read from.
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

//==============================================================================
bool holdSameTracks (const SongState& a, const SongState& b)
{
    return a.tracks.isSameAs (b.tracks) || a.tracks == b.tracks;
}

bool holdSameTrack (const SongState& a, const SongState& b, TrackID id)
{
    // Its value (its folder among it), and its switches' entries; one in neither the same.
    const auto* first = a.tracks.tracks.find (id.getRaw());
    const auto* second = b.tracks.tracks.find (id.getRaw());

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second && holdSameSwitchesOf (a.trackSwitches, b.trackSwitches, id.getRaw());
}

bool holdSameMaster (const SongState& a, const SongState& b)
{
    return a.tracks.master == b.tracks.master && holdSameSwitchesOf (a.trackSwitches, b.trackSwitches, 0);
}

bool holdSameTrackList (const SongState& a, const SongState& b, TrackID folder)
{
    const auto* first = a.tracks.findList (folder);
    const auto* second = b.tracks.findList (folder);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
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

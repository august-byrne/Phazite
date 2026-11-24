// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/TracksArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
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

    void countWalked()
    {
        elementsWalked.fetch_add (1, std::memory_order_relaxed);
    }

    void countCompared (size_t entries)
    {
        listEntriesCompared.fetch_add ((std::uint64_t) entries, std::memory_order_relaxed);
    }

    //==============================================================================
    // Where an element is: the song's root, its track list, a track of it (in it, or in a folder there), its master, a
    // channel's LANES.
    bool isSongRoot (const juce::ValueTree& element)
    {
        return element.hasType (schema::PHAZITE) && ! element.getParent().isValid();
    }

    bool isSongTrackList (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACKS) && isSongRoot (element.getParent());
    }

    bool isSongTrack (const juce::ValueTree& element)
    {
        if (! element.hasType (schema::TRACK))
            return false;

        auto parent = element.getParent();

        while (parent.hasType (schema::TRACK))
            parent = parent.getParent();

        return isSongTrackList (parent);
    }

    bool isSongMaster (const juce::ValueTree& element)
    {
        return element.hasType (schema::MASTER) && isSongRoot (element.getParent());
    }

    bool isSongChannel (const juce::ValueTree& element)
    {
        return isSongTrack (element) || isSongMaster (element);
    }

    bool isSongLanes (const juce::ValueTree& element)
    {
        return element.hasType (schema::LANES) && isSongChannel (element.getParent());
    }

    //==============================================================================
    // The three values' properties of a TRACK and the MASTER (TracksValue.h, TrackSwitchesValue.h, TracksOutside.h), each
    // with the value it is of: the line between the tracks' listener and the tree's, written once.
    enum class Area
    {
        value,
        switches,
        view
    };

    struct AreaProperty
    {
        const schema::PropertyDescription* description = nullptr;
        Area area = Area::value;
    };

    AreaProperty described (const schema::PropertyDescription& description, Area area)
    {
        AreaProperty property;
        property.description = &description;
        property.area = area;
        return property;
    }

    const std::array<AreaProperty, 25>& getAreaProperties()
    {
        static const std::array<AreaProperty, 25> properties { {
            described (schema::trackID, Area::value),
            described (schema::trackKind, Area::value),
            described (schema::trackName, Area::value),
            described (schema::trackColour, Area::value),
            described (schema::folderBus, Area::value),
            described (schema::channelVolume, Area::value),
            described (schema::channelPan, Area::value),
            described (schema::channelFader, Area::value),
            described (schema::channelVCA, Area::value),
            described (schema::keyRoot, Area::value),
            described (schema::keyScale, Area::value),
            described (schema::trackInput, Area::value),
            described (schema::trackMonitoring, Area::value),
            described (schema::trackPadDevice, Area::value),
            described (schema::trackPad, Area::value),
            described (schema::trackMute, Area::switches),
            described (schema::trackSolo, Area::switches),
            described (schema::trackFrozen, Area::switches),
            described (schema::trackAutomationMode, Area::switches),
            described (schema::trackHeight, Area::view),
            described (schema::trackCollapsed, Area::view),
            described (schema::trackPinned, Area::view),
            described (schema::drumMode, Area::view),
            described (schema::drumOrder, Area::view),
            described (schema::drumHidden, Area::view),
        } };

        return properties;
    }

    // One of them described on the element's type (a MASTER's: its ID, fader, VCA, volume, pan, mode, height, collapsed), or
    // nullptr.
    const AreaProperty* findAreaProperty (const juce::ValueTree& element, const juce::Identifier& property)
    {
        for (const auto& each : getAreaProperties())
            if (each.description->getName() == property)
                return each.description->isOn (element.getType()) ? &each : nullptr;

        return nullptr;
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
    // The tracks of a list (the song's track list, a folder) read as the song holds them, each folder before the tracks in it,
    // each element counted (countTrackElementsWalked()): one whose ID the song can't count on, or that `take` refuses (of an ID
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

    // A view as the keeper keeps it: none for a view with nothing set (a track's that has none comes and goes untold).
    immer::map<std::uint64_t, TrackView> withView (const immer::map<std::uint64_t, TrackView>& views, std::uint64_t id, TrackView view)
    {
        if (view == TrackView())
            return views.find (id) != nullptr ? views.erase (id) : views;

        const auto* held = views.find (id);
        return held != nullptr && *held == view ? views : views.set (id, std::move (view));
    }

    // The song's tracks, their switches and their view, read once from its tree (readHeldTracks()), into what is kept: the
    // switches' and the view's entries of a track the song doesn't hold left as they are. And each track's element, where
    // asked.
    void readHeldAll (const juce::ValueTree& song, SongTracks& tracks, TrackSwitches& switches, TracksOutside& view,
                      std::unordered_map<std::uint64_t, juce::ValueTree>* elements)
    {
        tracks = {};

        readTracksIn (song.getChildWithName (schema::TRACKS),
                      [&] (const juce::ValueTree& element, TrackValue value)
                      {
                          const auto id = value.id.getRaw();

                          if (tracks.tracks.find (id) != nullptr)
                              return false;

                          auto next = withTrack (tracks, std::move (value));

                          if (next.isSameAs (tracks))
                              return false; // refused: its folder isn't one

                          tracks = std::move (next);
                          readHeldSwitchesOf (element, switches);
                          view.tracks = withView (view.tracks, id, readHeldViewOf (element));

                          if (elements != nullptr)
                              (*elements)[id] = element;

                          return true;
                      });

        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
        {
            if (auto value = readHeldTrack (master))
                tracks.master = std::move (*value);

            readHeldSwitchesOf (master, switches);
            view.master = readHeldViewOf (master);
        }
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
    // list's entries looked at once (a list holds each ID once).
    bool keepOrder (const IDList& before, const IDList& after)
    {
        std::unordered_map<std::uint64_t, size_t> places;
        places.reserve (after.size());

        for (const auto id : after)
            places.emplace (id.getRaw(), places.size());

        countCompared (after.size() + before.size());
        std::optional<size_t> last;

        for (const auto id : before)
        {
            if (const auto found = places.find (id.getRaw()); found != places.end())
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
} // namespace

//==============================================================================
SongValues::TracksKeeper::TracksKeeper (SongValues& v, Song& s)
    : values (v),
      song (s)
{
    // What the song holds at the start, each track read once, which no one is told.
    readHeldAll (song.getState(), told, switches, view, &elements);
}

void SongValues::TracksKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! isHeardAsTracks (tree, property))
        return;

    const auto before = told;
    const auto switchesBefore = switches;
    const auto viewBefore = view;

    if (tree.hasType (schema::LANE))
    {
        takeLanes (tree.getParent().getParent());
    }
    else if (const auto* found = findAreaProperty (tree, property))
    {
        switch (found->area)
        {
            case Area::value:
                takeValueProperty (tree, property);
                break;
            case Area::switches:
                takeSwitches (tree);
                break;
            case Area::view:
                takeViewProperty (tree, property);
                break;
        }
    }

    tell (before, switchesBefore, viewBefore);
}

void SongValues::TracksKeeper::childAdded (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! isHeardAsTracks (parent, child))
        return;

    const auto before = told;
    const auto switchesBefore = switches;
    const auto viewBefore = view;

    if (child.hasType (schema::TRACK))
        add (parent, child);
    else if (child.hasType (schema::TRACKS))
        settle(); // the song's track list itself
    else if (child.hasType (schema::MASTER))
        takeMaster();
    else if (child.hasType (schema::LANES))
        takeLanes (parent);
    else if (child.hasType (schema::LANE))
        takeLanes (parent.getParent());

    tell (before, switchesBefore, viewBefore);
}

void SongValues::TracksKeeper::childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! isHeardAsTracks (parent, child))
        return;

    const auto before = told;
    const auto switchesBefore = switches;
    const auto viewBefore = view;

    if (child.hasType (schema::TRACK))
        remove (child);
    else if (child.hasType (schema::TRACKS))
        settle();
    else if (child.hasType (schema::MASTER))
        takeMaster();
    else if (child.hasType (schema::LANES))
        takeLanes (parent);
    else if (child.hasType (schema::LANE))
        takeLanes (parent.getParent());

    tell (before, switchesBefore, viewBefore);
}

void SongValues::TracksKeeper::childOrderChanged (const juce::ValueTree& parent, int oldIndex, int newIndex)
{
    if (! isHeardAsTracks (parent))
        return;

    const auto before = told;
    const auto switchesBefore = switches;
    const auto viewBefore = view;

    if (parent.hasType (schema::LANES))
        takeLanes (parent.getParent());
    else if (parent.getChild (newIndex).hasType (schema::TRACK))
        reorder (parent, oldIndex, newIndex); // not a channel's devices, nor its lanes, moved among its other children

    tell (before, switchesBefore, viewBefore);
}

const TrackValue* SongValues::TracksKeeper::findKept (const juce::ValueTree& track) const
{
    const auto id = readHeldIDOf (track[schema::trackID]);
    const auto kept = elements.find (id);
    return kept != elements.end() && kept->second == track ? told.tracks.find (id) : nullptr;
}

bool SongValues::TracksKeeper::isKept (const juce::ValueTree& track) const
{
    return findKept (track) != nullptr;
}

void SongValues::TracksKeeper::takeValueProperty (const juce::ValueTree& element, const juce::Identifier& property)
{
    // A key's root and scale read together, as readHeldTrack() reads them (a scale alone is no key).
    const auto take = [&element, &property] (TrackValue& value)
    {
        if (property == schema::keyRoot.getName() || property == schema::keyScale.getName())
        {
            setTrackProperty (value, schema::keyRoot.getName(), element[schema::keyRoot]);
            setTrackProperty (value, schema::keyScale.getName(), element[schema::keyScale]);
        }
        else
        {
            setTrackProperty (value, property, element[property]);
        }
    };

    if (element.hasType (schema::MASTER))
    {
        auto master = told.master;
        take (master);
        told = withTrack (told, std::move (master));
        return;
    }

    // Nothing changes a TRACK's ID in place (a version gives the master's alone anew; a merge renumbers a track before it
    // comes in): two tracks that swapped their IDs so would be told as changes in place, and what plays would mark none of
    // their notes (SongPlayer.cpp: tracksChanged() marks a track's notes as it comes or goes alone).
    jassert (property != schema::trackID.getName());

    // Given another ID or another kind in place: the song's tracks found again, as what is in a folder goes with its kind.
    if (property == schema::trackID.getName() || property == schema::trackKind.getName())
        return settle();

    const auto* kept = findKept (element);

    if (kept == nullptr)
        return settle(); // an element the value wasn't read from

    auto value = *kept;
    take (value);
    told = withTrack (told, std::move (value));
}

void SongValues::TracksKeeper::takeSwitches (const juce::ValueTree& element)
{
    if (element.hasType (schema::TRACK) && ! isKept (element))
        return settle();

    // That element's four alone, under its key (the master's mode under 0).
    readHeldSwitchesOf (element, switches);
}

void SongValues::TracksKeeper::takeViewProperty (const juce::ValueTree& element, const juce::Identifier& property)
{
    if (element.hasType (schema::MASTER))
    {
        readHeldViewProperty (element, property, view.master);
        return;
    }

    const auto* kept = findKept (element);

    if (kept == nullptr)
        return settle();

    const auto id = kept->id.getRaw();
    const auto* held = view.tracks.find (id);
    auto next = held != nullptr ? *held : TrackView();
    readHeldViewProperty (element, property, next);
    view.tracks = withView (view.tracks, id, std::move (next));
}

void SongValues::TracksKeeper::takeLanes (const juce::ValueTree& channel)
{
    if (channel.hasType (schema::MASTER))
    {
        view.master.lanes = readHeldLanesOf (channel);
        return;
    }

    const auto* kept = findKept (channel);

    if (kept == nullptr)
        return settle();

    const auto id = kept->id.getRaw();
    const auto* held = view.tracks.find (id);
    auto next = held != nullptr ? *held : TrackView();
    next.lanes = readHeldLanesOf (channel);
    view.tracks = withView (view.tracks, id, std::move (next));
}

std::optional<TrackID> SongValues::TracksKeeper::findListOwner (const juce::ValueTree& list) const
{
    // The song's track list's owner is none (TrackID()); a folder's its ID; none at all for a folder the value doesn't hold
    // as one.
    if (isSongTrackList (list))
        return TrackID();

    const auto* kept = findKept (list);
    return kept != nullptr && kept->isFolder() ? std::optional (kept->id) : std::nullopt;
}

void SongValues::TracksKeeper::add (const juce::ValueTree& list, const juce::ValueTree& track)
{
    const auto found = findListOwner (list);

    if (! found.has_value())
        return settle();

    const auto owner = *found;

    // Its place among its list's tracks the value holds: the track list's (which holds tracks alone) by JUCE's index of the
    // element, where the value holds every other one of them; a folder's (whose devices and lanes are among them) by its
    // tracks before it, walked.
    const auto* held = told.findList (owner);
    const auto heldSize = held != nullptr ? (int) held->size() : 0;
    auto index = 0;

    if (owner == TrackID() && heldSize + 1 == list.getNumChildren())
    {
        index = list.indexOf (track);
    }
    else
    {
        for (const auto& sibling : list)
        {
            if (sibling == track)
                break;

            if (sibling.hasType (schema::TRACK))
            {
                countWalked();
                index += isKept (sibling) ? 1 : 0;
            }
        }
    }

    // It and the tracks in it, read as the song holds them, each at its place: one of an ID the song holds already (another
    // element), or refused (its folder isn't one), and the song's tracks are found again whole.
    auto next = told;
    auto nextSwitches = switches;
    auto nextView = view;
    std::vector<std::pair<std::uint64_t, juce::ValueTree>> taken;
    auto clash = false;
    auto first = true;

    const Take take = [&] (const juce::ValueTree& element, TrackValue value)
    {
        const auto id = value.id.getRaw();

        if (next.tracks.find (id) != nullptr || id == next.master.id.getRaw() || elements.contains (id))
        {
            clash = true;
            return false;
        }

        auto added = withTrack (next, std::move (value), first ? index : -1);
        first = false;

        if (added.isSameAs (next))
        {
            clash = true;
            return false;
        }

        next = std::move (added);
        readHeldSwitchesOf (element, nextSwitches);
        nextView.tracks = withView (nextView.tracks, id, readHeldViewOf (element));
        taken.emplace_back (id, element);
        return true;
    };

    countWalked();

    if (auto value = readHeldTrack (track))
    {
        const auto folder = value->isFolder();

        if (take (track, std::move (*value)) && folder)
            readTracksIn (track, take);
    }

    if (clash)
        return settle();

    told = std::move (next);
    switches = std::move (nextSwitches);
    view = std::move (nextView);

    for (auto& [id, element] : taken)
        elements[id] = element;
}

void SongValues::TracksKeeper::remove (const juce::ValueTree& track)
{
    // One the value was read from, with the tracks in it; their switches and view kept (the top of TracksArea.h). Any other
    // (of an ID another element holds, which the value was read from) changes nothing the song reads.
    const auto* kept = findKept (track);

    if (kept == nullptr)
        return;

    const std::function<void (TrackID)> forget = [this, &forget] (TrackID id)
    {
        elements.erase (id.getRaw());

        if (const auto* list = told.findList (id))
            for (const auto each : *list)
                forget (each);
    };

    const auto id = kept->id;
    forget (id);
    told = withoutTrack (told, id);
}

void SongValues::TracksKeeper::reorder (const juce::ValueTree& list, int oldIndex, int newIndex)
{
    const auto found = findListOwner (list);

    if (! found.has_value())
        return settle();

    const auto owner = *found;

    const auto* held = told.findList (owner);

    if (held == nullptr)
        return settle();

    // The track list's by the two places JUCE names, where the value holds every track in it and the one moved is the one
    // it held at its old place; else the list's tracks walked.
    const auto* moved = findKept (list.getChild (newIndex));

    if (owner == TrackID() && moved != nullptr && (int) held->size() == list.getNumChildren()
        && juce::isPositiveAndBelow (oldIndex, (int) held->size()) && (*held)[(size_t) oldIndex] == moved->id)
    {
        told = withListOrder (told, owner, held->erase ((size_t) oldIndex).insert ((size_t) newIndex, moved->id));
        return;
    }

    IDList order;

    for (const auto& child : list)
    {
        if (! child.hasType (schema::TRACK))
            continue;

        countWalked();

        if (const auto* each = findKept (child))
            order = order.push_back (each->id);
    }

    const auto next = withListOrder (told, owner, order);

    if (next.isSameAs (told) && ! std::equal (held->begin(), held->end(), order.begin(), order.end()))
        return settle(); // not the same tracks

    told = next;
}

void SongValues::TracksKeeper::takeMaster()
{
    // The master's element put in or taken out (a merge's choice of it whole): read again, its switch and its view too (kept
    // as they were while it has none). As a song without a MASTER has it, where it has none.
    masterMade = true;
    const auto master = song.getState().getChildWithName (schema::MASTER);
    auto value = master.isValid() ? readHeldTrack (master) : std::nullopt;
    told = withTrack (told, value.has_value() ? std::move (*value) : TrackValue::makeMaster());

    if (master.isValid())
    {
        readHeldSwitchesOf (master, switches);
        view.master = readHeldViewOf (master);
    }
}

void SongValues::TracksKeeper::settle()
{
    elements.clear();
    readHeldAll (song.getState(), told, switches, view, &elements);
}

void SongValues::TracksKeeper::tell (const SongTracks& before, const TrackSwitches& switchesBefore, const TracksOutside& viewBefore)
{
    // Copies of what the listeners are told, which stay while they are told whatever one of them changes meanwhile (cheap:
    // they share what they hold).
    std::vector<std::uint64_t> came; // the tracks told added: their view is theirs, told with them

    if (! told.isSameAs (before) || masterMade)
    {
        const auto now = told;
        std::vector<TrackChange> changes;
        std::vector<TrackID> reordered;

        const auto addChange = [&changes, &before, &now, &came] (const TrackValue* was, const TrackValue* is)
        {
            TrackChange change;
            change.id = is != nullptr ? is->id : was->id;
            change.wasThere = was != nullptr;
            change.isThere = is != nullptr;
            change.parts = was != nullptr && is != nullptr ? findParts (*was, *is) : (std::uint16_t) Change::made;

            // Its places in its lists, which a search of each finds, for one that came, went or came to another folder alone:
            // a drag's event of its volume searches no list.
            const auto placed = change.has (Change::made) || change.has (Change::place);

            if (was != nullptr)
            {
                change.parentBefore = was->parent;

                if (placed)
                    change.indexBefore = findIndex (before, was->parent, was->id);
            }

            if (is != nullptr)
            {
                change.parentAfter = is->parent;

                if (placed)
                    change.indexAfter = findIndex (now, is->parent, is->id);

                if (was == nullptr)
                    came.push_back (is->id.getRaw());
            }

            changes.push_back (change);
        };

        immer::diff (
            before.tracks, now.tracks, [&addChange] (const auto& entry) { addChange (nullptr, &entry.second); },
            [&addChange] (const auto& entry) { addChange (&entry.second, nullptr); },
            [&addChange] (const auto& was, const auto& is) { addChange (&was.second, &is.second); });

        // The master: its element put in or taken out (made), its ID given anew in place, or a field of it.
        if (masterMade || ! (before.master == now.master))
        {
            TrackChange change;
            change.id = now.master.id;
            change.onMaster = true;
            change.wasThere = change.isThere = true;
            change.parts = (std::uint16_t) (findParts (before.master, now.master) | (masterMade ? Change::made : 0)
                                            | (before.master.id != now.master.id ? Change::newID : 0));

            if (change.parts != 0)
                changes.push_back (change);
        }

        masterMade = false;

        // Each list that holds those it held before in another order.
        immer::diff (
            before.lists, now.lists, [] (const auto&) {}, [] (const auto&) {},
            [&reordered] (const auto& was, const auto& is)
            {
                if (! keepOrder (was.second, is.second))
                    reordered.push_back (TrackID::fromRaw (was.first));
            });

        for (auto& change : changes)
            change.now = change.onMaster ? &now.master : change.isThere ? now.tracks.find (change.id.getRaw()) : nullptr;

        if (! changes.empty() || ! reordered.empty())
            values.listeners.call ([&changes, &reordered, &now] (SongValues::Listener& listener)
                                   { listener.tracksChanged (changes, reordered, now); });
    }

    if (! switches.isSameAs (switchesBefore))
    {
        const auto now = switches;
        const auto found = findChanges (switchesBefore, now);
        std::vector<TrackSwitchChange> changes;
        addSwitchChanges (changes, found.mutes, Field::mute, told.master.id);
        addSwitchChanges (changes, found.solos, Field::solo, told.master.id);
        addSwitchChanges (changes, found.frozen, Field::frozen, told.master.id);
        addSwitchChanges (changes, found.modes, Field::automationMode, told.master.id);

        if (! changes.empty())
            values.listeners.call ([&changes, &now] (SongValues::Listener& listener) { listener.trackSwitchesChanged (changes, now); });
    }

    if (! view.isSameAs (viewBefore))
    {
        // Each track whose view changed, but one told added in this call, whose row is new (its view comes with it).
        std::vector<TrackID> tracks;
        const auto addTrack = [&tracks, &came] (std::uint64_t id)
        {
            if (std::find (came.begin(), came.end(), id) == came.end())
                tracks.push_back (TrackID::fromRaw (id));
        };

        immer::diff (
            viewBefore.tracks, view.tracks, [&addTrack] (const auto& entry) { addTrack (entry.first); }, [&addTrack] (const auto& entry)
            { addTrack (entry.first); }, [&addTrack] (const auto& was, const auto&) { addTrack (was.first); });

        const auto master = ! (viewBefore.master == view.master);

        if (! tracks.empty() || master)
            values.listeners.call ([&tracks, master] (SongValues::Listener& listener) { listener.tracksViewChanged (tracks, master); });
    }
}

//==============================================================================
bool isHeardAsTracks (const juce::ValueTree& element)
{
    // Children in another order: the track list's, a track's or the master's (its tracks, its devices, its lanes: the devices'
    // order is the devices' area's to tell, and nothing else of a channel's order is told), its lanes'.
    return isSongTrackList (element) || isSongChannel (element) || isSongLanes (element);
}

bool isHeardAsTracks (const juce::ValueTree& element, const juce::Identifier& property)
{
    if (element.hasType (schema::LANE))
        return isLaneProperty (property) && isSongLanes (element.getParent());

    return findAreaProperty (element, property) != nullptr && isSongChannel (element);
}

bool isHeardAsTracks (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! child.isValid())
        return isHeardAsTracks (parent);

    if (child.hasType (schema::TRACK))
        return isSongTrackList (parent) || isSongTrack (parent);

    if (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER))
        return isSongRoot (parent);

    if (child.hasType (schema::LANES))
        return isSongChannel (parent);

    return child.hasType (schema::LANE) && isSongLanes (parent);
}

//==============================================================================
SongTracks readHeldTracks (const juce::ValueTree& song)
{
    SongTracks tracks;
    TrackSwitches switches;
    TracksOutside view;
    readHeldAll (song, tracks, switches, view, nullptr);
    return tracks;
}

TrackSwitches readHeldTrackSwitches (const juce::ValueTree& song)
{
    SongTracks tracks;
    TrackSwitches switches;
    TracksOutside view;
    readHeldAll (song, tracks, switches, view, nullptr);
    return switches;
}

TracksOutside readHeldTracksOutside (const juce::ValueTree& song)
{
    SongTracks tracks;
    TrackSwitches switches;
    TracksOutside view;
    readHeldAll (song, tracks, switches, view, nullptr);
    return view;
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Schema.h"

#include <algorithm>
#include <functional>

namespace phazite
{
namespace
{
    // The largest ID in a part of the song: its items' (each kind's is named "id": trackID, clipID, noteID, deviceID,
    // chainID, sendID, modulatorID, sectionID), its channels' faders and VCAs, and its alias groups (clipLink).
    std::uint64_t findLargestID (const juce::ValueTree& tree)
    {
        std::uint64_t largest = 0;

        for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA, &schema::clipLink })
            largest = std::max (largest, (std::uint64_t) (juce::int64) tree.getProperty (*property, (juce::int64) 0));

        for (const auto& child : tree)
            largest = std::max (largest, findLargestID (child));

        return largest;
    }

    std::uint64_t readNextID (const juce::ValueTree& song)
    {
        return (std::uint64_t) (juce::int64) song.getProperty (schema::nextID, (juce::int64) 0);
    }

} // namespace

// Here, with the other counting of IDs and the name of the count (schema::nextID), rather than in Song.cpp.
std::uint64_t Song::makeID()
{
    // From 1: no item's ID is 0.
    const auto id = std::max ((std::uint64_t) 1, readNextID (state));
    state.setProperty (schema::nextID, (juce::int64) (id + 1), nullptr);
    return id;
}

std::uint64_t findNextID (const juce::ValueTree& song)
{
    return std::max (readNextID (song), findLargestID (song) + 1);
}

void countIDs (Song& song)
{
    auto state = song.getState();

    if (const auto next = findNextID (state); next != readNextID (state))
        state.setProperty (schema::nextID, (juce::int64) next, nullptr);
}

CopiedIDs giveNewIDs (Song& song, juce::ValueTree& copy)
{
    CopiedIDs copied;

    // What holds an ID (schema::holdsID()), but a note's, which a copy of its clip keeps, and an alias group's, which the
    // copies join.
    const std::function<void (juce::ValueTree)> renumber = [&] (juce::ValueTree tree)
    {
        if (tree.hasType (schema::NOTE))
            return;

        for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA }) // in the order they are given
        {
            if (const auto* old = tree.getPropertyPointer (*property); old != nullptr && schema::holdsID (tree, *property))
            {
                const auto id = song.makeID();
                copied[readID (*old)] = id;
                tree.setProperty (*property, (juce::int64) id, nullptr);
            }
        }

        for (const auto& child : tree)
            renumber (child);
    };

    // What names one (schema::namesID()): a lane, a modulation and a sidechain name what they show, modulate or listen to,
    // the copy's own if it holds it.
    const std::function<void (juce::ValueTree)> rename = [&] (juce::ValueTree tree)
    {
        for (int i = 0; i < tree.getNumProperties(); ++i)
        {
            const auto property = tree.getPropertyName (i);

            if (schema::namesID (tree, property))
                if (const auto found = copied.find (readID (tree[property])); found != copied.end())
                    tree.setProperty (property, writeIDLike (tree[property], found->second), nullptr);
        }

        for (const auto& child : tree)
            rename (child);
    };

    renumber (copy);
    rename (copy);
    return copied;
}

std::uint64_t readID (const juce::var& value)
{
    return (std::uint64_t) value.toString().getLargeIntValue();
}

juce::var writeIDLike (const juce::var& old, std::uint64_t id)
{
    // A device named by a lane, a curve or a modulation is text ("1042"); the other IDs are numbers.
    return old.isString() ? juce::var (juce::String ((juce::int64) id)) : juce::var ((juce::int64) id);
}

namespace schema
{
    namespace
    {
        // The song's rules as tables, which the functions below read and describeSongRules() writes out.

        // Items: elements with an ID of their own, each kind's named "id" (trackID). The master holds one too.
        const IdentifierList& getItemTypes()
        {
            static const IdentifierList types { &TRACK, &DEVICE, &CONTAINER, &CHAIN, &SEND, &MODULATOR, &MACRO, &CLIP, &NOTE, &SECTION };
            return types;
        }

        bool isItem (const juce::ValueTree& element)
        {
            return isAmong (element.getType(), getItemTypes());
        }

        // The other elements with a key: these properties, joined with "/"; or, for a beat, the beat with six decimals, so
        // that "4" and "4.0" are the same key.
        struct Key
        {
            const juce::Identifier* type;
            IdentifierList properties;
            bool isBeat = false;
        };

        const std::vector<Key>& getKeys()
        {
            static const std::vector<Key> keys {
                { &CURVE, { &curveDevice, &curveParameter } },
                { &LANE, { &laneDevice, &laneParameter } },
                { &MODULATES, { &modulatesDevice, &modulatesParameter } },
                { &VALUE, { &valueParameter } },
                { &TEMPO, { &mapBeat }, true },
                { &TIMESIGNATURE, { &mapBeat }, true },
                { &CHORD, { &chordStart }, true },
                { &CHORDKEY, { &chordStart }, true },
            };
            return keys;
        }

        // Elements with no key: their parent's elements of their type are kept as a whole. Any other element is keyed by its
        // type: the one of its type in its parent.
        const IdentifierList& getUnkeyedTypes()
        {
            static const IdentifierList types { &POINT, &CONTROL, &WARP };
            return types;
        }

        // Elements whose order among their parent's means something, by the name of their order.
        struct Order
        {
            const juce::Identifier* type;
            const char* group;
        };

        const std::vector<Order>& getOrders()
        {
            static const std::vector<Order> orders {
                { &TRACK, "tracks" }, { &DEVICE, "chain" },        { &CONTAINER, "chain" }, { &CHAIN, "chains" },
                { &SEND, "SEND" },    { &MODULATOR, "MODULATOR" }, { &MACRO, "MACRO" },     { &MODULATES, "MODULATES" },
                { &POINT, "POINT" },  { &CONTROL, "CONTROL" },     { &WARP, "WARP" },       { &LANE, "LANE" },
            };
            return orders;
        }

        // A property of the elements of a type.
        struct Property
        {
            const juce::Identifier* type;
            const juce::Identifier* property;
        };

        bool isIn (const std::vector<Property>& table, const juce::ValueTree& element, const juce::Identifier& property)
        {
            return std::any_of (table.begin(), table.end(),
                                [&] (const Property& row) { return element.hasType (*row.type) && property == *row.property; });
        }

        // What holds an ID of the song's besides each item's own and the master's: a channel's fader and VCA, an alias group.
        const std::vector<Property>& getOtherIDHolders()
        {
            static const std::vector<Property> holders {
                { &TRACK, &channelFader }, { &MASTER, &channelFader }, { &TRACK, &channelVCA },
                { &MASTER, &channelVCA },  { &CLIP, &clipLink },
            };
            return holders;
        }

        // What names an ID of the song's: a send's return, a sidechain's track, the device of a lane, a curve or what a
        // modulator moves, and the drum sampler whose pad a track plays.
        const std::vector<Property>& getIDNamers()
        {
            static const std::vector<Property> namers {
                { &SEND, &sendTo },       { &DEVICE, &deviceSidechain },    { &LANE, &laneDevice },
                { &CURVE, &curveDevice }, { &MODULATES, &modulatesDevice }, { &TRACK, &trackPadDevice },
            };
            return namers;
        }

        // How the song plays, rather than the song: kept on PHAZITE, no change to it (design question 18).
        const IdentifierList& getPlayingProperties()
        {
            static const IdentifierList list { &nextID,    &loopFollowsSong, &punch,     &lastInput, &loop,
                                               &loopStart, &loopEnd,         &playStart, &metronome };
            return list;
        }

        // What isn't counted, by the type of the element it is on.
        struct Uncounted
        {
            const juce::Identifier* type;
            std::vector<const IdentifierList*> lists;
        };

        const std::vector<Uncounted>& getUncountedProperties()
        {
            static const std::vector<Uncounted> table {
                { &TRACK, { &getTrackViewProperties() } },
                { &MASTER, { &getTrackViewProperties() } },
                { &DEVICE, { &getDeviceViewProperties() } },
                { &PHAZITE, { &getGridProperties(), &getPlayingProperties() } },
            };
            return table;
        }

        // Elements that aren't counted, with everything in them, by their parent's type.
        struct UncountedElement
        {
            const juce::Identifier* parent;
            const juce::Identifier* type;
        };

        const std::vector<UncountedElement>& getUncountedElements()
        {
            static const std::vector<UncountedElement> table { { &PHAZITE, &PIANOROLL }, { &TRACK, &LANE }, { &MASTER, &LANE } };
            return table;
        }

        // How a version's pieces split a song.
        const IdentifierList& getPieceTypes()
        {
            static const IdentifierList types { &TRACK, &MASTER, &CLIP, &DEVICE, &CONTAINER, &CURVE };
            return types;
        }

        const IdentifierList& getRunTypes()
        {
            static const IdentifierList types { &NOTE, &POINT };
            return types;
        }

        const std::vector<Property>& getBytesProperties()
        {
            static const std::vector<Property> properties { { &DEVICE, &pluginState } };
            return properties;
        }

        juce::String beatKey (const juce::var& beat)
        {
            return juce::String ((double) beat, 6);
        }

        juce::var toNames (const IdentifierList& list)
        {
            juce::Array<juce::var> names;

            for (const auto* name : list)
                names.add (name->toString());

            return names;
        }

        juce::var toRows (const std::vector<Property>& table)
        {
            juce::Array<juce::var> rows;

            for (const auto& row : table)
            {
                auto* object = new juce::DynamicObject();
                object->setProperty ("type", row.type->toString());
                object->setProperty ("property", row.property->toString());
                rows.add (juce::var (object));
            }

            return rows;
        }
    } // namespace

    bool isUncountedProperty (const juce::ValueTree& tree, const juce::Identifier& property)
    {
        for (const auto& row : getUncountedProperties())
            if (tree.hasType (*row.type))
                for (const auto* list : row.lists)
                    if (isAmong (property, *list))
                        return true;

        return false;
    }

    bool isUncountedElement (const juce::ValueTree& parent, const juce::ValueTree& element)
    {
        const auto& table = getUncountedElements();
        return std::any_of (table.begin(), table.end(),
                            [&] (const UncountedElement& row) { return parent.hasType (*row.parent) && element.hasType (*row.type); });
    }

    bool isCounted (const juce::ValueTree& tree, const juce::Identifier& property)
    {
        if (isUncountedProperty (tree, property))
            return false;

        for (auto element = tree; element.isValid(); element = element.getParent())
            if (isUncountedElement (element.getParent(), element))
                return false;

        return true;
    }

    juce::String getElementKey (const juce::ValueTree& element)
    {
        if (isItem (element))
            return element[trackID].toString(); // each kind's ID is named "id"

        for (const auto& key : getKeys())
        {
            if (! element.hasType (*key.type))
                continue;

            if (key.isBeat)
                return beatKey (element[*key.properties.front()]);

            juce::StringArray parts;

            for (const auto* property : key.properties)
                parts.add (element[*property].toString());

            return parts.joinIntoString ("/");
        }

        if (isAmong (element.getType(), getUnkeyedTypes()))
            return {};

        return element.getType().toString(); // one of its kind in its parent: MASTER, TEMPOMAP, TRACKS, VALUES...
    }

    juce::StringArray getKeyProperties (const juce::Identifier& type)
    {
        if (isAmong (type, getItemTypes()))
            return { trackID.toString() };

        for (const auto& key : getKeys())
        {
            if (type != *key.type)
                continue;

            juce::StringArray names;

            for (const auto* property : key.properties)
                names.add (property->toString());

            return names;
        }

        return {};
    }

    juce::String getOrderGroup (const juce::ValueTree& element)
    {
        for (const auto& order : getOrders())
            if (element.hasType (*order.type))
                return order.group;

        return {};
    }

    bool holdsID (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (property == trackID) // each kind's ID is named "id"
            return isItem (element) || element.hasType (MASTER);

        return isIn (getOtherIDHolders(), element, property);
    }

    bool namesID (const juce::ValueTree& element, const juce::Identifier& property)
    {
        return isIn (getIDNamers(), element, property);
    }

    bool isPieceOfItsOwn (const juce::ValueTree& element)
    {
        return isAmong (element.getType(), getPieceTypes());
    }

    bool goesInRuns (const juce::ValueTree& element)
    {
        return isAmong (element.getType(), getRunTypes());
    }

    bool isBytesProperty (const juce::ValueTree& element, const juce::Identifier& property)
    {
        return isIn (getBytesProperties(), element, property);
    }

    juce::var describeSongRules()
    {
        auto* rules = new juce::DynamicObject();

        // Items, and every other element's key.
        auto* items = new juce::DynamicObject();
        items->setProperty ("types", toNames (getItemTypes()));
        items->setProperty ("idProperty", trackID.toString());
        items->setProperty ("alsoHoldingAnID", MASTER.toString());
        rules->setProperty ("items", juce::var (items));

        juce::Array<juce::var> keys;

        for (const auto& key : getKeys())
        {
            auto* row = new juce::DynamicObject();
            row->setProperty ("type", key.type->toString());
            row->setProperty ("properties", toNames (key.properties));
            row->setProperty ("join", "/");

            if (key.isBeat)
                row->setProperty ("beat", "the number with 6 decimals");

            keys.add (juce::var (row));
        }

        rules->setProperty ("keys", keys);
        rules->setProperty ("unkeyed", toNames (getUnkeyedTypes()));
        rules->setProperty ("otherElementsKeyedBy", "their type");

        auto* orders = new juce::DynamicObject();

        for (const auto& order : getOrders())
            orders->setProperty (*order.type, juce::String (order.group));

        rules->setProperty ("orders", juce::var (orders));

        // IDs, and what names them.
        rules->setProperty ("otherIDHolders", toRows (getOtherIDHolders()));
        rules->setProperty ("idNamers", toRows (getIDNamers()));

        // What isn't counted as a change.
        auto* uncounted = new juce::DynamicObject();

        for (const auto& row : getUncountedProperties())
        {
            IdentifierList all;

            for (const auto* list : row.lists)
                all.insert (all.end(), list->begin(), list->end());

            uncounted->setProperty (*row.type, toNames (all));
        }

        rules->setProperty ("uncountedProperties", juce::var (uncounted));

        juce::Array<juce::var> uncountedElements;

        for (const auto& row : getUncountedElements())
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("parent", row.parent->toString());
            object->setProperty ("type", row.type->toString());
            uncountedElements.add (juce::var (object));
        }

        rules->setProperty ("uncountedElements", uncountedElements);

        // How a version's pieces split a song, and what is bytes.
        rules->setProperty ("piecesOfTheirOwn", toNames (getPieceTypes()));
        rules->setProperty ("inRuns", toNames (getRunTypes()));
        rules->setProperty ("bytes", toRows (getBytesProperties()));

        return juce::var (rules);
    }
} // namespace schema
} // namespace phazite

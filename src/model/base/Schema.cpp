// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Schema.h"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_set>

namespace phazite
{
namespace
{
    std::uint64_t findLargestIn (const juce::ValueTree& tree, std::uint64_t below, bool inUnknown)
    {
        std::uint64_t largest = 0;

        // Where the element holds one (a note's own ID too: a note is an item), and one the song can count on from: a file
        // from others may have "id" on an element that holds none, or "link" on a note, with any number in it. Inside an
        // element of a type Phazite doesn't know, only below maxCountedID: the loader gives nothing in there anew.
        const auto bound = inUnknown ? std::min (below, schema::maxCountedID) : below;

        for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA, &schema::clipLink })
            if (const auto* value = tree.getPropertyPointer (*property); value != nullptr && schema::holdsID (tree, *property))
                if (const auto id = (std::uint64_t) (juce::int64) *value; id < bound)
                    largest = std::max (largest, id);

        for (const auto& child : tree)
            largest = std::max (largest, findLargestIn (child, below, inUnknown || ! schema::isKnownType (child.getType())));

        return largest;
    }
} // namespace

std::uint64_t findLargestID (const juce::ValueTree& tree, std::uint64_t below)
{
    return findLargestIn (tree, below, ! schema::isKnownType (tree.getType()));
}

bool mustGiveIDsAnew (std::uint64_t largestID, std::uint64_t nextID)
{
    return largestID >= schema::renumberMark || nextID >= schema::renumberMark;
}

bool mustGiveIDsAnew (const juce::ValueTree& song)
{
    return mustGiveIDsAnew (findLargestID (song), readID (song[schema::nextID]));
}

bool areNumberedAlike (const juce::ValueTree& song, const juce::ValueTree& other)
{
    return song[schema::idNumbering].toString() == other[schema::idNumbering].toString();
}

namespace
{
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
    jassert (id < schema::maxID); // a loaded song counts from at most schema::maxCountedID, 2^61 below it
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
    //==============================================================================
    // The description as lookups.
    namespace
    {
        // A name by its pooled text (juce::Identifier keeps one of each): what a song's elements and properties are
        // named by, compared without looking a text up.
        const void* keyOf (const juce::Identifier& name) noexcept
        {
            return name.getCharPointer().getAddress();
        }

        bool holdsAnID (Form form) noexcept
        {
            return form == Form::ownID || form == Form::songID;
        }

        bool isAmong (const juce::Identifier& name, const std::vector<const PropertyDescription*>& list) noexcept
        {
            for (const auto* property : list)
                if (property->getName() == name)
                    return true;

            return false;
        }
    } // namespace

    Tables::Tables (std::vector<const Element*> elementsToUse, std::vector<const PropertyDescription*> propertiesToUse)
        : elements (std::move (elementsToUse)),
          properties (std::move (propertiesToUse))
    {
        for (const auto* element : elements)
            types[keyOf (*element)].element = element;

        for (const auto* property : properties)
        {
            const auto form = property->getForm();

            if ((holdsAnID (form) || form == Form::idName)
                && std::find (idNames.begin(), idNames.end(), keyOf (*property)) == idNames.end())
                idNames.push_back (keyOf (*property));

            for (const auto* element : property->getElements())
            {
                auto& type = types[keyOf (*element)];
                type.all.push_back (property);

                if (property->isFamily())
                    for (const auto& name : property->getFamilyNames())
                        type.byName[keyOf (name)] = property;
                else
                    type.byName[keyOf (*property)] = property;

                if (property->getRange().has_value())
                    (form == Form::numbers ? type.lists : type.ranged).push_back (property);

                if (form == Form::words)
                    type.worded.push_back (property);

                if (form == Form::hexID)
                    type.hexIDs.push_back (property);

                if (! property->isCountedOn (*element))
                    type.uncounted.push_back (property);

                if (holdsAnID (form))
                    type.holdingIDs.push_back (property);

                if (form == Form::idName)
                    type.namingIDs.push_back (property);

                if (property->getType() == ValueType::bytes)
                    type.bytes.push_back (property);
            }
        }
    }

    const Tables::OnType& Tables::on (const juce::Identifier& type) const noexcept
    {
        const auto found = types.find (keyOf (type));
        return found != types.end() ? found->second : none;
    }

    const PropertyDescription* Tables::findProperty (const juce::Identifier& type, const juce::Identifier& name) const noexcept
    {
        const auto& described = on (type).byName;
        const auto found = described.find (keyOf (name));
        return found != described.end() ? found->second : nullptr;
    }

    bool Tables::mayHoldID (const juce::Identifier& name) const noexcept
    {
        return std::find (idNames.begin(), idNames.end(), keyOf (name)) != idNames.end();
    }

    const Tables& getTables()
    {
        static const Tables tables { getElements(), getProperties() };
        return tables;
    }

    IdentifierList findProperties (const juce::Identifier& type, Kind kind)
    {
        IdentifierList found;

        for (const auto* property : getTables().on (type).all)
            if (property->getKind() == kind)
                found.push_back (&property->getName());

        return found;
    }

    //==============================================================================
    namespace
    {
        // The versions' rules as tables, which the functions below read and describeSongRules() writes out, with what the
        // description says of IDs, bytes and what isn't counted.
        using NameList = std::vector<const Name*>;

        bool isAmong (const juce::Identifier& name, const NameList& list) noexcept
        {
            for (const auto* each : list)
                if (each->getName() == name)
                    return true;

            return false;
        }

        // Items: elements with an ID of their own, each kind's named "id" (trackID). The master holds one too.
        const NameList& getItemTypes()
        {
            static const NameList types { &TRACK, &DEVICE, &CONTAINER, &CHAIN, &SEND, &MODULATOR, &MACRO, &CLIP, &NOTE, &SECTION };
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
            const Name* type;
            NameList properties;
            bool isBeat = false;
        };

        const std::vector<Key>& getKeys()
        {
            static const std::vector<Key> keys {
                { &CURVE, { &curveDevice, &curveParameter } },
                { &LANE, { &laneDevice, &laneParameter } },
                { &MODULATES, { &modulatesDevice, &modulatesParameter } },
                { &MIDILINK, { &midiLinkChannel, &midiLinkNumber } },
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
        const NameList& getUnkeyedTypes()
        {
            static const NameList types { &POINT, &CONTROL, &WARP };
            return types;
        }

        // Elements whose order among their parent's means something, by the name of their order.
        struct Order
        {
            const Name* type;
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

        // How a version's pieces split a song.
        const NameList& getPieceTypes()
        {
            static const NameList types { &TRACK, &MASTER, &CLIP, &DEVICE, &CONTAINER, &CURVE };
            return types;
        }

        const NameList& getRunTypes()
        {
            static const NameList types { &NOTE, &POINT };
            return types;
        }

        // The order song-rules.json has listed the rows the description gives in since before there was one
        // (tests/golden/format/song-rules.json, which another tool reads and which stays as it is): the elements of the
        // uncounted properties, and the properties it lists in another order than the description's. Any other row
        // follows them, in the description's order.
        const NameList& getRulesTypeOrder()
        {
            static const NameList types { &TRACK, &MASTER, &DEVICE, &PHAZITE };
            return types;
        }

        const NameList& getRulesPropertyOrder()
        {
            static const NameList properties {
                &gridSnap,        &gridAdaptive, &gridOffset,      &gridStep,       &gridTriplets,   &nextID,
                &loopFollowsSong, &punch,        &lastInput,       &loop,           &loopStart,      &loopEnd,
                &playStart,       &metronome,    &versionsBranch,  &versionsBase,   &sendTo,         &deviceSidechain,
                &laneDevice,      &curveDevice,  &modulatesDevice, &trackPadDevice, &midiLinkDevice,
            };
            return properties;
        }

        // Where in an order list a name is, by its address (two properties may share a saved name); past its end for one it
        // doesn't list.
        size_t findRank (const NameList& order, const Name* name)
        {
            return (size_t) std::distance (order.begin(), std::find (order.begin(), order.end(), name));
        }

        // A property on an element, as a row of song-rules.json.
        struct Row
        {
            const Element* type;
            const PropertyDescription* property;
        };

        // The rows of the properties of a form, or of bytes, in the order the rules list them.
        std::vector<Row> findRows (const std::function<bool (const PropertyDescription&)>& isOne)
        {
            std::vector<Row> rows;

            for (const auto* property : getProperties())
                if (isOne (*property))
                    for (const auto* element : property->getElements())
                        rows.push_back ({ element, property });

            std::stable_sort (rows.begin(), rows.end(),
                              [] (const Row& a, const Row& b)
                              {
                                  const auto& order = getRulesPropertyOrder();
                                  return findRank (order, a.property) < findRank (order, b.property);
                              });
            return rows;
        }

        juce::String beatKey (const juce::var& beat)
        {
            return juce::String ((double) beat, 6);
        }

        juce::var toNames (const NameList& list)
        {
            juce::Array<juce::var> names;

            for (const auto* name : list)
                names.add (name->toString());

            return names;
        }

        juce::var toRows (const std::vector<Row>& rows)
        {
            juce::Array<juce::var> written;

            for (const auto& row : rows)
            {
                auto* object = new juce::DynamicObject();
                object->setProperty ("type", row.type->toString());
                object->setProperty ("property", row.property->toString());
                written.add (juce::var (object));
            }

            return written;
        }
    } // namespace

    const std::map<juce::String, juce::StringArray>& getPlaces()
    {
        static const auto places = []
        {
            std::map<juce::String, juce::StringArray> made;

            for (const auto* element : getElements())
                for (const auto* parent : element->getParents())
                    made[parent->toString()].add (element->toString());

            return made;
        }();

        return places;
    }

    bool isKnownType (const juce::Identifier& type)
    {
        return getTables().findElement (type) != nullptr;
    }

    bool isUncountedProperty (const juce::ValueTree& tree, const juce::Identifier& property)
    {
        return isAmong (property, getTables().on (tree.getType()).uncounted);
    }

    bool isUncountedElement (const juce::ValueTree& parent, const juce::ValueTree& element)
    {
        const auto* described = getTables().findElement (element.getType());
        return described != nullptr && ! described->isCounted() && described->goesIn (parent.getType());
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
        return isAmong (property, getTables().on (element.getType()).holdingIDs);
    }

    bool namesID (const juce::ValueTree& element, const juce::Identifier& property)
    {
        return isAmong (property, getTables().on (element.getType()).namingIDs);
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
        return isAmong (property, getTables().on (element.getType()).bytes);
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

        // IDs, and what names them: besides each item's own ID, what holds one; and what names one.
        rules->setProperty ("otherIDHolders",
                            toRows (findRows ([] (const PropertyDescription& p) { return p.getForm() == Form::songID; })));
        rules->setProperty ("idNamers", toRows (findRows ([] (const PropertyDescription& p) { return p.getForm() == Form::idName; })));

        // What isn't counted as a change: properties by their elements' types, and elements by their parents'.
        std::vector<const Element*> types;

        for (const auto* element : getElements())
            if (! getTables().on (*element).uncounted.empty())
                types.push_back (element);

        std::stable_sort (types.begin(), types.end(),
                          [] (const Element* a, const Element* b)
                          {
                              const auto& order = getRulesTypeOrder();
                              return findRank (order, a) < findRank (order, b);
                          });

        auto* uncountedProperties = new juce::DynamicObject();

        for (const auto* type : types)
        {
            auto properties = getTables().on (*type).uncounted;
            std::stable_sort (properties.begin(), properties.end(),
                              [] (const PropertyDescription* a, const PropertyDescription* b)
                              {
                                  const auto& order = getRulesPropertyOrder();
                                  return findRank (order, a) < findRank (order, b);
                              });

            juce::Array<juce::var> names;

            for (const auto* property : properties)
                names.add (property->toString());

            uncountedProperties->setProperty (*type, names);
        }

        rules->setProperty ("uncountedProperties", juce::var (uncountedProperties));

        juce::Array<juce::var> uncountedElements;

        for (const auto* element : getElements())
        {
            if (element->isCounted())
                continue;

            for (const auto* parent : element->getParents())
            {
                auto* object = new juce::DynamicObject();
                object->setProperty ("parent", parent->toString());
                object->setProperty ("type", element->toString());
                uncountedElements.add (juce::var (object));
            }
        }

        rules->setProperty ("uncountedElements", uncountedElements);

        // How a version's pieces split a song, and what is bytes.
        rules->setProperty ("piecesOfTheirOwn", toNames (getPieceTypes()));
        rules->setProperty ("inRuns", toNames (getRunTypes()));
        rules->setProperty ("bytes", toRows (findRows ([] (const PropertyDescription& p) { return p.getType() == ValueType::bytes; })));

        return juce::var (rules);
    }

    //==============================================================================
    namespace
    {
        const char* getName (Kind kind)
        {
            switch (kind)
            {
                case Kind::song:
                    return "song";
                case Kind::view:
                    return "view";
                case Kind::state:
                    return "state";
            }

            return "song";
        }

        const char* getName (ValueType type)
        {
            switch (type)
            {
                case ValueType::boolean:
                    return "bool";
                case ValueType::int32:
                    return "int";
                case ValueType::int64:
                    return "int64";
                case ValueType::float32:
                    return "float";
                case ValueType::float64:
                    return "double";
                case ValueType::text:
                    return "text";
                case ValueType::bytes:
                    return "bytes";
            }

            return "text";
        }

        const char* getName (Form form)
        {
            switch (form)
            {
                case Form::plain:
                    return "plain";
                case Form::ownID:
                    return "ownID";
                case Form::songID:
                    return "songID";
                case Form::idName:
                    return "idName";
                case Form::words:
                    return "words";
                case Form::hexID:
                    return "hexID";
                case Form::colour:
                    return "colour";
                case Form::numbers:
                    return "numbers";
            }

            return "plain";
        }

        juce::var toNames (const std::vector<const Element*>& list)
        {
            juce::Array<juce::var> names;

            for (const auto* element : list)
                names.add (element->toString());

            return names;
        }
    } // namespace

    juce::var describeSchema()
    {
        auto elements = getElements();
        std::sort (elements.begin(), elements.end(), [] (const Element* a, const Element* b) { return a->toString() < b->toString(); });

        juce::Array<juce::var> writtenElements;

        for (const auto* element : elements)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("name", element->toString());
            object->setProperty ("in", toNames (element->getParents()));
            object->setProperty ("kind", getName (element->getKind()));
            object->setProperty ("counted", element->isCounted());

            if (element->isNamedByParameters())
                object->setProperty ("propertiesNamedBy", "its device's or modulator's parameters");

            writtenElements.add (juce::var (object));
        }

        auto properties = getProperties();
        const auto sortKey = [] (const PropertyDescription* property)
        {
            juce::StringArray elementNames;

            for (const auto* element : property->getElements())
                elementNames.add (element->toString());

            return property->toString() + " " + elementNames.joinIntoString (" ");
        };

        std::sort (properties.begin(), properties.end(),
                   [&sortKey] (const PropertyDescription* a, const PropertyDescription* b) { return sortKey (a) < sortKey (b); });

        juce::Array<juce::var> writtenProperties;

        for (const auto* property : properties)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("name", property->toString());

            if (property->isFamily())
                object->setProperty ("names", (int) property->getFamilyNames().size());

            object->setProperty ("on", toNames (property->getElements()));
            object->setProperty ("type", getName (property->getType()));

            if (property->getForm() != Form::plain)
                object->setProperty ("form", getName (property->getForm()));

            if (const auto& range = property->getRange())
            {
                auto* written = new juce::DynamicObject();
                written->setProperty ("low", range->low);
                written->setProperty ("high", range->high);
                written->setProperty ("whole", range->whole);

                if (range->fallback.has_value())
                    written->setProperty ("noNumberBecomes", *range->fallback);

                if (range->outsideGoes)
                    written->setProperty ("outsideGoes", true);

                if (range->powersOfTwo)
                    written->setProperty ("powersOfTwo", true);

                object->setProperty ("range", juce::var (written));
            }

            if (! property->getWords().empty())
            {
                juce::Array<juce::var> words;

                for (const auto* word : property->getWords())
                    words.add (juce::String (word));

                object->setProperty ("words", words);
            }

            if (property->getForm() == Form::hexID)
                object->setProperty ("digits", property->getHexDigits());

            if (property->isAlways())
                object->setProperty ("always", true);

            if (! property->getDefault().isVoid())
                object->setProperty ("default", property->getDefault());

            object->setProperty ("kind", getName (property->getKind()));

            if (property->getUncountedElements().empty())
                object->setProperty ("counted", true);
            else
                object->setProperty ("uncountedOn", toNames (property->getUncountedElements()));

            writtenProperties.add (juce::var (object));
        }

        auto* described = new juce::DynamicObject();
        described->setProperty ("elements", writtenElements);
        described->setProperty ("properties", writtenProperties);
        return juce::var (described);
    }
} // namespace schema
} // namespace phazite

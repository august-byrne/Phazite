// SPDX-License-Identifier: AGPL-3.0-or-later
// What describes each element and property a song saves (Schema.h): its name; where it goes, or what holds it; what it
// holds, as a C++ type and in what form; its range or its words; its default, where Schema.h's comment states one; its
// kind; and whether it counts as a change. Each description registers itself as it is made (getElements(),
// getProperties()), so that no saved name exists without one, and what reads a song (the loader, the consistency check,
// the versions' rules) reads them (getTables()) rather than tables of its own.
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace phazite::schema
{
// As Schema.h's header comment defines them: song = part of the song, changed in undo steps; view = how the project is
// shown, saved but never an undo step; state = remembered by the project, not an undo step.
enum class Kind
{
    song,
    view,
    state
};

// The C++ type a property's value is: bool, int, std::int64_t, float, double, juce::String, juce::MemoryBlock (bytes,
// written in base 64: SongText.h, writeBytes()).
enum class ValueType
{
    boolean,
    int32,
    int64,
    float32,
    float64,
    text,
    bytes
};

// What a value is, beyond its type: plain (what its type says); ownID, an item's own ID, which the song gives it
// (Song::makeID()); songID, another ID of the song's an element holds (a channel's fader, an alias group); idName, the
// ID of another item, which it names (a send's return, a lane's device); words, one of a list of words; hexID, lowercase
// hexadecimal digits, so many of them; colour, as juce::Colour::toString() writes it; numbers, numbers separated by
// spaces.
enum class Form
{
    plain,
    ownID,
    songID,
    idName,
    words,
    hexID,
    colour,
    numbers
};

class Element;
class PropertyDescription;

//==============================================================================
// The facts a description is made of, each written as a word of its own in Schema.h.

// A number's range, from low to high, whole or not; what a value that is no finite number becomes (fallback; none: it
// goes); whether a number outside goes rather than being brought in (a MIDI link's controller, which brought in would be
// another controller); and whether only powers of two within it are kept (a time signature's note value). For a list of
// numbers (Form::numbers), each number's.
struct Range
{
    double low = 0.0, high = 0.0;
    bool whole = false;
    std::optional<double> fallback {};
    bool outsideGoes = false;
    bool powersOfTwo = false;

    constexpr Range orElse (double value) const
    {
        auto changed = *this;
        changed.fallback = value;
        return changed;
    }

    constexpr Range goingOutside() const
    {
        auto changed = *this;
        changed.outsideGoes = true;
        return changed;
    }

    constexpr Range ofPowersOfTwo() const
    {
        auto changed = *this;
        changed.powersOfTwo = true;
        return changed;
    }
};

constexpr Range within (double low, double high)
{
    return { low, high };
}

constexpr Range whole (double low, double high)
{
    return { low, high, true };
}

// Text that is one of these words; anything else goes, as if it were none.
struct Words
{
    std::vector<const char*> list;
};

template <typename... Word>
Words oneOf (Word... words)
{
    return { { words... } };
}

// Every element it is on holds it (a track's kind).
struct Always
{
};

inline constexpr Always always {};

// Text of so many lowercase hexadecimal digits; text of any other form names nothing, and goes.
struct HexDigits
{
    int digits;
};

constexpr HexDigits hexDigits (int digits)
{
    return { digits };
}

// The default Schema.h's comment states ("default 64").
template <typename Value>
struct Default
{
    Value value;
};

template <typename Value>
constexpr Default<Value> byDefault (Value value)
{
    return { value };
}

// The elements a property is on; the elements an element goes in. By their addresses only: an element may be defined
// after what names it, and nothing reads through them while the descriptions are made.
struct Elements
{
    std::vector<const Element*> list;
};

template <typename... Each>
    requires (std::is_same_v<Each, Element> && ...)
Elements on (const Each&... elements)
{
    return { { &elements... } };
}

struct Parents
{
    std::vector<const Element*> list;
};

template <typename... Each>
    requires (std::is_same_v<Each, Element> && ...)
Parents under (const Each&... elements)
{
    return { { &elements... } };
}

// No change to the project (Schema.h: isUncountedProperty(), isUncountedElement()): an element with everything in it, or
// a property on every element it is on (uncounted), or on these alone (uncountedOn()).
struct Uncounted
{
    std::vector<const Element*> list;
};

inline const Uncounted uncounted {};

template <typename... Each>
    requires (std::is_same_v<Each, Element> && ...)
Uncounted uncountedOn (const Each&... elements)
{
    return { { &elements... } };
}

// An element whose properties are named by the parameters of its device's or modulator's description (VALUES), not
// here.
struct ParameterNames
{
};

inline constexpr ParameterNames namedByParameters {};

//==============================================================================
namespace detail
{
    inline std::vector<const Element*>& getElementList()
    {
        static std::vector<const Element*> list;
        return list;
    }

    inline std::vector<const PropertyDescription*>& getPropertyList()
    {
        static std::vector<const PropertyDescription*> list;
        return list;
    }

    template <typename Fact, typename... Facts>
    constexpr int count()
    {
        return (0 + ... + (std::is_same_v<Fact, Facts> ? 1 : 0));
    }
} // namespace detail

// Every element and property described, in the order they were made (Schema.h's): what the descriptions registered.
inline const std::vector<const Element*>& getElements()
{
    return detail::getElementList();
}

inline const std::vector<const PropertyDescription*>& getProperties()
{
    return detail::getPropertyList();
}

//==============================================================================
// A described name: what an element type and a property are wherever a juce::Identifier is taken (a ValueTree's type, a
// property's name), and what callers use of one.
class Name
{
public:
    operator const juce::Identifier&() const noexcept { return name; }
    const juce::Identifier& getName() const noexcept { return name; }
    juce::String toString() const { return name.toString(); }
    juce::String::CharPointerType getCharPointer() const noexcept { return name.getCharPointer(); }
    operator juce::StringRef() const noexcept { return name; }

protected:
    explicit Name (const char* text)
        : name (text)
    {
    }
    Name (const Name&) = default;
    Name& operator= (const Name&) = default;
    ~Name() = default;

private:
    juce::Identifier name;
};

inline bool operator== (const Name& a, const Name& b) noexcept
{
    return a.getName() == b.getName();
}

inline bool operator== (const Name& a, const juce::Identifier& b) noexcept
{
    return a.getName() == b;
}

//==============================================================================
// An element type of the song: its name (which it is, wherever a juce::Identifier is taken), the elements it goes in
// (none for the song's own), its kind, and whether it counts as a change, with everything in it, in each of them.
class Element final : public Name
{
public:
    template <typename... Facts>
    explicit Element (const char* text, const Facts&... facts)
        : Name (text)
    {
        static_assert (detail::count<Kind, Facts...>() == 1, "An element states its kind");
        (apply (facts), ...);
        detail::getElementList().push_back (this);
    }

    Element (const Element&) = delete;
    Element& operator= (const Element&) = delete;

    const std::vector<const Element*>& getParents() const noexcept { return parents; }
    Kind getKind() const noexcept { return kind; }
    bool isCounted() const noexcept { return counted; }
    bool isNamedByParameters() const noexcept { return byParameters; }

    // Whether it goes in an element of that type.
    bool goesIn (const juce::Identifier& parentType) const noexcept
    {
        for (const auto* parent : parents)
            if (parent->getName() == parentType)
                return true;

        return false;
    }

private:
    std::vector<const Element*> parents;
    Kind kind = Kind::song;
    bool counted = true, byParameters = false;

    void apply (const Parents& made) { parents = made.list; }
    void apply (Kind made) { kind = made; }
    void apply (const ParameterNames&) { byParameters = true; }

    void apply ([[maybe_unused]] const Uncounted& made)
    {
        jassert (made.list.empty()); // an element is uncounted as a whole, wherever it goes
        counted = false;
    }
};

//==============================================================================
// A saved property: its name (which it is, wherever a juce::Identifier is taken), the elements it is on, and what it holds,
// whatever its C++ type: what tables of properties hold. For a family of names made in a loop (a drum pad's sounds), the
// name is their pattern ("pad<note>Sound<layer>") and getFamilyNames() the names.
class PropertyDescription : public Name
{
public:
    const std::vector<const Element*>& getElements() const noexcept { return elements; }

    bool isOn (const juce::Identifier& elementType) const noexcept { return isAmong (elementType, elements); }

    ValueType getType() const noexcept { return type; }
    Form getForm() const noexcept { return form; }
    const std::optional<Range>& getRange() const noexcept { return range; }
    const std::vector<const char*>& getWords() const noexcept { return words; }
    int getHexDigits() const noexcept { return digits; }
    bool isAlways() const noexcept { return alwaysThere; }

    // The default Schema.h's comment states; void for none.
    const juce::var& getDefault() const noexcept { return defaultValue; }

    Kind getKind() const noexcept { return kind; }

    // Whether it counts as a change on an element of that type, and the elements it doesn't count on.
    bool isCountedOn (const juce::Identifier& elementType) const noexcept { return ! isAmong (elementType, uncountedElements); }
    const std::vector<const Element*>& getUncountedElements() const noexcept { return uncountedElements; }

    bool isFamily() const noexcept { return ! familyNames.empty(); }
    const std::vector<juce::Identifier>& getFamilyNames() const noexcept { return familyNames; }

    // A copy registers nothing: a test's, whose range it changes to see what reads the description follow
    // (Tables).
    PropertyDescription (const PropertyDescription&) = default;
    void setRange (std::optional<Range> changed) { range = changed; }

protected:
    PropertyDescription (const char* text, ValueType valueType)
        : Name (text),
          type (valueType)
    {
    }
    PropertyDescription& operator= (const PropertyDescription&) = default;

    void apply (const Elements& made) { elements = made.list; }
    void apply (Kind made) { kind = made; }
    void apply (const Uncounted& made) { uncountedElements = made.list.empty() ? elements : made.list; }
    void apply (const Always&) { alwaysThere = true; }

    void apply (Form made)
    {
        jassert (form == Form::plain);
        form = made;
    }

    void apply (const Range& made)
    {
        jassert (type != ValueType::boolean && type != ValueType::bytes);
        range = made;
    }

    void apply (const Words& made)
    {
        jassert (type == ValueType::text);
        words = made.list;
        apply (Form::words);
    }

    void apply (const HexDigits& made)
    {
        jassert (type == ValueType::text);
        digits = made.digits;
        apply (Form::hexID);
    }

    void setDefault (juce::var value) { defaultValue = std::move (value); }
    void setFamilyNames (std::vector<juce::Identifier> names) { familyNames = std::move (names); }
    void registerItself() { detail::getPropertyList().push_back (this); }

private:
    std::vector<const Element*> elements, uncountedElements;
    ValueType type;
    Form form = Form::plain;
    std::optional<Range> range;
    std::vector<const char*> words;
    int digits = 0;
    bool alwaysThere = false;
    juce::var defaultValue;
    Kind kind = Kind::song;
    std::vector<juce::Identifier> familyNames;

    static bool isAmong (const juce::Identifier& elementType, const std::vector<const Element*>& list) noexcept
    {
        for (const auto* element : list)
            if (element->getName() == elementType)
                return true;

        return false;
    }
};

namespace detail
{
    template <typename T>
    constexpr ValueType getValueType()
    {
        if constexpr (std::is_same_v<T, bool>)
            return ValueType::boolean;
        else if constexpr (std::is_same_v<T, int>)
            return ValueType::int32;
        else if constexpr (std::is_same_v<T, std::int64_t>)
            return ValueType::int64;
        else if constexpr (std::is_same_v<T, float>)
            return ValueType::float32;
        else if constexpr (std::is_same_v<T, double>)
            return ValueType::float64;
        else if constexpr (std::is_same_v<T, juce::String>)
            return ValueType::text;
        else
        {
            static_assert (std::is_same_v<T, juce::MemoryBlock>, "A saved property is a bool, an int, an std::int64_t, a float, "
                                                                 "a double, a juce::String or a juce::MemoryBlock");
            return ValueType::bytes;
        }
    }

    template <typename T>
    juce::var toVar (const T& value)
    {
        if constexpr (std::is_same_v<T, std::int64_t>)
            return juce::var ((juce::int64) value);
        else if constexpr (std::is_same_v<T, float>)
            return juce::var ((double) value);
        else
            return juce::var (value);
    }
} // namespace detail

// A saved property whose value is a T (bool, int, std::int64_t for an ID or a count, float, double, juce::String for
// text and words, juce::MemoryBlock for bytes): its name, the elements it is on, its kind, and what else Schema.h's
// comment says of it, as facts: a range (within(), whole()), words (oneOf()), hexadecimal digits (hexDigits()), a form
// (Form::ownID...), a default (byDefault()), always, uncounted.
template <typename T>
class Property final : public PropertyDescription
{
public:
    template <typename... Facts>
    Property (const char* text, const Elements& onElements, const Facts&... facts)
        : PropertyDescription (text, detail::getValueType<T>())
    {
        static_assert (detail::count<Kind, Facts...>() == 1, "A property states its kind");
        apply (onElements);
        (applyFact (facts), ...);
        registerItself();
    }

    Property (const Property&) = default; // registers nothing (PropertyDescription)

    const std::optional<T>& getDefaultValue() const noexcept { return defaultValue; }

private:
    std::optional<T> defaultValue;

    template <typename Value>
    void applyFact (const Default<Value>& made)
    {
        defaultValue = T (made.value);
        setDefault (detail::toVar (*defaultValue));
    }

    template <typename Fact>
    void applyFact (const Fact& made)
    {
        apply (made);
    }
};

// A family of saved text properties whose names are made in a loop (Schema.h: getPadSound()...): one description for
// them all, named by their pattern, with each of its names.
class Family final : public PropertyDescription
{
public:
    template <typename Names, typename... Facts>
    Family (const char* pattern, const Names& names, const Elements& onElements, const Facts&... facts)
        : PropertyDescription (pattern, ValueType::text)
    {
        static_assert (detail::count<Kind, Facts...>() == 1, "A family states its kind");
        apply (onElements);
        (apply (facts), ...);
        setFamilyNames (std::vector<juce::Identifier> (std::begin (names), std::end (names)));
        jassert (! getFamilyNames().empty());
        registerItself();
    }

    // Its name of that number, counted from 0, as its function counts them.
    const juce::Identifier& operator[] (size_t index) const noexcept { return getFamilyNames()[index]; }
};

//==============================================================================
// The description as lookups by an element's type, made once from getElements() and getProperties() (getTables()), or,
// for a test, from a changed copy of them, which the loader (model/project/internal/Project.h: makeConsistent()) and the
// consistency check (tests/SongCheck.h) take in place of the description's.
class Tables
{
public:
    Tables (std::vector<const Element*>, std::vector<const PropertyDescription*>);

    // What is described of an element type: the element, and the properties on it, all of them and those of each kind of
    // rule, in their order; a family's by each of its names.
    struct OnType
    {
        const Element* element = nullptr;
        std::vector<const PropertyDescription*> all, ranged, lists, worded, hexIDs, uncounted, holdingIDs, namingIDs, bytes;
        std::unordered_map<const void*, const PropertyDescription*> byName;
    };

    // Nothing described for a type the song hasn't.
    const OnType& on (const juce::Identifier& type) const noexcept;

    const Element* findElement (const juce::Identifier& type) const noexcept { return on (type).element; }
    const PropertyDescription* findProperty (const juce::Identifier& type, const juce::Identifier& name) const noexcept;

    // Whether any element has a property of that name that holds or names an ID of the song's (Form::ownID, songID,
    // idName), which its element's type then tells (holdsID(), namesID()).
    bool mayHoldID (const juce::Identifier& name) const noexcept;

    const std::vector<const Element*>& getElements() const noexcept { return elements; }
    const std::vector<const PropertyDescription*>& getProperties() const noexcept { return properties; }

private:
    std::vector<const Element*> elements;
    std::vector<const PropertyDescription*> properties;
    std::unordered_map<const void*, OnType> types;
    std::vector<const void*> idNames;
    OnType none;
};

// The description's, made at its first use.
const Tables& getTables();

// The whole description as data, for another tool and as the record that a name, a type, a range or a kind changed: its
// elements, with where each goes, and its properties, with their elements, type, form, range, words, default, kind and
// whether they count, each sorted by name (tests/golden/format/song-schema.json; docs/project-format.md).
juce::var describeSchema();
} // namespace phazite::schema

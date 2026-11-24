// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's state in a project file (src/model/base/internal/Schema.h): the song's count of IDs, and the description of
// what a song holds, which the loader and the consistency check read.
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

TEST_CASE ("A song numbers its items from its own count, which goes past every ID it holds")
{
    phazite::Song song { juce::ValueTree (phazite::schema::PHAZITE) };
    CHECK (song.makeID() == 1);
    CHECK (song.makeID() == 2);
    CHECK ((juce::int64) song.getState()[phazite::schema::nextID] == 3);

    // An item with a larger ID than the count (as a song read from a file can hold): counting the IDs moves the count past
    // it, and the next ID comes after it. The song opened from such a file (Project.h: makeSong()), its count set back.
    namespace schema = phazite::schema;
    juce::ValueTree file (schema::PHAZITE, { { schema::nextID, (juce::int64) 3 } });
    file.appendChild (juce::ValueTree (schema::MASTER, { { schema::trackID, (juce::int64) 1 }, { schema::channelFader, (juce::int64) 2 } }),
                      nullptr);
    file.appendChild (juce::ValueTree (schema::TRACKS, {},
                                       { juce::ValueTree (schema::TRACK, { { schema::trackID, (juce::int64) 10 },
                                                                           { schema::trackKind, schema::trackIsTrack },
                                                                           { schema::channelFader, (juce::int64) 11 } }) }),
                      nullptr);
    CHECK (phazite::findNextID (file) == 12);
    const auto opened = phazite::makeSong (file);
    REQUIRE (phazite::test::hasTrack (*opened, phazite::TrackID::fromRaw (10)));
    opened->getState().setProperty (schema::nextID, (juce::int64) 3, nullptr);
    CHECK (phazite::findNextID (*opened) == 12);
    phazite::countIDs (*opened);
    CHECK ((juce::int64) opened->getState()[schema::nextID] == 12);
    CHECK (opened->makeID() == 12);

    // A count further on than every ID stays where it is.
    opened->getState().setProperty (schema::nextID, (juce::int64) 50, nullptr);
    phazite::countIDs (*opened);
    CHECK (opened->makeID() == 50);
}

TEST_CASE ("A live song's next ID is past every ID its values hold beside its tree: a note's, a clip's, an alias group's, as it "
           "saves and opens again")
{
    // The song's tree holds no clip nor note (SongValues.h): its next ID worked out from the tree alone would be one a clip, a
    // note or an alias group of the song's has (IDsHeld.h: findLargestIDHeld(); Schema.h: findNextID() of a song).
    for (const auto* largestIs : { "a note's", "a clip's", "an alias group's" })
    {
        INFO ("the largest ID " << largestIs);
        phazite::test::TestSong s;
        const auto track = s.addTrack ("Keys");
        const auto clip = s.addMidiClip (track, 0.0, 4.0);
        auto& song = *phazite::findSong (s.context);
        std::uint64_t largest = 0;

        if (juce::String (largestIs) == "a note's")
        {
            s.addNote (clip, 60, 0.0, 1.0);
            const auto* notes = song.getValues()->findClipNotes (clip);
            REQUIRE ((notes != nullptr && notes->notes.size() == 1));
            largest = notes->notes[0].id.getRaw();
        }
        else if (juce::String (largestIs) == "a clip's")
        {
            largest = s.addMidiClip (track, 8.0, 4.0).getRaw();
        }
        else
        {
            // An alias group numbered past the song's count, as a file can hold one.
            const auto alias = s.copyClip (clip, 8.0, true);
            largest = (std::uint64_t) (juce::int64) song.getState()[phazite::schema::nextID] + 500;
            const phazite::UndoStep step (song, "Link");
            phazite::setClipLink (song, clip, largest);
            phazite::setClipLink (song, alias, largest);
        }

        // The count behind them all, as a song's whose count was worked out from its tree alone.
        song.getState().setProperty (phazite::schema::nextID, (juce::int64) 1, nullptr);
        REQUIRE (phazite::findLargestIDHeld (song) == largest);
        CHECK (phazite::findNextID (song.getState()) <= largest);
        CHECK (phazite::findNextID (song) == largest + 1);

        // Counted as the project saves: the file's count is past it; and opened again, so is the song's.
        REQUIRE (s.getProject().save().wasOk());
        const auto saved = phazite::test::readXml (s.getProject().getFile());
        REQUIRE (saved != nullptr);
        CHECK ((std::uint64_t) saved->getStringAttribute (phazite::schema::nextID.getName().toString()).getLargeIntValue() > largest);

        s.reopen();
        auto& reopened = *phazite::findSong (s.context);
        CHECK (phazite::findNextID (reopened) > largest);
        CHECK (reopened.makeID() > largest);
    }
}

TEST_CASE ("An ID two items of a song hold is found, whatever their kinds; none in a song that holds each once")
{
    // IDsHeld.h: findIDHeldTwice(), what the versions' passes assert of the song they made like another. The song's count set
    // back, as no Phazite does, so that the next item made takes an ID the song holds.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Keys");
    const auto clip = s.addMidiClip (track, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0);
    const auto alias = s.copyClip (clip, 8.0, true); // an alias group's ID, which its clips share, and its notes' are no items'
    REQUIRE (alias.isValid());
    CHECK (phazite::findIDHeldTwice (song) == 0);

    const auto countFrom = [&song] (std::uint64_t id) { song.getState().setProperty (phazite::schema::nextID, (juce::int64) id, nullptr); };

    SUBCASE ("a clip of a track's ID")
    {
        countFrom (track.getRaw());
        REQUIRE (s.addMidiClip (track, 16.0, 4.0).getRaw() == track.getRaw());
        CHECK (phazite::findIDHeldTwice (song) == track.getRaw());
    }

    SUBCASE ("a modulator of a clip's ID")
    {
        countFrom (clip.getRaw());
        REQUIRE (s.addModulator (track, phazite::ModulatorType::lfo).getRaw() == clip.getRaw());
        CHECK (phazite::findIDHeldTwice (song) == clip.getRaw());
    }
}

//==============================================================================
// The description of the song (Schema.h, SchemaDescription.h), and the loader (model/project/internal/Project.h:
// makeConsistent()) and the consistency check (SongCheck.h), which read it.
namespace
{
namespace schema = phazite::schema;
using namespace phazite::test;

// The representative golden project as its file holds it (GoldenProjectTests.cpp writes it), every value a text.
juce::ValueTree readRepresentative()
{
    const auto xml = juce::parseXML (getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    auto tree = juce::ValueTree::fromXml (*xml);
    REQUIRE (tree.hasType (schema::PHAZITE));

    // Where the song stands in its versions is a placeholder there ("$BRANCH"), which the golden test writes for the ids.
    for (const auto* placeholder : { &schema::versionsBranch, &schema::versionsBase })
        if (tree[*placeholder].toString().startsWithChar ('$'))
            tree.removeProperty (*placeholder, nullptr);

    return tree;
}

// The first element of a type in a song, its own first; none if it holds none.
juce::ValueTree findFirst (const juce::ValueTree& tree, const juce::Identifier& type)
{
    if (tree.hasType (type))
        return tree;

    for (const auto& child : tree)
        if (const auto found = findFirst (child, type); found.isValid())
            return found;

    return {};
}

// A property's default as the song holds it on an element of a type where it has none: the description's, but for the
// master's volume, which is -3 dB, a new song's level (Schema.h: channelVolume, whose description gives the one default of a
// track's, a chain's and the master's: 0; TracksValue.h: TrackValue::defaultVolume()).
juce::var getHeldDefault (const juce::Identifier& type, const schema::PropertyDescription& property)
{
    if (type == schema::MASTER.getName() && &property == &schema::channelVolume)
        return phazite::TrackValue::defaultVolume (phazite::TrackValue::Kind::master);

    return property.getDefault();
}

// The first element of a type that a property can be checked on as a song opened holds it: for a clip's, the first clip of
// the kind that holds the property (an audio clip's file, part, sound, gain, fades; one recorded round a loop's takes), as
// the song holds a clip of each kind's own properties alone (ClipsValue.h: ClipValue); else the first of the type.
juce::ValueTree findFirstFor (const juce::ValueTree& tree, const juce::Identifier& type, const juce::Identifier& property)
{
    // A device's sounds: of a device whose type has the property, which the loader keeps no other of (DevicesValue.h:
    // keepDeviceAsHeld()): a sampler's slices on a sampler's SOUNDS, not a drum sampler's.
    if (type == schema::SOUNDS)
    {
        juce::ValueTree found;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (found.isValid())
                    return;

                if (child.hasType (schema::SOUNDS)
                    && phazite::findSoundIndex (std::string_view (element[schema::deviceType].toString().toRawUTF8()), property) >= 0)
                    found = child;
                else
                    visit (child);
            }
        };

        visit (tree);
        return found;
    }

    if (type != schema::CLIP)
        return findFirst (tree, type);

    const auto isTake = property == schema::clipTakes || property == schema::clipTake || property == schema::clipTakeLength;
    const auto isAudio = isTake || property == schema::clipFile || property == schema::clipFileStart || property == schema::clipFileEnd
                         || property == schema::clipTempo || property == schema::clipPitch || property == schema::clipReversed
                         || property == schema::clipGain || property == schema::clipFadeIn || property == schema::clipFadeOut
                         || property == schema::clipFadeInShape || property == schema::clipFadeOutShape
                         || property == schema::clipFadeInBend || property == schema::clipFadeOutBend;
    juce::ValueTree found;
    const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (found.isValid())
                return;

            if (child.hasType (schema::CLIP) && (! isAudio || child[schema::clipKind].toString() == schema::clipIsAudio)
                && (! isTake || child.hasProperty (schema::clipTakes)))
                found = child;
            else
                visit (child);
        }
    };

    visit (tree);
    return found;
}

// Whether an element is still in the song it was found in.
bool isInSong (const juce::ValueTree& element)
{
    auto top = element;

    while (top.getParent().isValid())
        top = top.getParent();

    return top.hasType (schema::PHAZITE);
}

// Whether one of the problems is of a rule and names a property.
bool hasProblem (const std::vector<std::string>& problems, const std::string& rule, const juce::String& property)
{
    return std::any_of (problems.begin(), problems.end(), [&] (const std::string& problem)
                        { return problem.starts_with (rule + ": ") && juce::String (problem).contains (property); });
}

juce::String describeProblems (const std::vector<std::string>& problems)
{
    juce::StringArray lines;

    for (const auto& problem : problems)
        lines.add (problem);

    return lines.joinIntoString ("\n");
}

// What the loader keeps a number at, as the description says: brought within the range (made whole first where it is
// whole, a power of two where it says so), or gone where one outside goes; for what is no finite number, its fallback or
// none.
std::optional<double> findKept (const schema::Range& range, std::optional<double> number)
{
    if (! number.has_value() || ! std::isfinite (*number))
        return range.fallback;

    const auto rounded = range.whole ? std::round (*number) : *number;

    if (range.outsideGoes && (rounded < range.low || rounded > range.high))
        return std::nullopt;

    auto kept = juce::jlimit (range.low, range.high, rounded);

    if (range.powersOfTwo)
        kept = std::exp2 (std::round (std::log2 (kept)));

    return kept;
}

// Values outside a range, and in it but not whole, each as text, as a file holds them.
std::vector<juce::String> findWrongValues (const schema::Range& range)
{
    std::vector<juce::String> values { juce::String (range.low - 1.0, 6), juce::String (range.high + 1.0, 6), "not a number" };

    if (range.whole && range.low + 0.5 < range.high)
        values.push_back (juce::String (range.low + 0.5, 6));

    return values;
}

// Whether two values a file or whole state holds are the same: as numbers where both are, else as text.
bool isSameSaved (const juce::var& a, const juce::var& b)
{
    const auto x = phazite::readValueNumber (a), y = phazite::readValueNumber (b);
    return x.has_value() && y.has_value() ? juce::exactlyEqual (*x, *y) : a.toString() == b.toString();
}

// The same, of numbers a value holds as floats: each read as one.
bool isSameSavedFloat (const juce::var& a, const juce::var& b)
{
    const auto x = phazite::readValueNumber (a), y = phazite::readValueNumber (b);
    return x.has_value() && y.has_value() ? juce::exactlyEqual ((float) *x, (float) *y) : a.toString() == b.toString();
}

// A song of the representative project with one value changed, checked as it is and after the loader; and what the
// loader made of it, or none if it took it out (or the element it was on). For a clip, its warp marker, its note or its
// controller's value, also the song opened as a project opens it (makeSong()), which holds its clips and their notes as
// values beside its tree.
struct Changed
{
    juce::ValueTree element, original;
    std::vector<std::string> problemsBefore;
    phazite::Song::Ptr loaded, opened;
    juce::ValueTree loadedElement;
    phazite::ClipID clip;
    juce::String curveKey; // of a curve changed, or of the one a point changed is in
    juce::Identifier changedProperty;
    juce::Identifier parentType {}; // of the element changed, and its key: where a modulation is found (findModulationState())
    juce::String parentKey {};
    phazite::TrackID channel {}; // of a track's leaf changed, or a lane's: its track's (findChannelState())
    bool channelIsMaster = false;

    Changed (const schema::Element& type, const juce::Identifier& property, const juce::var& value,
             const schema::Tables& tables = schema::getTables())
        : changedProperty (property)
    {
        auto raw = readRepresentative();
        element = findFirstFor (raw, type, property);
        REQUIRE_MESSAGE (element.isValid(), "the representative project holds no " << type.toString());
        original = element.createCopy();
        parentType = element.getParent().getType();
        parentKey = schema::getElementKey (element.getParent());
        element.setProperty (property, value, nullptr);

        const phazite::Song rawSong { raw.createCopy() };
        problemsBefore = findSongProblems (rawSong, tables);

        // A clip, its warp marker, its note or its controller's value, a tempo or a time signature, a chord or a key change, a
        // MIDI link, a curve or its point, a section, a modulator, a macro or a modulation, a send, or a plug-in's value set by
        // hand: the loader's part alone (makeConsistent()), on a song that keeps everything in its tree, since a song made by
        // makeSong() holds its clips, their notes, its tempo map, its chord lane, its MIDI links, its curves, its sections, its
        // modulation, its sends, its devices' store and its devices as values beside it (Song::holdValues()); and the song
        // opened, whose values findOpened() reads back through whole state.
        if (isCurveElement (element))
            curveKey = schema::getElementKey (element.hasType (schema::CURVE) ? element : element.getParent());

        if (element.hasType (schema::CLIP) || element.hasType (schema::WARP) || element.hasType (schema::NOTE)
            || element.hasType (schema::CONTROL) || isTempoMapEntry (element) || isChordLaneEntry (element) || isMidiLink (element)
            || isCurveElement (element) || isSection (element) || isModulation (element) || isSend (element) || isStoreEntry (element)
            || isDeviceItem (element) || isTrackLeaf (element, property))
        {
            if (isTrackLeaf (element, property))
            {
                const auto channelElement = element.hasType (schema::LANE) ? element.getParent().getParent() : element;
                channelIsMaster = channelElement.hasType (schema::MASTER);
                channel = phazite::TrackID::fromRaw (phazite::readID (channelElement[schema::trackID]));
            }

            if (! isTempoMapEntry (element) && ! isChordLaneEntry (element) && ! isMidiLink (element) && ! isCurveElement (element)
                && ! isSection (element) && ! isModulation (element) && ! isSend (element) && ! isStoreEntry (element)
                && ! isDeviceItem (element) && ! isTrackLeaf (element, property))
            {
                const auto clipElement = element.hasType (schema::CLIP) ? element : element.getParent();
                clip = phazite::ClipID::fromRaw (phazite::readID (clipElement[schema::clipID]));
            }

            if (&tables == &schema::getTables())
            {
                opened = phazite::makeSong (raw.createCopy());
                REQUIRE (opened != nullptr);
            }

            loaded = new phazite::Song (raw);
            phazite::makeConsistent (*loaded, phazite::ReadAs::whole, tables);
        }
        else
        {
            loaded = phazite::makeSong (raw);
            REQUIRE (loaded != nullptr);

            if (&tables != &schema::getTables())
                phazite::makeConsistent (*loaded, phazite::ReadAs::whole, tables);
        }

        loadedElement = isInSong (element) ? element : juce::ValueTree();
    }

    // What the loaded song holds there: none if it took the value, or the element, out.
    std::optional<juce::var> getLoaded (const juce::Identifier& property) const
    {
        if (! loadedElement.isValid() || ! loadedElement.hasProperty (property))
            return std::nullopt;

        return loadedElement[property];
    }

    static bool isTempoMapEntry (const juce::ValueTree& element)
    {
        return element.hasType (schema::TEMPO) || element.hasType (schema::TIMESIGNATURE);
    }

    static bool isChordLaneEntry (const juce::ValueTree& element)
    {
        return element.hasType (schema::CHORD) || element.hasType (schema::CHORDKEY);
    }

    static bool isMidiLink (const juce::ValueTree& element) { return element.hasType (schema::MIDILINK); }
    static bool isSection (const juce::ValueTree& element) { return element.hasType (schema::SECTION); }

    static bool isModulation (const juce::ValueTree& element)
    {
        return element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO) || element.hasType (schema::MODULATES);
    }

    static bool isSend (const juce::ValueTree& element) { return element.hasType (schema::SEND); }

    // A track's or the master's switch or view property, or a lane's: the tracks' switches' and view's (TrackSwitchesValue.h,
    // TracksOutside.h), which the song holds beside its tree.
    static bool isTrackLeaf (const juce::ValueTree& element, const juce::Identifier& property)
    {
        return element.hasType (schema::LANE) || phazite::isSwitchProperty (element, property)
               || phazite::isViewProperty (element, property);
    }

    // The opened song's channel (a TRACK by its ID, or the MASTER) in its whole state, where the element changed is a track's
    // leaf or one of its lanes.
    juce::ValueTree findChannelState() const
    {
        const auto whole = phazite::copyWholeSong (*opened);
        return channelIsMaster ? whole.getChildWithName (schema::MASTER) : phazite::test::findTrackIn (whole, channel);
    }

    // A plug-in's value set by hand, the devices' store's (DeviceStoreValue.h), which the song holds beside its tree.
    static bool isStoreEntry (const juce::ValueTree& element)
    {
        return element.hasType (schema::VALUE) && element.getParent().hasType (schema::DEVICE);
    }

    // A device, a container or a chain, or a device's VALUES or SOUNDS: the devices' area's (DevicesValue.h), which the song
    // holds beside its tree.
    static bool isDeviceItem (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::CHAIN)
               || ((element.hasType (schema::VALUES) || element.hasType (schema::SOUNDS)) && element.getParent().hasType (schema::DEVICE));
    }

    // A modulator's, a macro's, a modulation's, a send's or a plug-in's value's owner in the whole state of the opened song: the
    // element of the original's parent's type and key (a track, the master or a container for a modulator or a macro; the
    // modulator or the macro for a modulation; a track for a send; a plug-in's device for its value), whose children the
    // opened song holds as values, written in.
    juce::ValueTree findModulationState() const
    {
        const std::function<juce::ValueTree (const juce::ValueTree&)> find = [&] (const juce::ValueTree& tree)
        {
            for (const auto& child : tree)
            {
                if (child.getType() == parentType && schema::getElementKey (child) == parentKey)
                    return child;

                if (const auto found = find (child); found.isValid())
                    return found;
            }

            return juce::ValueTree();
        };

        return find (phazite::copyWholeSong (*opened));
    }

    static bool isCurveElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::CURVE) || element.hasType (schema::POINT);
    }

    // A curve's in the whole state of the opened song: the song's automation for a curve, its curve (by the key the curve
    // changed or the one its point is in had) for a point.
    juce::ValueTree findCurveState() const
    {
        const auto automation = phazite::copyWholeSong (*opened).getChildWithName (schema::AUTOMATION);

        if (original.hasType (schema::CURVE))
            return automation;

        for (const auto& curve : automation)
            if (schema::getElementKey (curve) == curveKey)
                return curve;

        return {};
    }

    // The opened song's element in its clip's whole state (WholeState.h), as it holds its values: the clip itself, a note by
    // its ID, a warp marker or a controller's value by every property but the one changed; a tempo or a time signature in
    // its tempo map's whole state, a chord or a key change in its chord lane's, a MIDI link in its links', a curve or a point
    // in its automation's, and a section in its sections', the same way; none if the loader took it out.
    juce::ValueTree findOpened() const
    {
        REQUIRE (opened != nullptr);

        // A track's or the master's leaf: its channel itself, as whole state writes it.
        if (isTrackLeaf (original, changedProperty) && ! original.hasType (schema::LANE))
            return findChannelState();

        const auto clipState = original.hasType (schema::LANE) ? findChannelState().getChildWithName (schema::LANES)
                               : isTempoMapEntry (original)    ? phazite::copyWholeSong (*opened).getChildWithName (schema::TEMPOMAP)
                               : isChordLaneEntry (original)   ? phazite::copyWholeSong (*opened).getChildWithName (schema::CHORDLANE)
                               : isMidiLink (original)         ? phazite::copyWholeSong (*opened).getChildWithName (schema::MIDILINKS)
                               : isCurveElement (original)     ? findCurveState()
                               : isSection (original)          ? phazite::copyWholeSong (*opened).getChildWithName (schema::SECTIONS)
                               : isModulation (original)       ? findModulationState()
                               : isSend (original)             ? findModulationState()
                               : original.hasType (schema::VALUE) || isDeviceItem (element)
                                   ? findModulationState() // a plug-in's value, a device's (the copy has no parent)
                                   : phazite::copyWholeClip (*opened, clip);

        if (! clipState.isValid() || original.hasType (schema::CLIP))
            return clipState;

        for (const auto& child : clipState)
        {
            if (! child.hasType (original.getType()))
                continue;

            if (original.hasType (schema::NOTE))
            {
                if (isSameSaved (child[schema::noteID], original[schema::noteID]))
                    return child;

                continue;
            }

            auto same = true;

            // A point's numbers as the song holds them (CurveValues.h, PointValue: its position and its bend are floats, read
            // from its file's text as one; its beat a double, as every other number here).
            const auto isHeldAsFloat = [this] (const juce::Identifier& name)
            { return original.hasType (schema::POINT) && (name == schema::pointValue || name == schema::pointCurve); };

            for (int i = 0; i < original.getNumProperties(); ++i)
                if (const auto name = original.getPropertyName (i); name != changedProperty)
                    same = same
                           && (isHeldAsFloat (name) ? isSameSavedFloat (child[name], original[name])
                                                    : isSameSaved (child[name], original[name]));

            if (same)
                return child;
        }

        return {};
    }
};
} // namespace

TEST_CASE ("Every range the description gives is what the loader keeps a number to and what the check allows, on every element "
           "it is on")
{
    for (const auto* property : schema::getProperties())
    {
        if (! property->getRange().has_value() || property->getForm() == schema::Form::numbers)
            continue;

        const auto& range = *property->getRange();

        for (const auto* type : property->getElements())
        {
            for (const auto& value : findWrongValues (range))
            {
                INFO (type->toString() << "@" << property->toString() << " = " << value);
                const Changed changed { *type, *property, value };
                INFO (describeProblems (changed.problemsBefore));
                CHECK (hasProblem (changed.problemsBefore, "range", property->toString()));

                auto expected = findKept (range, phazite::readValueNumber (value));

                // A macro's value kept at 0, its default, is written as none, as the song holds a macro (ModulationValue.h:
                // keepModulatorAsHeld()).
                if (expected.has_value() && type == &schema::MACRO && property == &schema::macroValue
                    && juce::exactlyEqual (*expected, 0.0))
                    expected.reset();

                // A container's or a chain's own property kept at what Phazite's writer leaves out (a container's mix at 1, a
                // chain's volume and pan at 0, a range at its end) is written as none, as the song holds them (DevicesValue.h:
                // keepDeviceAsHeld()).
                // A send's level kept at 0 dB is written as none too, as the song holds a send (SendsValue.h: keepSendAsHeld()).
                for (const auto& [element, at, none] : { std::tuple (&schema::SEND, &schema::sendLevel.getName(), 0.0),
                                                         std::tuple (&schema::CONTAINER, &schema::deviceMix.getName(), 1.0),
                                                         std::tuple (&schema::CHAIN, &schema::channelVolume.getName(), 0.0),
                                                         std::tuple (&schema::CHAIN, &schema::channelPan.getName(), 0.0),
                                                         std::tuple (&schema::CHAIN, &schema::chainLowKey.getName(), 0.0),
                                                         std::tuple (&schema::CHAIN, &schema::chainHighKey.getName(), 127.0),
                                                         std::tuple (&schema::CHAIN, &schema::chainLowVelocity.getName(), 1.0),
                                                         std::tuple (&schema::CHAIN, &schema::chainHighVelocity.getName(), 127.0) })
                    if (expected.has_value() && type == element && property->getName() == *at && juce::exactlyEqual (*expected, none))
                        expected.reset();

                const auto loaded = changed.getLoaded (*property);
                REQUIRE (loaded.has_value() == expected.has_value());

                if (expected.has_value())
                    CHECK (juce::exactlyEqual (phazite::readValueNumber (*loaded).value_or (-1.0e300), *expected));

                checkSong (*changed.loaded);

                // A clip's note or controller's value as the opened song holds it, read back through whole state: the same,
                // or none where the loader took it out (a value whole state leaves out is the description's default).
                if (changed.opened != nullptr)
                {
                    INFO ("as the song opened holds it");
                    const auto opened = changed.findOpened();

                    // A mix on one of Phazite's own devices, which nothing reads (its mix is a parameter in its VALUES:
                    // Schema.h, deviceMix): what the loader kept of it is dropped as the song opens.
                    const auto isOwnDevicesMix = changed.original.hasType (schema::DEVICE) && property == &schema::deviceMix
                                                 && changed.original[schema::deviceType].toString() != schema::deviceIsPlugin;

                    if (expected.has_value() && ! isOwnDevicesMix)
                    {
                        REQUIRE (opened.isValid());
                        const auto held = opened.hasProperty (*property) ? opened[*property] : getHeldDefault (type->getName(), *property);

                        // A clip's own property as what the loader kept is then kept with the clip's others, as the song
                        // takes a clip in (ClipsValue.h: keepWithinClip(): a MIDI clip no shorter than the shortest, a take
                        // among its takes): what the loader kept, held by a clip's value.
                        auto wanted = *expected;

                        if (changed.original.hasType (schema::CLIP))
                        {
                            REQUIRE (changed.loadedElement.isValid());
                            const auto kept =
                                phazite::writeClipElement (phazite::keepWithinClip (phazite::readClipValue (changed.loadedElement)));
                            wanted = phazite::readValueNumber (kept.hasProperty (*property) ? kept[*property] : property->getDefault())
                                         .value_or (-1.0e300);
                        }

                        // A chord's as what the loader kept is then kept apart from the lane's other chords, as the song takes
                        // a lane in (ChordLaneValue.h: readHeldChordLane(), keepChordsApart(): a length reaching no further
                        // than the next chord's start): what the loader kept, held by the lane's value, the chord at its place
                        // among the lane's chords (none of them gone).
                        if (changed.original.hasType (schema::CHORD))
                        {
                            REQUIRE (changed.loadedElement.isValid());
                            const auto laneElement = changed.loadedElement.getParent();
                            const auto lane = phazite::readHeldChordLane (laneElement);
                            int place = 0, chords = 0;

                            for (const auto& child : laneElement)
                            {
                                if (child == changed.loadedElement)
                                    place = chords;

                                chords += child.hasType (schema::CHORD) ? 1 : 0;
                            }

                            REQUIRE (lane.chords.size() == (size_t) chords);
                            juce::ValueTree written (schema::CHORDLANE);
                            phazite::writeChordLane (lane, written);
                            const auto kept = written.getChild (place); // the chords written first
                            wanted = phazite::readValueNumber (kept.hasProperty (*property) ? kept[*property] : property->getDefault())
                                         .value_or (-1.0e300);
                        }

                        CHECK (juce::exactlyEqual (phazite::readValueNumber (held).value_or (-1.0e300), wanted));
                    }
                    else if (property == &schema::clipTake)
                    {
                        // A clip's take, which a value reads as the first where its element has none, and whole state writes with
                        // the clip's takes.
                        CHECK ((! opened.isValid() || ! opened.hasProperty (*property) || (int) opened[*property] == 1));
                    }
                    else if (property == &schema::sectionEnergy)
                    {
                        // A section's energy, which a value reads as its description's default where its element has none, and
                        // whole state writes with the section's others (SectionsValue.h: writeSectionElement()).
                        CHECK ((! opened.isValid() || ! opened.hasProperty (*property)
                                || juce::exactlyEqual (phazite::readValueNumber (opened[*property]).value_or (-1.0),
                                                       phazite::readValueNumber (property->getDefault()).value_or (-2.0))));
                    }
                    else
                    {
                        CHECK ((! opened.isValid() || ! opened.hasProperty (*property)));
                    }

                    checkSong (*changed.opened);
                }
            }
        }
    }
}

TEST_CASE ("Every list of numbers the description gives a range keeps each number to it, through the loader and the check")
{
    for (const auto* property : schema::getProperties())
    {
        if (! property->getRange().has_value() || property->getForm() != schema::Form::numbers)
            continue;

        const auto& range = *property->getRange();

        for (const auto* type : property->getElements())
        {
            const auto values = findWrongValues (range);
            juce::StringArray tokens;

            for (const auto& value : values)
                tokens.add (value.containsChar (' ') ? "none" : value);

            tokens.add (juce::String ((range.low + range.high) / 2.0, 6));
            INFO (type->toString() << "@" << property->toString() << " = " << tokens.joinIntoString (" "));
            const Changed changed { *type, *property, tokens.joinIntoString (" ") };
            INFO (describeProblems (changed.problemsBefore));
            CHECK (hasProblem (changed.problemsBefore, "range", property->toString()));

            juce::StringArray expected;

            for (const auto& token : tokens)
                if (const auto kept = findKept (range, phazite::readValueNumber (token)))
                    expected.add (juce::String (*kept, 6));

            // A step sequencer's steps without the zeros at the end, as the song holds a modulator (ModulationValue.h:
            // keepModulatorAsHeld()).
            if (type == &schema::MODULATOR && property == &schema::modulatorSteps)
                while (! expected.isEmpty() && expected[expected.size() - 1] == juce::String (0.0, 6))
                    expected.remove (expected.size() - 1);

            const auto loaded = changed.getLoaded (*property);
            REQUIRE (loaded.has_value());
            juce::StringArray read;

            for (const auto& token : juce::StringArray::fromTokens (loaded->toString(), " ", {}))
                read.add (juce::String (phazite::readValueNumber (token).value_or (-1.0e300), 6));

            CHECK (read == expected);
            checkSong (*changed.loaded);
        }
    }
}

TEST_CASE ("Every word list the description gives is what the loader keeps a word to and what the check allows")
{
    for (const auto* property : schema::getProperties())
    {
        if (property->getForm() != schema::Form::words)
            continue;

        for (const auto* type : property->getElements())
        {
            INFO (type->toString() << "@" << property->toString());
            const Changed changed { *type, *property, "notOneOfTheWords" };
            INFO (describeProblems (changed.problemsBefore));
            CHECK (hasProblem (changed.problemsBefore, "kind", property->toString()));

            // A word not on the list goes; one every element holds (a track's kind) is given one, or its element goes.
            const auto loaded = changed.getLoaded (*property);

            if (property->isAlways() && loaded.has_value())
            {
                const auto& words = property->getWords();
                CHECK (std::any_of (words.begin(), words.end(), [&] (const char* word) { return loaded->toString() == word; }));
            }
            else
            {
                CHECK_FALSE (loaded.has_value());
            }

            checkSong (*changed.loaded);
        }
    }
}

TEST_CASE ("Every id the description writes in hexadecimal digits keeps to their count, through the loader and the check")
{
    for (const auto* property : schema::getProperties())
    {
        if (property->getForm() != schema::Form::hexID)
            continue;

        for (const auto* type : property->getElements())
        {
            const auto digits = property->getHexDigits();
            const auto good = juce::String::repeatedString ("0123456789abcdef", digits).substring (0, digits);

            for (const auto& value : { good.substring (1), good + "0", good.toUpperCase(), juce::String ("not an id") })
            {
                INFO (type->toString() << "@" << property->toString() << " = " << value);
                const Changed changed { *type, *property, value };
                INFO (describeProblems (changed.problemsBefore));
                CHECK (hasProblem (changed.problemsBefore, "kind", property->toString()));
                CHECK_FALSE (changed.getLoaded (*property).has_value());
                checkSong (*changed.loaded);
            }

            const Changed kept { *type, *property, good };
            INFO (describeProblems (kept.problemsBefore));
            CHECK (kept.problemsBefore.empty());
            CHECK (kept.getLoaded (*property) == std::optional<juce::var> (good));
        }
    }
}

TEST_CASE ("A range changed in the description changes what the loader keeps a number to and what the check allows")
{
    // A copy of the description with a note's pitch at most 100, which nothing else reads.
    auto narrower = schema::notePitch;
    narrower.setRange (schema::whole (0.0, 100.0).orElse (60.0));
    auto properties = schema::getProperties();
    std::replace (properties.begin(), properties.end(), static_cast<const schema::PropertyDescription*> (&schema::notePitch),
                  static_cast<const schema::PropertyDescription*> (&narrower));
    const schema::Tables changed { schema::getElements(), properties };

    // Under the description: a pitch of 110 is one, and stays.
    const Changed asDescribed { schema::NOTE, schema::notePitch, 110 };
    CHECK_FALSE (hasProblem (asDescribed.problemsBefore, "range", "pitch"));
    CHECK (asDescribed.getLoaded (schema::notePitch) == std::optional<juce::var> (110));

    // Under the changed copy: the check finds it, and the loader brings it in.
    const Changed narrowed { schema::NOTE, schema::notePitch, 110, changed };
    CHECK (hasProblem (narrowed.problemsBefore, "range", "pitch"));
    REQUIRE (narrowed.getLoaded (schema::notePitch).has_value());
    CHECK ((int) *narrowed.getLoaded (schema::notePitch) == 100);
    CHECK (findSongProblems (*narrowed.loaded, changed).empty());
}

TEST_CASE ("The description holds together")
{
    const auto& elements = schema::getElements();
    const auto isDescribed = [&elements] (const schema::Element* element)
    { return std::find (elements.begin(), elements.end(), element) != elements.end(); };

    // Every element in its parents, which are described, but the song's own.
    for (const auto* element : elements)
    {
        INFO (element->toString());
        CHECK (element->getParents().empty() == (element == &schema::PHAZITE));

        for (const auto* parent : element->getParents())
            CHECK (isDescribed (parent));

        CHECK (schema::isKnownType (*element));
    }

    // Every property on described elements, at least one; and no two of one element's with one name.
    std::map<juce::String, std::set<juce::String>> names;

    for (const auto* property : schema::getProperties())
    {
        INFO (property->toString());
        CHECK_FALSE (property->getElements().empty());

        for (const auto* element : property->getElements())
        {
            CHECK (isDescribed (element));
            auto& onElement = names[element->toString()];

            if (property->isFamily())
                for (const auto& name : property->getFamilyNames())
                    CHECK_MESSAGE (onElement.insert (name.toString()).second, element->toString() << "@" << name.toString());
            else
                CHECK_MESSAGE (onElement.insert (property->toString()).second, element->toString() << "@" << property->toString());

            CHECK (schema::getTables().findProperty (*element, property->isFamily() ? property->getFamilyNames().front() : *property)
                   == property);
        }
    }

    // Every name the versions' rules give (song-rules.json) described, a property on the element it is given on.
    const auto rules = schema::describeSongRules();
    const auto isElement = [] (const juce::var& name) { return schema::isKnownType (juce::Identifier (name.toString())); };
    const auto isProperty = [] (const juce::var& type, const juce::var& name)
    { return schema::getTables().findProperty (juce::Identifier (type.toString()), juce::Identifier (name.toString())) != nullptr; };

    for (const auto& type : *rules["items"]["types"].getArray())
    {
        CHECK (isElement (type));
        CHECK (isProperty (type, rules["items"]["idProperty"]));
    }

    CHECK (isProperty (rules["items"]["alsoHoldingAnID"], rules["items"]["idProperty"]));

    for (const auto& key : *rules["keys"].getArray())
        for (const auto& property : *key["properties"].getArray())
            CHECK_MESSAGE (isProperty (key["type"], property), key["type"].toString() << "@" << property.toString());

    for (const auto& type : *rules["unkeyed"].getArray())
        CHECK (isElement (type));

    for (const auto& order : rules["orders"].getDynamicObject()->getProperties())
        CHECK (isElement (order.name.toString()));

    for (const auto* table : { "otherIDHolders", "idNamers", "bytes" })
        for (const auto& row : *rules[table].getArray())
            CHECK_MESSAGE (isProperty (row["type"], row["property"]),
                           table << ": " << row["type"].toString() << "@" << row["property"].toString());

    for (const auto& type : rules["uncountedProperties"].getDynamicObject()->getProperties())
        for (const auto& property : *type.value.getArray())
            CHECK_MESSAGE (isProperty (type.name.toString(), property), type.name.toString() << "@" << property.toString());

    for (const auto& row : *rules["uncountedElements"].getArray())
    {
        CHECK (isElement (row["parent"]));
        CHECK (isElement (row["type"]));
    }

    for (const auto* list : { "piecesOfTheirOwn", "inRuns" })
        for (const auto& type : *rules[list].getArray())
            CHECK (isElement (type));

    // The representative golden project, which the cases above change one value of, holds together as it is.
    {
        const phazite::Song representative { readRepresentative() };
        const auto problems = findSongProblems (representative);
        INFO (describeProblems (problems));
        CHECK (problems.empty());
    }

    // Every element and property the representative golden project holds described (a device's values set by hand are
    // named by its parameters).
    const std::function<void (const juce::ValueTree&)> visit = [&visit] (const juce::ValueTree& element)
    {
        const auto* described = schema::getTables().findElement (element.getType());
        REQUIRE_MESSAGE (described != nullptr, element.getType().toString());

        if (! described->isNamedByParameters())
            for (int i = 0; i < element.getNumProperties(); ++i)
                CHECK_MESSAGE (schema::getTables().findProperty (element.getType(), element.getPropertyName (i)) != nullptr,
                               element.getType().toString() << "@" << element.getPropertyName (i).toString());

        for (const auto& child : element)
            visit (child);
    };

    visit (readRepresentative());
}

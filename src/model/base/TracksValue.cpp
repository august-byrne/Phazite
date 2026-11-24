// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/TracksValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> tracksRead { 0 }; // countTracksReadAsValues(): a count alone, which orders nothing

    using Kind = TrackValue::Kind;
    using Monitoring = TrackValue::Monitoring;
    using IDList = immer::flex_vector<TrackID>;
    using Lists = immer::map<std::uint64_t, IDList>;

    bool isTrackOrMaster (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACK) || element.hasType (schema::MASTER);
    }

    // An ID as Phazite's code always read a track's, its fader's and its pad's device: a cast of the value (a text's leading
    // number), held within an int64's range (a double's cast past it is undefined), 0 for none.
    std::uint64_t castID (const juce::var& held)
    {
        return (std::uint64_t) toWhole<juce::int64> (held);
    }

    // An ID the song can count on from: more than 0 and below schema::maxID.
    bool isHeldID (std::uint64_t id) noexcept
    {
        return id > 0 && id < schema::maxID;
    }

    // An ID as the song holds it: as readID() reads it, none for one it can't count on from.
    std::uint64_t readHeldIDOf (const juce::var& held)
    {
        const auto id = readID (held);
        return isHeldID (id) ? id : 0;
    }

    // A number as Phazite's code always read a volume and a pan, a cast of the value (a text's leading number), held within a
    // float's range; `otherwise` for none and for what is no finite number.
    float readCastFloat (const juce::var& held, float otherwise)
    {
        return held.isVoid() ? otherwise : toFloat ((double) held, otherwise);
    }

    // A number as the song holds it: by its description, within its range (SongText.h: readDescribed()), `otherwise` for none
    // and for what is no finite number, which the loader takes away (Project.h: makeConsistent(), keepNumbers()).
    double readHeldNumber (const juce::var& held, const schema::PropertyDescription& property, double otherwise)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? readDescribed (held, property) : otherwise;
    }

    // A whole number as the song holds one, none for none and for what is no finite number.
    std::optional<int> readHeldWhole (const juce::var& held, const schema::PropertyDescription& property)
    {
        const auto number = readValueNumber (held);

        if (! number.has_value() || ! std::isfinite (*number))
            return std::nullopt;

        return roundToWhole<int> (readDescribed (held, property));
    }

    // A colour's text as juce::Colour::fromString() reads it, none where that is transparent (getTrackColour()'s rule: a
    // text that is no colour reads so).
    std::optional<juce::uint32> readColourOf (const juce::var& held)
    {
        if (held.isVoid())
            return std::nullopt;

        const auto colour = juce::Colour::fromString (held.toString());
        return colour.isTransparent() ? std::nullopt : std::optional<juce::uint32> (colour.getARGB());
    }

    // Whether a colour's text is what juce::Colour::toString() writes of a colour (its ARGB in lowercase hexadecimal digits,
    // without leading zeros), without a text made of it.
    bool isColourAsWritten (const juce::var& there, juce::uint32 argb)
    {
        if (! there.isString())
            return false;

        const auto text = there.toString();
        std::uint64_t value = 0;
        int digits = 0;

        for (auto* c = text.toRawUTF8(); *c != 0; ++c, ++digits)
        {
            const auto digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : -1;

            if (digit < 0 || digits >= 8 || (digits == 0 && digit == 0 && c[1] != 0))
                return false;

            value = value * 16 + (std::uint64_t) digit;
        }

        return digits > 0 && value == argb;
    }

    // A kind by its word (Schema.h: trackKind): a folder, a return, else a track.
    Kind readKindWord (const juce::var& word)
    {
        const auto text = word.toString();
        return text == schema::trackIsFolder ? Kind::folder : text == schema::trackIsReturn ? Kind::returnTrack : Kind::track;
    }

    // A kind as the song holds it: one of its words, or as the loader makes another (Project.h: makeConsistent(),
    // keepPlaces()): a folder where it holds a TRACK, else a track.
    Kind readHeldKind (const juce::ValueTree& track)
    {
        const auto text = track[schema::trackKind].toString();

        if (text == schema::trackIsFolder)
            return Kind::folder;

        if (text == schema::trackIsReturn)
            return Kind::returnTrack;

        if (text == schema::trackIsTrack)
            return Kind::track;

        return track.getChildWithName (schema::TRACK).isValid() ? Kind::folder : Kind::track;
    }

    // The words as values, made once: a text made for each track the loader looks at would allocate.
    const juce::var& getKindWord (Kind kind)
    {
        static const juce::var track (schema::trackIsTrack), folder (schema::trackIsFolder), returnTrack (schema::trackIsReturn);

        switch (kind)
        {
            case Kind::folder:
                return folder;
            case Kind::returnTrack:
                return returnTrack;
            case Kind::track:
            case Kind::master:
                break;
        }

        return track;
    }

    Monitoring readMonitoringWord (const juce::var& word)
    {
        const auto text = word.toString();
        return text == schema::monitorAlways  ? Monitoring::always
               : text == schema::monitorNever ? Monitoring::never
                                              : Monitoring::whileArmed;
    }

    const juce::var& getMonitoringWord (Monitoring monitoring)
    {
        static const juce::var always (schema::monitorAlways), never (schema::monitorNever), none;

        switch (monitoring)
        {
            case Monitoring::always:
                return always;
            case Monitoring::never:
                return never;
            case Monitoring::whileArmed:
                break;
        }

        return none;
    }

    // A scale's saved name (getSavedScaleName()) as a value, made once for each scale.
    const juce::var& getScaleWord (Scale scale)
    {
        static const auto words = []
        {
            std::vector<juce::var> made;

            for (const auto each : getKeyScales())
            {
                if ((size_t) each >= made.size())
                    made.resize ((size_t) each + 1);

                made[(size_t) each] = getSavedScaleName (each);
            }

            return made;
        }();

        static const juce::var none;
        return (size_t) scale < words.size() ? words[(size_t) scale] : none;
    }

    // A key as Phazite's code always read one (model/notes/Notes.cpp: readKey()): where it has a root, the root as an int
    // truncates it, within 0 to 11, and its scale by its saved name, else major.
    std::optional<Key> readKeyOf (const juce::ValueTree& element)
    {
        if (! element.hasProperty (schema::keyRoot))
            return std::nullopt;

        Key key;
        key.root = juce::jlimit (0, 11, toWhole<int> (element[schema::keyRoot]));
        key.scale = findScale (element[schema::keyScale].toString()).value_or (Scale::major);
        return key;
    }

    // The same as the song holds it: a root that is a finite number, within its range, made whole; its scale one of its
    // words, else major.
    std::optional<Key> readHeldKeyOf (const juce::ValueTree& element)
    {
        const auto root = readHeldWhole (element[schema::keyRoot], schema::keyRoot);

        if (! root.has_value())
            return std::nullopt;

        Key key;
        key.root = *root;
        key.scale = findScale (element[schema::keyScale].toString()).value_or (Scale::major);
        return key;
    }

    //==============================================================================
    // What both readers read of an element: whether it is one, and its ID as each reads one (the held one counts on from it).
    std::optional<TrackValue> readLenient (const juce::ValueTree& element)
    {
        if (! isTrackOrMaster (element))
            return std::nullopt;

        auto value = element.hasType (schema::MASTER) ? TrackValue::makeMaster() : TrackValue();
        value.id = TrackID::fromRaw (castID (element[schema::trackID]));

        if (! value.isMaster())
        {
            value.kind = readKindWord (element[schema::trackKind]);

            if (const auto parent = element.getParent(); parent.hasType (schema::TRACK))
                value.parent = TrackID::fromRaw (castID (parent[schema::trackID]));
        }

        if (const auto* name = element.getPropertyPointer (schema::trackName))
            value.name = name->toString();

        value.colour = readColourOf (element[schema::trackColour]);
        value.bus = (bool) element[schema::folderBus];
        value.volume = readCastFloat (element[schema::channelVolume], TrackValue::defaultVolume (value.kind));
        value.pan = readCastFloat (element[schema::channelPan], 0.0f);
        value.fader = DeviceID::fromRaw (castID (element[schema::channelFader]));
        value.vca = DeviceID::fromRaw (castID (element[schema::channelVCA]));
        value.key = readKeyOf (element);
        value.input = element[schema::trackInput].toString();
        value.monitoring = readMonitoringWord (element[schema::trackMonitoring]);
        value.padDevice = DeviceID::fromRaw (castID (element[schema::trackPadDevice]));

        if (element.hasProperty (schema::trackPad))
            value.pad = toWhole<int> (element[schema::trackPad]);

        return value;
    }

    std::optional<TrackValue> readHeld (const juce::ValueTree& element)
    {
        if (! isTrackOrMaster (element))
            return std::nullopt;

        const auto id = readHeldIDOf (element[schema::trackID]);

        if (id == 0)
            return std::nullopt;

        if (element.hasType (schema::MASTER))
        {
            auto master = TrackValue::makeMaster();
            master.id = TrackID::fromRaw (id);
            master.fader = DeviceID::fromRaw (readHeldIDOf (element[schema::channelFader]));
            master.vca = DeviceID::fromRaw (readHeldIDOf (element[schema::channelVCA]));
            master.volume = (float) readHeldNumber (element[schema::channelVolume], schema::channelVolume,
                                                    (double) TrackValue::defaultVolume (Kind::master));
            master.pan = (float) readHeldNumber (element[schema::channelPan], schema::channelPan, 0.0);
            return master;
        }

        TrackValue value;
        value.id = TrackID::fromRaw (id);
        value.kind = readHeldKind (element);

        if (const auto parent = element.getParent(); parent.hasType (schema::TRACK))
            value.parent = TrackID::fromRaw (readHeldIDOf (parent[schema::trackID]));

        if (const auto* name = element.getPropertyPointer (schema::trackName))
            value.name = name->toString();

        value.colour = readColourOf (element[schema::trackColour]);
        value.bus = (bool) element[schema::folderBus];
        value.volume =
            (float) readHeldNumber (element[schema::channelVolume], schema::channelVolume, (double) TrackValue::defaultVolume (value.kind));
        value.pan = (float) readHeldNumber (element[schema::channelPan], schema::channelPan, 0.0);
        value.fader = DeviceID::fromRaw (readHeldIDOf (element[schema::channelFader]));
        value.vca = DeviceID::fromRaw (readHeldIDOf (element[schema::channelVCA]));
        value.key = readHeldKeyOf (element);
        value.input = element[schema::trackInput].toString();
        value.monitoring = readMonitoringWord (element[schema::trackMonitoring]);
        value.padDevice = DeviceID::fromRaw (readID (element[schema::trackPadDevice]));
        value.pad = readHeldWhole (element[schema::trackPad], schema::trackPad);
        return value;
    }

    //==============================================================================
    // Each of an element's own properties as Phazite's writers write it (writeTrackElement(): which, and when), given to `each`
    // as its name and its value, in the writer's order: the one list of them, which the writer and the loader's rule
    // (keepTrackAsHeld()) both go by. Nothing allocated (a text shares its own). A colour is given as its ARGB value, which
    // the writer writes as its text (juce::Colour::toString()) and the rule compares with the text it holds.
    template <typename Each>
    void forEachWrittenProperty (const TrackValue& value, Each&& each)
    {
        each (schema::trackID.getName(), juce::var ((juce::int64) value.id.getRaw()));

        if (! value.isMaster())
            each (schema::trackKind.getName(), getKindWord (value.kind));

        if (value.name.has_value() && ! value.isMaster())
            each (schema::trackName.getName(), juce::var (*value.name));

        if (value.colour.has_value() && ! value.isMaster())
            each (schema::trackColour.getName(), juce::var ((juce::int64) *value.colour));

        if (value.bus && ! value.isMaster())
            each (schema::folderBus.getName(), juce::var (true));

        if (! juce::exactlyEqual (value.volume, TrackValue::defaultVolume (value.kind)))
            each (schema::channelVolume.getName(), juce::var (value.volume));

        if (! juce::exactlyEqual (value.pan, 0.0f))
            each (schema::channelPan.getName(), juce::var (value.pan));

        if (value.fader.isValid())
            each (schema::channelFader.getName(), juce::var ((juce::int64) value.fader.getRaw()));

        if (value.vca.isValid())
            each (schema::channelVCA.getName(), juce::var ((juce::int64) value.vca.getRaw()));

        if (value.isMaster())
            return;

        if (value.key.has_value())
        {
            each (schema::keyRoot.getName(), juce::var (value.key->root));
            each (schema::keyScale.getName(), getScaleWord (value.key->scale));
        }

        if (value.input.isNotEmpty())
            each (schema::trackInput.getName(), juce::var (value.input));

        if (value.monitoring != Monitoring::whileArmed)
            each (schema::trackMonitoring.getName(), getMonitoringWord (value.monitoring));

        if (value.padDevice.isValid())
            each (schema::trackPadDevice.getName(), juce::var ((juce::int64) value.padDevice.getRaw()));

        if (value.pad.has_value())
            each (schema::trackPad.getName(), juce::var (*value.pad));
    }

    // Whether the value covers a property of a TRACK, or of the MASTER: what the writer may write of it
    // (forEachWrittenProperty()), which the loader's rule makes the element hold as the writer writes it, and nothing else.
    bool isCovered (bool master, const juce::Identifier& property)
    {
        const auto isAmong = [&property] (std::initializer_list<const juce::Identifier*> list)
        {
            for (const auto* each : list)
                if (*each == property)
                    return true;

            return false;
        };

        if (master)
            return isAmong ({ &schema::trackID.getName(), &schema::channelFader.getName(), &schema::channelVCA.getName(),
                              &schema::channelVolume.getName(), &schema::channelPan.getName() });

        return isAmong ({ &schema::trackID.getName(), &schema::trackKind.getName(), &schema::trackName.getName(),
                          &schema::trackColour.getName(), &schema::folderBus.getName(), &schema::channelVolume.getName(),
                          &schema::channelPan.getName(), &schema::channelFader.getName(), &schema::channelVCA.getName(),
                          &schema::keyRoot.getName(), &schema::keyScale.getName(), &schema::trackInput.getName(),
                          &schema::trackMonitoring.getName(), &schema::trackPadDevice.getName(), &schema::trackPad.getName() });
    }

    // Whether an element's property holds what the writer writes of it (a colour by its ARGB value: forEachWrittenProperty()).
    bool isAsWritten (const juce::var* there, const juce::Identifier& property, const juce::var& written)
    {
        if (there == nullptr)
            return false;

        if (property == schema::trackColour.getName())
            return isColourAsWritten (*there, (juce::uint32) (juce::int64) written);

        return isSameValue (*there, written);
    }

    // What the writer writes into an element of a property (a colour as its text).
    juce::var toWritten (const juce::Identifier& property, const juce::var& written)
    {
        if (property == schema::trackColour.getName())
            return juce::Colour ((juce::uint32) (juce::int64) written).toString();

        return written;
    }

    // Whether an element's own properties the value covers are those the writer writes of what it holds, and no other.
    // Nothing allocated.
    bool holdsPropertiesAsWritten (const juce::ValueTree& element, const TrackValue& held)
    {
        auto written = 0;
        auto same = true;

        forEachWrittenProperty (held,
                                [&element, &written, &same] (const juce::Identifier& property, const juce::var& value)
                                {
                                    same = same && isAsWritten (element.getPropertyPointer (property), property, value);
                                    ++written;
                                });

        if (! same)
            return false;

        auto covered = 0;

        for (int i = 0; i < element.getNumProperties(); ++i)
            if (isCovered (held.isMaster(), element.getPropertyName (i)))
                ++covered;

        return covered == written;
    }

    // The element's own properties the value covers made those the writer writes of what it holds: each set where it is
    // another text, each the writer leaves out taken away; the others left as they are. Whether any changed.
    bool keepPropertiesAsWritten (juce::ValueTree& element, const TrackValue& held)
    {
        auto changed = false;
        std::vector<juce::Identifier> written;

        forEachWrittenProperty (held,
                                [&element, &written, &changed] (const juce::Identifier& property, const juce::var& value)
                                {
                                    written.push_back (property);

                                    if (! isAsWritten (element.getPropertyPointer (property), property, value))
                                    {
                                        element.setProperty (property, toWritten (property, value), nullptr);
                                        changed = true;
                                    }
                                });

        for (auto i = element.getNumProperties(); --i >= 0;)
        {
            const auto property = element.getPropertyName (i);

            if (isCovered (held.isMaster(), property) && std::find (written.begin(), written.end(), property) == written.end())
            {
                element.removeProperty (property, nullptr);
                changed = true;
            }
        }

        return changed;
    }

    // Every property the tracks' three values cover (this value's, the switches' and the view's: TrackSwitchesValue.h,
    // TracksOutside.h), by its description.
    const std::array<const schema::PropertyDescription*, 25>& getAreaProperties()
    {
        static const std::array<const schema::PropertyDescription*, 25> properties {
            &schema::trackID,        &schema::trackKind,   &schema::trackName,       &schema::trackColour,         &schema::folderBus,
            &schema::channelVolume,  &schema::channelPan,  &schema::channelFader,    &schema::channelVCA,          &schema::keyRoot,
            &schema::keyScale,       &schema::trackInput,  &schema::trackMonitoring, &schema::trackPadDevice,      &schema::trackPad,
            &schema::trackMute,      &schema::trackSolo,   &schema::trackFrozen,     &schema::trackAutomationMode, &schema::trackHeight,
            &schema::trackCollapsed, &schema::trackPinned, &schema::drumMode,        &schema::drumOrder,           &schema::drumHidden
        };

        return properties;
    }

    // The properties the area covers that aren't described on the element's type taken away (a MASTER's name, key, input,
    // pinned or drum rows, which the lenient readers read and the held ones don't; every one the area covers is described on
    // TRACK). Whether any was. Nothing allocated where none is there.
    bool keepOnlyDescribed (juce::ValueTree& element)
    {
        auto changed = false;

        for (const auto* property : getAreaProperties())
        {
            if (! property->isOn (element.getType()) && element.hasProperty (property->getName()))
            {
                element.removeProperty (property->getName(), nullptr);
                changed = true;
            }
        }

        return changed;
    }

    //==============================================================================
    // A list with an ID taken out (the list gone where it is left empty), and with one put at a place (from 0, or past the
    // end for after the others).
    Lists withoutInList (Lists lists, TrackID owner, TrackID id)
    {
        if (const auto* list = lists.find (owner.getRaw()))
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                if ((*list)[i] == id)
                {
                    const auto rest = list->erase (i);
                    return rest.empty() ? lists.erase (owner.getRaw()) : lists.set (owner.getRaw(), rest);
                }
            }
        }

        return lists;
    }

    Lists withInList (const Lists& lists, TrackID owner, TrackID id, int index)
    {
        const auto* found = lists.find (owner.getRaw());
        auto list = found != nullptr ? *found : IDList();
        list = juce::isPositiveAndNotGreaterThan (index, (int) list.size()) ? list.insert ((size_t) index, id) : list.push_back (id);
        return lists.set (owner.getRaw(), std::move (list));
    }

    // Whether a track is the folder given or in it, through its folders up (as far as there are tracks, so that a value whose
    // folders go round ends).
    bool isInOrIs (const SongTracks& tracks, TrackID track, TrackID folder)
    {
        auto steps = tracks.tracks.size() + 1;

        for (auto at = track; at.isValid() && steps-- > 0;)
        {
            if (at == folder)
                return true;

            const auto* value = tracks.tracks.find (at.getRaw());
            at = value != nullptr ? value->parent : TrackID();
        }

        return false;
    }

    // Each TRACK under an element (the track list, a folder) in its order, a folder before the tracks in it, as the song's
    // index orders them (SongIndex.h: getTracks(): only a folder's TRACKs are looked through).
    void forEachTrackElement (const juce::ValueTree& parent, const std::function<bool (const juce::ValueTree&)>& visit)
    {
        for (const auto& child : parent)
            if (child.hasType (schema::TRACK) && visit (child) && readKindWord (child[schema::trackKind]) == Kind::folder)
                forEachTrackElement (child, visit);
    }
} // namespace

//==============================================================================
float TrackValue::defaultVolume (Kind kind) noexcept
{
    return kind == Kind::master ? -3.0f : 0.0f;
}

TrackValue TrackValue::makeMaster()
{
    TrackValue master;
    master.kind = Kind::master;
    master.volume = defaultVolume (Kind::master);
    return master;
}

bool TrackValue::operator== (const TrackValue& other) const
{
    return id == other.id && kind == other.kind && parent == other.parent && name == other.name && colour == other.colour
           && bus == other.bus && juce::exactlyEqual (volume, other.volume) && juce::exactlyEqual (pan, other.pan) && fader == other.fader
           && vca == other.vca && key == other.key && input == other.input && monitoring == other.monitoring && padDevice == other.padDevice
           && pad == other.pad;
}

const TrackValue* SongTracks::find (TrackID id) const noexcept
{
    if (! id.isValid())
        return nullptr;

    if (id == master.id)
        return &master;

    return tracks.find (id.getRaw());
}

const immer::flex_vector<TrackID>* SongTracks::findList (TrackID folder) const noexcept
{
    return lists.find (folder.getRaw());
}

bool SongTracks::isSameAs (const SongTracks& other) const noexcept
{
    return tracks.identity() == other.tracks.identity() && lists.identity() == other.lists.identity() && master == other.master
           && masterPutIn == other.masterPutIn;
}

bool SongTracks::operator== (const SongTracks& other) const
{
    return isSameAs (other) || (tracks == other.tracks && lists == other.lists && master == other.master);
}

SongTracks withTrack (const SongTracks& tracks, TrackValue value, int index)
{
    if (value.isMaster())
    {
        auto next = tracks;
        value.parent = {};
        next.master = std::move (value);
        return next;
    }

    if (! value.id.isValid() || value.id == tracks.master.id)
        return tracks;

    // A folder stays one while it holds tracks: their list names it.
    if (const auto* was = tracks.tracks.find (value.id.getRaw());
        was != nullptr && was->isFolder() && ! value.isFolder() && tracks.lists.find (value.id.getRaw()) != nullptr)
        return tracks;

    // Its folder one the value holds as a folder, and not itself nor in it.
    if (value.parent.isValid())
    {
        const auto* folder = tracks.tracks.find (value.parent.getRaw());

        if (folder == nullptr || ! folder->isFolder() || isInOrIs (tracks, value.parent, value.id))
            return tracks;
    }

    const auto id = value.id;
    const auto parent = value.parent;
    const auto* old = tracks.tracks.find (id.getRaw());
    const auto oldParent = old != nullptr ? std::optional (old->parent) : std::nullopt;
    auto next = tracks;
    next.tracks = next.tracks.set (id.getRaw(), std::move (value));

    // One whose folder stays keeps its place in its list.
    if (oldParent == parent)
        return next;

    if (oldParent.has_value())
        next.lists = withoutInList (next.lists, *oldParent, id);

    next.lists = withInList (next.lists, parent, id, index);
    return next;
}

SongTracks withoutTrack (const SongTracks& tracks, TrackID id)
{
    const auto* old = tracks.tracks.find (id.getRaw());

    if (old == nullptr)
        return tracks;

    auto next = tracks;

    // The tracks in it first, with their lists.
    const std::function<void (TrackID)> erase = [&next, &erase] (TrackID each)
    {
        if (const auto* list = next.lists.find (each.getRaw()))
        {
            const auto inside = *list;
            next.lists = next.lists.erase (each.getRaw());

            for (const auto track : inside)
                erase (track);
        }

        next.tracks = next.tracks.erase (each.getRaw());
    };

    const auto parent = old->parent;
    erase (id);
    next.lists = withoutInList (next.lists, parent, id);
    return next;
}

SongTracks withListOrder (const SongTracks& tracks, TrackID folder, immer::flex_vector<TrackID> order)
{
    const auto* list = tracks.findList (folder);

    if (list == nullptr || list->size() != order.size())
        return tracks;

    // The same IDs, each once: both sorted, in a vector each (two allocations, however long the list: a track moved in 200
    // allocates as in 20).
    std::vector<std::uint64_t> held, given;
    held.reserve (list->size());
    given.reserve (order.size());

    for (const auto id : *list)
        held.push_back (id.getRaw());

    for (const auto id : order)
        given.push_back (id.getRaw());

    std::sort (held.begin(), held.end());
    std::sort (given.begin(), given.end());

    if (held != given || std::adjacent_find (given.begin(), given.end()) != given.end())
        return tracks;

    auto next = tracks;
    next.lists = next.lists.set (folder.getRaw(), std::move (order));
    return next;
}

bool holdsItsLists (const SongTracks& tracks)
{
    std::set<std::uint64_t> listed, ids;

    // No ID twice among the tracks', the faders' and the VCAs'.
    const auto holdOnce = [&ids] (std::uint64_t id) { return id == 0 || ids.insert (id).second; };

    if (! tracks.master.isMaster() || tracks.master.parent.isValid() || ! holdOnce (tracks.master.id.getRaw())
        || ! holdOnce (tracks.master.fader.getRaw()) || ! holdOnce (tracks.master.vca.getRaw()))
        return false;

    for (const auto& [key, value] : tracks.tracks)
    {
        if (key != value.id.getRaw() || ! value.id.isValid() || value.isMaster() || ! holdOnce (value.id.getRaw())
            || ! holdOnce (value.fader.getRaw()) || ! holdOnce (value.vca.getRaw()))
            return false;

        if (value.parent.isValid())
        {
            const auto* folder = tracks.tracks.find (value.parent.getRaw());

            if (folder == nullptr || ! folder->isFolder() || isInOrIs (tracks, value.parent, value.id))
                return false;
        }
    }

    for (const auto& [owner, list] : tracks.lists)
    {
        if (list.empty())
            return false;

        if (owner != 0)
            if (const auto* folder = tracks.tracks.find (owner); folder == nullptr || ! folder->isFolder())
                return false;

        for (const auto& id : list)
        {
            const auto* value = tracks.tracks.find (id.getRaw());

            if (value == nullptr || value->parent.getRaw() != owner || ! listed.insert (id.getRaw()).second)
                return false;
        }
    }

    return listed.size() == tracks.tracks.size();
}

//==============================================================================
TrackOrder::Place TrackOrder::getPlace (TrackID id) const
{
    const auto found = std::lower_bound (places.begin(), places.end(), id.getRaw(),
                                         [] (const auto& place, std::uint64_t raw) { return place.first < raw; });
    return found != places.end() && found->first == id.getRaw() ? found->second : Place();
}

TrackOrder makeTrackOrder (const SongTracks& tracks)
{
    TrackOrder order;
    order.order.reserve (tracks.tracks.size());
    order.places.reserve (tracks.tracks.size());
    int number = 0;

    const std::function<void (TrackID)> add = [&tracks, &order, &number, &add] (TrackID owner)
    {
        const auto* list = tracks.findList (owner);

        if (list == nullptr)
            return;

        for (const auto id : *list)
        {
            const auto* value = tracks.tracks.find (id.getRaw());
            const auto folder = value != nullptr && value->isFolder();
            number += folder ? 0 : 1;
            TrackOrder::Place place;
            place.index = (int) order.order.size();
            place.number = folder ? 0 : number;
            order.places.emplace_back (id.getRaw(), place);
            order.order.push_back (id);

            if (folder)
                add (id);
        }
    };

    add ({});
    std::stable_sort (order.places.begin(), order.places.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    return order;
}

//==============================================================================
std::optional<TrackValue> readTrackValue (const juce::ValueTree& element)
{
    auto value = readLenient (element);

    if (value.has_value())
        tracksRead.fetch_add (1, std::memory_order_relaxed);

    return value;
}

std::optional<TrackValue> readHeldTrack (const juce::ValueTree& element)
{
    auto value = readHeld (element);

    if (value.has_value())
        tracksRead.fetch_add (1, std::memory_order_relaxed);

    return value;
}

juce::ValueTree writeTrackElement (const TrackValue& value)
{
    juce::ValueTree element (value.isMaster() ? schema::MASTER : schema::TRACK);

    forEachWrittenProperty (value, [&element] (const juce::Identifier& property, const juce::var& written)
                            { element.setProperty (property, toWritten (property, written), nullptr); });

    return element;
}

void writeMasterInto (juce::ValueTree& master, const TrackValue& value)
{
    if (master.hasType (schema::MASTER) && value.isMaster() && ! holdsPropertiesAsWritten (master, value))
        keepPropertiesAsWritten (master, value);
}

SongTracks readTracksValue (const juce::ValueTree& song)
{
    SongTracks tracks;

    forEachTrackElement (song.getChildWithName (schema::TRACKS),
                         [&tracks] (const juce::ValueTree& element)
                         {
                             // One of an ID read already, or none, with the tracks in it left out.
                             auto value = readTrackValue (element);

                             if (! value.has_value() || ! value->id.isValid() || tracks.tracks.find (value->id.getRaw()) != nullptr)
                                 return false;

                             tracks = withTrack (tracks, std::move (*value));
                             return true;
                         });

    if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
        if (auto value = readTrackValue (master))
            tracks.master = std::move (*value);

    return tracks;
}

bool setTrackProperty (TrackValue& value, const juce::Identifier& property, const juce::var& held)
{
    const auto master = value.isMaster();

    if (! isCovered (master, property))
        return false;

    if (property == schema::trackID.getName())
        value.id = TrackID::fromRaw (readHeldIDOf (held));
    else if (property == schema::trackKind.getName())
        value.kind = held.isVoid() ? Kind::track : readKindWord (held);
    else if (property == schema::trackName.getName())
        value.name = held.isVoid() ? std::nullopt : std::optional (held.toString());
    else if (property == schema::trackColour.getName())
        value.colour = readColourOf (held);
    else if (property == schema::folderBus.getName())
        value.bus = (bool) held;
    else if (property == schema::channelVolume.getName())
        value.volume = (float) readHeldNumber (held, schema::channelVolume, (double) TrackValue::defaultVolume (value.kind));
    else if (property == schema::channelPan.getName())
        value.pan = (float) readHeldNumber (held, schema::channelPan, 0.0);
    else if (property == schema::channelFader.getName())
        value.fader = DeviceID::fromRaw (readHeldIDOf (held));
    else if (property == schema::channelVCA.getName())
        value.vca = DeviceID::fromRaw (readHeldIDOf (held));
    else if (property == schema::keyRoot.getName())
    {
        // A root alone keeps the scale held, major for none.
        if (const auto root = readHeldWhole (held, schema::keyRoot))
        {
            Key key;
            key.root = *root;
            key.scale = value.key.has_value() ? value.key->scale : Scale::major;
            value.key = key;
        }
        else
        {
            value.key.reset();
        }
    }
    else if (property == schema::keyScale.getName())
    {
        // A scale alone is no key: nothing changes without a root.
        if (value.key.has_value())
            value.key->scale = findScale (held.toString()).value_or (Scale::major);
    }
    else if (property == schema::trackInput.getName())
        value.input = held.toString();
    else if (property == schema::trackMonitoring.getName())
        value.monitoring = readMonitoringWord (held);
    else if (property == schema::trackPadDevice.getName())
        value.padDevice = DeviceID::fromRaw (readID (held));
    else if (property == schema::trackPad.getName())
        value.pad = readHeldWhole (held, schema::trackPad);

    return true;
}

bool keepTrackAsHeld (juce::ValueTree element)
{
    if (! isTrackOrMaster (element))
        return false;

    // What the area covers but the element's type doesn't describe taken away first, so that both readers read none of it.
    const auto dropped = keepOnlyDescribed (element);

    // Read without a count or an allocation, and rewritten only where a property isn't as the writer writes it.
    const auto held = readHeld (element);

    if (! held.has_value() || holdsPropertiesAsWritten (element, *held))
        return dropped;

    tracksRead.fetch_add (1, std::memory_order_relaxed);
    return keepPropertiesAsWritten (element, *held) || dropped;
}

std::uint64_t findLargestIDIn (const SongTracks& tracks)
{
    std::uint64_t largest = 0;
    forEachIDHeld (tracks, [&largest] (std::uint64_t id) { largest = countHeldID (largest, id); });
    return largest;
}

void forEachIDHeld (const SongTracks& tracks, const std::function<void (std::uint64_t)>& each)
{
    const auto visit = [&each] (const TrackValue& value)
    {
        for (const auto id : { value.id.getRaw(), value.fader.getRaw(), value.vca.getRaw() })
            if (id != 0)
                each (id);
    };

    for (const auto& [key, value] : tracks.tracks)
        visit (value);

    visit (tracks.master);
}

void forEachIDNamed (const SongTracks& tracks, const std::function<void (std::uint64_t)>& each)
{
    for (const auto& [key, value] : tracks.tracks)
        if (value.padDevice.isValid())
            each (value.padDevice.getRaw());
}

//==============================================================================
TrackID readTrackElementID (const juce::ValueTree& element)
{
    return isTrackOrMaster (element) ? TrackID::fromRaw (castID (element[schema::trackID])) : TrackID();
}

std::optional<TrackValue::Kind> readTrackKind (const juce::ValueTree& element)
{
    if (element.hasType (schema::MASTER))
        return Kind::master;

    return element.hasType (schema::TRACK) ? std::optional (readKindWord (element[schema::trackKind])) : std::nullopt;
}

std::uint64_t countTracksReadAsValues() noexcept
{
    return tracksRead.load (std::memory_order_relaxed);
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <functional>
#include <optional>
#include <utility>

namespace phazite
{
namespace
{
    using Mode = SavedAutomationMode;

    // A mode by its word (Schema.h: trackAutomationMode), none (read) for none and any other word.
    std::optional<Mode> readModeWord (const juce::var& word)
    {
        const auto text = word.toString();

        if (text == schema::automationTouch)
            return Mode::touch;

        if (text == schema::automationLatch)
            return Mode::latch;

        if (text == schema::automationWrite)
            return Mode::write;

        return std::nullopt;
    }

    // A mode's word as a value, made once.
    const juce::var& getModeWord (Mode mode)
    {
        static const juce::var touch (schema::automationTouch), latch (schema::automationLatch), write (schema::automationWrite);

        switch (mode)
        {
            case Mode::latch:
                return latch;
            case Mode::write:
                return write;
            case Mode::touch:
                break;
        }

        return touch;
    }

    // An element's key among the switches: a TRACK's ID as a cast reads it (as Phazite's code always read it), or as the
    // song holds it (one it can count on from, else none); the MASTER's 0; none for any other element.
    std::optional<std::uint64_t> findKey (const juce::ValueTree& element, bool held)
    {
        if (element.hasType (schema::MASTER))
            return (std::uint64_t) 0;

        if (! element.hasType (schema::TRACK))
            return std::nullopt;

        const auto id = held ? readID (element[schema::trackID]) : (std::uint64_t) toWhole<juce::int64> (element[schema::trackID]);

        if (id == 0 || (held && id >= schema::maxID))
            return std::nullopt;

        return id;
    }

    immer::map<std::uint64_t, bool> withSwitch (const immer::map<std::uint64_t, bool>& switches, std::uint64_t key, bool on)
    {
        return on ? switches.set (key, true) : switches.erase (key);
    }

    void readInto (const juce::ValueTree& element, TrackSwitches& switches, bool held)
    {
        const auto key = findKey (element, held);

        if (! key.has_value())
            return;

        if (element.hasType (schema::TRACK))
        {
            switches.mutes = withSwitch (switches.mutes, *key, readTrackMute (element));
            switches.solos = withSwitch (switches.solos, *key, readTrackSolo (element));
            switches.frozen = withSwitch (switches.frozen, *key, readTrackFrozen (element));
        }

        if (const auto mode = readSavedAutomationMode (element))
            switches.modes = switches.modes.set (*key, *mode);
        else
            switches.modes = switches.modes.erase (*key);
    }

    // Each of an element's switches as Phazite's writers write it (writeSwitchesInto()), given to `each` as its name and its
    // value: the one list of them, which the writer and the loader's rule (keepSwitchesAsHeld()) both go by.
    template <typename Each>
    void forEachWrittenSwitch (const juce::ValueTree& element, const TrackSwitches& switches, std::uint64_t key, Each&& each)
    {
        if (element.hasType (schema::TRACK))
        {
            if (switches.mutes.find (key) != nullptr)
                each (schema::trackMute.getName(), juce::var (true));

            if (switches.solos.find (key) != nullptr)
                each (schema::trackSolo.getName(), juce::var (true));

            if (switches.frozen.find (key) != nullptr)
                each (schema::trackFrozen.getName(), juce::var (true));
        }

        if (const auto* mode = switches.modes.find (key))
            each (schema::trackAutomationMode.getName(), getModeWord (*mode));
    }

    // The switches of a TRACK, or of the MASTER (its mode alone, as Schema.h describes it).
    bool isSwitch (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (property == schema::trackAutomationMode.getName())
            return true;

        return element.hasType (schema::TRACK)
               && (property == schema::trackMute.getName() || property == schema::trackSolo.getName()
                   || property == schema::trackFrozen.getName());
    }

    // Each TRACK under an element (the track list, a folder) in its order, a folder before the tracks in it.
    void forEachTrackElement (const juce::ValueTree& parent, const std::function<void (const juce::ValueTree&)>& visit)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
            {
                visit (child);

                if (child[schema::trackKind].toString() == schema::trackIsFolder)
                    forEachTrackElement (child, visit);
            }
        }
    }
} // namespace

bool TrackSwitches::isSameAs (const TrackSwitches& other) const noexcept
{
    return mutes.identity() == other.mutes.identity() && solos.identity() == other.solos.identity()
           && frozen.identity() == other.frozen.identity() && modes.identity() == other.modes.identity();
}

bool TrackSwitches::operator== (const TrackSwitches& other) const
{
    return isSameAs (other) || (mutes == other.mutes && solos == other.solos && frozen == other.frozen && modes == other.modes);
}

TrackSwitchesChanges findChanges (const TrackSwitches& before, const TrackSwitches& after)
{
    TrackSwitchesChanges changes;
    changes.mutes = findMapChanges (before.mutes, after.mutes);
    changes.solos = findMapChanges (before.solos, after.solos);
    changes.frozen = findMapChanges (before.frozen, after.frozen);
    changes.modes = findMapChanges (before.modes, after.modes);
    return changes;
}

TrackSwitches withChanges (TrackSwitches now, const TrackSwitchesChanges& changes, ChangeDirection direction)
{
    now.mutes = withMapChanges (std::move (now.mutes), changes.mutes, direction);
    now.solos = withMapChanges (std::move (now.solos), changes.solos, direction);
    now.frozen = withMapChanges (std::move (now.frozen), changes.frozen, direction);
    now.modes = withMapChanges (std::move (now.modes), changes.modes, direction);
    return now;
}

namespace
{
    immer::map<std::uint64_t, bool> withSwitchSet (const immer::map<std::uint64_t, bool>& switches, std::uint64_t key, bool on)
    {
        if ((switches.find (key) != nullptr) == on)
            return switches;

        return withSwitch (switches, key, on);
    }

    template <typename Map>
    bool holdSameEntry (const Map& a, const Map& b, std::uint64_t key)
    {
        const auto* first = a.find (key);
        const auto* second = b.find (key);
        return first == nullptr || second == nullptr ? first == second : *first == *second;
    }
} // namespace

TrackSwitches withMute (const TrackSwitches& switches, std::uint64_t key, bool on)
{
    auto next = switches;
    next.mutes = withSwitchSet (switches.mutes, key, on);
    return next.mutes.identity() == switches.mutes.identity() ? switches : next;
}

TrackSwitches withSolo (const TrackSwitches& switches, std::uint64_t key, bool on)
{
    auto next = switches;
    next.solos = withSwitchSet (switches.solos, key, on);
    return next.solos.identity() == switches.solos.identity() ? switches : next;
}

TrackSwitches withFrozen (const TrackSwitches& switches, std::uint64_t key, bool on)
{
    auto next = switches;
    next.frozen = withSwitchSet (switches.frozen, key, on);
    return next.frozen.identity() == switches.frozen.identity() ? switches : next;
}

TrackSwitches withAutomationMode (const TrackSwitches& switches, std::uint64_t key, std::optional<SavedAutomationMode> mode)
{
    const auto* held = switches.modes.find (key);

    if (held == nullptr ? ! mode.has_value() : mode.has_value() && *held == *mode)
        return switches;

    auto next = switches;
    next.modes = mode.has_value() ? switches.modes.set (key, *mode) : switches.modes.erase (key);
    return next;
}

bool holdSameSwitchesOf (const TrackSwitches& a, const TrackSwitches& b, std::uint64_t key)
{
    return holdSameEntry (a.mutes, b.mutes, key) && holdSameEntry (a.solos, b.solos, key) && holdSameEntry (a.frozen, b.frozen, key)
           && holdSameEntry (a.modes, b.modes, key);
}

bool holdsSwitchesOf (const TrackSwitches& switches, std::uint64_t key)
{
    return switches.mutes.find (key) != nullptr || switches.solos.find (key) != nullptr || switches.frozen.find (key) != nullptr
           || switches.modes.find (key) != nullptr;
}

TrackSwitches withoutIDsFrom (const TrackSwitches& switches, std::uint64_t first)
{
    jassert (first > 0); // the master's mode, under 0, stays
    auto kept = switches;
    kept.mutes = withoutIDsFrom (switches.mutes, first);
    kept.solos = withoutIDsFrom (switches.solos, first);
    kept.frozen = withoutIDsFrom (switches.frozen, first);
    kept.modes = withoutIDsFrom (switches.modes, first);
    return kept;
}

void readSwitchesOf (const juce::ValueTree& element, TrackSwitches& switches)
{
    readInto (element, switches, false);
}

void readHeldSwitchesOf (const juce::ValueTree& element, TrackSwitches& switches)
{
    readInto (element, switches, true);
}

void writeSwitchesInto (juce::ValueTree& element, const TrackSwitches& switches)
{
    if (const auto key = findKey (element, true))
        forEachWrittenSwitch (element, switches, *key, [&element] (const juce::Identifier& property, const juce::var& value)
                              { element.setProperty (property, value, nullptr); });
}

bool keepSwitchesAsHeld (juce::ValueTree element)
{
    const auto key = findKey (element, true);

    if (! key.has_value())
        return false;

    // Each switch the element holds looked at as it is, no value made (nothing allocated): a switch as true, as the writer
    // writes one that is on (an off one is none), a mode as its word.
    auto asWritten = true;

    for (int i = 0; i < element.getNumProperties() && asWritten; ++i)
    {
        const auto property = element.getPropertyName (i);

        if (! isSwitch (element, property))
            continue;

        const auto& there = element[property];

        if (property == schema::trackAutomationMode.getName())
        {
            const auto mode = readModeWord (there);
            asWritten = mode.has_value() && isSameValue (there, getModeWord (*mode));
        }
        else
        {
            asWritten = (bool) there && isSameValue (there, juce::var (true));
        }
    }

    if (asWritten)
        return false;

    // Read as the song holds them, and written again.
    TrackSwitches held;
    readHeldSwitchesOf (element, held);

    for (auto i = element.getNumProperties(); --i >= 0;)
        if (const auto property = element.getPropertyName (i); isSwitch (element, property))
            element.removeProperty (property, nullptr);

    writeSwitchesInto (element, held);
    return true;
}

bool holdsSwitchesIn (const juce::ValueTree& element)
{
    if (! element.hasType (schema::TRACK) && ! element.hasType (schema::MASTER))
        return false;

    for (int i = 0; i < element.getNumProperties(); ++i)
        if (isSwitch (element, element.getPropertyName (i)))
            return true;

    return false;
}

bool isSwitchProperty (const juce::ValueTree& element, const juce::Identifier& property)
{
    return (element.hasType (schema::TRACK) || element.hasType (schema::MASTER)) && isSwitch (element, property);
}

void takeSwitchesOutOf (juce::ValueTree element, juce::UndoManager* undo)
{
    if (! element.hasType (schema::TRACK) && ! element.hasType (schema::MASTER))
        return;

    for (auto i = element.getNumProperties(); --i >= 0;)
        if (const auto property = element.getPropertyName (i); isSwitch (element, property))
            element.removeProperty (property, undo);
}

TrackSwitches readTrackSwitches (const juce::ValueTree& song)
{
    TrackSwitches switches;
    forEachTrackElement (song.getChildWithName (schema::TRACKS),
                         [&switches] (const juce::ValueTree& track) { readSwitchesOf (track, switches); });

    if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
        readSwitchesOf (master, switches);

    return switches;
}

bool readTrackMute (const juce::ValueTree& element)
{
    return element.hasType (schema::TRACK) && (bool) element[schema::trackMute];
}

bool readTrackSolo (const juce::ValueTree& element)
{
    return element.hasType (schema::TRACK) && (bool) element[schema::trackSolo];
}

bool readTrackFrozen (const juce::ValueTree& element)
{
    return element.hasType (schema::TRACK) && (bool) element[schema::trackFrozen];
}

std::optional<SavedAutomationMode> readSavedAutomationMode (const juce::ValueTree& element)
{
    if (! element.hasType (schema::TRACK) && ! element.hasType (schema::MASTER))
        return std::nullopt;

    return readModeWord (element[schema::trackAutomationMode]);
}
} // namespace phazite

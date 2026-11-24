// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongOwnArea.h"
#include "Song.h"
#include "internal/Schema.h"
#include "internal/SongOwnValue.h"
#include "internal/SongValues.h"
#include "internal/WholeState.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The song's LASTKEY, PIANOROLL or MERGING (a child of its root of one of those types), or an element in one (which none
    // holds as Schema.h describes them: an element Phazite doesn't know). Looked for no deeper than that, as each property
    // changed anywhere in the song asks it, once for each listener of the root.
    bool isInSongOwnElement (const juce::ValueTree& element)
    {
        const auto isOne = [] (const juce::ValueTree& each)
        { return isSongOwnElement (each.getType()) && each.getParent().hasType (schema::PHAZITE); };

        return isOne (element) || (element.getParent().isValid() && isOne (element.getParent()));
    }

    // A property of the root's that the song holds beside its tree: one of its own, or its count of IDs.
    bool isHeldProperty (const juce::Identifier& property)
    {
        return isSongOwnProperty (property) || property == schema::nextID.getName();
    }

    // Whether two optional ends of a loop differ, exactly (GCC: no == between floating-point values).
    bool loopEndsDiffer (const std::optional<double>& a, const std::optional<double>& b)
    {
        return a.has_value() != b.has_value() || (a.has_value() && ! juce::exactlyEqual (*a, *b));
    }

    // The song's own as the song holds it, written into a root of its own (SongOwnValue.h: writeSongOwnInto()): what the safety
    // net writes what it takes in into, to read it back as the song holds it.
    juce::ValueTree writeHeld (const Song& song)
    {
        juce::ValueTree root (schema::PHAZITE);
        writeSongOwnInto (root, getSongOwn (song), getSongStanding (song), getSongOutside (song));
        return root;
    }

    // The three values read from such a root, each set through its setter where it differs (the song is written outside the
    // history: the mode of the moment).
    void setHeld (Song& song, const juce::ValueTree& root)
    {
        setSongOwn (song, readHeldSongOwn (root));
        storeStandingOutsideHistory (song, readHeldSongStanding (root));
        setSongOutside (song, readHeldSongOutside (root));
    }
} // namespace

//==============================================================================
void setSongOwn (Song& song, SongOwnValue next)
{
    if (auto* values = song.getValues())
        values->setOwn (song, std::move (next));
    else
        jassertfalse; // a song that holds no values holds none of its own to set
}

void setSongStanding (Song& song, SongStanding next)
{
    if (auto* values = song.getValues())
        values->setStanding (song, std::move (next));
    else
        jassertfalse;
}

void storeStandingOutsideHistory (Song& song, SongStanding next)
{
    if (auto* values = song.getValues())
        values->setStandingOutsideHistory (std::move (next));
    else
        jassertfalse;
}

void setSongOutside (Song& song, SongOutside next)
{
    if (auto* values = song.getValues())
        values->setOutside (std::move (next));
    else
        jassertfalse;
}

//==============================================================================
bool SongValues::setOwn (Song& song, SongOwnValue next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next == songOwn.get())
        return false;

    if (song.isWritingOutsideHistory())
        songOwn.setOutsideHistory (std::move (next));
    else
        songOwn.set (std::move (next));

    return true;
}

bool SongValues::setStanding (Song& song, SongStanding next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next == songStanding.get())
        return false;

    if (song.isWritingOutsideHistory())
        songStanding.setOutsideHistory (std::move (next));
    else
        songStanding.set (std::move (next));

    return true;
}

bool SongValues::setStandingOutsideHistory (SongStanding next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (next == songStanding.get())
        return false;

    songStanding.setOutsideHistory (std::move (next));
    return true;
}

bool SongValues::setOutside (SongOutside next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (next == songOutside.get())
        return false;

    songOutside.set (std::move (next));
    return true;
}

//==============================================================================
SongValues::SongOwnTeller::SongOwnTeller (ValueArea<SongOwnValue>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::SongOwnTeller::~SongOwnTeller()
{
    area.removeListener (this);
}

void SongValues::SongOwnTeller::valueAreaChanged (const SongOwnValue& before, const SongOwnValue& after)
{
    SongOwnChange change;
    const auto partIf = [&change] (bool differs, SongOwnChange::Part part)
    {
        if (differs)
            change.parts = (std::uint8_t) (change.parts | part);
    };

    partIf (before.outline != after.outline, SongOwnChange::outline);
    partIf (before.notes != after.notes, SongOwnChange::notes);
    partIf (before.idNumbering != after.idNumbering, SongOwnChange::idNumbering);
    partIf (before.patternsKeepTheirSpeed != after.patternsKeepTheirSpeed, SongOwnChange::patterns);
    partIf (before.merging != after.merging, SongOwnChange::merging);

    if (change.parts != 0)
        listeners.call ([&change] (SongValues::Listener& listener) { listener.songOwnChanged (change); });
}

SongValues::SongStandingTeller::SongStandingTeller (ValueArea<SongStanding>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::SongStandingTeller::~SongStandingTeller()
{
    area.removeListener (this);
}

void SongValues::SongStandingTeller::valueAreaChanged (const SongStanding& before, const SongStanding& after)
{
    const auto lastKey = before.lastKey != after.lastKey;
    const auto names = before.versionsBranch != after.versionsBranch || before.versionsBase != after.versionsBase;

    if (lastKey || names)
        listeners.call ([lastKey, names] (SongValues::Listener& listener) { listener.songStandingChanged (lastKey, names); });
}

SongValues::SongOutsideTeller::SongOutsideTeller (OutsideValue<SongOutside>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::SongOutsideTeller::~SongOutsideTeller()
{
    area.removeListener (this);
}

void SongValues::SongOutsideTeller::outsideValueChanged (const SongOutside& before, const SongOutside& after)
{
    if (before == after)
        return;

    // What plays (SongPlayer.h: a plan's loop and metronome): the loop's four and the metronome.
    const auto plays = before.loop != after.loop || before.metronome != after.metronome || before.loopFollowsSong != after.loopFollowsSong
                       || loopEndsDiffer (before.loopStart, after.loopStart) || loopEndsDiffer (before.loopEnd, after.loopEnd);

    listeners.call ([plays] (SongValues::Listener& listener) { listener.songOutsideChanged (plays); });
}

//==============================================================================
SongValues::SongOwnKeeper::SongOwnKeeper (SongValues& v, Song& s)
    : values (v),
      song (s)
{
}

void SongValues::SongOwnKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    // The song's root alone, a property it holds beside its tree, written (not taken out: its own taking out tells too), and
    // nothing while the history undoes or redoes a step (the Undo of a step that wrote one refused takes it out again).
    if (tree != song.getState() || ! isHeldProperty (property) || ! tree.hasProperty (property)
        || song.getUndoManager().isPerformingUndoRedo())
        return;

    if (! song.isWritingOutsideHistory())
    {
        jassertfalse; // the song's own written to its tree: its setters change it (SongOwnValue.h), its count makeID()
        return;
    }

    auto root = song.getState();

    if (property == schema::nextID.getName())
    {
        song.setIDCount (readID (root[schema::nextID])); // the count as it is, as countIDsFrom() would set it
    }
    else
    {
        auto held = writeHeld (song);
        held.setProperty (property, root[property], nullptr);
        setHeld (song, held);
    }

    ++values.songOwnAdopted;
    root.removeProperty (property, nullptr);
}

void SongValues::SongOwnKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A LASTKEY, a PIANOROLL or a MERGING added to the song's root.
    if (parent != song.getState() || ! isSongOwnElement (child.getType()))
        return;

    if (! song.isWritingOutsideHistory())
    {
        jassertfalse; // the song's own added to its tree: its setters change it (SongOwnValue.h)
        return;
    }

    // The song's own with this element in place of its own of the type.
    auto held = writeHeld (song);
    held.removeChild (held.getChildWithName (child.getType()), nullptr);
    held.appendChild (child.createCopy(), nullptr);
    setHeld (song, held);

    ++values.songOwnAdopted;
    parent.removeChild (child, nullptr);
}

void SongValues::SongOwnKeeper::adopt (SongValues&, Song& loaded)
{
    // What its reader read into the three areas and the song's count of IDs before (readHeldSongOwnIn(): SongValues.cpp,
    // adopt()), out of the tree: every property of the root (its own and its count, read in; any other, Phazite doesn't know: one
    // the descriptions don't put on PHAZITE), its LASTKEY, PIANOROLL and MERGING, and every element of a type Phazite doesn't
    // know there, with everything in it (dropped: no value holds it; SongOwnValue.h: isKnownRootProperty(), isKnownRootElement(),
    // the rule a merge leaves them out by).
    auto root = loaded.getState();
    root.removeAllProperties (nullptr);
    std::vector<juce::ValueTree> gone;

    for (const auto& child : root)
        if (isSongOwnElement (child.getType()) || ! isKnownRootElement (child.getType()))
            gone.push_back (child);

    for (const auto& child : gone)
        root.removeChild (child, nullptr);
}

void readHeldSongOwnIn (const juce::ValueTree& song, SongState& into)
{
    // As the loader left the root: each of its own read as held, written back, itself (SongOwnValue.h: keepSongOwnAsHeld()),
    // asked of a root holding its properties and its own elements alone (a copy of the whole song would copy all it holds).
   #if JUCE_DEBUG
    juce::ValueTree own (schema::PHAZITE);
    own.copyPropertiesFrom (song, nullptr);

    for (const auto& child : song)
        if (isSongOwnElement (child.getType()))
            own.appendChild (child.createCopy(), nullptr);

    jassert (! keepSongOwnAsHeld (own));
   #endif

    into.songOwn = readHeldSongOwn (song);
    into.songStanding = readHeldSongStanding (song);
    into.songOutside = readHeldSongOutside (song);
    into.nextID = readID (song[schema::nextID]);
}

//==============================================================================
bool isHeardAsSongOwn (const juce::ValueTree& element)
{
    return isInSongOwnElement (element);
}

bool isHeardAsSongOwn (const juce::ValueTree& element, const juce::Identifier& property)
{
    return (element.hasType (schema::PHAZITE) && isHeldProperty (property)) || isInSongOwnElement (element);
}

bool isHeardAsSongOwn (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (parent.hasType (schema::PHAZITE) && isSongOwnElement (child.getType())) || isInSongOwnElement (parent);
}

//==============================================================================
void writeSongOwnIn (juce::ValueTree& element, const SongState& kept)
{
    if (! element.hasType (schema::PHAZITE))
        return;

    writeSongOwnInto (element, kept.songOwn, kept.songStanding, kept.songOutside);

    if (kept.nextID != 0)
        element.setProperty (schema::nextID, (juce::int64) kept.nextID, nullptr);
}

bool holdSameSongOwn (const SongState& kept, const SongState& otherKept)
{
    return kept.songOwn == otherKept.songOwn && kept.songStanding.lastKey == otherKept.songStanding.lastKey;
}

bool holdSameSongOwnOf (const juce::ValueTree& element, const SongState& kept, const juce::ValueTree&, const SongState& otherKept)
{
    // The other element is of the same type, its counterpart in the other song (WholeState.h: holdSameKeptOf()).
    return ! element.hasType (schema::PHAZITE) || holdSameSongOwn (kept, otherKept);
}
} // namespace phazite

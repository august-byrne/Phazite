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

namespace phazite
{
namespace
{
    // Whether two optional ends of a loop differ, exactly (GCC: no == between floating-point values).
    bool loopEndsDiffer (const std::optional<double>& a, const std::optional<double>& b)
    {
        return a.has_value() != b.has_value() || (a.has_value() && ! juce::exactlyEqual (*a, *b));
    }
} // namespace

//==============================================================================
void setSongOwn (Song& song, SongOwnValue next)
{
    song.getValues()->setOwn (song, std::move (next));
}

void setSongStanding (Song& song, SongStanding next)
{
    song.getValues()->setStanding (song, std::move (next));
}

void storeStandingOutsideHistory (Song& song, SongStanding next)
{
    song.getValues()->setStandingOutsideHistory (std::move (next));
}

void setSongOutside (Song& song, SongOutside next)
{
    song.getValues()->setOutside (std::move (next));
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
} // namespace phazite

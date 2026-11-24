// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongOwnArea.h"
#include "Song.h"
#include "internal/Schema.h"
#include "internal/SongOwnValue.h"
#include "internal/SongValues.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace phazite
{
namespace
{
    // In the step in progress, or outside the history while the song is written outside it (Song::OutsideHistory).
    juce::UndoManager* getUndoOfTheMoment (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // The song's LASTKEY, PIANOROLL or MERGING (a child of its root of one of those types), or an element in one (which none
    // holds as Schema.h describes them: an element Phazite doesn't know). Looked for no deeper than that, as each property
    // changed anywhere in the song asks it, once for each listener of the root.
    bool isInSongOwnElement (const juce::ValueTree& element)
    {
        const auto isOne = [] (const juce::ValueTree& each)
        { return isSongOwnElement (each.getType()) && each.getParent().hasType (schema::PHAZITE); };

        return isOne (element) || (element.getParent().isValid() && isOne (element.getParent()));
    }

    // Each setter's writes, bracketed for the keeper (SongValues::SongOwnWrite): the values written into the song's root by what
    // differs, one of them, the others as the song holds them.
    void write (Song& song, SongOwnPart part, const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside,
                juce::UndoManager* undo)
    {
        const SongValues::SongOwnWrite bracket (song);
        writeSongOwnPart (song.getState(), part, own, standing, outside, undo);
    }
} // namespace

//==============================================================================
void setSongOwn (Song& song, SongOwnValue next)
{
    if (next == getSongOwn (song))
        return;

    write (song, SongOwnPart::own, next, getSongStanding (song), getSongOutside (song), getUndoOfTheMoment (song));
}

void setSongStanding (Song& song, SongStanding next)
{
    if (next == getSongStanding (song))
        return;

    write (song, SongOwnPart::standing, getSongOwn (song), next, getSongOutside (song), getUndoOfTheMoment (song));
}

void storeStandingOutsideHistory (Song& song, SongStanding next)
{
    if (next == getSongStanding (song))
        return;

    write (song, SongOwnPart::standing, getSongOwn (song), next, getSongOutside (song), nullptr);
}

void setSongOutside (Song& song, SongOutside next)
{
    if (next == getSongOutside (song))
        return;

    write (song, SongOwnPart::outside, getSongOwn (song), getSongStanding (song), next, nullptr);
}

//==============================================================================
SongValues::SongOwnWrite::SongOwnWrite (Song& song)
    : keeper (song.getValues() != nullptr ? song.getValues()->findSongOwnKeeper() : nullptr)
{
    if (keeper != nullptr)
        keeper->beginWrite();
}

SongValues::SongOwnWrite::~SongOwnWrite()
{
    if (keeper != nullptr)
        keeper->endWrite();
}

//==============================================================================
SongValues::SongOwnKeeper::SongOwnKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      toldOwn (getSongOwn (s)),
      toldStanding (getSongStanding (s)),
      toldOutside (getSongOutside (s))
{
    song.addHistoryListener (this);
}

SongValues::SongOwnKeeper::~SongOwnKeeper()
{
    song.removeHistoryListener (this);
}

void SongValues::SongOwnKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (isHeardAsSongOwn (tree, property))
        heard();
}

void SongValues::SongOwnKeeper::childChanged (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (isHeardAsSongOwn (parent, child))
        heard();
}

void SongValues::SongOwnKeeper::heard()
{
    // In a setter's bracket: told as it ends. In an Undo or a Redo: told once the move is whole (historyMoved()). Else at once.
    marked = true;

    if (writing == 0 && ! song.getUndoManager().isPerformingUndoRedo())
        tell();
}

void SongValues::SongOwnKeeper::endWrite()
{
    jassert (writing > 0);

    if (--writing == 0 && marked && ! song.getUndoManager().isPerformingUndoRedo())
        tell();
}

void SongValues::SongOwnKeeper::historyMoved()
{
    if (! marked || writing > 0)
        return;

    const juce::ScopedValueSetter<bool> moving (tellingMove, true);
    tell();
}

void SongValues::SongOwnKeeper::tell()
{
    marked = false;

    // What the song holds now against what was told, each value as a whole, then field by field.
    auto own = getSongOwn (song);
    auto standing = getSongStanding (song);
    auto outside = getSongOutside (song);

    SongOwnChange change;

    if (! (own == toldOwn))
    {
        const auto partIf = [&change] (bool differs, SongOwnChange::Part part)
        {
            if (differs)
                change.parts = (std::uint8_t) (change.parts | part);
        };

        partIf (own.outline != toldOwn.outline, SongOwnChange::outline);
        partIf (own.notes != toldOwn.notes, SongOwnChange::notes);
        partIf (own.idNumbering != toldOwn.idNumbering, SongOwnChange::idNumbering);
        partIf (own.patternsKeepTheirSpeed != toldOwn.patternsKeepTheirSpeed, SongOwnChange::patterns);
        partIf (own.merging != toldOwn.merging, SongOwnChange::merging);
    }

    const auto keyChanged = standing.lastKey != toldStanding.lastKey;
    const auto namesChanged = standing.versionsBranch != toldStanding.versionsBranch || standing.versionsBase != toldStanding.versionsBase;
    const auto outsideChanged = ! (outside == toldOutside);

    // What plays (SongPlayer.h: a plan's loop and metronome): the loop's four and the metronome.
    const auto loopEndsDiffer = [] (const std::optional<double>& a, const std::optional<double>& b)
    { return a.has_value() != b.has_value() || (a.has_value() && ! juce::exactlyEqual (*a, *b)); };

    const auto plays =
        outsideChanged
        && (outside.loop != toldOutside.loop || outside.metronome != toldOutside.metronome
            || outside.loopFollowsSong != toldOutside.loopFollowsSong || loopEndsDiffer (outside.loopStart, toldOutside.loopStart)
            || loopEndsDiffer (outside.loopEnd, toldOutside.loopEnd));

    // What is told is told once: what the listeners do may set the song's own again, which is told after.
    toldOwn = std::move (own);
    toldStanding = std::move (standing);
    toldOutside = std::move (outside);

    if (change.parts != 0)
        values.listeners.call ([&change] (Listener& listener) { listener.songOwnChanged (change); });

    if (keyChanged || namesChanged)
        values.listeners.call ([keyChanged, namesChanged] (Listener& listener)
                               { listener.songStandingChanged (keyChanged, namesChanged); });

    if (outsideChanged)
        values.listeners.call ([plays] (Listener& listener) { listener.songOutsideChanged (plays); });
}

//==============================================================================
bool isHeardAsSongOwn (const juce::ValueTree& element)
{
    return isInSongOwnElement (element);
}

bool isHeardAsSongOwn (const juce::ValueTree& element, const juce::Identifier& property)
{
    return (element.hasType (schema::PHAZITE) && isSongOwnProperty (property)) || isInSongOwnElement (element);
}

bool isHeardAsSongOwn (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (parent.hasType (schema::PHAZITE) && isSongOwnElement (child.getType())) || isInSongOwnElement (parent);
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestSongOwn.h"

#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/NoteValues.h"
#include "model/notes/internal/Notes.h"
#include "model/versions/internal/VersionHistory.h"

namespace phazite::test
{
void setStepProbe (Song& song, const juce::String& text)
{
    REQUIRE_MESSAGE (getSections (song).empty(), "The step probe applies an outline, which would take the song's sections away.");
    Outline outline;
    outline.id = text;
    applyOutline (song, outline);
}

juce::String getStepProbe (const Song& song)
{
    return getAppliedOutline (song);
}

void changeSongInStep (Song& song, int n)
{
    static int probesSet = 0; // each a text not set before, in this run of the tests

    for (int i = 0; i < n; ++i)
        changeSongInStep (song, "probe " + juce::String (++probesSet));
}

void changeSongInStep (Song& song, const juce::String& probe)
{
    const UndoStep step (song, "Change Probe");
    setStepProbe (song, probe);
}

namespace
{
    TrackID findProbeTrack (const Song& song, int place)
    {
        const auto tracks = getTrackIDs (song);
        REQUIRE_MESSAGE ((place >= 0 && place < (int) tracks.size()), "The song has no track for that probe: make it first.");
        return tracks[(size_t) place];
    }

    const juce::String probeName ("probe ");
} // namespace

void setTrackProbe (Song& song, int place, int value)
{
    setTrackName (song, findProbeTrack (song, place), probeName + juce::String (value));
}

int getTrackProbe (const Song& song, int place)
{
    const auto name = getTrackName (song, findProbeTrack (song, place));
    return name.startsWith (probeName) ? name.substring (probeName.length()).getIntValue() : -1;
}

void changeOutsideHistory (Song& song)
{
    song.markChangedOutsideHistory();
}

void setKeptOutsideHistory (Song& song, int value)
{
    REQUIRE ((value >= 0 && value < 12));
    setLastKey (song, Key { value, Scale::major });
}

int getKeptOutsideHistory (const Song& song)
{
    const auto key = getLastKey (song);
    return key.has_value() ? key->root : -1;
}

void changeRoundTheHistory (Song& song)
{
    const Song::OutsideHistory outside (song);
    const auto start = getSectionsEnd (song);
    REQUIRE (insertSection (song, start, { .name = "Round", .length = BeatDuration::fromBeats (4.0) }).isValid());
}

juce::ValueTree copyWaitingMerge (const Song& song)
{
    return phazite::test::copyWholeSong (song).getChildWithName (schema::MERGING);
}

bool hasWaitingMerge (const Song& song)
{
    return copyWaitingMerge (song).isValid();
}

void changeWaitingMerge (Song& song, const juce::Identifier& property, const juce::var& value)
{
    auto merging = copyWaitingMerge (song);
    REQUIRE (merging.isValid());
    merging.setProperty (property, value, nullptr);
    setWaitingMerge (song, merging);
}
} // namespace phazite::test

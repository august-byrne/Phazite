// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Migrations.h"
#include "internal/Schema.h"

#include <algorithm>
#include <iterator>

namespace phazite
{
namespace
{
    //==========================================================================
    // Version 0 to 1. The tables are frozen: they say what version 0 meant, whatever later versions do.

    // gridLevel, a Tracktion snap level 5..13, exists only here.
    const juce::Identifier legacyGridLevel { "gridLevel" };
    constexpr const char* v1GridSteps[] = {
        "1/16 beat", "1/8 beat", "1/4 beat", "1/2 beat", "1 beat", "1 bar", "2 bars", "4 bars", "8 bars"
    };

    // By Tracktion's scale number 0..10: ionian and aeolian, which Phazite never offered, have the notes of major and minor.
    constexpr const char* v1Scales[] = { "major",      "minor", "major",   "dorian",       "phrygian",     "lydian",
                                         "mixolydian", "minor", "locrian", "melodicMinor", "harmonicMinor" };

    bool isV1ScaleName (const juce::String& value)
    {
        return std::any_of (std::begin (v1Scales), std::end (v1Scales), [&value] (const char* name) { return value == name; });
    }

    // The size of a fixed grid, as version 0 read it: levels outside 5..13 as the nearest.
    void upgradeGridStep (juce::ValueTree grid)
    {
        if (! grid.hasProperty (legacyGridLevel))
            return;

        const auto level = juce::jlimit (5, 13, (int) grid[legacyGridLevel]);
        grid.setProperty (schema::gridStep, v1GridSteps[(size_t) (level - 5)], nullptr);
        grid.removeProperty (legacyGridLevel, nullptr);
    }

    // Every key's scale, on tracks and LASTKEY alike, as version 0 read it: numbers outside 0..10 as the nearest.
    void upgradeScales (juce::ValueTree tree)
    {
        if (tree.hasProperty (schema::phaziteKeyScale) && ! isV1ScaleName (tree[schema::phaziteKeyScale].toString()))
            tree.setProperty (schema::phaziteKeyScale, v1Scales[(size_t) juce::jlimit (0, 10, (int) tree[schema::phaziteKeyScale])],
                              nullptr);

        for (auto child : tree)
            upgradeScales (child);
    }

    void upgrade0To1 (juce::ValueTree& editState)
    {
        // The playlist's grid is on the Phazite state, and the piano roll's on its child.
        if (auto phaziteState = editState.getChildWithName (schema::PHAZITE); phaziteState.isValid())
        {
            upgradeGridStep (phaziteState);

            for (auto child : phaziteState)
                upgradeGridStep (child);
        }

        upgradeScales (editState);
    }

    //==========================================================================
    // Version 1 to 2: version 2 can hold Phazite's own devices, which a version 1 project has none of, so nothing
    // changes; the new version keeps an older Phazite from opening a project with them, and dropping them.
    void upgrade1To2 (juce::ValueTree&)
    {
    }

    //==========================================================================
    // Version 2 to 3: every note gets an ID, 1 and up among the notes of its sequence, in the order they are saved in.

    // Tracktion's element for a note, as version 2 saved it.
    const juce::Identifier v2Note { "NOTE" };

    void upgrade2To3 (juce::ValueTree& tree)
    {
        auto next = 1;

        for (auto child : tree)
        {
            if (child.hasType (v2Note))
                child.setProperty (schema::phaziteNoteID, next++, nullptr);
            else
                upgrade2To3 (child);
        }
    }

    //==========================================================================
    // Version 3 to 4: version 4 adds automation lanes (PHAZITELANE), which a version 3 project has none of, so nothing
    // changes; the new version keeps an older Phazite from opening a project with them, and dropping them.
    void upgrade3To4 (juce::ValueTree&)
    {
    }

    //==========================================================================
    // Version 4 to 5: version 4's automation clips become their parameters' own automation once the Edit is made, which
    // gives the song's tempo (Migrations.h: createEditFromState()), so the saved state doesn't change here; the new
    // version keeps an older Phazite from opening a project whose automation it would read differently.
    void upgrade4To5 (juce::ValueTree&)
    {
    }

    //==========================================================================
    // Version 5 to 6: version 6 lets lanes collapse (laneCollapsed), which a version 5 project's never are, so nothing
    // changes.
    void upgrade5To6 (juce::ValueTree&)
    {
    }

    //==========================================================================
    // Version 6 to 7: version 7 adds the chord lane (CHORDLANE) and parts made from it (phaziteChordPart), which a version
    // 6 project has none of, so nothing changes; the new version keeps an older Phazite from opening a project with them,
    // and dropping them.
    void upgrade6To7 (juce::ValueTree&)
    {
    }

    //==========================================================================
    // Version 7 to 8: version 8 can hold the devices of step 2.10 and the synth's unison, which a version 7 project has
    // none of (a synth without the unison's parameters plays one voice, as it did), so nothing changes; the new version
    // keeps an older Phazite from opening a project with them, and dropping them or playing it without.
    void upgrade7To8 (juce::ValueTree&)
    {
    }

    //==========================================================================
    // Each version's upgrade to the next: steps[n] upgrades version n.
    using Step = void (*) (juce::ValueTree&);
    constexpr Step steps[] = { upgrade0To1, upgrade1To2, upgrade2To3, upgrade3To4, upgrade4To5, upgrade5To6, upgrade6To7, upgrade7To8 };
    static_assert (std::size (steps) == (size_t) schema::currentVersion, "every version but the current one needs an upgrade");
} // namespace

int getSchemaVersion (const juce::ValueTree& editState)
{
    const auto text = editState.getChildWithName (schema::PHAZITE)[schema::schemaVersion].toString().trim();

    if (text.isEmpty() || ! text.containsOnly ("0123456789"))
        return 0;

    return text.getIntValue();
}

void writeCurrentSchemaVersion (juce::ValueTree& editState)
{
    if (! editState.isValid() || getSchemaVersion (editState) == schema::currentVersion)
        return;

    editState.getOrCreateChildWithName (schema::PHAZITE, nullptr).setProperty (schema::schemaVersion, schema::currentVersion, nullptr);
}

bool migrateToCurrentSchema (juce::ValueTree& editState)
{
    if (! editState.isValid())
        return true;

    const auto version = getSchemaVersion (editState);

    if (version > schema::currentVersion)
        return false;

    for (auto from = version; from < schema::currentVersion; ++from)
        steps[(size_t) from](editState);

    writeCurrentSchemaVersion (editState);
    return true;
}
} // namespace phazite

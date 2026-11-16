// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UndoCheck.h"
#include "GoldenFiles.h"
#include "PlanCheck.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/internal/ProjectContext.h"

#include <limits>

namespace phazite::test
{
namespace
{
    constexpr int maxDifferencesListed = 12;

    bool isPlugin (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) && element[schema::deviceType].toString() == schema::deviceIsPlugin;
    }

    bool isChannel (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACK) || element.hasType (schema::MASTER);
    }

    // What the project remembers outside the undo history, besides what isn't counted (UndoCheck.h): which tracks are
    // frozen, and what a plug-in keeps of its own.
    bool isRememberedProperty (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (isChannel (element))
            return property == schema::trackFrozen;

        return isPlugin (element)
               && (property == schema::pluginState || property == schema::pluginProgram || property == schema::pluginFormatID
                   || property == schema::pluginVersion);
    }

    // The key set last, and a plug-in's values set by hand, stored with its state.
    bool isRememberedElement (const juce::ValueTree& parent, const juce::ValueTree& element)
    {
        return (parent.hasType (schema::PHAZITE) && element.hasType (schema::LASTKEY))
               || (isPlugin (parent) && element.hasType (schema::VALUE));
    }

    // The switches that are no undo steps.
    bool isSwitch (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (isChannel (element))
            return property == schema::trackMute || property == schema::trackSolo || property == schema::trackAutomationMode;

        return element.hasType (schema::CHAIN) && (property == schema::chainMute || property == schema::chainSolo);
    }

    bool holds (SongPart part, const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (part == SongPart::whole)
            return true;

        if (schema::isUncountedProperty (element, property) || isRememberedProperty (element, property))
            return false;

        return part == SongPart::undoable || ! isSwitch (element, property);
    }

    bool holds (SongPart part, const juce::ValueTree& parent, const juce::ValueTree& element)
    {
        return part == SongPart::whole || ! (schema::isUncountedElement (parent, element) || isRememberedElement (parent, element));
    }

    // A copy of an element with the part of it and of what it holds that a text of that part holds.
    juce::ValueTree copyPart (const juce::ValueTree& element, SongPart part)
    {
        juce::ValueTree copy (element.getType());

        for (int i = 0; i < element.getNumProperties(); ++i)
            if (const auto name = element.getPropertyName (i); holds (part, element, name))
                copy.setProperty (name, element[name], nullptr);

        for (const auto& child : element)
            if (holds (part, element, child))
                copy.appendChild (copyPart (child, part), nullptr);

        return copy;
    }
} // namespace

juce::String writeSong (const Song& song, SongPart part)
{
    juce::MemoryOutputStream text;
    writeProjectText (text, copyPart (song.getState(), part));
    return text.toString();
}

juce::StringArray describeSongDifferences (const juce::String& expected, const juce::String& actual)
{
    if (expected == actual)
        return {};

    // By their places in the song, as the golden projects' differences are; and for what that comparison takes for the
    // same (elements of one kind in another order, numbers a hair apart), the lines themselves.
    const auto expectedXml = juce::parseXML (expected);
    const auto actualXml = juce::parseXML (actual);

    if (expectedXml != nullptr && actualXml != nullptr)
        if (const auto differences = findDifferences (*expectedXml, *actualXml, { .maxDifferences = maxDifferencesListed });
            ! differences.isEmpty())
            return differences;

    return compareLines (expected, actual, maxDifferencesListed);
}

//==============================================================================
UndoCheck::UndoCheck (Song& songToFollow)
    : song (songToFollow),
      player (std::make_unique<SongPlayer> (songToFollow))
{
    // Every step from here on stays in the history, whatever it holds.
    song.getUndoManager().setMaxNumberOfStoredUnits (std::numeric_limits<int>::max() / 2, 30);
    base = getDepth();
    history.push_back (writeSong (song, SongPart::undoableWithoutSwitches));
    compilePlan(); // what the plans after each change keep from
}

namespace
{
    Song& getOpenSong (ProjectContext& context)
    {
        auto* open = findSong (context);
        REQUIRE_MESSAGE (open != nullptr, "The project context has no song open to check the undo steps of.");
        return *open;
    }
} // namespace

UndoCheck::~UndoCheck() = default;

void UndoCheck::compilePlan()
{
    // As a render compiles it: while checking is on (tests/TestMain.cpp), what the plan keeps from the plan before is
    // compared with the song (a stale part is a JUCE assertion, which fails the test); and the whole plan with a fresh
    // player's whole compile of the song as it is now.
    const auto plan = player->compile (44100.0, 512, std::nullopt);
    const auto whole = SongPlayer (song).compile (44100.0, 512, std::nullopt);

    if (const auto differences = describePlanDifferences (*plan, *whole); ! differences.isEmpty())
        fail ("After \"" + song.getUndoManager().getUndoDescription()
              + "\", the plan compiled from what changed doesn't play as a "
                "whole compile does: "
              + differences.joinIntoString ("; "));
}

UndoCheck::UndoCheck (ProjectContext& context)
    : UndoCheck (getOpenSong (context))
{
}

int UndoCheck::getDepth() const
{
    return song.getUndoManager().getUndoDescriptions().size();
}

void UndoCheck::compare (const juce::String& what, const juce::String& when, const juce::String& expected, const juce::String& actual)
{
    // What finds the song's items by their IDs holds what the song does, after each change, undo and redo.
    if (! song.getIndex().isExact())
        fail (what + ": " + when + ": the song's index (model/base/internal/SongIndex.h) differs from a walk through the song");

    if (expected == actual)
        return;

    juce::String report;
    report << what << ": " << when << ":";

    for (const auto& line : describeSongDifferences (expected, actual))
        report << "\n  " << line;

    fail (report);
}

void UndoCheck::fail (const juce::String& failure)
{
    if (onFailure != nullptr)
        onFailure (failure);
    else
        FAIL_CHECK (failure.toStdString());
}

// Before a change: if the test changed the song without the check (it built more of it, or undid something), the steps
// the check didn't follow are noted as the history has them now, each by going back to it, down to one it knows.
void UndoCheck::catchUp()
{
    auto depth = getDepth() - base;

    if (depth < 0) // undone to before the check began: it begins here
    {
        base += depth;
        history.clear();
        depth = 0;
    }

    const auto top = depth;
    history.resize ((size_t) top + 1);

    for (;; --depth)
    {
        compilePlan();
        const auto now = writeSong (song, SongPart::undoableWithoutSwitches);

        if (history[(size_t) depth] == now)
            break;

        history[(size_t) depth] = now;

        if (depth == 0)
            break;

        undo (song);
    }

    for (; depth < top; ++depth)
        redo (song);
}

void UndoCheck::check (const juce::String& what, const std::function<void()>& change)
{
    run (what, change, false);
}

void UndoCheck::command (CommandRegistry& registry, const juce::String& id)
{
    check (id, [&registry, &id] { REQUIRE_MESSAGE (registry.invoke (id), (id + " is unknown or greyed out").toStdString()); });
}

void UndoCheck::checkSeveral (const juce::String& what, const std::function<void()>& changes)
{
    run (
        what,
        [this, &what, &changes]
        {
            song.getUndoManager().beginNewTransaction (what);
            changes();
        },
        true);
}

void UndoCheck::run (const juce::String& what, const std::function<void()>& change, bool severalSteps)
{
    catchUp();
    const auto depthBefore = getDepth();
    const auto before = writeSong (song, SongPart::undoable);
    const auto keptBefore = writeSong (song, SongPart::undoableWithoutSwitches);

    change();
    compilePlan();

    const auto depthAfter = getDepth();
    const auto after = writeSong (song, SongPart::undoable);
    const auto steps = depthAfter - depthBefore;

    if (steps < 0)
    {
        fail (what + ": the song's undo history went back (" + juce::String (depthBefore - base) + " steps before, "
              + juce::String (depthAfter - base) + " after); an undo that a test runs itself goes through stepBackOrForth()");
        return;
    }

    // What the history held past here is gone with a new step; without one, it is as it was.
    if (steps == 0)
    {
        compare (what,
                 "changed what undo steps keep without an undo step of its own (it joined the step before, or went round the history)",
                 keptBefore, writeSong (song, SongPart::undoableWithoutSwitches));
        return;
    }

    if (steps > 1 && ! severalSteps)
        fail (what + ": made " + juce::String (steps) + " undo steps (" + song.getUndoManager().getUndoDescription()
              + " the last), where a change is one");

    // A change through the history that begins no step joins the step in progress. Here it found none (the check's last
    // Undo and Redo ended it), and made a step without a name.
    if (! severalSteps && song.getUndoManager().getUndoDescription().isEmpty())
        fail (what + ": made an undo step without a name: it began none, so that its change joins whatever step came before");

    // Back through its steps, the song at each kept for the walk, to the song before; and forth to the song after.
    history.resize ((size_t) (depthBefore - base + 1));
    std::vector<juce::String> between;

    for (int i = 0; i < steps; ++i)
    {
        undo (song);
        between.push_back (writeSong (song, SongPart::undoableWithoutSwitches));
    }

    compilePlan();
    compare (what, "Undo doesn't give the song as it was before", before, writeSong (song, SongPart::undoable));

    for (int i = 0; i < steps; ++i)
        redo (song);

    compilePlan();
    compare (what, "Redo doesn't give the song as it was after", after, writeSong (song, SongPart::undoable));

    if (getDepth() != depthAfter)
        fail (what + ": after Undo and Redo, the history has " + juce::String (getDepth() - base) + " steps, not "
              + juce::String (depthAfter - base));

    for (auto i = (int) between.size() - 1; --i >= 0;) // the last one is the song before, which the history has
        history.push_back (between[(size_t) i]);

    history.push_back (writeSong (song, SongPart::undoableWithoutSwitches));
    numSteps += steps;
}

void UndoCheck::stepBackOrForth (const juce::String& what, const std::function<void()>& undoOrRedo)
{
    undoOrRedo();
    compilePlan();
    const auto step = getDepth() - base;

    if (step < 0 || step >= (int) history.size())
    {
        fail (what + ": the song's undo history is at step " + juce::String (step) + ", which the check didn't follow it to");
        return;
    }

    compare (what, "the song isn't as it was at step " + juce::String (step), history[(size_t) step],
             writeSong (song, SongPart::undoableWithoutSwitches));
}

void UndoCheck::walkBackAndForth()
{
    catchUp();
    const auto whole = writeSong (song, SongPart::whole);
    const auto end = getDepth() - base;

    if (end < 0 || end >= (int) history.size())
    {
        fail ("The walk: the song's undo history is at step " + juce::String (end) + ", which the check didn't follow it to");
        return;
    }

    // The first step that differs each way is reported: the ones past it differ for it.
    auto same = true;
    const auto compareStep = [this, &same] (int step, const char* direction)
    {
        compilePlan();
        const auto now = writeSong (song, SongPart::undoableWithoutSwitches);

        if (same && now != history[(size_t) step])
        {
            same = false;
            compare ("The walk",
                     juce::String (direction) + " to step " + juce::String (step) + " of " + juce::String ((int) history.size() - 1)
                         + " (after \"" + song.getUndoManager().getUndoDescription() + "\") doesn't give the song as it was there",
                     history[(size_t) step], now);
        }
    };

    for (auto step = end; --step >= 0;)
    {
        undo (song);
        compareStep (step, "Undo");
    }

    same = true;

    for (auto step = 1; step <= end; ++step)
    {
        redo (song);
        compareStep (step, "Redo");
    }

    compare ("The walk", "back to where the check began and forth again, the whole song isn't as it was", whole,
             writeSong (song, SongPart::whole));
}
} // namespace phazite::test

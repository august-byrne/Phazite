// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's controller lane with the mouse (ui/pianoroll/PianoRollParts.h: ControlLaneView): its chooser and what it
// lists, a draw with Snap on and off, a click, a ramp, the Delete tool, Delete All, Escape, a clip deleted in the middle of a
// draw, two clips shown, and the lane each clip opens on.
#include "EditorScreens.h"
#include "TestHelpers.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/Visits.h"
#include "model/commands/internal/Undo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
using phazite::ControlKind;
using phazite::ControlLane;
using phazite::test::beatAt;
using phazite::test::Mouse;
using phazite::test::PianoRollScreen;

const ControlLane pedal { ControlKind::controller, 64, 1 };
const ControlLane bend { ControlKind::pitchWheel, 0, 1 };
constexpr int top = 3; // the lane's top row that the pointer reaches: the line that sizes the lane takes those above it

using Values = std::vector<std::pair<double, int>>;

// A piano roll with its controller lane: the lane shown and found, and its values as the model has them.
struct LaneScreen : PianoRollScreen
{
    explicit LaneScreen (const ControlLane& toShow = pedal)
    {
        pianoRoll.showLane (toShow);
        lane = phazite::test::findComponentWithTitle (pianoRoll, "Controller lane");
        REQUIRE (lane != nullptr);
        REQUIRE (lane->isShowing());
        CHECK_FALSE (velocity->isShowing());
    }

    void add (double beat, int value, const ControlLane& of = pedal)
    {
        phazite::addControl (getSong(), clip, beatAt (beat), of.kind, of.number, value, of.channel);
        phazite::test::dispatchPendingMessages();
    }

    Values valuesOf (phazite::ClipID of, const ControlLane& laneOf = pedal) const
    {
        Values values;

        for (const auto& point : phazite::getControlValues (context, { of }, laneOf))
            values.emplace_back (point.beat.inBeats(), point.value);

        return values;
    }

    Values values() const { return valuesOf (clip); }

    // Where a value is drawn in the lane, by its height.
    int yFor (int value, int highest = 127) const
    {
        return juce::roundToInt (3.0f + ((float) lane->getHeight() - 5.0f) * (1.0f - (float) value / (float) highest));
    }

    juce::Point<int> at (double beat, int value) const { return { xOf (beat), yFor (value) }; }

    // The lane's chooser, which a screen reader reads as the lane it shows.
    juce::TextButton& chooser() const
    {
        const auto shown = pianoRoll.getLaneShown();
        auto* button = dynamic_cast<juce::TextButton*> (
            phazite::test::findComponentWithTitle (const_cast<phazite::ui::PianoRoll&> (pianoRoll),
                                                   "Lane: " + (shown.has_value() ? shown->getName() : juce::String ("Velocity"))));
        REQUIRE (button != nullptr);
        return *button;
    }

    // The menu's items, in order, as their texts (a section's header too), without the submenus' items.
    std::vector<juce::String> menuTexts()
    {
        std::vector<juce::String> texts;

        for (juce::PopupMenu::MenuItemIterator it (pianoRoll.getLaneMenu()); it.next();)
            if (! it.getItem().isSeparator)
                texts.push_back (it.getItem().text);

        return texts;
    }

    juce::Component* lane = nullptr;
};

bool isSpacedAtLeastAGap (const Values& values)
{
    for (size_t i = 1; i < values.size(); ++i)
        if (values[i].first - values[i - 1].first < phazite::getShortestControlGap().inBeats() - 1.0e-9)
            return false;

    return true;
}

// The changes of the song its listeners hear: each change of its tree, counted at its root, and each of its clips' values
// (model/base/internal/SongValues.h), as the speed gates count them; and how many controllers' values each change of the
// values took away or brought, clip by clip.
struct Heard : juce::ValueTree::Listener,
               phazite::ValueArea<phazite::NotesByClip>::Listener
{
    explicit Heard (phazite::Song& song)
        : root (song.getState()),
          values (*song.getValues())
    {
        root.addListener (this);
        values.notes.addListener (this);
    }

    ~Heard() override
    {
        values.notes.removeListener (this);
        root.removeListener (this);
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++changes; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++changes; }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++changes; }

    void valueAreaChanged (const phazite::NotesByClip& before, const phazite::NotesByClip& after) override
    {
        ++changes;

        using Key = std::tuple<int, int, double, int, int>;
        const auto count = [] (const phazite::ClipNotes* clip)
        {
            std::map<Key, int> held;

            if (clip != nullptr)
                for (const auto& control : clip->controls)
                    ++held[{ (int) control.kind, control.number, control.beat, control.value, control.channel }];

            return held;
        };

        for (const auto& [id, value] : after.clips)
        {
            if (const auto* old = before.find (phazite::ClipID::fromRaw (id)); old != nullptr && old->isSameAs (value))
                continue;

            auto gone = count (before.find (phazite::ClipID::fromRaw (id)));

            for (const auto& [key, number] : count (&value))
                gone[key] -= number;

            for (const auto& [key, number] : gone)
                valuesChanged += std::abs (number);
        }
    }

    juce::ValueTree root;
    phazite::SongValues& values;
    int changes = 0, valuesChanged = 0;
};

// A long take's 100,000 values in a clip on the second track, 800 beats long: the bend and the mod wheel by turns, 1/128 of a
// beat apart, as a take appends them; shown, on the bend's lane, all of it in view.
phazite::ClipID addLongTake (LaneScreen& screen)
{
    const auto take = phazite::insertMidiClip (screen.getSong(), screen.tracks[1], { beatAt (0.0), beatAt (800.0) }, nullptr);
    std::vector<phazite::ControlValue> many;

    for (int i = 0; i < 100000; ++i)
        many.push_back ({ beatAt (i / 128.0), i % 2 == 0 ? ControlKind::pitchWheel : ControlKind::controller, 1, (i * 37) % 128, 1 });

    phazite::addControls (screen.getSong(), take, many);
    screen.pianoRoll.showClip (take);
    screen.pianoRoll.showLane (bend);
    screen.pianoRoll.zoomTo ({ beatAt (0.0), beatAt (800.0) });
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    return take;
}
} // namespace

TEST_CASE ("The lane's chooser lists what the clips hold first, marks what the lane doesn't show, and shows the lane chosen")
{
    PianoRollScreen screen;
    phazite::addControl (screen.getSong(), screen.clip, beatAt (1.0), ControlKind::controller, 64, 127, 1);
    phazite::addControl (screen.getSong(), screen.clip, beatAt (3.0), ControlKind::controller, 64, 0, 1);
    phazite::addControl (screen.getSong(), screen.clip, beatAt (2.0), ControlKind::pitchWheel, 0, 12000, 2);
    phazite::test::dispatchPendingMessages();

    // On the velocities, the chooser says what else the clips hold: it plays. A screen reader reads it as the lane shown, and
    // a click on it leaves the keys with the piano roll, as the chevron's does.
    auto* chooser = dynamic_cast<juce::TextButton*> (phazite::test::findComponentWithTitle (screen.pianoRoll, "Lane: Velocity"));
    REQUIRE (chooser != nullptr);
    CHECK (chooser->isShowing());
    CHECK_FALSE (chooser->getMouseClickGrabsKeyboardFocus());
    CHECK (chooser->getButtonText() == "Velocity");
    CHECK (chooser->getTooltip() == "Velocity\nThese clips also hold Sustain Pedal (CC 64) and Pitch Bend, Channel 2");
    CHECK_FALSE (screen.pianoRoll.getLaneShown().has_value());

    // Its menu: the velocities, what the clips hold, then the usual lanes they don't (the pedal is among the first already).
    auto menu = screen.pianoRoll.getLaneMenu();
    std::vector<juce::String> texts;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (! it.getItem().isSeparator && it.getItem().subMenu == nullptr)
            texts.push_back (it.getItem().text);

    const std::vector<juce::String> expected { "Velocity",    "In these clips",   "Sustain Pedal (CC 64)", "Pitch Bend, Channel 2",
                                               "Pitch Bend",  "Mod Wheel (CC 1)", "Expression (CC 11)",    "Volume (CC 7)",
                                               "Pan (CC 10)", "Channel Pressure", "Program Change" };
    CHECK (texts == expected);
    CHECK (phazite::test::findMenuItem (menu, { "Velocity" })->isTicked);
    REQUIRE (phazite::test::findMenuItem (menu, { "Channel", "Channel 2" }) != nullptr);
    CHECK_FALSE (phazite::test::findMenuItem (menu, { "Channel", "Channel 2" })->isEnabled); // not for the velocities

    // The pedal chosen: its lane shows in the velocities' place, and the chooser marks the bend still.
    REQUIRE (phazite::test::runMenuItem (menu, { "Sustain Pedal (CC 64)" }));
    CHECK (screen.pianoRoll.getLaneShown() == pedal);
    CHECK (chooser->getButtonText() == "Sustain Pedal (CC 64)");
    CHECK (chooser->getTitle() == "Lane: Sustain Pedal (CC 64)");
    CHECK (chooser->getTooltip() == "Sustain Pedal (CC 64)\nThese clips also hold Pitch Bend, Channel 2");
    auto* lane = phazite::test::findComponentWithTitle (screen.pianoRoll, "Controller lane");
    REQUIRE (lane != nullptr);
    CHECK (lane->isShowing());
    CHECK_FALSE (screen.velocity->isShowing());

    // The same kind on another channel, another controller on that channel, and back to the velocities.
    menu = screen.pianoRoll.getLaneMenu();
    CHECK (phazite::test::findMenuItem (menu, { "Sustain Pedal (CC 64)" })->isTicked);
    CHECK (phazite::test::findMenuItem (menu, { "Channel", "Channel 1" })->isTicked);
    REQUIRE (phazite::test::runMenuItem (menu, { "Channel", "Channel 2" }));
    CHECK (screen.pianoRoll.getLaneShown() == ControlLane { ControlKind::controller, 64, 2 });
    CHECK (chooser->getButtonText() == "Sustain Pedal (CC 64), Channel 2");
    REQUIRE (phazite::test::runMenuItem (screen.pianoRoll.getLaneMenu(), { "Other Controller", "Brightness (CC 74), Channel 2" }));
    CHECK (screen.pianoRoll.getLaneShown() == ControlLane { ControlKind::controller, 74, 2 });
    REQUIRE (phazite::test::runMenuItem (screen.pianoRoll.getLaneMenu(), { "Velocity" }));
    CHECK_FALSE (screen.pianoRoll.getLaneShown().has_value());
    CHECK (screen.velocity->isShowing());
    CHECK_FALSE (lane->isShowing());

    // Folded, the row shows the chooser still.
    auto* chevron = phazite::test::findComponentWithTitle (screen.pianoRoll, "Fold the lane");
    REQUIRE (chevron != nullptr);
    Mouse (screen.pianoRoll).click (screen.pianoRoll.getLocalArea (chevron, chevron->getLocalBounds()).getCentre());
    CHECK (screen.pianoRoll.isVelocityLaneFolded());
    CHECK (chooser->isShowing());
    CHECK_FALSE (screen.velocity->isShowing());
}

TEST_CASE ("A drag in a controller lane draws a value at each step of the grid, with Alt at each pixel, and a click sets one")
{
    LaneScreen screen;
    phazite::test::UndoCheck undo (screen.getSong());
    Mouse mouse (*screen.lane);

    // From near the top (the lane's top pixels are the line that sizes it), halfway through beat 1, down to the foot halfway through beat 4: a value at each beat passed, the first
    // where the press was, each lower.
    undo.check ("a draw on the grid",
                [&]
                {
                    mouse.press (screen.at (1.5, 120));
                    mouse.dragTo (screen.at (4.5, 0));
                    mouse.release();
                    mouse.pause();
                });
    CHECK (screen.describeUndo() == "Undo Draw Sustain Pedal");
    auto values = screen.values();
    REQUIRE (values.size() == 4);

    for (size_t i = 0; i < values.size(); ++i)
        CHECK (values[i].first == doctest::Approx (1.0 + (double) i));

    CHECK (values[0].second == doctest::Approx (120).epsilon (0.02));
    CHECK (std::is_sorted (values.begin(), values.end(), [] (const auto& a, const auto& b) { return a.second > b.second; }));
    CHECK (values.back().second < 40);

    // With Alt, off the grid: a value at each pixel passed (no closer than a 64th of a beat), rising.
    undo.check ("a draw off the grid",
                [&]
                {
                    mouse.press (screen.at (5.0, 30).translated (2, 0), juce::ModifierKeys::altModifier);
                    mouse.dragTo (screen.at (6.0, 110));
                    mouse.release();
                    mouse.pause();
                });
    values = screen.values();
    const auto offGrid =
        std::count_if (values.begin(), values.end(), [] (const auto& value) { return value.first > 5.0 && value.first < 6.1; });
    CHECK (offGrid > 8);
    CHECK (isSpacedAtLeastAGap (values));

    // A click: one value, in place of those in its step of the grid.
    undo.check ("a click", [&] { mouse.click (screen.at (5.5, 64)); });
    values = screen.values();
    CHECK (std::count_if (values.begin(), values.end(), [] (const auto& value) { return value.first >= 5.0 && value.first < 6.0; }) == 1);
    CHECK (std::find (values.begin(), values.end(), std::pair { 5.0, 64 }) != values.end());
    undo.walkBackAndForth();
}

TEST_CASE ("A right drag in a controller lane draws a straight ramp as the button comes up, and Escape takes a ramp or a draw back")
{
    LaneScreen screen;
    phazite::test::UndoCheck undo (screen.getSong());
    Mouse mouse (*screen.lane);

    // Shown while it is dragged, put in place as the button comes up: from the foot at beat 1 to the top at beat 3.
    undo.check ("a ramp",
                [&]
                {
                    mouse.press (screen.at (1.0, 0), juce::ModifierKeys::rightButtonModifier);
                    mouse.dragTo (screen.at (3.0, 127));
                    CHECK (screen.values().empty());
                    mouse.release();
                    mouse.pause();
                });
    CHECK (screen.describeUndo() == "Undo Draw Sustain Pedal");
    const auto ramp = screen.values();
    REQUIRE (ramp.size() > 60);
    CHECK (ramp.size() <= 129);
    CHECK (ramp.front() == std::pair { 1.0, 0 });
    CHECK (ramp.back().first == doctest::Approx (3.0));
    CHECK (ramp.back().second == 127);
    CHECK (isSpacedAtLeastAGap (ramp));

    // Escape in the middle of another ramp, and of a draw: as they were, nothing to undo or redo of them, and the mouse does
    // nothing more until it goes up.
    const auto escape = juce::KeyPress (juce::KeyPress::escapeKey);

    for (const auto right : { true, false })
    {
        CAPTURE (right);
        mouse.press (screen.at (2.0, 100), right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys());
        mouse.dragTo (screen.at (6.0, 10));
        CHECK (screen.pianoRoll.keyPressed (escape));
        mouse.dragTo (screen.at (7.0, 50));
        mouse.release();
        mouse.pause();
        CHECK (screen.values() == ramp);
        CHECK (screen.describeUndo() == "Undo Draw Sustain Pedal");
        CHECK_FALSE (screen.getSong().getUndoManager().canRedo());
    }

    undo.walkBackAndForth();
}

TEST_CASE ("The Delete tool's drag deletes a controller's values it passes over, and Delete All every one the clips shown play")
{
    LaneScreen screen;

    for (const auto& [beat, value] : { std::pair { 1.0, 10 }, std::pair { 2.0, 20 }, std::pair { 3.0, 30 }, std::pair { 5.0, 50 } })
        screen.add (beat, value);

    screen.add (2.5, 99, { ControlKind::controller, 64, 2 }); // another channel's
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (screen.getSong());
    Mouse mouse (*screen.lane);
    screen.pianoRoll.setTool (phazite::ui::Tool::erase);

    undo.check ("a delete across beats 1 to 3",
                [&]
                {
                    mouse.press (screen.at (1.5, 64));
                    mouse.dragTo (screen.at (3.5, 64));
                    mouse.release();
                    mouse.pause();
                });
    CHECK (screen.describeUndo() == "Undo Delete Sustain Pedal");
    CHECK (screen.values() == Values { { 5.0, 50 } });
    CHECK (screen.valuesOf (screen.clip, { ControlKind::controller, 64, 2 }) == Values { { 2.5, 99 } });

    // Delete All, from the chooser's menu, as one step; then there is nothing to delete.
    undo.check ("Delete All",
                [&] { REQUIRE (phazite::test::runMenuItem (screen.pianoRoll.getLaneMenu(), { "Delete All Sustain Pedal Values" })); });
    CHECK (screen.describeUndo() == "Undo Delete Sustain Pedal");
    CHECK (screen.values().empty());
    CHECK_FALSE (phazite::test::findMenuItem (screen.pianoRoll.getLaneMenu(), { "Delete All Sustain Pedal Values" })->isEnabled);
    undo.walkBackAndForth();
}

TEST_CASE ("A draw across two clips shown puts each value in the clip that plays there, and nothing where no clip plays")
{
    LaneScreen screen;
    const auto second = phazite::insertMidiClip (screen.getSong(), screen.tracks[0], { beatAt (10.0), beatAt (12.0) }, nullptr);
    REQUIRE (second.isValid());
    screen.pianoRoll.showClips ({ screen.clip, second });
    screen.pianoRoll.showLane (pedal);
    screen.pianoRoll.zoomTo ({ beatAt (0.0), beatAt (14.0) });
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (screen.getSong());
    Mouse mouse (*screen.lane);

    // Where no clip plays, the hint says so.
    mouse.moveTo (screen.at (9.0, 64));
    CHECK (screen.lane->getDescription() == "No clip plays here: values are drawn only where a clip plays");

    undo.check ("a draw across two clips",
                [&]
                {
                    mouse.press (screen.at (6.5, 120));
                    mouse.dragTo (screen.at (11.5, 20));
                    mouse.release();
                    mouse.pause();
                });
    const auto first = screen.values(), then = screen.valuesOf (second);
    REQUIRE (first.size() == 2);
    REQUIRE (then.size() == 2);
    CHECK (first[0].first == doctest::Approx (6.0));
    CHECK (first[1].first == doctest::Approx (7.0));
    CHECK (then[0].first == doctest::Approx (10.0));
    CHECK (then[1].first == doctest::Approx (11.0));
    CHECK (then[1].second < first[0].second);
    CHECK (phazite::getClipRange (screen.getSong(), screen.clip) == phazite::BeatRange (beatAt (0.0), beatAt (8.0))); // no clip grew
    undo.walkBackAndForth();
}

TEST_CASE ("A clip deleted by another edit in the middle of a draw in its lane: the draw goes on and ends, one step")
{
    LaneScreen screen;
    screen.add (2.0, 40);
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (screen.getSong());
    Mouse mouse (*screen.lane);

    undo.check ("a draw whose clip goes",
                [&]
                {
                    mouse.press (screen.at (1.5, 100));
                    mouse.dragTo (screen.at (3.5, 60));
                    phazite::deleteClips (screen.context, { screen.clip }, false);
                    phazite::test::deliverChanges();
                    mouse.dragTo (screen.at (6.5, 10));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (phazite::getClips (screen.context, screen.tracks[0]).empty());
    CHECK (screen.pianoRoll.getClips().empty());
    undo.walkBackAndForth();
}

TEST_CASE ("The hint bar says what a drag in a controller lane would draw, and where; a clip shown again opens on its lane")
{
    LaneScreen screen;
    Mouse mouse (*screen.lane);
    mouse.moveTo (screen.at (1.5, 64));
    CHECK (screen.lane->getDescription().startsWith ("Sustain Pedal 6"));
    CHECK (screen.lane->getDescription().endsWith (" at 1.2.1: drag to draw, right-drag for a straight ramp"));

    screen.pianoRoll.showLane (bend);
    mouse.moveTo (screen.at (1.5, 0));
    CHECK (screen.lane->getDescription().startsWith ("Pitch Bend -100% at 1.2.1"));
    screen.pianoRoll.showLane (ControlLane { ControlKind::programChange, 0, 1 });
    mouse.moveTo (screen.at (1.5, 64).translated (0, -1));
    CHECK (screen.lane->getDescription().startsWith ("Program "));
    CHECK (screen.lane->getDescription().contains (" at 1.2.1"));
    screen.pianoRoll.showLane (ControlLane { ControlKind::aftertouch, 0, 1 });
    mouse.moveTo (screen.at (1.5, 64));
    CHECK (screen.lane->getDescription().startsWith ("A note's pressure can't be drawn yet"));

    // A note's pressure isn't drawn: a drag changes nothing.
    phazite::addControl (screen.getSong(), screen.clip, beatAt (3.0), ControlKind::aftertouch, 60, 70, 1);
    screen.getSong().getUndoManager().clearUndoHistory();
    mouse.press (screen.at (2.5, 10));
    mouse.dragTo (screen.at (4.5, 10));
    mouse.release();
    CHECK_FALSE (screen.getSong().getUndoManager().canUndo());
    CHECK (screen.valuesOf (screen.clip, { ControlKind::aftertouch, 0, 1 }) == Values { { 3.0, 70 } });

    // Left on the pedal, the clip opens on it again; another clip, never shown, on the velocities; an alias on its link's.
    screen.pianoRoll.showLane (pedal);
    const auto other = phazite::insertMidiClip (screen.getSong(), screen.tracks[1], { beatAt (0.0), beatAt (4.0) }, nullptr);
    screen.selection().selectOnly (other);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.pianoRoll.getLaneShown().has_value());

    screen.selection().selectOnly (screen.clip);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.pianoRoll.getLaneShown() == pedal);

    const auto alias = phazite::copyClips (screen.context, { screen.clip }, true).front();
    phazite::moveClip (screen.context, alias, beatAt (16.0), screen.tracks[0]);
    screen.selection().selectOnly (other);
    phazite::test::dispatchPendingMessages();
    screen.selection().selectOnly (alias);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.pianoRoll.getClips() == std::vector { alias });
    CHECK (screen.pianoRoll.getLaneShown() == pedal);
}

TEST_CASE ("A controller lane reads a long take's 100,000 values once for each change of the song, not at each paint")
{
    // A long take's bend: a value at each 64th of a beat for over 1,500 beats.
    LaneScreen screen;
    const auto take = phazite::insertMidiClip (screen.getSong(), screen.tracks[1], { beatAt (0.0), beatAt (1600.0) }, nullptr);
    std::vector<phazite::ControlValue> many;

    for (int i = 0; i < 100000; ++i)
        many.push_back ({ beatAt (i / 64.0), ControlKind::pitchWheel, 0, (i * 37) % 16384, 1 });

    phazite::addControls (screen.getSong(), take, many);
    screen.pianoRoll.showClip (take);
    screen.pianoRoll.showLane (bend);
    screen.pianoRoll.zoomTo ({ beatAt (1000.0), beatAt (1008.0) });
    phazite::test::dispatchPendingMessages();

    // Painted again, it reads nothing from the song, which didn't change: what it draws was read once for it.
    const auto first = screen.lane->createComponentSnapshot (screen.lane->getLocalBounds());
    const auto before = phazite::countElementsVisited();
    const auto again = screen.lane->createComponentSnapshot (screen.lane->getLocalBounds());
    CHECK (phazite::countElementsVisited() == before);
    CHECK (first.getWidth() == again.getWidth());
}

TEST_CASE ("A step of a draw in a controller lane changes only the cells it drew another value in, and a still pointer nothing")
{
    LaneScreen screen;
    Mouse mouse (*screen.lane);
    auto& history = screen.getSong().getUndoManager();
    Heard heard (screen.getSong());

    // Off the grid (Alt), a cell is a pixel wide, or 1/64 of a beat where a pixel is less. Out to the right in steps of 4
    // pixels, the value going up and down, and back to the left on another line: at each report, the cells it passed, each
    // removed and added at most, and the cell after them; never what the drag passed before.
    const auto cellPixels = std::max (1.0, phazite::getShortestControlGap().inBeats() * screen.pianoRoll.getView().pixelsPerBeat);
    const auto mostChanges = 2 * ((int) std::ceil (4.0 / cellPixels) + 3);
    const auto start = screen.at (1.0, 40);
    mouse.press (start, juce::ModifierKeys::altModifier);
    std::vector<int> heardAtEach;

    const auto report = [&] (juce::Point<int> to)
    {
        const auto before = heard.valuesChanged;
        mouse.dragTo (to);
        heardAtEach.push_back (heard.valuesChanged - before);
        CHECK (heard.valuesChanged - before <= mostChanges);
    };

    for (int i = 1; i <= 60; ++i)
        report ({ start.x + i * 4, screen.yFor (40 + (i * 23) % 80) });

    for (int i = 59; i >= 0; --i)
        report ({ start.x + i * 4, screen.yFor (100 - (i * 17) % 70) });

    CAPTURE (mostChanges);
    CHECK (*std::max_element (heardAtEach.begin(), heardAtEach.end()) > 0);

    // The pointer still, where the last report left it: no change of the song, and nothing in the drag's step.
    const auto changes = heard.changes;
    const auto actions = history.getNumActionsInCurrentTransaction();
    mouse.dragTo ({ start.x, screen.yFor (100) });
    CHECK (heard.changes == changes);
    CHECK (history.getNumActionsInCurrentTransaction() == actions);
    mouse.release();
    mouse.pause();
    CHECK (isSpacedAtLeastAGap (screen.values()));
    CHECK (screen.describeUndo() == "Undo Draw Sustain Pedal");

    // On the grid, a pixel along within one step of it, at the same height: the same cell and value, so nothing.
    history.clearUndoHistory();
    mouse.press (screen.at (5.5, 90));
    const auto afterPress = heard.changes;
    mouse.dragTo (screen.at (5.5, 90).translated (1, 0));
    CHECK (heard.changes == afterPress);
    mouse.release();
    mouse.pause();
}

TEST_CASE ("A draw in a controller lane keeps its cells' length when the view zooms in the middle of it")
{
    LaneScreen screen;
    Mouse mouse (*screen.lane);

    // On a grid that follows the zoom: its step as the draw begins, at this zoom.
    auto grid = screen.pianoRoll.getGrid();
    grid.adaptive = true;
    screen.pianoRoll.setGrid (grid);
    const auto stepAtPress = screen.pianoRoll.getGridStepInBeats();
    mouse.press (screen.at (1.0, 100));

    // Zoomed in, the grid's step is shorter; the draw goes on in the cells it began with.
    screen.pianoRoll.zoomTo ({ beatAt (0.0), beatAt (4.0) });
    REQUIRE (screen.pianoRoll.getGridStepInBeats() < stepAtPress);
    mouse.dragTo (screen.at (3.9, 20));
    mouse.release();
    mouse.pause();
    REQUIRE (screen.values().size() > 1);

    for (const auto& [beat, value] : screen.values())
    {
        CAPTURE (beat);
        CHECK (std::abs (beat / stepAtPress - std::round (beat / stepAtPress)) < 1.0e-6);
    }
}

TEST_CASE ("The pitch bend is drawn back to none on its middle line, whose row gives its middle at an even and an odd height")
{
    for (const auto odd : { false, true })
    {
        CAPTURE (odd);
        LaneScreen screen (bend);

        for (int height = 80; height < 84 && (screen.lane->getHeight() % 2 == 1) != odd; ++height)
            screen.pianoRoll.setVelocityLane (height, false);

        const auto height = screen.lane->getHeight();
        REQUIRE ((height % 2 == 1) == odd);
        Mouse mouse (*screen.lane);

        // The middle line, the bend's rest, on a whole row: drawn there, sharp, the rows beside it as the lane's ground.
        const auto middle = juce::roundToInt (3.0f + ((float) height - 5.0f) * (1.0f - 8192.0f / 16383.0f));
        const auto image = screen.lane->createComponentSnapshot (screen.lane->getLocalBounds());
        const auto x = screen.xOf (1.3);
        CHECK (image.getPixelAt (x, middle) != image.getPixelAt (x, middle - 3));
        CHECK (image.getPixelAt (x, middle - 1) == image.getPixelAt (x, middle - 3));
        CHECK (image.getPixelAt (x, middle + 1) == image.getPixelAt (x, middle + 3));

        // A draw along it: none, 8192, held (the cells after leave it out as a repeat).
        mouse.press ({ screen.xOf (1.25), middle });
        mouse.dragTo ({ screen.xOf (3.75), middle });
        mouse.release();
        mouse.pause();
        CHECK (screen.valuesOf (screen.clip, bend) == Values { { 1.0, 8192 } });

        // A ramp from the top down to the middle line ends on none; the top row and the foot row give the bend's ends.
        mouse.press ({ screen.xOf (4.0), top }, juce::ModifierKeys::rightButtonModifier);
        mouse.dragTo ({ screen.xOf (5.0), middle });
        mouse.release();
        mouse.pause();
        auto values = screen.valuesOf (screen.clip, bend);
        REQUIRE (values.size() > 2);
        CHECK (values.back() == std::pair { 5.0, 8192 });
        CHECK (std::find (values.begin(), values.end(), std::pair { 4.0, 16383 }) != values.end());

        mouse.click ({ screen.xOf (6.5), top });
        mouse.click ({ screen.xOf (7.5), height - 1 });
        values = screen.valuesOf (screen.clip, bend);
        CHECK (std::find (values.begin(), values.end(), std::pair { 6.0, 16383 }) != values.end());
        CHECK (std::find (values.begin(), values.end(), std::pair { 7.0, 0 }) != values.end());
    }

    // The other lanes rest at their foot: its row and the top row give their ends, at either height.
    for (const auto height : { 80, 81 })
    {
        CAPTURE (height);
        LaneScreen screen;
        screen.pianoRoll.setVelocityLane (height, false);
        Mouse mouse (*screen.lane);
        mouse.click ({ screen.xOf (1.5), top });
        mouse.click ({ screen.xOf (2.5), screen.lane->getHeight() - 1 });
        mouse.click ({ screen.xOf (3.5), screen.lane->getHeight() - 2 });
        CHECK (screen.values() == Values { { 1.0, 127 }, { 2.0, 0 } });
    }
}

TEST_CASE ("A click on a controller's value with that value, or a draw out and back to the values there, leaves no undo step")
{
    LaneScreen screen;
    Mouse mouse (*screen.lane);
    auto& history = screen.getSong().getUndoManager();

    // The values there: what clicks at two heights give, at beats 1 and 2.
    const auto rowA = screen.yFor (100), rowB = screen.yFor (30);
    mouse.click ({ screen.xOf (1.5), rowA });
    mouse.click ({ screen.xOf (2.5), rowB });
    const auto there = screen.values();
    REQUIRE (there.size() == 2);
    history.clearUndoHistory();
    phazite::test::UndoCheck undo (screen.getSong());

    undo.check ("a click on the value there", [&] { mouse.click ({ screen.xOf (1.5), rowA }); });
    CHECK_FALSE (history.canUndo());
    CHECK (screen.values() == there);

    // Out over beats 2 and 3 at another value, and back over them at the one there: as it was, no step.
    undo.check ("a draw out and back",
                [&]
                {
                    mouse.press ({ screen.xOf (1.5), rowA });
                    mouse.dragTo ({ screen.xOf (3.5), screen.yFor (70) });
                    CHECK (screen.values() != there);
                    mouse.dragTo ({ screen.xOf (3.5), rowB });
                    mouse.dragTo ({ screen.xOf (2.5), rowB });
                    mouse.release();
                    mouse.pause();
                });
    CHECK_FALSE (history.canUndo());
    CHECK_FALSE (history.canRedo());
    CHECK (screen.values() == there);
    CHECK (undo.getNumSteps() == 0);

    // A draw that changes something is a step still.
    undo.check ("a draw", [&] { mouse.click ({ screen.xOf (3.5), screen.yFor (70) }); });
    CHECK (history.canUndo());
    undo.walkBackAndForth();
}

TEST_CASE ("The Delete tool's drag across a long take's bend, with its mod wheel between, removes the bend as one change of the clip")
{
    LaneScreen screen;
    const auto take = addLongTake (screen);
    const ControlLane modWheel { ControlKind::controller, 1, 1 };
    const auto modWheelBefore = screen.valuesOf (take, modWheel);
    REQUIRE (screen.valuesOf (take, bend).size() == 50000);
    screen.pianoRoll.setTool (phazite::ui::Tool::erase);
    Mouse mouse (*screen.lane);
    const auto middle = screen.lane->getHeight() / 2;
    mouse.press ({ 0, middle });

    // Everything in one report: one change of the clip's value (SongValues.h), not 50,000 removals.
    Heard heard (screen.getSong());
    mouse.dragTo ({ screen.lane->getWidth() - 1, middle });
    CHECK (heard.changes <= 4);
    mouse.release();
    mouse.pause();
    CHECK (screen.valuesOf (take, bend).empty());
    CHECK (screen.valuesOf (take, modWheel) == modWheelBefore);

    phazite::undo (screen.getSong());
    CHECK (screen.valuesOf (take, bend).size() == 50000);
    CHECK (screen.valuesOf (take, modWheel) == modWheelBefore);
}

TEST_CASE ("A controller lane draws at most one step for each pixel column, however many values are in view")
{
    // A long take's bend zoomed out: dozens of values a column.
    std::vector<phazite::ControlPoint> values;

    for (int i = 0; i < 100000; ++i)
        values.push_back ({ {}, beatAt (i / 64.0), (i * 37) % 16384, 0 });

    phazite::ui::TimelineView view;
    view.pixelsPerBeat = 1000.0 / 1600.0;
    const auto steps = phazite::ui::PianoRoll::getControlSteps (values, view, { 0, 1000 }, view.beatToColumn (beatAt (1600.0)));
    CHECK (steps.size() <= 1002);
    CHECK (steps.size() > 900);

    for (size_t i = 1; i < steps.size(); ++i)
        CHECK (steps[i].left > steps[i - 1].left);

    // A column's step holds its last value, spans its lowest to its highest, and has no dot among so many.
    REQUIRE (steps.size() > 10);
    const auto& tenth = steps[10];
    CHECK (tenth.lowest < tenth.highest);
    CHECK (tenth.value >= tenth.lowest);
    CHECK (tenth.value <= tenth.highest);
    CHECK_FALSE (tenth.dot);

    // Zoomed in, values a beat apart: each its own step, from its place to the next one's, with its dot.
    view.pixelsPerBeat = 40.0;
    view.start = beatAt (10.0);
    std::vector<phazite::ControlPoint> sparse;

    for (int i = 0; i < 40; ++i)
        sparse.push_back ({ {}, beatAt (i), i * 3, 0 });

    const auto apart = phazite::ui::PianoRoll::getControlSteps (sparse, view, { 0, 400 }, view.beatToColumn (beatAt (40.0)));
    REQUIRE (apart.size() == 12); // the one that holds as the view starts, at beat 9, to the one at beat 20 (at its right edge)
    CHECK (apart[1].left == view.beatToColumn (beatAt (10.0)));
    CHECK (apart[1].right == view.beatToColumn (beatAt (11.0)));
    CHECK (apart[1].value == 30);
    CHECK (apart[1].lowest == 27);
    CHECK (apart[1].highest == 30);
    CHECK (apart[1].dot);
}

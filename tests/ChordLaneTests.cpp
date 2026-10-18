// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane in the playlist (src/ui/playlist/ChordLane.h), its header, the Chord Progressions window
// (src/ui/shell/ProgressionsPanel.h), and the inspector's switch for parts that follow the chords, driven as the user
// would: the mouse on the lane, and the items of the menus it shows.
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/chords/ChordModel.h"
#include "model/chords/Progressions.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/Preview.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/mixer/Inspector.h"
#include "ui/playlist/ChordLane.h"
#include "ui/playlist/Playlist.h"
#include "ui/shell/ProgressionsPanel.h"

namespace
{
phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

const phazite::Progression& getProgression (const char* id)
{
    const auto* progression = phazite::findProgression (id);
    INFO (id);
    REQUIRE (progression != nullptr);
    return *progression;
}

// Runs a menu's item, found by the texts of the submenus that lead to it and its own.
bool runMenuItem (const juce::PopupMenu& menu, const std::vector<juce::String>& path)
{
    const auto* current = &menu;

    for (size_t level = 0; level < path.size(); ++level)
    {
        const juce::PopupMenu* next = nullptr;

        for (juce::PopupMenu::MenuItemIterator it (*current); it.next();)
        {
            const auto& item = it.getItem();

            if (item.text != path[level])
                continue;

            if (level + 1 == path.size())
            {
                if (item.action == nullptr || ! item.isEnabled)
                    return false;

                item.action();
                return true;
            }

            next = item.subMenu.get();
            break;
        }

        if (next == nullptr)
            return false;

        current = next;
    }

    return false;
}

// A playlist in a window, with two tracks, as the app shows it.
struct ChordScreen
{
    ChordScreen()
    {
        edit->ensureNumberOfAudioTracks (2);
        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        setEdit (context, edit.get());
        phazite::test::dispatchPendingMessages();

        lane = dynamic_cast<phazite::ui::ChordLane*> (phazite::test::findComponentWithTitle (playlist, "Chords"));
        header = dynamic_cast<phazite::ui::ChordLaneHeader*> (phazite::test::findComponentWithTitle (playlist, "Chord lane"));
        REQUIRE (lane != nullptr);
        REQUIRE (header != nullptr);
    }

    ~ChordScreen() { setEdit (context, nullptr); }

    juce::String names() const
    {
        juce::StringArray all;

        for (const auto& chord : phazite::getLaneChords (context))
            all.add (chord.name);

        return all.joinIntoString (" ");
    }

    // Where a beat is in the lane, halfway down it.
    juce::Point<int> at (double beats) const
    {
        return { juce::roundToInt (playlist.getView().beatToX (beat (beats))), lane->getHeight() / 2 };
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Playlist playlist { commands, context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
    phazite::ui::ChordLane* lane = nullptr;
    phazite::ui::ChordLaneHeader* header = nullptr;
};
} // namespace

TEST_CASE ("The chord lane: a click on empty space offers the key's chords, and adds one there, a bar long")
{
    ChordScreen screen;
    CHECK (screen.lane->getHeight() == phazite::ui::ChordLane::preferredHeight);
    CHECK (screen.lane->getDescription().startsWith ("The song's chords"));

    // In bar 2: the seven chords of C major, then the others in a submenu.
    const auto menu = screen.lane->getMenuAt (screen.at (5.0));
    REQUIRE (runMenuItem (menu, { "F    IV" }));
    CHECK (screen.names() == juce::String ("F"));

    const auto chords = phazite::getLaneChords (screen.context);
    REQUIRE (chords.size() == 1);
    CHECK (chords[0].range.getStart() == beat (4.0));
    CHECK (chords[0].range.getEnd() == beat (8.0));
    CHECK (phazite::describeUndo (screen.edit.get()) == juce::String ("Undo Add Chord"));

    // In the bar before, up to the chord after it; and one from outside the key.
    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (1.0)), { "Outside the Key", "Bb    bVII" }));
    CHECK (screen.names() == juce::String ("Bb F"));
}

TEST_CASE ("The chord lane: a chord's menu changes it, its kind and inversion, and deletes it")
{
    ChordScreen screen;
    phazite::applyProgression (screen.context, getProgression ("popAnthem"), { beat (0.0), beat (16.0) });
    phazite::test::dispatchPendingMessages();

    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (1.0)), { "Am    vi" }));
    CHECK (screen.names() == juce::String ("Am G Am F"));

    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (1.0)), { "Kind", "Am7" }));
    CHECK (screen.names() == juce::String ("Am7 G Am F"));

    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (1.0)), { "Inversion", "First Inversion: Am7/C" }));
    CHECK (screen.names() == juce::String ("Am7/C G Am F"));

    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (5.0)), { "Delete Chord" }));
    CHECK (screen.names() == juce::String ("Am7/C Am F"));
    CHECK (phazite::describeUndo (screen.edit.get()) == juce::String ("Undo Delete Chord"));

    // The key from a chord on.
    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (9.0)), { "Change Key from Here", "D", "Major" }));
    CHECK (screen.names() == juce::String ("Am7/C Bm G"));
    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.at (9.0)), { "Remove Key Change Here" }));
    CHECK (screen.names() == juce::String ("Am7/C Am F"));
}

TEST_CASE ("The chord lane: dragging a chord's right edge makes it longer, over the chord after it, as one undo step")
{
    ChordScreen screen;
    phazite::applyProgression (screen.context, getProgression ("popAnthem"), { beat (0.0), beat (16.0) });
    phazite::test::dispatchPendingMessages();

    phazite::test::Mouse mouse (*screen.lane);
    mouse.press (screen.at (4.0) - juce::Point<int> (2, 0));
    mouse.dragTo (screen.at (5.0));
    mouse.dragTo (screen.at (6.0));
    CHECK (screen.lane->getDragTarget() == beat (6.0));
    mouse.release();

    const auto chords = phazite::getLaneChords (screen.context);
    REQUIRE (chords.size() == 4);
    CHECK (chords[0].range.getEnd() == beat (6.0));
    CHECK (chords[1].range.getStart() == beat (6.0));
    CHECK (phazite::describeUndo (screen.edit.get()) == juce::String ("Undo Change Chord Length"));
    CHECK_FALSE (screen.lane->getDragTarget().has_value());
}

TEST_CASE ("The chord lane's header: the song's key, which the chords follow, and the parts made from them")
{
    ChordScreen screen;
    CHECK (screen.header->getKeyButton().getButtonText() == juce::String ("C major"));
    phazite::applyProgression (screen.context, getProgression ("popAnthem"), { beat (0.0), beat (16.0) });

    REQUIRE (runMenuItem (screen.header->getKeyMenu(), { "D", "Major" }));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.header->getKeyButton().getButtonText() == juce::String ("D major"));
    CHECK (screen.names() == juce::String ("D A Bm G"));

    const auto tracksBefore = te::getAudioTracks (*screen.edit).size();
    REQUIRE (runMenuItem (screen.header->getMoreMenu(), { "Make a Part from the Chords", "Bass", "Root and fifth" }));
    const auto tracks = te::getAudioTracks (*screen.edit);
    REQUIRE (tracks.size() == tracksBefore + 1);
    CHECK (tracks.getLast()->getName() == juce::String ("Bass"));
    REQUIRE (tracks.getLast()->getClips().size() == 1);
    CHECK (phazite::followsChords (screen.context, phazite::getClipID (*tracks.getLast()->getClips()[0])));
}

TEST_CASE ("The Chord Progressions window: browse by mood and genre, play one, and put it on the chord lane")
{
    const auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, edit.get());
    const auto previewer = phazite::createPreviewer (phazite::test::getEngine());

    {
        phazite::ui::ProgressionsPanel panel (context, *previewer);
        phazite::test::TestWindow window (panel, 640, 560);
        CHECK (panel.getShown().size() == phazite::getBuiltInProgressions().size());
        CHECK (panel.getList().getNumRowsOnScreen() > 3);

        // A mood and a genre narrow the list.
        panel.setFilter ("dark", "trap");
        CHECK (panel.getMoodBox().getText() == juce::String ("Dark"));
        CHECK (panel.getGenreBox().getText() == juce::String ("Trap"));
        REQUIRE_FALSE (panel.getShown().empty());

        for (const auto* progression : panel.getShown())
            CHECK ((progression->moods.contains ("dark") && progression->genres.contains ("trap")));

        CHECK (panel.getSelected() == panel.getShown().front());
        panel.setFilter ({}, {});

        // With nothing selected in an empty song: once from its start.
        panel.select (&getProgression ("popAnthem"));
        CHECK (panel.getTargets()
               == std::vector<phazite::ui::ProgressionsPanel::Target> { phazite::ui::ProgressionsPanel::Target::wholeSong });
        panel.apply();
        CHECK (phazite::getLaneChords (context).size() == 4);
        CHECK (phazite::getLaneChords (context).back().range.getEnd() == beat (16.0));

        // A selected section is where it goes first.
        auto* section = phazite::insertSection (*edit, te::BeatPosition::fromBeats (16.0),
                                                { .name = "Verse", .length = te::BeatDuration::fromBeats (8.0) });
        REQUIRE (section != nullptr);
        getSelection (context).selectOnly (section);
        CHECK (panel.getTargets().front() == phazite::ui::ProgressionsPanel::Target::selectedSections);
        panel.setTarget (phazite::ui::ProgressionsPanel::Target::selectedSections);
        panel.select (&getProgression ("twoDarkChords"));
        panel.getApplyButton().triggerClick();
        phazite::test::dispatchPendingMessages();

        const auto chords = phazite::getLaneChords (context);
        REQUIRE (chords.size() == 5);
        CHECK (chords[4].name == juce::String ("Am"));
        CHECK (chords[4].range.getStart() == beat (16.0));
        CHECK (chords[4].range.getEnd() == beat (24.0));

        // Play: the progression as a MIDI file in the previewer, in the key where it would go.
        panel.play();
        CHECK (previewer->getFile().getFileName() == juce::String ("Phazite chord progression.mid"));
        panel.stop();
    }

    setEdit (context, nullptr);
}

TEST_CASE ("The inspector shows whether a part made from the chords follows them, and turns it back on")
{
    const auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, edit.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };

    {
        phazite::applyProgression (context, getProgression ("popAnthem"), { beat (0.0), beat (16.0) });
        const auto part = phazite::makeChordPart (context, phazite::PartStyle::bassRoots, { beat (0.0), beat (16.0) });
        REQUIRE (part.has_value());

        phazite::ui::Inspector inspector (commands, context);
        phazite::test::TestWindow window (inspector, 320, 700);
        context.getSongSelection().selectOnly (*part);
        phazite::test::dispatchPendingMessages();
        CHECK (inspector.getShown() == juce::String ("Clip"));

        auto* toggle = dynamic_cast<juce::ToggleButton*> (phazite::test::findComponentWithTitle (inspector, "Follows the chords"));
        REQUIRE (toggle != nullptr);
        CHECK (toggle->isVisible());
        CHECK (toggle->getToggleState());

        // A note edited by hand: it stops following.
        auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, phazite::toTracktion (*part)));
        REQUIRE (clip != nullptr);
        {
            const phazite::UndoStep step (*edit, "Change Velocity");
            clip->getSequence().getNotes().getFirst()->setVelocity (50, &edit->getUndoManager());
        }
        phazite::test::dispatchPendingMessages();
        CHECK_FALSE (toggle->getToggleState());

        phazite::test::Mouse mouse (*toggle);
        mouse.click (toggle->getLocalBounds().getCentre());
        phazite::test::dispatchPendingMessages();
        CHECK (phazite::followsChords (context, *part));
        CHECK (toggle->getToggleState());
    }

    setEdit (context, nullptr);
}

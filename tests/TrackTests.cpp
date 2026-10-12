// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/Commands.h"
#include "model/Selection.h"
#include "model/Tracks.h"

namespace
{
// An Edit with tracks named A, B, C..., a selection, and the track commands.
struct Tracks
{
    explicit Tracks (int numTracks)
    {
        edit->ensureNumberOfAudioTracks (numTracks);

        for (int i = 0; i < numTracks; ++i)
            te::getAudioTracks (*edit)[i]->setName (juce::String::charToString ((juce::juce_wchar) ('A' + i)));

        edit->getUndoManager().clearUndoHistory();
        selection.edit = edit.get();
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addTrackCommands (registry, selection);
    }

    ~Tracks() { selection.deselectAll(); }

    te::Track* operator[] (const char* name) const
    {
        for (auto* track : phazite::getPlaylistTracks (*edit))
            if (track->getName() == name)
                return track;

        return nullptr;
    }

    juce::String getOrder() const
    {
        juce::StringArray names;

        for (auto* track : phazite::getPlaylistTracks (*edit))
            names.add (track->getName());

        return names.joinIntoString (" ");
    }

    juce::String getSelectedNames() const
    {
        juce::StringArray names;

        for (auto* track : phazite::getSelectedTracks (selection))
            names.add (track->getName());

        return names.joinIntoString (" ");
    }

    void select (std::initializer_list<const char*> names)
    {
        selection.deselectAll();

        for (const auto* name : names)
            selection.addToSelection ((*this)[name]);
    }

    juce::String getUndoName() const { return edit->getUndoManager().getUndoDescription(); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("The track palette has eight distinct, named colors")
{
    const auto& colours = phazite::getTrackColours();

    for (size_t i = 0; i < colours.size(); ++i)
    {
        CHECK (juce::String (colours[i].name).isNotEmpty());
        CHECK (colours[i].colour.isOpaque());

        for (auto j = i + 1; j < colours.size(); ++j)
            CHECK (colours[i].colour != colours[j].colour);
    }
}

TEST_CASE ("The playlist shows audio tracks and folders, but not the global tracks")
{
    Tracks tracks (1);
    auto& edit = *tracks.edit;
    edit.ensureTempoTrack();
    edit.ensureArrangerTrack();

    auto folder = edit.insertNewFolderTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr, false);
    REQUIRE (folder != nullptr);
    folder->setName ("Folder");
    auto inFolder = edit.insertNewAudioTrack (te::TrackInsertPoint (folder.get(), nullptr), nullptr);
    REQUIRE (inFolder != nullptr);
    inFolder->setName ("Inside");

    CHECK (tracks.getOrder() == juce::String ("A Folder Inside"));

    // A track without a color of its own shows its row's color from the palette.
    const auto& colours = phazite::getTrackColours();
    CHECK (phazite::getTrackColour (*tracks["A"], 0) == colours[0].colour);
    CHECK (phazite::getTrackColour (*tracks["A"], 9) == colours[1].colour);
    tracks["A"]->setColour (colours[4].colour);
    CHECK (phazite::getTrackColour (*tracks["A"], 0) == colours[4].colour);
}

TEST_CASE ("A new track goes below the selected one, in the next color, and is selected")
{
    Tracks tracks (2);
    const auto& colours = phazite::getTrackColours();

    // With nothing selected, at the bottom.
    REQUIRE (tracks.registry.invoke ("track.add"));
    CHECK (tracks.getUndoName() == juce::String ("Add Track"));
    auto added = phazite::getPlaylistTracks (*tracks.edit).getLast();
    added->setName ("New");
    CHECK (tracks.getOrder() == juce::String ("A B New"));
    CHECK (tracks.getSelectedNames() == juce::String ("New"));
    CHECK (added->getColour() == colours[2].colour); // B shows the palette's second color

    // Below the selected track.
    tracks.select ({ "A" });
    REQUIRE (tracks.registry.invoke ("track.add"));
    CHECK (phazite::getPlaylistTracks (*tracks.edit)[1]->getColour() == colours[1].colour);
    CHECK (phazite::getSelectedTracks (tracks.selection).getFirst() == phazite::getPlaylistTracks (*tracks.edit)[1]);

    phazite::undo (*tracks.edit);
    CHECK (tracks.getOrder() == juce::String ("A B New"));
}

TEST_CASE ("Deleting tracks is one undo step, and selects the track that takes their place")
{
    Tracks tracks (4);
    tracks.select ({ "B", "C" });
    CHECK (phazite::getCurrentName (*tracks.registry.find ("edit.delete")) == juce::String ("Delete 2 Tracks"));

    REQUIRE (tracks.registry.invoke ("edit.delete"));
    CHECK (tracks.getOrder() == juce::String ("A D"));
    CHECK (tracks.getSelectedNames() == juce::String ("D"));
    CHECK (tracks.getUndoName() == juce::String ("Delete 2 Tracks"));

    phazite::undo (*tracks.edit);
    CHECK (tracks.getOrder() == juce::String ("A B C D"));

    // The last track: the one above it is selected.
    tracks.select ({ "D" });
    REQUIRE (tracks.registry.invoke ("edit.delete"));
    CHECK (tracks.getSelectedNames() == juce::String ("C"));

    // Nothing selected: nothing to delete.
    tracks.selection.deselectAll();
    CHECK_FALSE (tracks.registry.invoke ("edit.delete"));
}

TEST_CASE ("Duplicating a track puts the copy right after it")
{
    Tracks tracks (2);
    tracks.select ({ "A" });

    REQUIRE (tracks.registry.invoke ("edit.duplicate"));
    CHECK (tracks.getOrder() == juce::String ("A A B"));
    CHECK (tracks.getUndoName() == juce::String ("Duplicate Track"));

    const auto copy = phazite::getPlaylistTracks (*tracks.edit)[1];
    CHECK (phazite::getSelectedTracks (tracks.selection) == juce::Array<te::Track*> { copy });
}

TEST_CASE ("Tracks move up and down, and selected neighbours move together")
{
    Tracks tracks (4);
    tracks.select ({ "B", "C" });

    REQUIRE (tracks.registry.invoke ("track.moveUp"));
    CHECK (tracks.getOrder() == juce::String ("B C A D"));
    CHECK (tracks.getUndoName() == juce::String ("Move Track Up"));

    REQUIRE (tracks.registry.invoke ("track.moveUp")); // already at the top
    CHECK (tracks.getOrder() == juce::String ("B C A D"));

    REQUIRE (tracks.registry.invoke ("track.moveDown"));
    CHECK (tracks.getOrder() == juce::String ("A B C D"));

    tracks.select ({ "D" });
    REQUIRE (tracks.registry.invoke ("track.moveDown")); // already at the bottom
    CHECK (tracks.getOrder() == juce::String ("A B C D"));
}

TEST_CASE ("The arrow-key commands step through the tracks")
{
    Tracks tracks (3);

    REQUIRE (tracks.registry.invoke ("track.selectNext")); // nothing selected: the first
    CHECK (tracks.getSelectedNames() == juce::String ("A"));
    REQUIRE (tracks.registry.invoke ("track.selectNext"));
    CHECK (tracks.getSelectedNames() == juce::String ("B"));
    REQUIRE (tracks.registry.invoke ("track.selectPrevious"));
    REQUIRE (tracks.registry.invoke ("track.selectPrevious"));
    CHECK (tracks.getSelectedNames() == juce::String ("A")); // stays at the top

    REQUIRE (tracks.registry.invoke ("edit.selectAll"));
    CHECK (tracks.getSelectedNames() == juce::String ("A B C"));
    REQUIRE (tracks.registry.invoke ("edit.deselectAll"));
    CHECK (tracks.getSelectedNames().isEmpty());
}

TEST_CASE ("Mute and solo switch the selected tracks on together, then off")
{
    Tracks tracks (3);
    const auto* mute = tracks.registry.find ("track.mute");
    REQUIRE (mute != nullptr);

    tracks["A"]->setMute (true);
    tracks.select ({ "A", "B" });
    CHECK_FALSE (mute->isTicked());

    REQUIRE (tracks.registry.invoke ("track.mute")); // not all were muted: now all are
    CHECK (tracks["A"]->isMuted (false));
    CHECK (tracks["B"]->isMuted (false));
    CHECK (mute->isTicked());

    REQUIRE (tracks.registry.invoke ("track.mute"));
    CHECK_FALSE (tracks["A"]->isMuted (false));
    CHECK_FALSE (tracks["B"]->isMuted (false));

    REQUIRE (tracks.registry.invoke ("track.solo"));
    CHECK (tracks["A"]->isSolo (false));
    CHECK (tracks["B"]->isSolo (false));
    CHECK_FALSE (tracks["C"]->isSolo (false));
}

TEST_CASE ("Track heights stay within limits, and change for the selection or all tracks")
{
    Tracks tracks (2);
    auto& a = *tracks["A"];
    auto& b = *tracks["B"];

    CHECK (phazite::getTrackHeight (a) == phazite::TrackHeight::standard);
    phazite::setTrackHeight (a, 100000);
    CHECK (phazite::getTrackHeight (a) == phazite::TrackHeight::maximum);
    phazite::setTrackHeight (a, 0);
    CHECK (phazite::getTrackHeight (a) == phazite::TrackHeight::minimum);
    CHECK_FALSE (tracks.edit->getUndoManager().canUndo()); // heights aren't undo steps

    tracks.select ({ "B" });
    REQUIRE (tracks.registry.invoke ("track.taller"));
    CHECK (phazite::getTrackHeight (b) == phazite::TrackHeight::standard + phazite::TrackHeight::step);
    CHECK (phazite::getTrackHeight (a) == phazite::TrackHeight::minimum);

    tracks.selection.deselectAll();
    REQUIRE (tracks.registry.invoke ("track.shorter"));
    CHECK (phazite::getTrackHeight (b) == phazite::TrackHeight::standard);
    CHECK (phazite::getTrackHeight (a) == phazite::TrackHeight::minimum);
}

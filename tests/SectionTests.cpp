// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestClips.h"
#include "TestHelpers.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/TrackColours.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/transport/internal/Tempo.h"

#include <set>

namespace
{
phazite::BeatPosition at (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

phazite::BeatDuration lasting (double b)
{
    return phazite::BeatDuration::fromBeats (b);
}

// A song with two tracks, the selection, section and outline commands, and the electronic outline's sections:
// Intro 0-64, Build-up 64-96, Drop 96-160, Breakdown 160-224, Build-up 224-256, Drop 256-320, Outro 320-384
// (in beats; 4/4 at 120 BPM, so a beat is half a second).
struct Song
{
    Song()
    {
        tracks = phazite::ensureTracks (*song, 2);
        selection.setSong (song.get());
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addSectionCommands (registry, selection);
        phazite::addOutlineCommands (registry, selection);

        if (const auto* outline = phazite::findOutline ("electronic"))
            phazite::applyOutline (*song, *outline);

        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { selection.deselectAll(); }

    phazite::Song& getSong() const { return *song; }
    phazite::TrackID getTrack (int track) const { return tracks[(size_t) track]; }

    phazite::ClipID addClip (int track, double start, double end, const juce::String& name)
    {
        const auto clip = phazite::test::addMidiClip (getSong(), getTrack (track), { at (start), at (end) }, name);
        REQUIRE (clip.isValid());
        return clip;
    }

    // The clip that starts at a beat on a track: none if no clip does.
    phazite::ClipID findClip (int track, double start) const
    {
        for (const auto clip : phazite::getClips (getSong(), getTrack (track)))
            if (std::abs (phazite::getClipRange (getSong(), clip).getStart().inBeats() - start) < 1.0e-6)
                return clip;

        return {};
    }

    // A clip's name, where it ends, and how many clips a track has.
    juce::String getClipName (phazite::ClipID clip) const
    {
        return phazite::findClipState (getSong(), clip)[phazite::schema::clipName].toString();
    }

    double getClipEnd (phazite::ClipID clip) const { return phazite::getClipRange (getSong(), clip).getEnd().inBeats(); }
    int getNumClips (int track) const { return (int) phazite::getClips (getSong(), getTrack (track)).size(); }

    phazite::Section section (int index) const
    {
        const auto sections = phazite::getSections (*song);
        REQUIRE (index < (int) sections.size());
        return sections[(size_t) index];
    }

    phazite::SectionID id (int index) const { return section (index).id; }

    juce::String getSectionNames() const
    {
        juce::StringArray names;

        for (const auto& s : phazite::getSections (*song))
            names.add (s.name);

        return names.joinIntoString (", ");
    }

    phazite::Song::Ptr song = phazite::createSong();
    std::vector<phazite::TrackID> tracks;
    phazite::Selection selection;
    phazite::RangeSelection range { selection };
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("The built-in outlines are the design's four and four more, with their section lengths")
{
    const auto& outlines = phazite::getBuiltInOutlines();
    REQUIRE (outlines.size() == 8);

    const auto barsOf = [] (const phazite::Outline& outline)
    {
        juce::StringArray bars;

        for (const auto& section : outline.sections)
            bars.add (juce::String (section.bars));

        return bars.joinIntoString (" ");
    };

    CHECK (outlines[0].id == juce::String ("pop"));
    CHECK (barsOf (outlines[0]) == juce::String ("4 8 4 8 8 4 8 8 16 4"));
    CHECK (outlines[1].id == juce::String ("rock"));
    CHECK (barsOf (outlines[1]) == juce::String ("8 16 8 16 8 8 8 16 8"));
    CHECK (outlines[2].id == juce::String ("electronic"));
    CHECK (barsOf (outlines[2]) == juce::String ("16 8 16 16 8 16 16"));
    CHECK (outlines[3].id == juce::String ("hipHop"));
    CHECK (barsOf (outlines[3]) == juce::String ("4 8 16 8 16 8 4"));
    CHECK (outlines[4].id == juce::String ("lofi"));
    CHECK (outlines[5].id == juce::String ("rnb"));
    CHECK (outlines[6].id == juce::String ("drumAndBass"));
    CHECK (outlines[7].id == juce::String ("techno"));

    for (const auto& outline : outlines)
    {
        CAPTURE (outline.id.toStdString());

        // The description ends with how long the song is, which the sections add up to.
        auto total = 0;

        for (const auto& section : outline.sections)
            total += section.bars;

        CHECK (outline.description.endsWith (" " + juce::String (total) + " bars."));

        for (const auto& section : outline.sections)
        {
            CHECK (section.role.isNotEmpty());
            CHECK (section.tip.isNotEmpty());
            CHECK (section.energy >= 0.0f);
            CHECK (section.energy <= 1.0f);
        }
    }

    // Files that are missing what an outline needs aren't outlines.
    CHECK_FALSE (phazite::Outline::fromJSON (R"({ "name": "No id", "sections": [ { "name": "A", "bars": 4 } ] })").has_value());
    CHECK_FALSE (phazite::Outline::fromJSON (R"({ "id": "a", "name": "A", "sections": [ { "name": "A", "bars": 0 } ] })").has_value());
    CHECK_FALSE (phazite::Outline::fromJSON (R"({ "id": "a", "name": "A", "sections": [] })").has_value());
    CHECK (phazite::Outline::fromJSON (R"({ "id": "a", "name": "A", "sections": [ { "name": "A", "bars": 4 } ] })").has_value());

    // A section's color is a palette color's name, whatever its case; another name gives none, so the role's is used.
    const auto coloured = phazite::Outline::fromJSON (R"({ "id": "a", "name": "A", "sections": [
        { "name": "A", "bars": 4, "colour": "sky BLUE" }, { "name": "B", "bars": 4, "colour": "Magenta" } ] })");
    REQUIRE (coloured.has_value());
    REQUIRE (coloured->sections.size() == 2);
    CHECK (coloured->sections[0].colour == phazite::findTrackColour ("Sky blue"));
    CHECK_FALSE (coloured->sections[1].colour.has_value());
}

TEST_CASE ("Applying the electronic outline creates its seven sections at the project tempo")
{
    auto song = phazite::createSong();
    phazite::setTempo (*song, 128.0);

    const auto* outline = phazite::findOutline ("electronic");
    REQUIRE (outline != nullptr);
    song->getUndoManager().beginNewTransaction ("Apply Outline");
    phazite::applyOutline (*song, *outline);

    const auto sections = phazite::getSections (*song);
    REQUIRE (sections.size() == 7);

    const char* names[] = { "Intro", "Build-up", "Drop", "Breakdown", "Build-up", "Drop", "Outro" };
    const int bars[] = { 16, 8, 16, 16, 8, 16, 16 };
    auto bar = 0;

    for (int i = 0; i < 7; ++i)
    {
        CAPTURE (i);
        const auto& section = sections[(size_t) i];
        CHECK (section.name == juce::String (names[i]));
        CHECK (section.range.getStart().inBeats() == doctest::Approx (bar * 4.0));
        CHECK (section.range.getLength().inBeats() == doctest::Approx (bars[i] * 4.0));

        // At 128 BPM a beat lasts 60/128 seconds.
        CHECK (phazite::toSeconds (*song, section.range.getStart()) == doctest::Approx (bar * 4.0 * 60.0 / 128.0));
        CHECK (section.tip.isNotEmpty());
        bar += bars[i];
    }

    CHECK (sections[2].role == juce::String ("drop"));
    CHECK (sections[2].energy == doctest::Approx (1.0));
    CHECK (sections[2].colour == phazite::getRoleColour ("drop"));
    CHECK (phazite::getAppliedOutline (*song) == juce::String ("electronic"));

    // Another outline replaces them, and undo brings them back.
    song->getUndoManager().beginNewTransaction ("Apply Outline");
    phazite::applyOutline (*song, *phazite::findOutline ("pop"));
    CHECK (phazite::getSections (*song).size() == 10);

    phazite::undo (*song);
    CHECK (phazite::getSections (*song).size() == 7);
    phazite::undo (*song);
    CHECK (phazite::getSections (*song).empty());
}

TEST_CASE ("Outlines by id")
{
    Song song; // with the electronic outline's sections
    auto& s = song.getSong();

    // An id no outline has changes nothing.
    const auto before = s.getState().createCopy();
    CHECK_FALSE (phazite::applyOutline (s, "unknown"));
    CHECK (s.getState().isEquivalentTo (before));
    CHECK (phazite::getAppliedOutline (s) == juce::String ("electronic"));

    // Pop's id lays out pop's sections, one after the other, in bars of 4 beats.
    const auto* pop = phazite::findOutline ("pop");
    REQUIRE (pop != nullptr);
    REQUIRE (phazite::applyOutline (s, "pop"));

    const auto sections = phazite::getSections (s);
    REQUIRE (sections.size() == pop->sections.size());
    auto bar = 0;

    for (size_t i = 0; i < sections.size(); ++i)
    {
        CAPTURE (i);
        const auto& expected = pop->sections[i];
        CHECK (sections[i].name == expected.name);
        CHECK (sections[i].range.getStart().inBeats() == doctest::Approx (bar * 4.0));
        CHECK (sections[i].range.getLength().inBeats() == doctest::Approx (expected.bars * 4.0));
        bar += expected.bars;
    }

    // It is the outline applied while the song has sections.
    CHECK (phazite::isOutlineApplied (s, "pop"));
    CHECK_FALSE (phazite::isOutlineApplied (s, "electronic"));

    phazite::removeSections (s);

    CHECK (phazite::getAppliedOutline (s) == juce::String ("pop"));
    CHECK_FALSE (phazite::isOutlineApplied (s, "pop"));

    // The command that applies it.
    CHECK (phazite::getOutlineCommandID ("pop") == juce::String ("section.outline.pop"));
    CHECK (song.registry.find (phazite::getOutlineCommandID ("pop")) != nullptr);
}

TEST_CASE ("Moving the Drop moves everything in it")
{
    Song song;
    song.addClip (0, 96, 160, "Riff"); // the first Drop, exactly
    song.addClip (1, 80, 112, "Fill"); // from the Build-up into the Drop
    song.addClip (0, 160, 176, "Pad"); // in the Breakdown
    song.getSong().getUndoManager().beginNewTransaction ("Move Section");

    // Before the first Build-up.
    phazite::moveSection (song.getSong(), song.id (2), at (64));

    CHECK (song.getSectionNames() == juce::String ("Intro, Drop, Build-up, Breakdown, Build-up, Drop, Outro"));
    CHECK (song.section (1).range.getStart().inBeats() == doctest::Approx (64.0));
    CHECK (song.section (1).range.getEnd().inBeats() == doctest::Approx (128.0));
    CHECK (song.section (2).range.getStart().inBeats() == doctest::Approx (128.0));

    // The riff went with the Drop; the fill was split at the Drop's start, and each part went with its section.
    const auto riff = song.findClip (0, 64);
    REQUIRE (riff.isValid());
    CHECK (song.getClipName (riff) == juce::String ("Riff"));
    CHECK (song.getClipEnd (riff) == doctest::Approx (128.0));

    REQUIRE (song.findClip (1, 64).isValid()); // the Fill's part in the Drop
    CHECK (song.getClipEnd (song.findClip (1, 64)) == doctest::Approx (80.0));
    REQUIRE (song.findClip (1, 144).isValid()); // its part in the Build-up
    CHECK (song.getClipEnd (song.findClip (1, 144)) == doctest::Approx (160.0));

    CHECK (song.findClip (0, 160).isValid()); // the Breakdown didn't move

    // One undo step puts it all back.
    phazite::undo (song.getSong());
    CHECK (song.getSectionNames() == juce::String ("Intro, Build-up, Drop, Breakdown, Build-up, Drop, Outro"));
    CHECK (song.findClip (0, 96).isValid());
    CHECK (song.findClip (1, 80).isValid());
    CHECK (song.getNumClips (1) == 1);

    SUBCASE ("and moving a section later puts it before the destination")
    {
        // The first Build-up, to the Breakdown's start: after the Drop.
        phazite::moveSection (song.getSong(), song.id (1), at (160));
        CHECK (song.getSectionNames() == juce::String ("Intro, Drop, Build-up, Breakdown, Build-up, Drop, Outro"));
        CHECK (song.findClip (0, 64).isValid()); // the riff, earlier by the Build-up's length
        CHECK (song.findClip (0, 160).isValid());
    }
}

TEST_CASE ("Duplicating a section copies what is in it, as aliases if asked")
{
    Song song;
    const auto riff = song.addClip (0, 96, 160, "Riff");
    phazite::test::addMidiNote (song.getSong(), riff, 60, at (0), lasting (1.0), 100);
    song.addClip (0, 160, 176, "Pad");

    const auto copy = phazite::getSection (song.getSong(), phazite::duplicateSection (song.getSong(), song.id (2), true));
    REQUIRE (copy.has_value());
    CHECK (copy->name == juce::String ("Drop"));
    CHECK (copy->range.getStart().inBeats() == doctest::Approx (160.0));
    CHECK (song.getSectionNames() == juce::String ("Intro, Build-up, Drop, Drop, Breakdown, Build-up, Drop, Outro"));

    // The riff's copy is an alias of it, and what came after moved later.
    const auto riffCopy = song.findClip (0, 160);
    REQUIRE (riffCopy.isValid());
    CHECK (song.getClipName (riffCopy) == juce::String ("Riff"));
    CHECK (phazite::hasAliases (song.getSong(), { riffCopy }));
    CHECK (phazite::hasAliases (song.getSong(), { riff }));
    CHECK (song.findClip (0, 224).isValid()); // the Pad
    CHECK (song.section (4).range.getStart().inBeats() == doctest::Approx (224.0));

    // A plain copy isn't linked.
    REQUIRE (phazite::copySection (song.getSong(), song.id (0), at (0), false).isValid());
    CHECK (song.getSectionNames().startsWith ("Intro, Intro, Build-up"));
}

TEST_CASE ("Duplicating a section moves what comes after it, even after a clip across its edge is split")
{
    // Splitting the Fill puts its new part at the end of the track's list of clips, after the Lead: inserting
    // the copy's time must still move the Lead.
    Song song;
    song.addClip (1, 80, 112, "Fill");  // from the Build-up into the Drop
    song.addClip (1, 176, 192, "Lead"); // in the Breakdown

    REQUIRE (phazite::duplicateSection (song.getSong(), song.id (2), false).isValid());
    CHECK (song.findClip (1, 240).isValid()); // the Lead, a Drop later
    CHECK_FALSE (song.findClip (1, 176).isValid());
    CHECK (song.findClip (1, 160).isValid()); // the Fill's part in the Drop, copied
    CHECK (song.getClipEnd (song.findClip (1, 160)) == doctest::Approx (176.0));
}

TEST_CASE ("Deleting a section deletes what is in it, and the rest of the song closes up")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");
    song.addClip (1, 80, 112, "Fill");
    song.addClip (0, 160, 176, "Pad");

    phazite::deleteSection (song.getSong(), song.id (2));

    CHECK (song.getSectionNames() == juce::String ("Intro, Build-up, Breakdown, Build-up, Drop, Outro"));
    CHECK (song.section (0).range.getEnd().inBeats() == doctest::Approx (64.0)); // sections before it are left alone
    CHECK (song.section (1).range.getEnd().inBeats() == doctest::Approx (96.0));
    CHECK (song.section (2).range.getStart().inBeats() == doctest::Approx (96.0));
    CHECK (song.getNumClips (0) == 1);       // the Riff went
    CHECK (song.findClip (0, 96).isValid()); // the Pad came back to the Breakdown's new start

    REQUIRE (song.findClip (1, 80).isValid()); // the Fill lost its part in the Drop
    CHECK (song.getClipEnd (song.findClip (1, 80)) == doctest::Approx (96.0));
}

TEST_CASE ("Resizing a section moves the rest of the song, unless it is resized alone")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");

    // The first Build-up, from 8 bars to 16: everything after it moves 8 bars later.
    phazite::resizeSection (song.getSong(), song.id (1), lasting (64.0), true);
    CHECK (song.section (1).range.getEnd().inBeats() == doctest::Approx (128.0));
    CHECK (song.section (2).range.getStart().inBeats() == doctest::Approx (128.0));
    CHECK (song.findClip (0, 128).isValid());

    // Back to 8 bars: the time is taken out again.
    phazite::resizeSection (song.getSong(), song.id (1), lasting (32.0), true);
    CHECK (song.section (2).range.getStart().inBeats() == doctest::Approx (96.0));
    CHECK (song.findClip (0, 96).isValid());

    // Alone, it can't grow into the next section, and nothing else moves.
    phazite::resizeSection (song.getSong(), song.id (1), lasting (64.0), false);
    CHECK (song.section (1).range.getEnd().inBeats() == doctest::Approx (96.0));
    phazite::resizeSection (song.getSong(), song.id (1), lasting (16.0), false);
    CHECK (song.section (1).range.getEnd().inBeats() == doctest::Approx (80.0));
    CHECK (song.section (2).range.getStart().inBeats() == doctest::Approx (96.0));
    CHECK (song.findClip (0, 96).isValid());
}

TEST_CASE ("Inserting and removing time stretch the sections they fall inside")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");

    // 8 beats inside the first Drop: it gets longer, the clip is split, and later sections move.
    phazite::insertTime (song.getSong(), at (100), lasting (8.0));
    CHECK (phazite::getSections (song.getSong()).size() == 7);
    CHECK (song.section (2).range.getEnd().inBeats() == doctest::Approx (168.0));
    CHECK (song.section (3).range.getStart().inBeats() == doctest::Approx (168.0));
    CHECK (song.findClip (0, 108).isValid());

    phazite::removeTime (song.getSong(), { at (100), at (108) });
    CHECK (song.section (2).range.getEnd().inBeats() == doctest::Approx (160.0));
    CHECK (song.section (3).range.getStart().inBeats() == doctest::Approx (160.0));
    CHECK (song.findClip (0, 100).isValid());

    // Sections before the range are left alone.
    CHECK (song.section (0).range.getEnd().inBeats() == doctest::Approx (64.0));
    CHECK (song.section (1).range.getEnd().inBeats() == doctest::Approx (96.0));

    // Removing a range that covers a whole section deletes it.
    phazite::removeTime (song.getSong(), { at (64), at (96) });
    CHECK (song.getSectionNames() == juce::String ("Intro, Drop, Breakdown, Build-up, Drop, Outro"));
}

TEST_CASE ("The section commands")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");
    auto& undoManager = song.getSong().getUndoManager();

    song.selection.selectOnly (song.id (2));
    CHECK (phazite::getSelectedClips (song.selection).empty()); // a section isn't a clip
    CHECK (phazite::getCurrentName (*song.registry.find ("edit.delete")) == juce::String ("Delete Section"));
    CHECK (phazite::getCurrentName (*song.registry.find ("edit.duplicateAsAlias")) == juce::String ("Duplicate Section as Aliases"));

    REQUIRE (song.registry.invoke ("edit.duplicate"));
    CHECK (phazite::getSections (song.getSong()).size() == 8);
    CHECK (undoManager.getUndoDescription() == juce::String ("Duplicate Section"));
    CHECK (phazite::getSelectedSections (song.selection).front().range.getStart().inBeats() == doctest::Approx (160.0));

    REQUIRE (song.registry.invoke ("edit.delete"));
    CHECK (phazite::getSections (song.getSong()).size() == 7);
    CHECK (song.findClip (0, 96).isValid());

    // Add Section goes after the last one, 8 bars long.
    REQUIRE (song.registry.invoke ("section.add"));
    REQUIRE (phazite::getSections (song.getSong()).size() == 8);
    CHECK (song.section (7).range.getStart().inBeats() == doctest::Approx (384.0));
    CHECK (song.section (7).range.getLength().inBeats() == doctest::Approx (32.0));

    // Each outline is a command, ticked while it is the one applied.
    CHECK (song.registry.find ("section.outline.electronic")->isTicked());
    REQUIRE (song.registry.invoke ("section.outline.hipHop"));
    CHECK (phazite::getSections (song.getSong()).size() == 7);
    CHECK (song.registry.find ("section.outline.hipHop")->isTicked());
    CHECK (undoManager.getUndoDescription() == juce::String ("Apply Outline: Hip-hop and trap"));
}

TEST_CASE ("Deleting and duplicating sections directly")
{
    Song song;
    const auto count = [&song] { return phazite::getSections (song.getSong()).size(); };
    REQUIRE (count() == 7);

    // Each copy goes right after its original, the last first, so that copying one doesn't move another still to be
    // copied; with a selection, the copies are selected instead of what was. Intro 0-64 and its copy 64-128,
    // Build-up 128-160, Drop 160-224 and its copy 224-288, and the rest 64 beats later.
    song.selection.selectOnly (song.tracks[0]);
    const auto copies = phazite::duplicateSections (song.getSong(), { song.id (0), song.id (2) }, false, &song.selection);
    REQUIRE (copies.size() == 2);
    CHECK (count() == 9);
    CHECK (phazite::getSelectedSections (song.selection).size() == 2);
    CHECK_FALSE (song.selection.isSelected (song.tracks[0]));
    CHECK (song.section (1).range.getStart().inBeats() == doctest::Approx (64.0));
    CHECK (song.section (4).range.getStart().inBeats() == doctest::Approx (224.0));

    // Deleting them, the last first, puts the song back as it was.
    phazite::deleteSections (song.getSong(), { song.id (1), song.id (4) });
    CHECK (count() == 7);
    CHECK (song.section (1).range.getStart().inBeats() == doctest::Approx (64.0));  // the build-up
    CHECK (song.section (3).range.getStart().inBeats() == doctest::Approx (160.0)); // the breakdown
    CHECK (song.section (6).range.getEnd().inBeats() == doctest::Approx (384.0));

    // No sections: nothing is copied.
    song.selection.selectOnly (song.tracks[0]);
    CHECK (phazite::duplicateSections (song.getSong(), {}, false, &song.selection).empty());
    CHECK (song.selection.isSelected (song.tracks[0]));
}

TEST_CASE ("A new section goes after the last one, 8 bars long and named in turn")
{
    Song song; // seven sections, to beat 384
    CHECK (phazite::getNewSectionName (song.getSong()) == juce::String ("Section 8"));

    const auto added = phazite::getSection (song.getSong(), phazite::appendSection (song.getSong(), &song.selection));
    REQUIRE (added.has_value());
    CHECK (added->name == juce::String ("Section 8"));
    CHECK (added->range.getStart().inBeats() == doctest::Approx (384.0));
    CHECK (added->range.getEnd().inBeats() == doctest::Approx (384.0 + 4 * phazite::barsPerNewSection));
    CHECK (song.selection.isSelected (added->id));
    CHECK (phazite::getNewSectionName (song.getSong()) == juce::String ("Section 9"));

    // In a song without sections, at its start.
    auto empty = phazite::createSong();
    const auto first = phazite::getSection (*empty, phazite::appendSection (*empty, nullptr));
    REQUIRE (first.has_value());
    CHECK (first->name == juce::String ("Section 1"));
    CHECK (first->range.getStart().inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("The range several sections cover")
{
    Song song;
    CHECK (phazite::getSectionsRange ({}).isEmpty());

    // The intro (0-64) and the first drop (96-160), in any order: from beat 0 to beat 160.
    const auto range = phazite::getSectionsRange ({ song.section (2), song.section (0) });
    CHECK (range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (range.getEnd().inBeats() == doctest::Approx (160.0));
}

TEST_CASE ("Sections are kept in the project itself, with IDs no track, clip or other section has, even after an undo")
{
    Song song;
    auto& s = song.getSong();

    // In the song's own element for them.
    CHECK (s.getState().getChildWithName (phazite::schema::SECTIONS).getNumChildren() == 7);

    // Their IDs are the song's: a clip made after them gets another, and so does a copy of one.
    std::set<std::uint64_t> ids { song.addClip (0, 0, 4, "Clip").getRaw() };

    for (const auto track : phazite::getTrackIDs (s))
        ids.insert (track.getRaw());

    for (const auto& section : phazite::getSections (s))
        CHECK (ids.insert (section.id.getRaw()).second);

    const auto copy = phazite::duplicateSection (s, song.id (0), false);
    REQUIRE (copy.isValid());
    CHECK (ids.insert (copy.getRaw()).second);

    // A section added after the copy is undone doesn't get the copy's ID back, which a view may still hold.
    phazite::undo (s);
    CHECK_FALSE (phazite::getSection (s, copy).has_value());
    const auto added = phazite::appendSection (s, nullptr);
    CHECK (added.isValid());
    CHECK (added != copy);

    // An ID no section has names none.
    CHECK_FALSE (phazite::getSection (s, phazite::SectionID::fromRaw (*ids.begin())).has_value());
}

TEST_CASE ("Each role has its palette colour")
{
    const auto colourOf = [] (const char* name) { return phazite::findTrackColour (name).value_or (juce::Colours::black); };

    CHECK (phazite::getRoleColour ("intro") == colourOf ("Gray"));
    CHECK (phazite::getRoleColour ("verse") == colourOf ("Blue"));
    CHECK (phazite::getRoleColour ("buildUp") == colourOf ("Yellow"));
    CHECK (phazite::getRoleColour ("drop") == colourOf ("Vermilion"));
    CHECK (phazite::getRoleColour ("bridge") == colourOf ("Reddish purple"));
    CHECK (phazite::getRoleColour ("solo") == colourOf ("Bluish green"));
    CHECK (phazite::getRoleColour ("something else") == colourOf ("Sky blue"));
}

TEST_CASE ("Section edits made with the mouse are undo steps of their own, and new sections don't overlap")
{
    Song song;
    auto& undo = song.getSong().getUndoManager();

    // Moving, copying and resizing, each named for Undo.
    phazite::moveSection (song.getSong(), song.id (2), at (0.0));
    CHECK (undo.getUndoDescription() == juce::String ("Move Section"));
    phazite::undo (song.getSong());

    phazite::copySection (song.getSong(), song.id (2), at (0.0), true);
    CHECK (undo.getUndoDescription() == juce::String ("Copy Section as Aliases"));
    phazite::undo (song.getSong());

    phazite::resizeSection (song.getSong(), song.id (0), lasting (32.0), true);
    CHECK (undo.getUndoDescription() == juce::String ("Resize Section"));
    phazite::undo (song.getSong());

    // Renaming, but not to nothing or to the same name, and a color.
    phazite::renameSection (song.getSong(), song.id (0), "  Opening  ");
    CHECK (song.section (0).name == juce::String ("Opening"));
    CHECK (undo.getUndoDescription() == juce::String ("Rename Section"));
    undo.clearUndoHistory();
    phazite::renameSection (song.getSong(), song.id (0), " ");
    phazite::renameSection (song.getSong(), song.id (0), "Opening");
    CHECK_FALSE (undo.canUndo());

    phazite::setSectionColour (song.getSong(), song.id (0), juce::Colours::orange);
    CHECK (song.section (0).colour == juce::Colours::orange);
    CHECK (undo.getUndoDescription() == juce::String ("Change Section Color"));

    // Every beat of the song is in a section, so there is no room for a new one in it; after it, a new one starts at the
    // bar and lasts 8 bars.
    CHECK_FALSE (phazite::addSectionAt (song.getSong(), at (100.0), &song.selection).isValid());
    const auto added = phazite::getSection (song.getSong(), phazite::addSectionAt (song.getSong(), at (390.0), &song.selection));
    REQUIRE (added.has_value());
    CHECK (added->range.getStart().inBeats() == doctest::Approx (388.0));
    CHECK (added->range.getEnd().inBeats() == doctest::Approx (388.0 + 32.0));
    CHECK (song.selection.isSelected (added->id));
    CHECK (undo.getUndoDescription() == juce::String ("Add Section"));

    // In a song without sections: from the bar the beat is in, and a new one before another stops at it.
    auto empty = phazite::createSong();
    const auto atStart = phazite::getSection (*empty, phazite::addSectionAt (*empty, at (2.0), nullptr));
    const auto later = phazite::getSection (*empty, phazite::addSectionAt (*empty, at (41.0), nullptr));
    const auto between = phazite::getSection (*empty, phazite::addSectionAt (*empty, at (37.0), nullptr));
    REQUIRE (atStart.has_value());
    REQUIRE (later.has_value());
    REQUIRE (between.has_value());
    CHECK (atStart->range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (atStart->range.getEnd().inBeats() == doctest::Approx (32.0));
    CHECK (later->range.getStart().inBeats() == doctest::Approx (40.0));
    CHECK (between->range.getStart().inBeats() == doctest::Approx (36.0));
    CHECK (between->range.getEnd().inBeats() == doctest::Approx (40.0));
}

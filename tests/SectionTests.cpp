// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/Arrangement.h"
#include "model/Clips.h"
#include "model/Commands.h"
#include "model/Outlines.h"
#include "model/Sections.h"
#include "model/Selection.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// An Edit with two tracks, the selection and section commands, and the electronic outline's sections:
// Intro 0-64, Build-up 64-96, Drop 96-160, Breakdown 160-224, Build-up 224-256, Drop 256-320, Outro 320-384
// (in beats; 4/4 at 120 BPM, so a beat is half a second).
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        tracks = te::getAudioTracks (*edit);
        selection.edit = edit.get();
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addSectionCommands (registry, selection);

        if (const auto* outline = phazite::findOutline ("electronic"))
            phazite::applyOutline (*edit, *outline);

        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { selection.deselectAll(); }

    te::MidiClip& addClip (int track, double start, double end, const juce::String& name)
    {
        auto* clip = phazite::insertMidiClip (*tracks[track], { beats (start), beats (end) }, nullptr);
        REQUIRE (clip != nullptr);
        clip->setName (name);
        return *clip;
    }

    te::Clip* findClip (int track, double start) const
    {
        for (auto* clip : tracks[track]->getClips())
            if (std::abs (clip->getStartBeat().inBeats() - start) < 1.0e-6)
                return clip;

        return nullptr;
    }

    te::ArrangerClip& section (int index) const
    {
        auto* found = phazite::getSections (*edit)[index];
        REQUIRE (found != nullptr);
        return *found;
    }

    juce::String getSectionNames() const
    {
        juce::StringArray names;

        for (auto* s : phazite::getSections (*edit))
            names.add (s->getName());

        return names.joinIntoString (", ");
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    juce::Array<te::AudioTrack*> tracks;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("The built-in outlines are the design's four, with its section lengths")
{
    const auto& outlines = phazite::getBuiltInOutlines();
    REQUIRE (outlines.size() == 4);

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

    for (const auto& outline : outlines)
    {
        CAPTURE (outline.id.toStdString());
        CHECK (outline.description.isNotEmpty());

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
}

TEST_CASE ("Applying the electronic outline creates its seven sections at the project tempo")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->tempoSequence.getTempos()[0]->setBpm (128.0);

    const auto* outline = phazite::findOutline ("electronic");
    REQUIRE (outline != nullptr);
    edit->getUndoManager().beginNewTransaction ("Apply Outline");
    phazite::applyOutline (*edit, *outline);

    const auto sections = phazite::getSections (*edit);
    REQUIRE (sections.size() == 7);

    const char* names[] = { "Intro", "Build-up", "Drop", "Breakdown", "Build-up", "Drop", "Outro" };
    const int bars[] = { 16, 8, 16, 16, 8, 16, 16 };
    auto bar = 0;

    for (int i = 0; i < 7; ++i)
    {
        CAPTURE (i);
        CHECK (sections[i]->getName() == juce::String (names[i]));
        CHECK (sections[i]->getStartBeat().inBeats() == doctest::Approx (bar * 4.0));
        CHECK (sections[i]->getLengthInBeats().inBeats() == doctest::Approx (bars[i] * 4.0));

        // At 128 BPM a beat lasts 60/128 seconds.
        CHECK (sections[i]->getPosition().getStart().inSeconds() == doctest::Approx (bar * 4.0 * 60.0 / 128.0));
        CHECK (phazite::getSectionTip (*sections[i]).isNotEmpty());
        bar += bars[i];
    }

    CHECK (phazite::getSectionRole (*sections[2]) == juce::String ("drop"));
    CHECK (phazite::getSectionEnergy (*sections[2]) == doctest::Approx (1.0));
    CHECK (phazite::getSectionColour (*sections[2]) == phazite::getRoleColour ("drop"));
    CHECK (phazite::getAppliedOutline (*edit) == juce::String ("electronic"));

    // Another outline replaces them, and undo brings them back.
    edit->getUndoManager().beginNewTransaction ("Apply Outline");
    phazite::applyOutline (*edit, *phazite::findOutline ("pop"));
    CHECK (phazite::getSections (*edit).size() == 10);

    phazite::undo (*edit);
    CHECK (phazite::getSections (*edit).size() == 7);
    phazite::undo (*edit);
    CHECK (phazite::getSections (*edit).isEmpty());
}

TEST_CASE ("Moving the Drop moves everything in it")
{
    Song song;
    song.addClip (0, 96, 160, "Riff"); // the first Drop, exactly
    song.addClip (1, 80, 112, "Fill"); // from the Build-up into the Drop
    song.addClip (0, 160, 176, "Pad"); // in the Breakdown
    song.edit->getUndoManager().beginNewTransaction ("Move Section");

    // Before the first Build-up.
    phazite::moveSection (song.section (2), beats (64));

    CHECK (song.getSectionNames() == juce::String ("Intro, Drop, Build-up, Breakdown, Build-up, Drop, Outro"));
    CHECK (song.section (1).getStartBeat().inBeats() == doctest::Approx (64.0));
    CHECK (song.section (1).getEndBeat().inBeats() == doctest::Approx (128.0));
    CHECK (song.section (2).getStartBeat().inBeats() == doctest::Approx (128.0));

    // The riff went with the Drop; the fill was split at the Drop's start, and each part went with its section.
    auto* riff = song.findClip (0, 64);
    REQUIRE (riff != nullptr);
    CHECK (riff->getName() == juce::String ("Riff"));
    CHECK (riff->getEndBeat().inBeats() == doctest::Approx (128.0));

    REQUIRE (song.findClip (1, 64) != nullptr); // the Fill's part in the Drop
    CHECK (song.findClip (1, 64)->getEndBeat().inBeats() == doctest::Approx (80.0));
    REQUIRE (song.findClip (1, 144) != nullptr); // its part in the Build-up
    CHECK (song.findClip (1, 144)->getEndBeat().inBeats() == doctest::Approx (160.0));

    CHECK (song.findClip (0, 160) != nullptr); // the Breakdown didn't move

    // One undo step puts it all back.
    phazite::undo (*song.edit);
    CHECK (song.getSectionNames() == juce::String ("Intro, Build-up, Drop, Breakdown, Build-up, Drop, Outro"));
    CHECK (song.findClip (0, 96) != nullptr);
    CHECK (song.findClip (1, 80) != nullptr);
    CHECK (song.tracks[1]->getClips().size() == 1);

    SUBCASE ("and moving a section later puts it before the destination")
    {
        // The first Build-up, to the Breakdown's start: after the Drop.
        phazite::moveSection (song.section (1), beats (160));
        CHECK (song.getSectionNames() == juce::String ("Intro, Drop, Build-up, Breakdown, Build-up, Drop, Outro"));
        CHECK (song.findClip (0, 64) != nullptr); // the riff, earlier by the Build-up's length
        CHECK (song.findClip (0, 160) != nullptr);
    }
}

TEST_CASE ("Duplicating a section copies what is in it, as aliases if asked")
{
    Song song;
    auto& riff = song.addClip (0, 96, 160, "Riff");
    riff.getSequence().addNote (60, beats (0), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);
    song.addClip (0, 160, 176, "Pad");

    auto* copy = phazite::duplicateSection (song.section (2), true);
    REQUIRE (copy != nullptr);
    CHECK (copy->getName() == juce::String ("Drop"));
    CHECK (copy->getStartBeat().inBeats() == doctest::Approx (160.0));
    CHECK (song.getSectionNames() == juce::String ("Intro, Build-up, Drop, Drop, Breakdown, Build-up, Drop, Outro"));

    // The riff's copy is an alias of it, and what came after moved later.
    auto* riffCopy = song.findClip (0, 160);
    REQUIRE (riffCopy != nullptr);
    CHECK (riffCopy->getName() == juce::String ("Riff"));
    CHECK (riffCopy->isLinked());
    CHECK (riff.isLinked());
    CHECK (song.findClip (0, 224) != nullptr); // the Pad
    CHECK (song.section (4).getStartBeat().inBeats() == doctest::Approx (224.0));

    // A plain copy isn't linked.
    auto* plain = phazite::copySection (song.section (0), beats (0), false);
    REQUIRE (plain != nullptr);
    CHECK (song.getSectionNames().startsWith ("Intro, Intro, Build-up"));
}

TEST_CASE ("Duplicating a section moves what comes after it, even after a clip across its edge is split")
{
    // Splitting the Fill puts its new part at the end of the track's list of clips, after the Lead: inserting
    // the copy's time must still move the Lead.
    Song song;
    song.addClip (1, 80, 112, "Fill");  // from the Build-up into the Drop
    song.addClip (1, 176, 192, "Lead"); // in the Breakdown

    REQUIRE (phazite::duplicateSection (song.section (2), false) != nullptr);
    CHECK (song.findClip (1, 240) != nullptr); // the Lead, a Drop later
    CHECK (song.findClip (1, 176) == nullptr);
    CHECK (song.findClip (1, 160) != nullptr); // the Fill's part in the Drop, copied
    CHECK (song.findClip (1, 160)->getEndBeat().inBeats() == doctest::Approx (176.0));
}

TEST_CASE ("Deleting a section deletes what is in it, and the rest of the song closes up")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");
    song.addClip (1, 80, 112, "Fill");
    song.addClip (0, 160, 176, "Pad");

    phazite::deleteSection (song.section (2));

    CHECK (song.getSectionNames() == juce::String ("Intro, Build-up, Breakdown, Build-up, Drop, Outro"));
    CHECK (song.section (0).getEndBeat().inBeats() == doctest::Approx (64.0)); // sections before it are left alone
    CHECK (song.section (1).getEndBeat().inBeats() == doctest::Approx (96.0));
    CHECK (song.section (2).getStartBeat().inBeats() == doctest::Approx (96.0));
    CHECK (song.tracks[0]->getClips().size() == 1); // the Riff went
    CHECK (song.findClip (0, 96) != nullptr);       // the Pad came back to the Breakdown's new start

    REQUIRE (song.findClip (1, 80) != nullptr); // the Fill lost its part in the Drop
    CHECK (song.findClip (1, 80)->getEndBeat().inBeats() == doctest::Approx (96.0));
}

TEST_CASE ("Resizing a section moves the rest of the song, unless it is resized alone")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");

    // The first Build-up, from 8 bars to 16: everything after it moves 8 bars later.
    phazite::resizeSection (song.section (1), te::BeatDuration::fromBeats (64.0), true);
    CHECK (song.section (1).getEndBeat().inBeats() == doctest::Approx (128.0));
    CHECK (song.section (2).getStartBeat().inBeats() == doctest::Approx (128.0));
    CHECK (song.findClip (0, 128) != nullptr);

    // Back to 8 bars: the time is taken out again.
    phazite::resizeSection (song.section (1), te::BeatDuration::fromBeats (32.0), true);
    CHECK (song.section (2).getStartBeat().inBeats() == doctest::Approx (96.0));
    CHECK (song.findClip (0, 96) != nullptr);

    // Alone, it can't grow into the next section, and nothing else moves.
    phazite::resizeSection (song.section (1), te::BeatDuration::fromBeats (64.0), false);
    CHECK (song.section (1).getEndBeat().inBeats() == doctest::Approx (96.0));
    phazite::resizeSection (song.section (1), te::BeatDuration::fromBeats (16.0), false);
    CHECK (song.section (1).getEndBeat().inBeats() == doctest::Approx (80.0));
    CHECK (song.section (2).getStartBeat().inBeats() == doctest::Approx (96.0));
    CHECK (song.findClip (0, 96) != nullptr);
}

TEST_CASE ("Inserting and removing time stretch the sections they fall inside")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");

    // 8 beats inside the first Drop: it gets longer, the clip is split, and later sections move.
    phazite::insertTime (*song.edit, beats (100), te::BeatDuration::fromBeats (8.0));
    CHECK (phazite::getSections (*song.edit).size() == 7);
    CHECK (song.section (2).getEndBeat().inBeats() == doctest::Approx (168.0));
    CHECK (song.section (3).getStartBeat().inBeats() == doctest::Approx (168.0));
    CHECK (song.findClip (0, 108) != nullptr);

    phazite::removeTime (*song.edit, { beats (100), beats (108) });
    CHECK (song.section (2).getEndBeat().inBeats() == doctest::Approx (160.0));
    CHECK (song.section (3).getStartBeat().inBeats() == doctest::Approx (160.0));
    CHECK (song.findClip (0, 100) != nullptr);

    // Sections before the range are left alone.
    CHECK (song.section (0).getEndBeat().inBeats() == doctest::Approx (64.0));
    CHECK (song.section (1).getEndBeat().inBeats() == doctest::Approx (96.0));

    // Removing a range that covers a whole section deletes it.
    phazite::removeTime (*song.edit, { beats (64), beats (96) });
    CHECK (song.getSectionNames() == juce::String ("Intro, Drop, Breakdown, Build-up, Drop, Outro"));
}

TEST_CASE ("The section commands")
{
    Song song;
    song.addClip (0, 96, 160, "Riff");
    auto& undoManager = song.edit->getUndoManager();

    song.selection.selectOnly (song.section (2));
    CHECK (phazite::getSelectedClips (song.selection).isEmpty()); // a section isn't a clip
    CHECK (phazite::getCurrentName (*song.registry.find ("edit.delete")) == juce::String ("Delete Section"));
    CHECK (phazite::getCurrentName (*song.registry.find ("edit.duplicateAsAlias")) == juce::String ("Duplicate Section as Aliases"));

    REQUIRE (song.registry.invoke ("edit.duplicate"));
    CHECK (phazite::getSections (*song.edit).size() == 8);
    CHECK (undoManager.getUndoDescription() == juce::String ("Duplicate Section"));
    CHECK (phazite::getSelectedSections (song.selection).getFirst()->getStartBeat().inBeats() == doctest::Approx (160.0));

    REQUIRE (song.registry.invoke ("edit.delete"));
    CHECK (phazite::getSections (*song.edit).size() == 7);
    CHECK (song.findClip (0, 96) != nullptr);

    // Add Section goes after the last one, 8 bars long.
    REQUIRE (song.registry.invoke ("section.add"));
    REQUIRE (phazite::getSections (*song.edit).size() == 8);
    CHECK (song.section (7).getStartBeat().inBeats() == doctest::Approx (384.0));
    CHECK (song.section (7).getLengthInBeats().inBeats() == doctest::Approx (32.0));

    // Each outline is a command, ticked while it is the one applied.
    CHECK (song.registry.find ("section.outline.electronic")->isTicked());
    REQUIRE (song.registry.invoke ("section.outline.hipHop"));
    CHECK (phazite::getSections (*song.edit).size() == 7);
    CHECK (song.registry.find ("section.outline.hipHop")->isTicked());
    CHECK (undoManager.getUndoDescription() == juce::String ("Apply Outline: Hip-hop and trap"));
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// The chord lane as the UI reaches it (src/model/chords/ChordModel.h): its chords and keys, progressions laid over the
// song, each edit one undo step, chords that go with the song's time, parts made from the chords that follow them until
// their notes are edited, the chord commands, and a progression written out for the previewer.
#include "TestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/base/internal/Schema.h"
#include "model/chords/ChordModel.h"
#include "model/chords/Progressions.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::ChordType;
using phazite::Key;
using phazite::Numeral;
using phazite::PartStyle;
using phazite::Scale;

phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

phazite::BeatRange beats (double start, double end)
{
    return { beat (start), beat (end) };
}

const phazite::Progression& getProgression (const char* id)
{
    const auto* progression = phazite::findProgression (id);
    INFO (id);
    REQUIRE (progression != nullptr);
    return *progression;
}

// An open project in 4/4 with one empty track.
struct Song
{
    Song() { setEdit (context, edit.get()); }
    ~Song() { setEdit (context, nullptr); }

    juce::String names() const
    {
        juce::StringArray all;

        for (const auto& chord : phazite::getLaneChords (context))
            all.add (chord.name);

        return all.joinIntoString (" ");
    }

    // Each chord's start and end, in beats.
    juce::String places() const
    {
        juce::StringArray all;

        for (const auto& chord : phazite::getLaneChords (context))
            all.add (juce::String (juce::roundToInt (chord.range.getStart().inBeats())) + "-"
                     + juce::String (juce::roundToInt (chord.range.getEnd().inBeats())));

        return all.joinIntoString (" ");
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    void undo() { phazite::undo (*edit); }

    te::MidiClip* findClip (phazite::ClipID id) const
    {
        return dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, phazite::toTracktion (id)));
    }

    std::vector<int> pitchesOf (phazite::ClipID id) const
    {
        std::vector<int> pitches;
        auto* clip = findClip (id);
        REQUIRE (clip != nullptr);

        for (auto* note : clip->getSequence().getNotes())
            pitches.push_back (note->getNoteNumber());

        std::sort (pitches.begin(), pitches.end());
        return pitches;
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
};
} // namespace

TEST_CASE ("The chord lane: empty, in the new project's key, until chords go on it")
{
    Song song;
    CHECK (phazite::getLaneChords (song.context).empty());
    CHECK_FALSE (phazite::hasLaneChords (song.context));

    const auto keys = phazite::getLaneKeyChanges (song.context);
    REQUIRE (keys.size() == 1);
    CHECK (keys[0].start == beat (0.0));
    CHECK (keys[0].key == Key {});

    // A new project's key is the lane's until a key is chosen; nothing is written by reading.
    phazite::setLastKey (*song.edit, { 7, Scale::major });
    CHECK (phazite::getLaneKeyAt (song.context, beat (10.0)) == Key { 7, Scale::major });
    CHECK_FALSE (phazite::findPhaziteState (*song.edit).getChildWithName (phazite::schema::CHORDLANE).isValid());
}

TEST_CASE ("The chord lane: a progression laid over a range, repeated or cut to fit it, as one undo step")
{
    Song song;
    phazite::applyProgression (song.context, getProgression ("popAnthem"), beats (0.0, 16.0));
    CHECK (song.names() == juce::String ("C G Am F"));
    CHECK (song.places() == juce::String ("0-4 4-8 8-12 12-16"));
    CHECK (song.describeUndo() == juce::String ("Undo Apply Chord Progression"));

    const auto chords = phazite::getLaneChords (song.context);
    CHECK (chords[2].numeral == Numeral { 6, 0, ChordType::minor, 0 });
    CHECK (chords[2].pitchClasses == std::vector<int> { 9, 0, 4 });

    song.undo();
    CHECK (song.names().isEmpty());

    // Twice over a longer range; cut over a shorter one; in place of what was there, and around a range inside a chord.
    phazite::applyProgression (song.context, getProgression ("popAnthem"), beats (0.0, 32.0));
    CHECK (song.names() == juce::String ("C G Am F C G Am F"));

    phazite::applyProgression (song.context, getProgression ("twoDarkChords"), beats (4.0, 14.0));
    CHECK (song.names() == juce::String ("C Am F F C G Am F"));
    CHECK (song.places() == juce::String ("0-4 4-12 12-14 14-16 16-20 20-24 24-28 28-32"));

    // A minor progression in a major key plays in its relative minor, and its chords are numerals in the lane's key.
    phazite::applyProgression (song.context, getProgression ("epicMinor"), beats (0.0, 16.0));
    CHECK (song.names().startsWith ("Am F C G"));
    CHECK (phazite::getLaneChords (song.context)[0].numeral == Numeral { 6, 0, ChordType::minor, 0 });
}

TEST_CASE ("The chord lane: chords added, changed, resized and deleted, each one undo step")
{
    Song song;
    phazite::addLaneChord (song.context, beats (0.0, 16.0), { 1, 0, ChordType::major, 0 });
    CHECK (song.describeUndo() == juce::String ("Undo Add Chord"));

    // A chord added inside another splits it.
    phazite::addLaneChord (song.context, beats (4.0, 8.0), { 4, 0, ChordType::major, 0 });
    CHECK (song.names() == juce::String ("C F C"));
    CHECK (song.places() == juce::String ("0-4 4-8 8-16"));

    phazite::changeLaneChord (song.context, beat (4.0), { 2, 0, ChordType::minorSeventh, 0 });
    CHECK (song.names() == juce::String ("C Dm7 C"));
    CHECK (song.describeUndo() == juce::String ("Undo Change Chord"));

    // Growing over the next chord cuts it; shrinking leaves a gap.
    phazite::setLaneChordEnd (song.context, beat (4.0), beat (10.0));
    CHECK (song.places() == juce::String ("0-4 4-10 10-16"));
    CHECK (song.describeUndo() == juce::String ("Undo Change Chord Length"));
    song.undo();
    CHECK (song.places() == juce::String ("0-4 4-8 8-16"));

    phazite::setLaneChordEnd (song.context, beat (0.0), beat (2.0));
    CHECK (song.places() == juce::String ("0-2 4-8 8-16"));

    phazite::deleteLaneChord (song.context, beat (4.0));
    CHECK (song.names() == juce::String ("C C"));
    CHECK (song.describeUndo() == juce::String ("Undo Delete Chord"));

    // Beats where no chord starts change nothing.
    phazite::deleteLaneChord (song.context, beat (5.0));
    phazite::changeLaneChord (song.context, beat (5.0), {});
    CHECK (song.describeUndo() == juce::String ("Undo Delete Chord"));

    // The chords the key offers: its scale's seven, then borrowed ones and dominants, each once.
    const auto choices = phazite::getChordChoices (song.context, beat (0.0));
    REQUIRE (choices.size() == 15);
    CHECK (choices[6] == Numeral { 7, 0, ChordType::diminished, 0 });
    CHECK (choices[7] == Numeral { 4, 0, ChordType::minor, 0 });
    CHECK (choices[8] == Numeral { 7, -1, ChordType::major, 0 });
}

TEST_CASE ("The chord lane: the chords follow the key, and key changes along the song")
{
    Song song;
    phazite::applyProgression (song.context, getProgression ("popAnthem"), beats (0.0, 32.0));

    phazite::setLaneKey (song.context, beat (0.0), { 9, Scale::major });
    CHECK (song.names() == juce::String ("A E F#m D A E F#m D"));
    CHECK (song.describeUndo() == juce::String ("Undo Change Key"));

    // A key change from bar 5: the chords after it are in that key.
    phazite::setLaneKey (song.context, beat (16.0), { 5, Scale::major });
    CHECK (song.names() == juce::String ("A E F#m D F C Dm Bb"));

    const auto changes = phazite::getLaneKeyChanges (song.context);
    REQUIRE (changes.size() == 2);
    CHECK (changes[1].start == beat (16.0));
    CHECK (phazite::getLaneKeyAt (song.context, beat (15.0)) == Key { 9, Scale::major });
    CHECK (phazite::getLaneKeyAt (song.context, beat (16.0)) == Key { 5, Scale::major });

    phazite::removeLaneKeyChange (song.context, beat (16.0));
    CHECK (song.names() == juce::String ("A E F#m D A E F#m D"));
    CHECK (song.describeUndo() == juce::String ("Undo Remove Key Change"));

    // The key at the song's start stays.
    phazite::removeLaneKeyChange (song.context, beat (0.0));
    CHECK (phazite::getLaneKeyChanges (song.context).size() == 1);
    CHECK (song.describeUndo() == juce::String ("Undo Remove Key Change"));
}

TEST_CASE ("The chord lane: chords go with the song's time, as sections do")
{
    Song song;
    phazite::applyProgression (song.context, getProgression ("popAnthem"), beats (0.0, 16.0));
    phazite::setLaneKey (song.context, beat (8.0), { 2, Scale::major });

    // Inserted time moves what is after it and lengthens a chord it falls inside; the key change moves too.
    phazite::insertTime (*song.edit, te::BeatPosition::fromBeats (6.0), te::BeatDuration::fromBeats (4.0));
    CHECK (song.places() == juce::String ("0-4 4-12 12-16 16-20"));
    CHECK (phazite::getLaneKeyChanges (song.context)[1].start == beat (12.0));

    // Removed time takes the chords inside it away and shortens those that reach into it.
    phazite::removeTime (*song.edit, { te::BeatPosition::fromBeats (6.0), te::BeatPosition::fromBeats (10.0) });
    CHECK (song.places() == juce::String ("0-4 4-8 8-12 12-16"));
    CHECK (phazite::getLaneKeyChanges (song.context)[1].start == beat (8.0));

    // A moved range takes its chords along, cut at its edges, and those it passes move the other way; key changes stay.
    phazite::setLaneKey (song.context, beat (8.0), { 0, Scale::major });
    phazite::moveTime (*song.edit, { te::BeatPosition::fromBeats (0.0), te::BeatPosition::fromBeats (4.0) },
                       te::BeatPosition::fromBeats (12.0));
    CHECK (song.names() == juce::String ("G Am C F"));
    CHECK (song.places() == juce::String ("0-4 4-8 8-12 12-16"));

    // A copied range: its chords at the destination, after room is made for them.
    phazite::copyTime (*song.edit, { te::BeatPosition::fromBeats (0.0), te::BeatPosition::fromBeats (8.0) },
                       te::BeatPosition::fromBeats (16.0), false);
    CHECK (song.names() == juce::String ("G Am C F G Am"));
    CHECK (song.places() == juce::String ("0-4 4-8 8-12 12-16 16-20 20-24"));

    // The time line reaches the end of the last chord.
    CHECK (phazite::getTimelineEnd (song.context) >= beat (24.0));
}

TEST_CASE ("Parts made from the chords: a new track with the synth, notes that follow the chords until they are edited")
{
    Song song;
    phazite::applyProgression (song.context, getProgression ("popAnthem"), beats (0.0, 16.0));
    const auto tracksBefore = te::getAudioTracks (*song.edit).size();

    const auto part = phazite::makeChordPart (song.context, PartStyle::bassRoots, beats (0.0, 16.0));
    REQUIRE (part.has_value());
    CHECK (song.describeUndo() == juce::String ("Undo Make Bass Line"));

    // At the end of the song, named for its part, with Phazite's synth, and selected.
    const auto tracks = te::getAudioTracks (*song.edit);
    REQUIRE (tracks.size() == tracksBefore + 1);
    auto* bass = tracks.getLast();
    CHECK (bass->getName() == juce::String ("Bass"));
    auto* instrument = phazite::getInstrument (*bass);
    REQUIRE (instrument != nullptr);
    CHECK (instrument->getPluginType() == juce::String (phazite::devices::Synth::typeID));
    CHECK (phazite::getSelection (song.context).isSelected (bass));

    CHECK (song.pitchesOf (*part) == std::vector<int> { 36, 41, 43, 45 });
    CHECK (phazite::getChordPart (song.context, *part) == PartStyle::bassRoots);
    CHECK (phazite::followsChords (song.context, *part));

    // Changing a chord changes the part's notes in the same undo step, and undo puts both back.
    phazite::changeLaneChord (song.context, beat (0.0), { 2, 0, ChordType::minor, 0 });
    CHECK (song.pitchesOf (*part) == std::vector<int> { 38, 41, 43, 45 });
    CHECK (phazite::followsChords (song.context, *part));
    song.undo();
    CHECK (song.pitchesOf (*part) == std::vector<int> { 36, 41, 43, 45 });

    // So does a change of key: the bass moves to D major.
    phazite::setLaneKey (song.context, beat (0.0), { 2, Scale::major });
    CHECK (song.pitchesOf (*part) == std::vector<int> { 38, 43, 45, 47 });
    song.undo();

    // A note edited by hand: the part stops following, and keeps its notes when the chords change.
    auto* clip = song.findClip (*part);
    REQUIRE (clip != nullptr);
    {
        const phazite::UndoStep step (*song.edit, "Change Velocity");
        clip->getSequence().getNotes().getFirst()->setVelocity (60, &song.edit->getUndoManager());
    }
    CHECK_FALSE (phazite::followsChords (song.context, *part));
    phazite::changeLaneChord (song.context, beat (0.0), { 2, 0, ChordType::minor, 0 });
    CHECK (song.pitchesOf (*part) == std::vector<int> { 36, 41, 43, 45 });

    // Following again makes its notes from the chords again; stopping makes it a clip like any other.
    phazite::setFollowsChords (song.context, *part, true);
    CHECK (song.describeUndo() == juce::String ("Undo Follow the Chords"));
    CHECK (song.pitchesOf (*part) == std::vector<int> { 38, 41, 43, 45 });
    CHECK (phazite::followsChords (song.context, *part));

    phazite::setFollowsChords (song.context, *part, false);
    CHECK (song.describeUndo() == juce::String ("Undo Stop Following the Chords"));
    CHECK_FALSE (phazite::getChordPart (song.context, *part).has_value());
    CHECK_FALSE (phazite::followsChords (song.context, *part));
}

TEST_CASE ("Parts made from the chords go where the selection says: a time range, sections, or all of the chords")
{
    Song song;
    CHECK_FALSE (phazite::getChordPartRange (song.context).has_value());
    CHECK_FALSE (phazite::canMakeChordPart (song.context));

    phazite::applyProgression (song.context, getProgression ("popAnthem"), beats (4.0, 20.0));
    CHECK (phazite::getChordPartRange (song.context) == beats (4.0, 20.0));
    CHECK (phazite::canMakeChordPart (song.context));

    auto* section = phazite::insertSection (*song.edit, te::BeatPosition::fromBeats (8.0),
                                            { .name = "Verse", .length = te::BeatDuration::fromBeats (8.0) });
    REQUIRE (section != nullptr);
    phazite::getSelection (song.context).selectOnly (section);
    CHECK (phazite::getSelectedSectionsRange (song.context) == beats (8.0, 16.0));
    CHECK (phazite::getChordPartRange (song.context) == beats (8.0, 16.0));

    song.context.getSongRange().set (beats (12.0, 14.0), { phazite::getTrackID (*te::getAudioTracks (*song.edit)[0]) });
    CHECK (phazite::getSelectedTimeRange (song.context) == beats (12.0, 14.0));
    CHECK (phazite::getChordPartRange (song.context) == beats (12.0, 14.0));

    // The commands make their parts there.
    phazite::CommandRegistry registry;
    phazite::addChordCommands (registry, song.context);

    for (const auto* id : { "chords.makeChordPart", "chords.makeBassLine", "chords.makeArpeggio" })
    {
        const auto* command = registry.find (id);
        INFO (id);
        REQUIRE (command != nullptr);
        CHECK (phazite::CommandRegistry::isEnabled (*command));
    }

    const auto clip = phazite::makeChordPartHere (song.context, PartStyle::arpeggioUp);
    REQUIRE (clip.has_value());
    auto* made = song.findClip (*clip);
    REQUIRE (made != nullptr);
    CHECK (made->getStartBeat().inBeats() == doctest::Approx (12.0));
    CHECK (made->getEndBeat().inBeats() == doctest::Approx (14.0));
    CHECK (made->getSequence().getNotes().size() == 4); // eighth notes over two beats
    CHECK (made->getTrack()->getName() == juce::String ("Arpeggio"));
}

TEST_CASE ("The chord lane and its parts are saved, and read back the same")
{
    Song song;
    phazite::setLaneKey (song.context, beat (0.0), { 2, Scale::minor });
    phazite::applyProgression (song.context, getProgression ("canon"), beats (0.0, 32.0));
    phazite::addLaneChord (song.context, beats (32.0, 36.0), { 4, -1, ChordType::majorSeventh, 2 });
    const auto part = phazite::makeChordPart (song.context, PartStyle::bassRootFifth, beats (0.0, 36.0));
    REQUIRE (part.has_value());

    const auto namesBefore = song.names();
    const auto placesBefore = song.places();

    // As saved: the chords' numerals by name, the key by name, and the part's style by name.
    const auto lane = phazite::findPhaziteState (*song.edit).getChildWithName (phazite::schema::CHORDLANE);
    REQUIRE (lane.isValid());
    const auto key = lane.getChildWithName (phazite::schema::CHORDKEY);
    CHECK (key[phazite::schema::phaziteKeyScale].toString() == juce::String ("minor"));
    CHECK ((int) key[phazite::schema::phaziteKeyRoot] == 2);

    auto last = juce::ValueTree();

    for (auto child : lane)
        if (child.hasType (phazite::schema::CHORD) && (double) child[phazite::schema::chordStart] > 31.0)
            last = child;

    REQUIRE (last.isValid());
    CHECK (last[phazite::schema::chordType].toString() == juce::String ("majorSeventh"));
    CHECK ((int) last[phazite::schema::chordAccidental] == -1);
    CHECK ((int) last[phazite::schema::chordInversion] == 2);

    auto* clip = song.findClip (*part);
    REQUIRE (clip != nullptr);
    CHECK (clip->state[phazite::schema::phaziteChordPart].toString() == juce::String ("bassRootFifth"));

    // A copy of the project's state, opened as another Edit, reads the same chords, and its part still follows them.
    const phazite::test::TemporaryFolder temp;
    auto copy = phazite::createEditFromState (phazite::test::getEngine(), song.edit->state.createCopy(),
                                              temp.folder.getChildFile ("Copy.tracktionedit"));
    REQUIRE (copy != nullptr);
    phazite::ProjectContext other { phazite::test::getEngineContext() };
    setEdit (other, copy.get());

    juce::StringArray names, places;

    for (const auto& chord : phazite::getLaneChords (other))
    {
        names.add (chord.name);
        places.add (juce::String (juce::roundToInt (chord.range.getStart().inBeats())) + "-"
                    + juce::String (juce::roundToInt (chord.range.getEnd().inBeats())));
    }

    CHECK (names.joinIntoString (" ") == namesBefore);
    CHECK (places.joinIntoString (" ") == placesBefore);
    CHECK (phazite::followsChords (other, *part));
    setEdit (other, nullptr);
}

TEST_CASE ("A progression written out for the previewer: held chords with their roots, as long as the progression")
{
    Song song;
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Preview.mid");
    REQUIRE (phazite::writeProgressionPreview (song.context, getProgression ("popAnthem"), beat (0.0), file));

    juce::FileInputStream stream (file);
    juce::MidiFile midi;
    REQUIRE (midi.readFrom (stream));
    REQUIRE (midi.getNumTracks() == 1);

    const auto ticks = (double) midi.getTimeFormat();
    auto noteOns = 0;
    auto lastOff = 0.0;

    for (const auto* event : *midi.getTrack (0))
    {
        if (event->message.isNoteOn())
            ++noteOns;
        else if (event->message.isNoteOff())
            lastOff = std::max (lastOff, event->message.getTimeStamp() / ticks);
    }

    CHECK (noteOns == 16); // four chords of three notes, and four roots
    CHECK (lastOff == doctest::Approx (16.0));
    CHECK (phazite::getProgressionLength (song.context, getProgression ("popAnthem"), beat (0.0)).inBeats() == doctest::Approx (16.0));
    CHECK (phazite::getProgressionLength (song.context, getProgression ("twelveBarBlues"), beat (0.0)).inBeats() == doctest::Approx (48.0));
}

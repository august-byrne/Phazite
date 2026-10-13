// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/Clips.h"
#include "model/Commands.h"
#include "model/Playback.h"
#include "model/Sections.h"
#include "model/Selection.h"
#include "model/StarterProject.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

te::TimePosition seconds (double s)
{
    return te::TimePosition::fromSeconds (s);
}

// "start-end" in seconds, which failure messages show readably.
juce::String describe (te::TimeRange range)
{
    return juce::String (range.getStart().inSeconds(), 3) + "-" + juce::String (range.getEnd().inSeconds(), 3);
}

// An Edit at 120 BPM in 4/4, so a beat is half a second and a bar 2 seconds, with two tracks and the
// loop's, selection's and sections' commands.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        tracks = te::getAudioTracks (*edit);
        selection.edit = edit.get();
        phazite::addSelectionCommands (registry, selection, range);
        phazite::addSectionCommands (registry, selection);
        phazite::addLoopCommands (registry, selection, range);
    }

    ~Song() { selection.deselectAll(); }

    te::MidiClip& addClip (int track, double start, double end)
    {
        auto* clip = phazite::insertMidiClip (*tracks[track], { beats (start), beats (end) }, nullptr);
        REQUIRE (clip != nullptr);
        return *clip;
    }

    juce::String getLoop() const { return describe (edit->getTransport().getLoopRange()); }
    juce::String getSongLoop() const { return describe (phazite::getSongLoopRange (*edit)); }
    bool isLooping() const { return edit->getTransport().looping.get(); }
    bool isTicked (const char* id) const { return registry.find (id)->isTicked(); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    juce::Array<te::AudioTrack*> tracks;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("The song's loop runs to the end of the bar its last clip ends in")
{
    Song song;
    CHECK (song.getSongLoop() == "0.000-2.000"); // an empty song: a bar

    song.addClip (0, 0.0, 4.0);
    CHECK (song.getSongLoop() == "0.000-2.000"); // ends on the first bar line

    song.addClip (1, 2.0, 4.5);
    CHECK (song.getSongLoop() == "0.000-4.000"); // half a beat into the second bar

    song.addClip (0, 12.0, 17.0);
    CHECK (song.getSongLoop() == "0.000-10.000"); // a beat into the fifth bar

    // Bars last longer at a slower tempo: 4 seconds at 60 BPM.
    Song slow;
    slow.edit->tempoSequence.getTempos()[0]->setBpm (60.0);
    slow.addClip (0, 0.0, 6.0);
    CHECK (slow.getSongLoop() == "0.000-8.000");
}

TEST_CASE ("The loop follows the song as clips are added, moved, trimmed and deleted")
{
    Song song;
    phazite::loopSong (*song.edit);
    phazite::SongLoopFollower follower (*song.edit);
    CHECK (song.isLooping());
    CHECK (phazite::doesLoopFollowSong (*song.edit));
    CHECK (song.getLoop() == "0.000-2.000");

    auto& clip = song.addClip (0, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "0.000-4.000");

    // Placed past the end, as the owner did: the loop grows.
    clip.setStart (seconds (6.0), false, true);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "0.000-10.000");

    clip.setLength (te::TimeDuration::fromSeconds (1.0), true);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "0.000-8.000");

    clip.removeFromParent();
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "0.000-2.000");

    // A tempo change moves where bars end: at 240 BPM a bar is a second.
    song.addClip (1, 0.0, 6.0);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "0.000-4.000");
    song.edit->tempoSequence.getTempos()[0]->setBpm (240.0);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == song.getSongLoop());
    CHECK (song.getLoop() != "0.000-4.000");
}

TEST_CASE ("A project saved with another loop around its song gets the song's again")
{
    Song song;
    song.addClip (0, 0.0, 16.0);
    song.edit->getTransport().setLoopRange ({ seconds (0.0), seconds (2.0) });

    phazite::SongLoopFollower follower (*song.edit);
    CHECK (song.getLoop() == "0.000-8.000");
}

TEST_CASE ("Looping a range on purpose stops the loop following the song, until the song is looped again")
{
    Song song;
    phazite::SongLoopFollower follower (*song.edit);
    song.addClip (0, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "0.000-4.000");

    phazite::loopRange (*song.edit, { seconds (1.0), seconds (3.0) });
    CHECK_FALSE (phazite::doesLoopFollowSong (*song.edit));
    CHECK (song.isLooping());
    CHECK (song.getLoop() == "1.000-3.000");

    song.addClip (1, 16.0, 24.0);
    phazite::test::dispatchPendingMessages();
    CHECK (song.getLoop() == "1.000-3.000");

    // An empty range is no loop: nothing changes.
    phazite::loopRange (*song.edit, { seconds (5.0), seconds (5.0) });
    CHECK (song.getLoop() == "1.000-3.000");

    phazite::loopSong (*song.edit);
    CHECK (phazite::doesLoopFollowSong (*song.edit));
    CHECK (song.getLoop() == "0.000-12.000");
}

TEST_CASE ("The loop's commands turn looping on and off, and loop a selection, sections or the song")
{
    Song song;
    phazite::SongLoopFollower follower (*song.edit);
    song.addClip (0, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();

    // Loop: on and off. The loop is the song's either way.
    CHECK_FALSE (song.isLooping());
    CHECK_FALSE (song.isTicked ("transport.loop"));
    REQUIRE (song.registry.invoke ("transport.loop"));
    CHECK (song.isLooping());
    CHECK (song.isTicked ("transport.loop"));
    CHECK (song.isTicked ("transport.loopSong"));
    CHECK (song.getLoop() == "0.000-4.000");
    REQUIRE (song.registry.invoke ("transport.loop"));
    CHECK_FALSE (song.isLooping());
    CHECK_FALSE (song.isTicked ("transport.loopSong"));

    // Loop Selection needs a time range selected.
    CHECK_FALSE (song.registry.invoke ("transport.loopSelection"));
    song.range.set ({ beats (2.0), beats (6.0) }, { song.tracks[0] });
    REQUIRE (song.registry.invoke ("transport.loopSelection"));
    CHECK (song.isLooping());
    CHECK (song.getLoop() == "1.000-3.000");
    CHECK (song.isTicked ("transport.loop"));
    CHECK_FALSE (song.isTicked ("transport.loopSong"));

    REQUIRE (song.registry.invoke ("transport.loopSong"));
    CHECK (song.getLoop() == "0.000-4.000");
    CHECK (song.isTicked ("transport.loopSong"));

    // Loop Section: from the first selected section's start to the last one's end.
    CHECK_FALSE (song.registry.invoke ("section.loop"));
    auto* verse = phazite::insertSection (*song.edit, beats (8.0), { .name = "Verse", .length = te::BeatDuration::fromBeats (8.0) });
    auto* chorus = phazite::insertSection (*song.edit, beats (16.0), { .name = "Chorus", .length = te::BeatDuration::fromBeats (8.0) });
    REQUIRE (verse != nullptr);
    REQUIRE (chorus != nullptr);
    song.selection.selectOnly (verse);
    song.selection.addToSelection (chorus);
    REQUIRE (song.registry.invoke ("section.loop"));
    CHECK (song.getLoop() == "4.000-12.000");
    CHECK_FALSE (phazite::doesLoopFollowSong (*song.edit));
}

TEST_CASE ("The starter project loops its song")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::addStarterContent (*edit);

    CHECK (edit->getTransport().looping.get());
    CHECK (phazite::doesLoopFollowSong (*edit));
    CHECK (describe (edit->getTransport().getLoopRange()) == describe (phazite::getSongLoopRange (*edit)));
}

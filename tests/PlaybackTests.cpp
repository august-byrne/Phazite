// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HostedDevice.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/commands/CommandRegistry.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/StarterProject.h"
#include "model/transport/internal/Playback.h"

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
    phazite::SongFollower follower (*song.edit);
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

    phazite::SongFollower follower (*song.edit);
    CHECK (song.getLoop() == "0.000-8.000");
}

TEST_CASE ("Looping a range on purpose stops the loop following the song, until the song is looped again")
{
    Song song;
    phazite::SongFollower follower (*song.edit);
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

TEST_CASE ("A ruler reads and sets the loop in beats")
{
    Song song;
    song.addClip (0, 0.0, 8.0);
    phazite::loopSong (*song.edit);

    const auto loop = phazite::getLoopBeatRange (*song.edit);
    REQUIRE (loop.has_value());
    CHECK (loop->first == doctest::Approx (0.0));
    CHECK (loop->second == doctest::Approx (8.0));

    phazite::loopBeatRange (*song.edit, 2.0, 6.0); // a beat is half a second
    CHECK (song.getLoop() == "1.000-3.000");
    CHECK (song.isLooping());
    CHECK_FALSE (phazite::doesLoopFollowSong (*song.edit));

    // Nothing for an empty or backwards range; before the song's start is its start.
    phazite::loopBeatRange (*song.edit, 6.0, 6.0);
    phazite::loopBeatRange (*song.edit, 6.0, 2.0);
    CHECK (song.getLoop() == "1.000-3.000");
    phazite::loopBeatRange (*song.edit, -2.0, 2.0);
    CHECK (song.getLoop() == "0.000-1.000");

    phazite::toggleLoop (*song.edit);
    CHECK_FALSE (phazite::getLoopBeatRange (*song.edit).has_value());
}

TEST_CASE ("The loop's commands turn looping on and off, and loop a selection, sections or the song")
{
    Song song;
    phazite::SongFollower follower (*song.edit);
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

    // Loop Selection loops the time range selected, or with none, the song, as Ctrl+L does in Ableton Live.
    REQUIRE (song.registry.invoke ("transport.loopSelection"));
    CHECK (song.isLooping());
    CHECK (song.isTicked ("transport.loopSong"));
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

TEST_CASE ("Playback starts where the playhead was last put, and stopping goes back there")
{
    Song song;
    auto& transport = song.edit->getTransport();

    // A click in a ruler puts the playhead there, and playback starts there from now on.
    phazite::movePlayhead (*song.edit, seconds (4.0));
    CHECK (phazite::getPlayStart (*song.edit).inSeconds() == doctest::Approx (4.0));
    CHECK (transport.getPosition().inSeconds() == doctest::Approx (4.0));

    // Playback took the playhead on; stopping puts it back, so that the part can be heard again.
    transport.setPosition (seconds (9.0));
    phazite::stopAndReturn (*song.edit);
    CHECK (transport.getPosition().inSeconds() == doctest::Approx (4.0));
    CHECK_FALSE (transport.isPlaying());
}

TEST_CASE ("Go to start and end")
{
    Song song;
    phazite::addTransportCommands (song.registry, song.selection);
    song.addClip (0, 2.0, 6.0); // ends 3 seconds in, inside the second bar
    auto& transport = song.edit->getTransport();

    // Home and End put the playhead at the song's start or its end (the end of the bar its last clip ends in), and
    // playback starts there.
    phazite::goToEnd (*song.edit);
    CHECK (transport.getPosition().inSeconds() == doctest::Approx (4.0));
    CHECK (phazite::getPlayStart (*song.edit).inSeconds() == doctest::Approx (4.0));

    phazite::goToStart (*song.edit);
    CHECK (transport.getPosition().inSeconds() == doctest::Approx (0.0));
    CHECK (phazite::getPlayStart (*song.edit).inSeconds() == doctest::Approx (0.0));

    // Their commands act on the selection manager's Edit.
    REQUIRE (song.registry.invoke ("transport.goToEnd"));
    CHECK (transport.getPosition().inSeconds() == doctest::Approx (4.0));
    REQUIRE (song.registry.invoke ("transport.goToStart"));
    CHECK (transport.getPosition().inSeconds() == doctest::Approx (0.0));

    // Without one, every transport command is greyed out.
    song.selection.edit = nullptr;

    for (const auto* id : { "transport.playStop", "transport.playPause", "transport.goToStart", "transport.goToEnd" })
    {
        CAPTURE (id);
        CHECK_FALSE (song.registry.find (id)->isEnabled());
        CHECK_FALSE (song.registry.invoke (id));
    }
}

TEST_CASE ("The song ends with the bar its last clip ends in, and its sections don't make it longer")
{
    Song song;
    const auto songEnd = [&song] { return juce::String (phazite::getSongEnd (*song.edit).inSeconds(), 3); };
    CHECK (songEnd() == "0.000");                // without clips it ends where it starts,
    CHECK (song.getSongLoop() == "0.000-2.000"); // though it loops its first bar

    // An outline runs on past what is in it so far: Tracktion's length counts its sections; the song's end doesn't.
    REQUIRE (phazite::applyOutline (*song.edit, "rock")); // 96 bars
    CHECK (song.edit->getLength().inSeconds() == doctest::Approx (192.0));
    CHECK (songEnd() == "0.000");
    CHECK (song.getSongLoop() == "0.000-2.000");

    auto& clip = song.addClip (1, 2.0, 4.5); // half a beat into the second bar
    CHECK (songEnd() == "4.000");
    CHECK (song.getSongLoop() == "0.000-4.000");

    clip.setMuted (true); // a muted clip counts
    CHECK (songEnd() == "4.000");

    song.edit->tempoSequence.getTimeSigs()[0]->setStringTimeSig ("3/4"); // beat 4.5 is in the second bar of three beats
    CHECK (songEnd() == "3.000");
}

TEST_CASE ("Go to End goes to the song's end, not to the end of its sections")
{
    Song song;
    phazite::addTransportCommands (song.registry, song.selection);
    REQUIRE (phazite::applyOutline (*song.edit, "rock"));
    const auto playhead = [&song] { return song.edit->getTransport().getPosition().inSeconds(); };

    phazite::movePlayhead (*song.edit, seconds (10.0));
    REQUIRE (song.registry.invoke ("transport.goToEnd"));
    CHECK (playhead() == doctest::Approx (0.0)); // no clips: the song's start

    song.addClip (0, 0.0, 8.0);
    song.addClip (1, 2.0, 12.5); // ends half a beat into the fourth bar
    REQUIRE (song.registry.invoke ("transport.goToEnd"));
    CHECK (playhead() == doctest::Approx (8.0));
    CHECK (phazite::getPlayStart (*song.edit).inSeconds() == doctest::Approx (8.0)); // playback starts there
    CHECK (song.getSongLoop() == "0.000-8.000");                                     // where the loop ends
}

TEST_CASE ("The loop's functions")
{
    Song song;
    song.addClip (0, 0.0, 6.0); // the song is two bars, 0-4 seconds

    // Looping goes on and off; on, the loop goes around the song again while it follows the song.
    CHECK_FALSE (phazite::isLoopOn (*song.edit));
    phazite::toggleLoop (*song.edit);
    CHECK (phazite::isLoopOn (*song.edit));
    CHECK (phazite::isLoopingSong (*song.edit));
    CHECK (song.getLoop() == "0.000-4.000");
    phazite::toggleLoop (*song.edit);
    CHECK_FALSE (phazite::isLoopOn (*song.edit));
    CHECK_FALSE (phazite::isLoopingSong (*song.edit));

    // The selected range, or the song while none is selected.
    song.range.set ({ beats (2.0), beats (4.0) }, { song.tracks[0] });
    phazite::loopSelection (*song.edit, song.range);
    CHECK (song.getLoop() == "1.000-2.000");
    CHECK (phazite::isLoopOn (*song.edit));
    CHECK_FALSE (phazite::isLoopingSong (*song.edit));

    song.range.clear();
    phazite::loopSelection (*song.edit, song.range);
    CHECK (phazite::isLoopingSong (*song.edit));
    CHECK (song.getLoop() == "0.000-4.000");

    // Around sections, from the earliest start to the latest end; without sections, the loop stays.
    auto* first = phazite::insertSection (*song.edit, beats (0.0), { .name = "A", .length = te::BeatDuration::fromBeats (4.0) });
    auto* third = phazite::insertSection (*song.edit, beats (8.0), { .name = "C", .length = te::BeatDuration::fromBeats (4.0) });
    REQUIRE (first != nullptr);
    REQUIRE (third != nullptr);
    phazite::loopSections ({ third, first });
    CHECK (song.getLoop() == "0.000-6.000");
    CHECK_FALSE (phazite::isLoopingSong (*song.edit));
    phazite::loopSections ({});
    CHECK (song.getLoop() == "0.000-6.000");
}

TEST_CASE ("Stopping just after notes begin leaves nothing of them sounding, and goes back to the play start")
{
    // As when Stop comes just as the loop goes round and its first notes begin: a drum hit, or a synth note with a long
    // release, at the song's start, played, then stopped as soon as it is heard.
    auto& engine = phazite::test::getEngine();
    phazite::test::HostedDevice device;

    const auto leftAfterStop = [&] (const char* type, int note, double length, float release)
    {
        auto edit = te::Edit::createSingleTrackEdit (engine);
        auto& track = *te::getAudioTracks (*edit)[0];
        auto instrument = edit->getPluginCache().createNewPlugin (type, {});
        REQUIRE (phazite::setInstrument (track, instrument) != nullptr);

        if (release > 0.0f)
            instrument->getAutomatableParameterByID ("release")->setParameter (release, juce::sendNotification);

        auto clip = track.insertMIDIClip ("Note", edit->tempoSequence.toTime ({ beats (0.0), beats (8.0) }), nullptr);
        clip->getSequence().addNote (note, beats (0.0), te::BeatDuration::fromBeats (length), 110, 0, nullptr);
        edit->getTransport().ensureContextAllocated();
        phazite::test::settle (300);

        phazite::playOrStop (*edit);
        auto heard = false;
        device.process (40,
                        [&heard] (const juce::AudioBuffer<float>& buffer, int)
                        {
                            heard = buffer.getMagnitude (0, buffer.getNumSamples()) > 0.05f;
                            return heard;
                        });
        REQUIRE (heard);

        // Space: the block after it lets the notes go, and fades out those that had only just begun.
        phazite::playOrStop (*edit);
        auto left = 0.0f;
        device.process (40,
                        [&left] (const juce::AudioBuffer<float>& buffer, int block)
                        {
                            if (block >= 2)
                                left = std::max (left, buffer.getMagnitude (0, buffer.getNumSamples()));

                            return false;
                        });

        // Back at the play start, though playback stopped within 200 ms of starting.
        phazite::test::dispatchMessagesUntil ([] { return false; }, 400);
        CHECK (edit->getTransport().getPosition() == te::TimePosition());
        return left;
    };

    CHECK (leftAfterStop (phazite::devices::DrumSampler::typeID, 36, 0.25, 0.0f) < 1.0e-4f);
    CHECK (leftAfterStop (phazite::devices::Synth::typeID, 60, 4.0, 400.0f) < 1.0e-4f);
}

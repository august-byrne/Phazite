// SPDX-License-Identifier: AGPL-3.0-or-later
// The alias rule for audio clips (src/model/base/internal/Schema.h: clipLink; src/model/arrangement/internal/AudioClips.h:
// shareTheSound()): the audio clips of an alias group share their file, tempo, pitch, direction and warp markers, which every
// edit of them made through a clip of the group that isn't its first (and Locate Audio File, by the file's path) writes into
// each, after it, its Undo and its Redo, each through an UndoCheck; and every edit of what a clip keeps of its own (its gain,
// fades, mute, place, the part of its file it plays, its take) changes that clip alone. All through the model's functions
// and commands, by IDs and values (a recording's takes set up through Clips.h's setClip(), below). The song's check
// (SongCheck.h) states the same of every song; the loader and a merge keep it (InconsistentProjectTests.cpp,
// VersionsMergeTests.cpp).
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/PlaylistValues.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
using phazite::ClipID;
using phazite::test::at;
using phazite::test::checkSong;
using phazite::test::lasting;
namespace schema = phazite::schema;

// Drum hits at times in a file of so many seconds, each noise falling away over 30 ms, for Quantize Audio to find.
juce::File writeHitsAt (const juce::File& file, const std::vector<double>& times, double seconds)
{
    constexpr double rate = 44100.0;
    juce::AudioBuffer<float> audio (1, (int) std::lround (seconds * rate));
    audio.clear();
    juce::Random random (5);

    for (const auto time : times)
        for (int i = 0, first = (int) std::lround (time * rate); i < (int) (0.2 * rate) && first + i < audio.getNumSamples(); ++i)
            audio.setSample (0, first + i, 0.5f * std::exp ((float) -i / (float) (0.03 * rate)) * (random.nextFloat() * 2.0f - 1.0f));

    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

// A clip's element as text, as the song would save it (WholeState.h: copyWholeClip()): every property but its ID, sorted, and
// its warp markers in their order.
juce::String describeClip (const phazite::Song& song, ClipID clip)
{
    const auto state = phazite::copyWholeClip (song, clip);
    REQUIRE (state.isValid());
    juce::StringArray properties;

    for (int i = 0; i < state.getNumProperties(); ++i)
        if (const auto name = state.getPropertyName (i); name != juce::Identifier (schema::clipID))
            properties.add (name.toString() + "=" + state[name].toString());

    properties.sort (false);
    juce::StringArray markers;

    for (const auto& child : state)
        if (child.hasType (schema::WARP))
            markers.add (child[schema::warpTime].toString() + ":" + child[schema::warpPlaysAt].toString());

    return properties.joinIntoString (" ") + " | markers " + markers.joinIntoString (" ");
}

// What the clips of a group share, as the model reads it through a clip: its file, tempo, pitch and direction, and its warp
// markers, each a time of the file with where it plays straight (the beat each plays at differs with the clip's place).
juce::String describeShared (const phazite::Song& song, ClipID clip)
{
    juce::StringArray markers;
    const auto value = phazite::getClipValue (song, clip);
    REQUIRE (value.has_value());

    for (const auto& warp : value->sound.warps)
        markers.add (juce::String (warp.time, 6) + ":" + juce::String (warp.playsAt, 6));

    return "file " + phazite::getClipFile (song, clip).getFileName() + ", tempo " + juce::String (phazite::getClipTempo (song, clip), 6)
           + ", pitch " + juce::String (phazite::getClipPitch (song, clip), 4)
           + (phazite::isClipReversed (song, clip) ? ", backwards" : ", forwards") + ", markers " + markers.joinIntoString (" ");
}

std::vector<ClipID> sorted (std::vector<ClipID> clips)
{
    std::sort (clips.begin(), clips.end());
    return clips;
}

// A song of two tracks: on the first, an audio clip of two seconds of hits from beat 0 (at 120 BPM, a beat is half a
// second); on the second, an alias of it from beat 8; and on the first again, an alias from beat 16 trimmed to start half a
// beat in, so that it plays another part of the file. Edits are made through the second member, onOther, never the group's
// first.
struct AudioAliases
{
    AudioAliases()
    {
        song.setTempo (120.0);
        other = song.addTrack ("Other");
        const auto file = writeHitsAt (song.getSoundsFolder().getChildFile ("Hits.wav"), { 0.0, 0.53, 0.97, 1.54 }, 2.0);
        clip = song.addAudioClip (song.firstTrack, file, 0.0);
        onOther = song.copyClip (clip, 8.0, true);
        phazite::moveClip (song.context, onOther, at (8.0), other);
        snippet = song.copyClip (clip, 16.0, true);
        phazite::trimClipStart (song.context, snippet, at (16.5));
        members = { clip, onOther, snippet };

        REQUIRE (sorted (phazite::getLinkedClips (song.context, clip)) == sorted (members));
        REQUIRE (phazite::getClipTrack (getSong(), onOther) == other);
        REQUIRE (phazite::getFilePart (*phazite::getClipValue (getSong(), snippet)).start == doctest::Approx (0.25));
        REQUIRE (shareTheSound());
        getSong().getUndoManager().clearUndoHistory();
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }

    // Whether every member shares what the first has, as the model reads it and as the loader compares the elements (each
    // clip whole, as the song would save it: WholeState.h, copyWholeClip()).
    bool shareTheSound() const
    {
        const auto& target = getSong();
        const auto first = phazite::copyWholeClip (target, members.front());

        return std::all_of (members.begin(), members.end(),
                            [&] (ClipID member)
                            {
                                return describeShared (target, member) == describeShared (target, members.front())
                                       && phazite::shareTheSound (first, phazite::copyWholeClip (target, member));
                            });
    }

    // Every member, a line each, for comparing the group before and after, and for a failure's message.
    std::string describeGroup() const
    {
        juce::StringArray lines;

        for (const auto member : members)
            lines.add ("clip " + juce::String ((juce::int64) member.getRaw()) + ": " + describeClip (getSong(), member));

        return lines.joinIntoString ("\n").toStdString();
    }

    // An edit of what the group shares, made through a member: one undo step, exact (UndoCheck), after which every member
    // shares the same, as `verify` says of each; Undo gives every member as it was before, Redo as it was after, and the song
    // is left as it was before the edit. With ownStep, the edit is a model function that is a step of its own made later (a
    // job that reads audio first); else it joins the step begun for it.
    void checkShared (phazite::test::UndoCheck& check, const juce::String& name, const std::function<void()>& edit,
                      const std::function<void (ClipID)>& verify, bool ownStep = false)
    {
        INFO (name.toStdString());
        auto& target = getSong();
        const auto before = describeGroup();
        REQUIRE (shareTheSound());
        run (check, name, edit, ownStep);

        const auto after = describeGroup();
        CHECK (after != before);
        CHECK_MESSAGE (shareTheSound(), after);

        for (const auto member : members)
        {
            INFO ("in clip " << member.getRaw());
            verify (member);
        }

        check.stepBackOrForth ("Undo " + name, [&target] { phazite::undo (target); });
        CHECK (describeGroup() == before);
        CHECK (shareTheSound());

        check.stepBackOrForth ("Redo " + name, [&target] { phazite::redo (target); });
        CHECK (describeGroup() == after);
        CHECK (shareTheSound());

        check.stepBackOrForth ("Undo " + name + " again", [&target] { phazite::undo (target); });
        CHECK (describeGroup() == before);
        checkSong (target);
    }

    // An edit of what a clip keeps of its own, made through a member: one undo step, exact, which changes that member and
    // leaves every other as it was, the group still sharing what it shares; Undo and Redo the same; the song left as it
    // was before.
    void checkOwn (phazite::test::UndoCheck& check, const juce::String& name, ClipID edited, const std::function<void()>& edit,
                   bool ownStep = false)
    {
        INFO (name.toStdString());
        auto& target = getSong();
        const auto before = describeGroup();
        std::vector<juce::String> each;

        for (const auto member : members)
            each.push_back (describeClip (target, member));

        run (check, name, edit, ownStep);
        const auto after = describeGroup();
        CHECK_MESSAGE (shareTheSound(), after);

        for (size_t i = 0; i < members.size(); ++i)
        {
            INFO ("clip " << members[i].getRaw() << "\n" << after);

            if (members[i] == edited)
                CHECK (describeClip (target, members[i]) != each[i]);
            else
                CHECK (describeClip (target, members[i]) == each[i]);
        }

        check.stepBackOrForth ("Undo " + name, [&target] { phazite::undo (target); });
        CHECK (describeGroup() == before);
        check.stepBackOrForth ("Redo " + name, [&target] { phazite::redo (target); });
        CHECK (describeGroup() == after);
        CHECK (shareTheSound());
        check.stepBackOrForth ("Undo " + name + " again", [&target] { phazite::undo (target); });
        CHECK (describeGroup() == before);
        checkSong (target);
    }

    void run (phazite::test::UndoCheck& check, const juce::String& name, const std::function<void()>& edit, bool ownStep)
    {
        auto& target = getSong();
        check.check (name,
                     [&]
                     {
                         if (ownStep)
                         {
                             edit();
                             return;
                         }

                         const phazite::UndoStep step (target, name);
                         edit();
                     });
    }

    // The audio jobs' work (Normalize, Quantize Audio), which makes its step once what it read is in.
    void waitForAudioWork() const
    {
        REQUIRE (phazite::test::runTimersUntil ([this] { return ! phazite::isReadingAudio (song.context); }));
    }

    phazite::test::TestSong song;
    phazite::TrackID other;
    ClipID clip, onOther, snippet;
    std::vector<ClipID> members;
};
} // namespace

TEST_CASE ("Every edit of what the audio clips of an alias group share, made through one of them, reaches each, after it, its "
           "Undo and its Redo")
{
    AudioAliases a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    auto& context = a.song.context;
    const auto onOther = a.onOther;

    a.checkShared (
        check, "Change Clip Pitch", [&] { phazite::setClipPitch (context, onOther, 3.5f); },
        [&] (ClipID member) { CHECK (phazite::getClipPitch (song, member) == doctest::Approx (3.5f)); });

    a.checkShared (
        check, "Reverse Clip", [&] { phazite::setClipReversed (context, onOther, true); },
        [&] (ClipID member) { CHECK (phazite::isClipReversed (song, member)); });

    a.checkShared (
        check, "Follow Tempo", [&] { phazite::setClipFollowsTempo (context, onOther, true); },
        [&] (ClipID member) { CHECK (phazite::getClipTempo (song, member) == doctest::Approx (120.0)); });

    a.checkShared (
        check, "Set the pitch of the selected clips",
        [&] { phazite::setClipsTo (context, { onOther }, phazite::ClipProperty::pitch, -2.0); },
        [&] (ClipID member) { CHECK (phazite::getClipPitch (song, member) == doctest::Approx (-2.0f)); }, true);

    a.checkShared (
        check, "Change the pitch of the selected clips",
        [&] { phazite::changeClips (context, { onOther }, phazite::ClipProperty::pitch, 1.0); },
        [&] (ClipID member) { CHECK (phazite::getClipPitch (song, member) == doctest::Approx (1.0f)); }, true);

    // The commands on what is selected: the clip through which they are run.
    a.song.select (onOther);
    a.checkShared (
        check, "clip.reverse", [&] { a.song.run ("clip.reverse"); },
        [&] (ClipID member) { CHECK (phazite::isClipReversed (song, member)); }, true);
    a.checkShared (
        check, "clip.followTempo", [&] { a.song.run ("clip.followTempo"); },
        [&] (ClipID member) { CHECK (phazite::getClipTempo (song, member) > 0.0); }, true);

    // A warp marker added through a clip that doesn't follow the tempo yet makes it follow first (AudioClips.h: Warp
    // markers), and so every clip of the group: each follows the song's tempo, played at 120 BPM, with the one marker.
    REQUIRE (phazite::getClipTempo (song, a.clip) <= 0.0);
    a.checkShared (
        check, "Add Warp Marker to a clip that doesn't follow the tempo", [&] { REQUIRE (phazite::addWarpMarker (context, onOther, 0.5)); },
        [&] (ClipID member)
        {
            CHECK (phazite::getClipTempo (song, member) == doctest::Approx (120.0));
            CHECK (phazite::getWarpMarkers (song, member).size() == 1);
        });
    CHECK (phazite::getClipTempo (song, a.snippet) <= 0.0); // undone again, in every clip

    // Locate Audio File, by the path every clip of the group plays: the project's copy of the file found in its place in each.
    const auto played = phazite::getClipValue (song, a.clip)->sound.file;
    const auto found =
        writeHitsAt (a.song.getSoundsFolder().getChildFile ("Found").getChildFile ("Hits found.wav"), { 0.0, 0.53, 0.97, 1.54 }, 2.0);
    a.checkShared (
        check, "Locate Audio File",
        [&]
        {
            REQUIRE (phazite::locateAudioFile (context, played, found));
            phazite::test::waitForAudioWork (context); // copied into the project in the background
        },
        [&] (ClipID member)
        {
            CHECK (phazite::getClipValue (song, member)->sound.file != played);
            CHECK (phazite::getClipFile (song, member).getFileName() == juce::String ("Hits found.wav"));
            CHECK (phazite::getClipFile (song, member).existsAsFile());
        },
        true);

    // Following the tempo, with a marker at half a second of the file, kept for the edits after it.
    check.checkSeveral ("follow the tempo, with a marker at 0.5 s",
                        [&]
                        {
                            phazite::setClipFollowsTempo (context, onOther, true);
                            REQUIRE (phazite::addWarpMarker (context, onOther, 0.5));
                        });
    REQUIRE (a.shareTheSound());
    REQUIRE (phazite::getWarpMarkers (song, a.clip).size() == 1);

    a.checkShared (
        check, "Change Clip Tempo", [&] { phazite::setClipTempo (context, onOther, 100.0); },
        [&] (ClipID member) { CHECK (phazite::getClipTempo (song, member) == doctest::Approx (100.0)); });

    a.checkShared (
        check, "Add Warp Marker", [&] { REQUIRE (phazite::addWarpMarker (context, onOther, 1.0)); },
        [&] (ClipID member) { CHECK (phazite::getWarpMarkers (song, member).size() == 2); });

    // The marker at 0.5 s plays at beat 9 through the clip on the Other track: moved to beat 9.25. The audio at the clip's
    // start stays where it is, held by a marker of its own.
    a.checkShared (
        check, "Move Warp Marker", [&] { phazite::moveWarpMarker (context, onOther, 0.5, at (9.25)); },
        [&] (ClipID member) { CHECK (phazite::getWarpMarkers (song, member).size() == 2); });

    // Through the trimmed clip, whose start is another time of the file: the marker it adds to hold its start is every
    // clip's.
    a.checkShared (
        check, "Move Warp Marker through the trimmed clip", [&] { phazite::moveWarpMarker (context, a.snippet, 0.5, at (16.75)); },
        [&] (ClipID member) { CHECK (phazite::getWarpMarkers (song, member).size() == 2); });

    a.checkShared (
        check, "Remove Warp Marker", [&] { phazite::removeWarpMarker (context, onOther, 0.5); },
        [&] (ClipID member) { CHECK (phazite::getWarpMarkers (song, member).empty()); });

    a.checkShared (
        check, "Remove Warp Markers", [&] { phazite::removeWarpMarkers (context, onOther); },
        [&] (ClipID member) { CHECK (phazite::getWarpMarkers (song, member).empty()); });

    a.checkShared (
        check, "Stop Following Tempo", [&] { phazite::setClipFollowsTempo (context, onOther, false); },
        [&] (ClipID member)
        {
            CHECK (phazite::getClipTempo (song, member) == doctest::Approx (0.0));
            CHECK (phazite::getWarpMarkers (song, member).empty());
        });

    // Quantize Audio, whose step comes once the hits are found in the background.
    a.checkShared (
        check, "Quantize Audio",
        [&]
        {
            phazite::quantizeAudio (context, { onOther }, lasting (1.0));
            a.waitForAudioWork();
        },
        [&] (ClipID member) { CHECK_FALSE (phazite::getWarpMarkers (song, member).empty()); }, true);

    // Every sample of the song at its own speed again, by the song's switch.
    a.checkShared (
        check, "transport.samplesFollowTempo", [&] { a.song.run ("transport.samplesFollowTempo"); },
        [&] (ClipID member) { CHECK (phazite::getClipTempo (song, member) == doctest::Approx (0.0)); }, true);

    check.walkBackAndForth();
    checkSong (song);
}

TEST_CASE ("An edit of what an audio clip of an alias group keeps of its own changes that clip alone")
{
    AudioAliases a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    auto& context = a.song.context;
    const auto onOther = a.onOther;

    a.checkOwn (check, "Change Clip Gain", onOther, [&] { phazite::setClipGain (context, onOther, -6.0f); });
    a.checkOwn (
        check, "Normalize Clip", onOther,
        [&]
        {
            phazite::normalizeClip (context, onOther);
            a.waitForAudioWork();
        },
        true);
    a.checkOwn (check, "Fade In", onOther, [&] { phazite::fadeClipTo (context, onOther, true, at (8.5)); });
    a.checkOwn (check, "Change Fade Shape", onOther,
                [&] { phazite::setFadeCurve (context, onOther, false, { phazite::FadeShape::sCurve, 0.5f }); });
    a.checkOwn (check, "Fade Out", onOther, [&] { phazite::fadeClipTo (context, onOther, false, at (11.0)); });
    a.checkOwn (check, "Change Fade In Shape", onOther,
                [&] { phazite::setFadeCurve (context, onOther, true, { phazite::FadeShape::sCurve, -0.5f }); });
    a.checkOwn (check, "Mute Clip", onOther, [&] { phazite::toggleClipsMuted (context, { onOther }); });
    a.checkOwn (check, "Move Clip", onOther, [&] { phazite::moveClip (context, onOther, at (10.0), a.other); });
    a.checkOwn (check, "Trim Clip End", onOther, [&] { phazite::trimClipEnd (context, onOther, at (10.0)); });
    a.checkOwn (check, "Trim Clip Start", onOther, [&] { phazite::trimClipStart (context, onOther, at (9.0)); });

    // Slip, of the trimmed clip, which plays a part of its file: another part of it, the clip where it is.
    a.checkOwn (check, "Slip Clip", a.snippet,
                [&] { phazite::slipClip (context, a.snippet, phazite::getSlipOffset (context, a.snippet), at (16.75), at (17.0)); });

    check.walkBackAndForth();
    checkSong (song);
}

TEST_CASE ("Rename writes the name into each clip of an audio alias group, as into each of a MIDI one")
{
    AudioAliases a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    check.check ("Rename Clip", [&] { phazite::renameClips (a.song.context, { a.onOther }, "Loop"); });

    for (const auto member : a.members)
    {
        INFO ("clip " << member.getRaw());
        CHECK (phazite::getClip (a.song.context, member)->name == "Loop");
    }

    CHECK (a.shareTheSound());
    checkSong (song);
}

TEST_CASE ("Choosing another take of an audio clip of an alias group changes that clip alone")
{
    // A clip recorded round a looped range of a second, three times: its file holds three takes, one after another, and it
    // plays the last. Only a recording through the engine makes one, so its take values are written as the recorder writes
    // them (AudioRecorder.cpp), in a step of their own before the test's; then an alias of it, which plays the same take.
    phazite::test::TestSong test;
    test.setTempo (120.0);
    const auto file = writeHitsAt (test.getSoundsFolder().getChildFile ("Takes.wav"), { 0.1, 1.1, 2.1 }, 3.0);
    const auto clip = test.addAudioClip (test.firstTrack, file, 0.0);
    auto& song = *phazite::findSong (test.context);

    {
        const phazite::UndoStep step (song, "Record");
        auto value = *phazite::getClipValue (song, clip);
        value.takes = 3;
        value.take = 3;
        value.takeLength = 1.0;
        value.start = at (0.0);
        value.fileStart = 2.0;
        value.fileEnd = 3.0;
        phazite::setClip (song, value);
    }

    const auto alias = test.copyClip (clip, 8.0, true);
    REQUIRE (sorted (phazite::getLinkedClips (test.context, clip)) == sorted ({ clip, alias }));
    REQUIRE (phazite::getClipTakes (test.context, alias).count == 3);
    REQUIRE (phazite::getClipTakes (test.context, alias).playing == 3);
    song.getUndoManager().clearUndoHistory();
    checkSong (song);

    const auto clipBefore = describeClip (song, clip), aliasBefore = describeClip (song, alias);
    phazite::test::UndoCheck check (test.context);
    check.check ("Choose Take", [&] { phazite::setClipTake (test.context, alias, 1); });

    // The alias plays the first take, the first second of the file; the clip it was copied from plays the last still.
    CHECK (phazite::getClipTakes (test.context, alias).playing == 1);
    CHECK (phazite::getFilePart (*phazite::getClipValue (song, alias)).start == doctest::Approx (0.0));
    CHECK (phazite::getClipTakes (test.context, clip).playing == 3);
    CHECK (describeClip (song, clip) == clipBefore);
    const auto aliasAfter = describeClip (song, alias);
    CHECK (aliasAfter != aliasBefore);
    CHECK (phazite::shareTheSound (phazite::copyWholeClip (song, clip), phazite::copyWholeClip (song, alias)));

    check.stepBackOrForth ("Undo Choose Take", [&song] { phazite::undo (song); });
    CHECK (describeClip (song, alias) == aliasBefore);
    CHECK (describeClip (song, clip) == clipBefore);
    check.stepBackOrForth ("Redo Choose Take", [&song] { phazite::redo (song); });
    CHECK (describeClip (song, alias) == aliasAfter);
    CHECK (describeClip (song, clip) == clipBefore);
    checkSong (song);
}

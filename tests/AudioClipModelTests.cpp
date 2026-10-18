// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip as the audio clip editor reaches it, by ID (src/model/arrangement/AudioClipModel.h): its file and where
// all of it plays, its gain, pitch, reverse and following the song's tempo, each change one undo step; what the selection
// asks the editor dock to show; and the Reverse and Follow Tempo commands for the selected clips.
#include "UiTestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/SongGesture.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

// An open project at 120 BPM (a beat is half a second) with tracks A and B: A with a MIDI clip from beat 4 to 8, and B
// with an audio clip from beat 0 to 8, a sine 4 seconds long.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        setEdit (context, edit.get());

        auto* notes = phazite::insertMidiClip (*a, { te::BeatPosition::fromBeats (4.0), te::BeatPosition::fromBeats (8.0) }, nullptr);
        REQUIRE (notes != nullptr);
        midi = phazite::getClipID (*notes);

        const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 4.0);
        const auto imported = phazite::importFiles (context, { file.getFullPathName() }, phazite::getTrackID (*b), beat (0.0));
        REQUIRE (imported.size() == 1);
        audio = imported[0];
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    phazite::AudioClipInfo get() const
    {
        const auto info = phazite::getAudioClip (context, audio);
        REQUIRE (info.has_value());
        return *info;
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
    phazite::ClipID midi, audio;
};
} // namespace

TEST_CASE ("An audio clip by ID: its file, where all of it plays, and how")
{
    Song song;
    const auto info = song.get();
    CHECK (info.clip.id == song.audio);
    CHECK (info.clip.track == phazite::getTrackID (*song.b));
    CHECK (info.fileName == juce::String ("Tone.wav"));
    CHECK (info.sampleRate == doctest::Approx (44100.0));
    CHECK (info.channels == 1);
    CHECK (info.seconds == doctest::Approx (4.0));
    CHECK (info.content.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (info.content.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (info.gain == doctest::Approx (0.0f));
    CHECK (info.pitch == doctest::Approx (0.0f));
    CHECK_FALSE (info.reversed);
    CHECK_FALSE (info.followsTempo);
    CHECK (info.tempo == doctest::Approx (0.0));
    CHECK (info.state == phazite::AudioState::ready);

    // Trimmed, the clip plays less of its file, which plays where it did.
    phazite::trimClipStart (song.context, song.audio, beat (2.0));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));

    // A MIDI clip isn't one, and neither is nothing.
    CHECK_FALSE (phazite::getAudioClip (song.context, song.midi).has_value());
    CHECK_FALSE (phazite::getAudioClip (song.context, {}).has_value());
}

TEST_CASE ("An audio clip's end stops where its audio does, though its start was trimmed")
{
    Song song;
    phazite::trimClipStart (song.context, song.audio, beat (2.0)); // a second of the file off its start
    phazite::trimClipEnd (song.context, song.audio, beat (12.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0)); // not 10: the file ends there
}

TEST_CASE ("An audio clip's gain and pitch change as undo steps of their own, within the editor's range")
{
    Song song;
    auto& context = song.context;

    phazite::setClipGain (context, song.audio, 6.0f);
    CHECK (song.get().gain == doctest::Approx (6.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Clip Gain"));
    phazite::setClipGain (context, song.audio, 100.0f);
    CHECK (song.get().gain == doctest::Approx (phazite::maxClipGain));
    phazite::setClipGain (context, song.audio, -100.0f);
    CHECK (song.get().gain == doctest::Approx (phazite::minClipGain));
    phazite::undo (*song.edit);
    phazite::undo (*song.edit);
    CHECK (song.get().gain == doctest::Approx (6.0f));

    // In semitones, with cents as hundredths, the clip keeping its length.
    phazite::setClipPitch (context, song.audio, 2.15f);
    CHECK (song.get().pitch == doctest::Approx (2.15f));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (song.describeUndo() == juce::String ("Undo Change Clip Pitch"));
    phazite::setClipPitch (context, song.audio, -100.0f);
    CHECK (song.get().pitch == doctest::Approx (-phazite::maxClipPitch));

    // A drag of a field is one step, however many values it sets.
    {
        phazite::SongGesture drag (context, "Change Clip Gain");

        for (const auto gain : { 1.0f, 2.0f, 3.0f })
            phazite::setClipGain (context, song.audio, gain);
    }

    CHECK (song.get().gain == doctest::Approx (3.0f));
    phazite::undo (*song.edit);
    CHECK (song.get().gain == doctest::Approx (6.0f));
}

TEST_CASE ("A reversed clip plays the same part of its audio, backwards, where it is")
{
    Song song;
    auto& context = song.context;

    // The clip plays the file from half a second in, for two and a half seconds.
    phazite::trimClipStart (context, song.audio, beat (1.0));
    phazite::trimClipEnd (context, song.audio, beat (6.0));
    song.edit->getUndoManager().clearUndoHistory();

    phazite::setClipReversed (context, song.audio, true);
    CHECK (song.get().reversed);
    CHECK (song.describeUndo() == juce::String ("Undo Reverse Clip"));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (1.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (6.0));

    // Backwards, the file's last second (which the clip doesn't play) comes first: the clip starts two beats into it.
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (-1.0));
    CHECK (song.get().content.getEnd().inBeats() == doctest::Approx (7.0));

    // Tracktion makes the reversed copy of the file in the background; the clip is silent until it is ready.
    CHECK (song.get().state != phazite::AudioState::missing);
    CHECK (phazite::test::dispatchMessagesUntil ([&song] { return song.get().state == phazite::AudioState::ready; }, 10000));

    phazite::undo (*song.edit);
    CHECK_FALSE (song.get().reversed);
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));

    phazite::redo (*song.edit);
    CHECK (song.get().reversed);
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (-1.0));

    phazite::setClipReversed (context, song.audio, false);
    CHECK_FALSE (song.get().reversed);
    CHECK (song.describeUndo() == juce::String ("Undo Play Clip Forwards"));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("A clip that follows the tempo keeps its start and its audio, and its own tempo stretches it")
{
    Song song;
    auto& context = song.context;
    phazite::trimClipStart (context, song.audio, beat (2.0)); // it plays the file from a second in
    song.edit->getUndoManager().clearUndoHistory();

    // From the song's tempo, so nothing changes yet.
    phazite::setClipFollowsTempo (context, song.audio, true);
    CHECK (song.describeUndo() == juce::String ("Undo Follow Tempo"));
    CHECK (song.get().followsTempo);
    CHECK (song.get().tempo == doctest::Approx (120.0));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (song.get().content.getEnd().inBeats() == doctest::Approx (8.0));

    // The time in the file that plays at a beat, which the waveforms draw: a second in where the clip starts.
    auto* clip = dynamic_cast<te::AudioClipBase*> (te::findClipForID (*song.edit, phazite::toTracktion (song.audio)));
    REQUIRE (clip != nullptr);
    CHECK (phazite::getFileTimeAt (*clip, te::BeatPosition::fromBeats (2.0)) == doctest::Approx (1.0));
    CHECK (phazite::getFileTimeAt (*clip, te::BeatPosition::fromBeats (0.0)) == doctest::Approx (0.0)); // trimmed off

    // Played at 60 BPM, the audio plays twice as fast in the song, and the clip ends sooner.
    phazite::setClipTempo (context, song.audio, 60.0);
    CHECK (song.describeUndo() == juce::String ("Undo Change Clip Tempo"));
    CHECK (song.get().tempo == doctest::Approx (60.0));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (5.0));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (1.0));
    CHECK (song.get().content.getEnd().inBeats() == doctest::Approx (5.0));
    CHECK (phazite::getFileTimeAt (*clip, te::BeatPosition::fromBeats (2.0)) == doctest::Approx (1.0)); // the same audio
    CHECK (phazite::getFileTimeAt (*clip, te::BeatPosition::fromBeats (3.0)) == doctest::Approx (2.0)); // a beat a second
    phazite::setClipTempo (context, song.audio, 1000.0);
    CHECK (song.get().tempo == doctest::Approx (phazite::maxClipTempo));
    phazite::undo (*song.edit);

    // At its own speed again, from where it starts.
    phazite::setClipFollowsTempo (context, song.audio, false);
    CHECK (song.describeUndo() == juce::String ("Undo Stop Following Tempo"));
    CHECK_FALSE (song.get().followsTempo);
    CHECK (song.get().tempo == doctest::Approx (0.0));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));

    phazite::undo (*song.edit);
    CHECK (song.get().followsTempo);
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (5.0));

    // A tempo setting changes nothing while the clip plays at its own speed.
    phazite::undo (*song.edit);
    phazite::undo (*song.edit);
    CHECK_FALSE (song.get().followsTempo);
    phazite::setClipTempo (context, song.audio, 90.0);
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0));
}

TEST_CASE ("The selection says which editor the dock's first tab shows, and which audio clip")
{
    Song song;
    auto& context = song.context;
    auto& selection = context.getSongSelection();
    CHECK_FALSE (phazite::getClipTypeToEdit (context).has_value());

    selection.add (song.midi);
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::midi);
    CHECK_FALSE (phazite::getAudioClipToEdit (context, beat (0.0)).has_value());

    // MIDI clips win: the piano roll shows them.
    selection.add (song.audio);
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::midi);

    selection.clear();
    selection.add (song.audio);
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::audio);
    CHECK (phazite::getAudioClipToEdit (context, beat (0.0)) == song.audio);

    // A track: its audio clip, the one at the playhead or the nearest; a track without audio clips shows in the piano roll.
    selection.clear();
    selection.add (phazite::getTrackID (*song.b));
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::audio);
    CHECK (phazite::getAudioClipToEdit (context, beat (20.0)) == song.audio);
    CHECK (phazite::getAudioClipNearest (context, phazite::getTrackID (*song.b), beat (3.0)) == song.audio);
    CHECK_FALSE (phazite::getAudioClipNearest (context, phazite::getTrackID (*song.a), beat (3.0)).has_value());

    selection.clear();
    selection.add (phazite::getTrackID (*song.a));
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::midi);
    CHECK_FALSE (phazite::getAudioClipToEdit (context, beat (0.0)).has_value());
}

TEST_CASE ("Reverse and Follow Tempo are commands for the selected audio clips")
{
    Song song;
    auto& context = song.context;
    phazite::CommandRegistry registry;
    phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
    const auto* reverse = registry.find ("clip.reverse");
    const auto* follow = registry.find ("clip.followTempo");
    REQUIRE (reverse != nullptr);
    REQUIRE (follow != nullptr);

    // Nothing to act on with only a MIDI clip selected.
    context.getSongSelection().add (song.midi);
    CHECK_FALSE (reverse->isEnabled());
    CHECK_FALSE (follow->isEnabled());

    context.getSongSelection().add (song.audio);
    CHECK (reverse->isEnabled());
    CHECK_FALSE (reverse->isTicked());
    CHECK (registry.invoke ("clip.reverse"));
    CHECK (song.get().reversed);
    CHECK (reverse->isTicked());
    CHECK (song.describeUndo() == juce::String ("Undo Reverse"));

    CHECK (registry.invoke ("clip.followTempo"));
    CHECK (song.get().followsTempo);
    CHECK (follow->isTicked());

    // Again, they turn off.
    CHECK (registry.invoke ("clip.reverse"));
    CHECK_FALSE (song.get().reversed);
    CHECK (registry.invoke ("clip.followTempo"));
    CHECK_FALSE (song.get().followsTempo);
}

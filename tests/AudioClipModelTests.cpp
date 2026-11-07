// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip as the audio clip editor reaches it, by ID (src/model/arrangement/AudioClipModel.h): its file and where
// all of it plays, its gain, pitch, reverse and following the song's tempo, each change one undo step; what the selection
// asks the editor dock to show; Normalize; and the Reverse, Follow Tempo and Normalize commands for the selected clips.
#include "UiTestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/SongGesture.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
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
        const auto tracks = phazite::ensureTracks (*song, 2);
        a = tracks[0];
        b = tracks[1];
        setSong (context, song.get());

        midi = phazite::insertMidiClip (*song, a, { beat (4.0), beat (8.0) }, nullptr);
        REQUIRE (midi.isValid());

        const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 4.0);
        const auto imported = phazite::importFiles (context, { file.getFullPathName() }, b, beat (0.0));
        REQUIRE (imported.size() == 1);
        audio = imported[0];
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { setSong (context, nullptr); }

    phazite::AudioClipInfo get() const
    {
        const auto info = phazite::getAudioClip (context, audio);
        REQUIRE (info.has_value());
        return *info;
    }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::test::TemporaryFolder temp;
    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::TrackID a, b;
    phazite::ClipID midi, audio;
};
} // namespace

TEST_CASE ("An audio clip by ID: its file, where all of it plays, and how")
{
    Song song;
    const auto info = song.get();
    CHECK (info.clip.id == song.audio);
    CHECK (info.clip.track == song.b);
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
    phazite::undo (*song.song);
    phazite::undo (*song.song);
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
    phazite::undo (*song.song);
    CHECK (song.get().gain == doctest::Approx (6.0f));
}

TEST_CASE ("The inspector's edits change an audio clip's gain and pitch with its start and length, and leave a MIDI clip's be")
{
    Song song;
    auto& context = song.context;
    const std::vector clips { song.audio, song.midi };

    // By an amount: the audio clip's gain; set: its pitch, as one undo step for both clips.
    phazite::changeClips (context, clips, phazite::ClipProperty::gain, -3.0);
    CHECK (song.get().gain == doctest::Approx (-3.0f));
    phazite::setClipsTo (context, clips, phazite::ClipProperty::pitch, 5.0);
    CHECK (song.get().pitch == doctest::Approx (5.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Clip Pitch"));

    // Moved together, as far as the earlier (the audio clip, at the song's start) can go: not at all, back.
    phazite::changeClips (context, clips, phazite::ClipProperty::start, -2.0);
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (phazite::getClip (context, song.midi)->range.getStart().inBeats() == doctest::Approx (4.0));

    // Set, the earlier goes there and the other with it; each length becomes the one set.
    phazite::setClipsTo (context, clips, phazite::ClipProperty::start, 2.0);
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (phazite::getClip (context, song.midi)->range.getStart().inBeats() == doctest::Approx (6.0));
    phazite::setClipsTo (context, clips, phazite::ClipProperty::length, 2.0);
    CHECK (song.get().clip.range.getLength().inBeats() == doctest::Approx (2.0));
    CHECK (phazite::getClip (context, song.midi)->range.getLength().inBeats() == doctest::Approx (2.0));
    CHECK (song.describeUndo() == juce::String ("Undo Change Clip Length"));
}

TEST_CASE ("A reversed clip plays the same part of its audio, backwards, where it is")
{
    Song song;
    auto& context = song.context;

    // The clip plays the file from half a second in, for two and a half seconds.
    phazite::trimClipStart (context, song.audio, beat (1.0));
    phazite::trimClipEnd (context, song.audio, beat (6.0));
    song.song->getUndoManager().clearUndoHistory();

    phazite::setClipReversed (context, song.audio, true);
    CHECK (song.get().reversed);
    CHECK (song.describeUndo() == juce::String ("Undo Reverse Clip"));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (1.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (6.0));

    // Backwards, the file's last second (which the clip doesn't play) comes first: the clip starts two beats into it.
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (-1.0));
    CHECK (song.get().content.getEnd().inBeats() == doctest::Approx (7.0));

    // Phazite renders the file reversed in the background; the clip is silent until the render is ready.
    CHECK (song.get().state != phazite::AudioState::missing);
    CHECK (phazite::test::dispatchMessagesUntil ([&song] { return song.get().state == phazite::AudioState::ready; }, 10000));

    phazite::undo (*song.song);
    CHECK_FALSE (song.get().reversed);
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));

    phazite::redo (*song.song);
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
    song.song->getUndoManager().clearUndoHistory();

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
    const auto& model = *song.song;
    REQUIRE (phazite::isAudioState (phazite::findClipState (model, song.audio)));
    CHECK (phazite::getFileTimeAt (model, song.audio, beat (2.0)) == doctest::Approx (1.0));
    CHECK (phazite::getFileTimeAt (model, song.audio, beat (0.0)) == doctest::Approx (0.0)); // trimmed off

    // Played at 60 BPM, the audio plays twice as fast in the song, and the clip ends sooner.
    phazite::setClipTempo (context, song.audio, 60.0);
    CHECK (song.describeUndo() == juce::String ("Undo Change Clip Tempo"));
    CHECK (song.get().tempo == doctest::Approx (60.0));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (5.0));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (1.0));
    CHECK (song.get().content.getEnd().inBeats() == doctest::Approx (5.0));
    CHECK (phazite::getFileTimeAt (model, song.audio, beat (2.0)) == doctest::Approx (1.0)); // the same audio
    CHECK (phazite::getFileTimeAt (model, song.audio, beat (3.0)) == doctest::Approx (2.0)); // a beat a second
    phazite::setClipTempo (context, song.audio, 1000.0);
    CHECK (song.get().tempo == doctest::Approx (phazite::maxClipTempo));
    phazite::undo (*song.song);

    // At its own speed again, from where it starts.
    phazite::setClipFollowsTempo (context, song.audio, false);
    CHECK (song.describeUndo() == juce::String ("Undo Stop Following Tempo"));
    CHECK_FALSE (song.get().followsTempo);
    CHECK (song.get().tempo == doctest::Approx (0.0));
    CHECK (song.get().clip.range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (song.get().content.getStart().inBeats() == doctest::Approx (0.0));

    phazite::undo (*song.song);
    CHECK (song.get().followsTempo);
    CHECK (song.get().clip.range.getEnd().inBeats() == doctest::Approx (5.0));

    // A tempo setting changes nothing while the clip plays at its own speed.
    phazite::undo (*song.song);
    phazite::undo (*song.song);
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
    selection.add (song.b);
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::audio);
    CHECK (phazite::getAudioClipToEdit (context, beat (20.0)) == song.audio);
    CHECK (phazite::getAudioClipNearest (context, song.b, beat (3.0)) == song.audio);
    CHECK_FALSE (phazite::getAudioClipNearest (context, song.a, beat (3.0)).has_value());

    selection.clear();
    selection.add (song.a);
    CHECK (phazite::getClipTypeToEdit (context) == phazite::ClipType::midi);
    CHECK_FALSE (phazite::getAudioClipToEdit (context, beat (0.0)).has_value());
}

namespace
{
// Writes a WAV file of a sine at 220 Hz, a part of it at each level (as a share of full scale), each part some seconds long.
juce::File writeLevels (const juce::File& file, std::initializer_list<std::pair<double, float>> parts)
{
    constexpr double sampleRate = 44100.0;
    juce::AudioBuffer<float> buffer (1, 0);

    for (const auto& [seconds, level] : parts)
    {
        const auto start = buffer.getNumSamples(), length = (int) (seconds * sampleRate);
        buffer.setSize (1, start + length, true);

        for (int i = 0; i < length; ++i)
            buffer.setSample (0, start + i,
                              level * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * (start + i) / sampleRate));
    }

    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return file;
}

// Imports a file onto track B at a beat, as a new clip.
phazite::ClipID importAt (Song& song, const juce::File& file, double start)
{
    const auto imported = phazite::importFiles (song.context, { file.getFullPathName() }, song.b, beat (start));
    REQUIRE (imported.size() == 1);
    return imported[0];
}

float getGain (Song& song, phazite::ClipID clip)
{
    const auto info = phazite::getAudioClip (song.context, clip);
    REQUIRE (info.has_value());
    return info->gain;
}

// The gain Normalize gives a clip that peaks at a level (a share of full scale).
float normalizedGainFor (float peak)
{
    return phazite::normalizedPeak - juce::Decibels::gainToDecibels (peak);
}
} // namespace

TEST_CASE ("Normalize sets an audio clip's gain from the loudest peak of the part of its file it plays, as one undo step")
{
    Song song;
    auto& context = song.context;

    // The tone peaks at a quarter of full scale, -12 dB: its gain goes up by 11 dB, to play its peak at -1 dB.
    CHECK (phazite::getClipPeak (*song.song, song.audio).value_or (0.0f) == doctest::Approx (0.25f).epsilon (0.001));
    phazite::normalizeClip (context, song.audio);
    CHECK (song.get().gain == doctest::Approx (normalizedGainFor (0.25f)).epsilon (0.001));
    CHECK (song.describeUndo() == juce::String ("Undo Normalize Clip"));
    phazite::undo (*song.song);
    CHECK (song.get().gain == doctest::Approx (0.0f));

    // Whatever the gain was: the file is loud for 2 seconds (4 beats at 120 BPM), then quiet for 2. Only the part a clip
    // plays counts, so a clip trimmed to the quiet part goes up further.
    const auto loudThenQuiet = writeLevels (song.temp.folder.getChildFile ("Loud then quiet.wav"), { { 2.0, 0.9f }, { 2.0, 0.1f } });
    const auto whole = importAt (song, loudThenQuiet, 16.0);
    phazite::setClipGain (context, whole, 6.0f);
    phazite::normalizeClip (context, whole);
    CHECK (getGain (song, whole) == doctest::Approx (normalizedGainFor (0.9f)).epsilon (0.001));

    const auto quietPart = importAt (song, loudThenQuiet, 32.0);
    phazite::trimClipStart (context, quietPart, beat (36.0));
    phazite::normalizeClip (context, quietPart);
    CHECK (getGain (song, quietPart) == doctest::Approx (normalizedGainFor (0.1f)).epsilon (0.001));

    // Within the editor's range: a clip too quiet for it goes up as far as it can, and a silent one, which has no peak, and a
    // MIDI clip keep their gains, with no undo step.
    const auto nearlySilent = importAt (song, writeLevels (song.temp.folder.getChildFile ("Whisper.wav"), { { 1.0, 0.0005f } }), 48.0);
    phazite::normalizeClip (context, nearlySilent);
    CHECK (getGain (song, nearlySilent) == doctest::Approx (phazite::maxClipGain));

    const auto silent = importAt (song, writeLevels (song.temp.folder.getChildFile ("Silence.wav"), { { 1.0, 0.0f } }), 56.0);
    const auto steps = song.describeUndo();
    phazite::normalizeClip (context, silent);
    phazite::normalizeClip (context, song.midi);
    CHECK (getGain (song, silent) == doctest::Approx (0.0f));
    CHECK (song.describeUndo() == steps);
}

TEST_CASE ("Normalize is a command for the selected audio clips, each from its own peak, as one undo step")
{
    Song song;
    auto& context = song.context;
    phazite::CommandRegistry registry;
    phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
    const auto* normalize = registry.find ("clip.normalize");
    REQUIRE (normalize != nullptr);

    context.getSongSelection().add (song.midi);
    CHECK_FALSE (normalize->isEnabled());

    // Two audio clips at different levels: each plays its peak at -1 dB.
    const auto loud = importAt (song, writeLevels (song.temp.folder.getChildFile ("Loud.wav"), { { 1.0, 0.8f } }), 16.0);
    context.getSongSelection().add (song.audio);
    context.getSongSelection().add (loud);
    CHECK (normalize->isEnabled());
    CHECK (registry.invoke ("clip.normalize"));
    CHECK (song.get().gain == doctest::Approx (normalizedGainFor (0.25f)).epsilon (0.001));
    CHECK (getGain (song, loud) == doctest::Approx (normalizedGainFor (0.8f)).epsilon (0.001));
    CHECK (song.describeUndo() == juce::String ("Undo Normalize"));

    phazite::undo (*song.song);
    CHECK (song.get().gain == doctest::Approx (0.0f));
    CHECK (getGain (song, loud) == doctest::Approx (0.0f));
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/Clips.h"
#include "model/Commands.h"
#include "model/Selection.h"
#include "model/SmartTool.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// An Edit with two tracks, a selection and the selection commands.
struct ClipEdit
{
    ClipEdit()
    {
        edit->ensureNumberOfAudioTracks (2);
        tracks = te::getAudioTracks (*edit);
        edit->getUndoManager().clearUndoHistory();
        selection.edit = edit.get();
        phazite::addSelectionCommands (registry, selection);
    }

    ~ClipEdit() { selection.deselectAll(); }

    te::MidiClip& addClip (int track, double start, double length)
    {
        auto* clip = phazite::insertMidiClip (*tracks[track], { beats (start), beats (start + length) }, nullptr);
        REQUIRE (clip != nullptr);
        return *clip;
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM: a beat is 0.5 s
    juce::Array<te::AudioTrack*> tracks;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::CommandRegistry registry;
};

// A one-second WAV file of silence, for audio clips.
struct SilentFile
{
    SilentFile()
    {
        juce::AudioBuffer<float> buffer (1, 44100);
        buffer.clear();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (temp.getFile());

        if (auto writer = juce::WavAudioFormat().createWriterFor (
                stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16)))
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    juce::TemporaryFile temp { ".wav" };
};
} // namespace

TEST_CASE ("The Smart Tool picks what a drag does from where it starts on a clip")
{
    using phazite::ClipZone;
    const juce::Rectangle<float> clip (100.0f, 50.0f, 200.0f, 60.0f);
    const auto zoneAt = [&clip] (float x, float y, bool hasFades = true) { return phazite::getClipZone (clip, { x, y }, hasFades); };

    CHECK (zoneAt (102.0f, 80.0f) == ClipZone::trimStart); // the edges
    CHECK (zoneAt (298.0f, 80.0f) == ClipZone::trimEnd);
    CHECK (zoneAt (105.0f, 52.0f) == ClipZone::fadeIn); // the top corners
    CHECK (zoneAt (295.0f, 52.0f) == ClipZone::fadeOut);
    CHECK (zoneAt (105.0f, 52.0f, false) == ClipZone::trimStart); // a MIDI clip has no fades
    CHECK (zoneAt (200.0f, 60.0f) == ClipZone::selectRange);      // the upper half
    CHECK (zoneAt (200.0f, 100.0f) == ClipZone::move);            // the lower half

    // A narrow clip keeps most of its width for moving.
    CHECK (phazite::getClipZone ({ 0.0f, 0.0f, 12.0f, 40.0f }, { 6.0f, 30.0f }, true) == ClipZone::move);

    for (const auto zone :
         { ClipZone::trimStart, ClipZone::trimEnd, ClipZone::fadeIn, ClipZone::fadeOut, ClipZone::selectRange, ClipZone::move })
        CHECK (phazite::describeClipZone (zone).isNotEmpty());
}

TEST_CASE ("A new MIDI clip is named after its track, and selected")
{
    ClipEdit e;
    e.tracks[0]->setName ("Bass");

    auto* clip = phazite::insertMidiClip (*e.tracks[0], { beats (4.0), beats (8.0) }, &e.selection);
    REQUIRE (clip != nullptr);
    CHECK (clip->getName() == juce::String ("Bass"));
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (clip->getEndBeat().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getSelectedClips (e.selection) == juce::Array<te::Clip*> { clip });
}

TEST_CASE ("Moving a clip keeps its length, and can take it to another track")
{
    ClipEdit e;
    auto& clip = e.addClip (0, 0.0, 4.0);

    phazite::moveClip (clip, beats (8.0), *e.tracks[1]);
    CHECK (clip.getTrack() == e.tracks[1]);
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (clip.getLengthInBeats().inBeats() == doctest::Approx (4.0));

    phazite::moveClip (clip, beats (-3.0), *e.tracks[1]); // not before the song's start
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("Trimming a clip leaves its content in place")
{
    ClipEdit e;
    auto& clip = e.addClip (0, 4.0, 4.0);
    clip.getSequence().addNote (60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);

    phazite::trimClipStart (clip, beats (5.0));
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (5.0));
    CHECK (clip.getEndBeat().inBeats() == doctest::Approx (8.0));
    CHECK (clip.getContentStartBeat().inBeats() == doctest::Approx (4.0)); // so the note is where it was

    phazite::trimClipStart (clip, beats (2.0)); // not before the content starts
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (4.0));

    phazite::trimClipEnd (clip, beats (6.0));
    CHECK (clip.getEndBeat().inBeats() == doctest::Approx (6.0));
    CHECK (clip.getContentStartBeat().inBeats() == doctest::Approx (4.0));

    phazite::trimClipEnd (clip, beats (0.0)); // a clip keeps some length
    CHECK (clip.getLengthInBeats().inBeats() == doctest::Approx (1.0 / 64.0));
}

TEST_CASE ("Copies of a clip are separate, and aliases stay linked")
{
    ClipEdit e;
    auto& original = e.addClip (0, 0.0, 4.0);

    const auto copies = phazite::copyClips ({ &original }, false);
    REQUIRE (copies.size() == 1);
    CHECK (copies[0] != &original);
    CHECK (copies[0]->getStartBeat().inBeats() == doctest::Approx (0.0)); // in the same place, to be moved
    CHECK_FALSE (copies[0]->isLinked());
    CHECK_FALSE (original.isLinked());

    const auto aliases = phazite::copyClips ({ &original }, true);
    REQUIRE (aliases.size() == 1);
    CHECK (original.isLinked());
    CHECK (aliases[0]->getLinkGroupID() == original.getLinkGroupID());

    // A copy of an alias is a separate clip again.
    const auto copyOfAlias = phazite::copyClips ({ aliases[0] }, false);
    REQUIRE (copyOfAlias.size() == 1);
    CHECK_FALSE (copyOfAlias[0]->isLinked());
    CHECK (aliases[0]->isLinked());
}

TEST_CASE ("Fades stay within an audio clip")
{
    ClipEdit e;
    SilentFile file;
    auto clip = e.tracks[0]->insertWaveClip ("Silence", file.temp.getFile(),
                                             { te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.0)) }, false);
    REQUIRE (clip != nullptr);

    phazite::setClipFade (*clip, true, te::TimeDuration::fromSeconds (0.25));
    CHECK (clip->getFadeIn().inSeconds() == doctest::Approx (0.25));

    phazite::setClipFade (*clip, false, te::TimeDuration::fromSeconds (-1.0));
    CHECK (clip->getFadeOut().inSeconds() == doctest::Approx (0.0));

    phazite::setClipFade (*clip, true, te::TimeDuration::fromSeconds (5.0));
    CHECK (clip->getFadeIn().inSeconds() <= 1.0);
}

TEST_CASE ("Delete and duplicate act on the selected clips")
{
    ClipEdit e;
    auto& a = e.addClip (0, 0.0, 4.0);
    auto& b = e.addClip (1, 4.0, 4.0);
    e.selection.selectOnly (a);
    e.selection.addToSelection (b);

    // Duplicates go right after the selection: here, 8 beats later.
    CHECK (phazite::getCurrentName (*e.registry.find ("edit.duplicate")) == juce::String ("Duplicate 2 Clips"));
    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (e.edit->getUndoManager().getUndoDescription() == juce::String ("Duplicate 2 Clips"));

    const auto copies = phazite::getSelectedClips (e.selection);
    REQUIRE (copies.size() == 2);
    CHECK (copies[0]->getTrack() == e.tracks[0]);
    CHECK (copies[0]->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (copies[1]->getTrack() == e.tracks[1]);
    CHECK (copies[1]->getStartBeat().inBeats() == doctest::Approx (12.0));

    REQUIRE (e.registry.invoke ("edit.delete"));
    CHECK (e.tracks[0]->getClips().size() == 1);
    CHECK (e.tracks[1]->getClips().size() == 1);

    phazite::undo (*e.edit);
    CHECK (e.tracks[0]->getClips().size() == 2);
}

TEST_CASE ("Select All selects every clip, or every track while tracks are selected")
{
    ClipEdit e;
    e.addClip (0, 0.0, 4.0);
    e.addClip (1, 0.0, 4.0);

    REQUIRE (e.registry.invoke ("edit.selectAll"));
    CHECK (phazite::getSelectedClips (e.selection).size() == 2);

    e.selection.selectOnly (e.tracks[0]);
    REQUIRE (e.registry.invoke ("edit.selectAll"));
    CHECK (phazite::getSelectedClips (e.selection).isEmpty());
    CHECK (e.selection.getNumObjectsSelected() == 2);
}

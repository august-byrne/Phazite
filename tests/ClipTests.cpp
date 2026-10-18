// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "ui/timeline/SmartTool.h"

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
        phazite::addSelectionCommands (registry, selection, range);
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
    phazite::RangeSelection range;
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
    using phazite::ui::ClipZone;
    const juce::Rectangle<float> clip (100.0f, 50.0f, 200.0f, 60.0f);
    const auto zoneAt = [&clip] (float x, float y, bool hasFades = true) { return phazite::ui::getClipZone (clip, { x, y }, hasFades); };

    CHECK (zoneAt (102.0f, 80.0f) == ClipZone::trimStart); // the edges
    CHECK (zoneAt (298.0f, 80.0f) == ClipZone::trimEnd);
    CHECK (zoneAt (105.0f, 52.0f) == ClipZone::fadeIn); // the top corners
    CHECK (zoneAt (295.0f, 52.0f) == ClipZone::fadeOut);
    CHECK (zoneAt (105.0f, 52.0f, false) == ClipZone::trimStart); // a MIDI clip has no fades
    CHECK (zoneAt (200.0f, 60.0f) == ClipZone::selectRange);      // the upper half
    CHECK (zoneAt (200.0f, 100.0f) == ClipZone::move);            // the lower half

    // A narrow clip keeps most of its width for moving, and a short one (on a short track) moves from anywhere
    // but its edges.
    CHECK (phazite::ui::getClipZone ({ 0.0f, 0.0f, 12.0f, 40.0f }, { 6.0f, 30.0f }, true) == ClipZone::move);
    CHECK (phazite::ui::getClipZone ({ 0.0f, 0.0f, 200.0f, 29.0f }, { 100.0f, 5.0f }, false) == ClipZone::move);
    CHECK (phazite::ui::getClipZone ({ 0.0f, 0.0f, 200.0f, 29.0f }, { 4.0f, 20.0f }, false) == ClipZone::trimStart);

    for (const auto zone :
         { ClipZone::trimStart, ClipZone::trimEnd, ClipZone::fadeIn, ClipZone::fadeOut, ClipZone::selectRange, ClipZone::move })
        CHECK (phazite::ui::describeClipZone (zone).isNotEmpty());
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

namespace
{
// A track's clips as "start-end" in beats, in order, such as "0-2 4-8".
juce::String describeClips (te::ClipTrack& track)
{
    juce::Array<te::Clip*> clips (track.getClips());
    std::sort (clips.begin(), clips.end(), [] (te::Clip* a, te::Clip* b) { return a->getStartBeat() < b->getStartBeat(); });
    juce::StringArray parts;

    for (auto* clip : clips)
        parts.add (juce::String (clip->getStartBeat().inBeats(), 2) + "-" + juce::String (clip->getEndBeat().inBeats(), 2));

    return parts.joinIntoString (" ");
}
} // namespace

TEST_CASE ("Split, mute and nudge act on the selected clips")
{
    ClipEdit e;
    auto& clip = e.addClip (0, 0.0, 8.0);
    e.selection.selectOnly (clip);
    e.edit->getTransport().setPosition (e.edit->tempoSequence.toTime (beats (2.0)));

    REQUIRE (e.registry.invoke ("edit.split")); // at the playhead
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-2.00 2.00-8.00"));

    e.selection.selectOnly (clip);
    REQUIRE (e.registry.invoke ("edit.mute"));
    CHECK (clip.isMuted());
    CHECK (e.registry.find ("edit.mute")->isTicked());
    REQUIRE (e.registry.invoke ("edit.mute"));
    CHECK_FALSE (clip.isMuted());

    phazite::nudgeClips ({ &clip }, te::BeatDuration::fromBeats (-1.0)); // not before the start of the song
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (0.0));
    phazite::nudgeClips ({ &clip }, te::BeatDuration::fromBeats (8.0));
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (8.0));
}

TEST_CASE ("Make Unique turns aliases back into separate clips")
{
    ClipEdit e;
    auto& original = e.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips ({ &original }, true);
    REQUIRE (aliases.size() == 1);

    e.selection.selectOnly (aliases[0]);
    REQUIRE (e.registry.invoke ("clip.makeUnique"));
    CHECK_FALSE (aliases[0]->isLinked());
    CHECK_FALSE (original.isLinked()); // left alone in its group, it is no one's alias either
    CHECK_FALSE (e.registry.find ("clip.makeUnique")->isEnabled());
}

TEST_CASE ("Delete and Close Gap moves what comes after back")
{
    ClipEdit e;
    e.addClip (0, 0.0, 4.0);
    auto& middle = e.addClip (0, 4.0, 4.0);
    e.addClip (0, 8.0, 4.0);

    e.selection.selectOnly (middle);
    REQUIRE (e.registry.invoke ("edit.deleteAndCloseGap"));
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-4.00 4.00-8.00"));
}

TEST_CASE ("A time range is deleted, split, inserted and removed")
{
    ClipEdit e;
    e.addClip (0, 0.0, 8.0);
    e.addClip (1, 0.0, 8.0);
    e.range.set ({ beats (2.0), beats (4.0) }, { e.tracks[0] });
    CHECK (phazite::describeSelection (e.selection, e.range, "Delete") == juce::String ("Delete Range"));

    // Delete: on the range's track only, where the clip is split around the hole.
    REQUIRE (e.registry.invoke ("edit.delete"));
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-2.00 4.00-8.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("0.00-8.00"));
    phazite::undo (*e.edit);
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-8.00"));

    // Split: at both edges.
    REQUIRE (e.registry.invoke ("edit.split"));
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-2.00 2.00-4.00 4.00-8.00"));
    phazite::undo (*e.edit);

    // Insert Time: on every track, as long as the range.
    REQUIRE (e.registry.invoke ("edit.insertTime"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("0.00-2.00 4.00-10.00"));
    phazite::undo (*e.edit);
    CHECK (describeClips (*e.tracks[1]) == juce::String ("0.00-8.00"));

    // Remove Time: from every track, closing the gap.
    REQUIRE (e.registry.invoke ("edit.removeTime"));
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-2.00 2.00-6.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("0.00-2.00 2.00-6.00"));

    // A range whose tracks are all gone is empty.
    e.edit->deleteTrack (e.tracks[0]);
    CHECK (e.range.isEmpty());
}

TEST_CASE ("Duplicate copies a time range right after it, and the range moves to the copy")
{
    ClipEdit e;
    auto& crossing = e.addClip (0, 1.0, 2.0); // reaches into the range from before it
    REQUIRE (phazite::addNote (crossing, 60, beats (2.5), te::BeatDuration::fromBeats (0.5), 100) != nullptr);
    auto& after = e.addClip (0, 4.5, 3.5); // where the copy lands
    e.addClip (1, 4.0, 2.0);               // after the range, on a track with nothing in it
    e.range.set ({ beats (2.0), beats (4.0) }, { e.tracks[0], e.tracks[1] });

    const auto* duplicate = e.registry.find ("edit.duplicate");
    REQUIRE (duplicate != nullptr);
    CHECK (duplicate->isEnabled());
    CHECK (phazite::getCurrentName (*duplicate) == juce::String ("Duplicate Range"));
    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (e.edit->getUndoManager().getUndoDescription() == juce::String ("Duplicate Range"));

    // The part inside the range goes right after it, in place of what was there; the clip it came from stays whole.
    CHECK (describeClips (*e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-8.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("4.00-6.00")); // nothing of the range landed there

    te::MidiClip* copy = nullptr;

    for (auto* clip : e.tracks[0]->getClips())
        if (clip != &crossing && clip != &after)
            copy = dynamic_cast<te::MidiClip*> (clip);

    REQUIRE (copy != nullptr);
    CHECK_FALSE (copy->isLinked());
    REQUIRE (copy->getSequence().getNotes().size() == 1);
    CHECK (phazite::getNoteStart (*copy, *copy->getSequence().getNotes().getFirst()).inBeats() == doctest::Approx (4.5));
    CHECK (phazite::getNoteStart (crossing, *crossing.getSequence().getNotes().getFirst()).inBeats() == doctest::Approx (2.5));

    // The copy is the range now, on both tracks, and nothing else got selected: Duplicate again repeats it.
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (e.range.getTracks().size() == 2);
    CHECK (e.selection.getNumObjectsSelected() == 0);

    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (describeClips (*e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-6.00 6.00-7.00 7.00-8.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("4.00-6.00 6.00-8.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (6.0));

    // Each was one undo step, and Undo puts the range back where that step found it, so Duplicate after Undo repeats
    // the same part again.
    phazite::undo (*e.edit);
    CHECK (describeClips (*e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-8.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (4.0));
    phazite::undo (*e.edit);
    CHECK (describeClips (*e.tracks[0]) == juce::String ("1.00-3.00 4.50-8.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("4.00-6.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (2.0));
    CHECK (e.range.getRange().getEnd().inBeats() == doctest::Approx (4.0));
    CHECK (e.range.getTracks().size() == 2);

    // Redo makes the copy again, and the range goes back to it.
    phazite::redo (*e.edit);
    CHECK (describeClips (*e.tracks[0]) == juce::String ("1.00-3.00 4.00-5.00 5.00-8.00"));
    CHECK (e.range.getRange().getStart().inBeats() == doctest::Approx (4.0));
    CHECK (e.range.getRange().getEnd().inBeats() == doctest::Approx (6.0));
}

TEST_CASE ("With a time range and a track selected, Duplicate acts on the range, as its name and Delete do")
{
    ClipEdit e; // a click on a track's header selects the track and leaves the range (Playlist::trackClicked)
    e.addClip (0, 0.0, 4.0);
    e.range.set ({ beats (0.0), beats (4.0) }, { e.tracks[0] });
    e.selection.selectOnly (e.tracks[1]);

    CHECK (phazite::getCurrentName (*e.registry.find ("edit.duplicate")) == juce::String ("Duplicate Range"));
    CHECK (phazite::getCurrentName (*e.registry.find ("edit.delete")) == juce::String ("Delete Range"));
    REQUIRE (e.registry.invoke ("edit.duplicate"));
    CHECK (te::getAudioTracks (*e.edit).size() == 2); // no track was copied
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-4.00 4.00-8.00"));
    CHECK (e.selection.isSelected (e.tracks[1]));
    CHECK_FALSE (e.registry.find ("edit.duplicateAsAlias")->isEnabled()); // still clips and sections only
}

TEST_CASE ("Each tool has a name, a command and a hint")
{
    juce::StringArray ids;

    for (const auto tool : phazite::ui::allTools)
    {
        CHECK (phazite::ui::getToolName (tool).isNotEmpty());
        CHECK (phazite::ui::describeTool (tool).isNotEmpty());
        CHECK_FALSE (ids.contains (phazite::ui::getToolCommandID (tool)));
        ids.add (phazite::ui::getToolCommandID (tool));
    }
}

TEST_CASE ("A crossfade overlaps two touching audio clips, as far as their audio reaches")
{
    ClipEdit e;
    SilentFile file; // a second long
    const auto seconds = [] (double s) { return te::TimePosition::fromSeconds (s); };
    auto first = e.tracks[0]->insertWaveClip ("A", file.temp.getFile(), { te::TimeRange (seconds (0.0), seconds (0.5)) }, false);
    auto second = e.tracks[0]->insertWaveClip (
        "B", file.temp.getFile(), { te::TimeRange (seconds (0.5), seconds (1.0)), te::TimeDuration::fromSeconds (0.25) }, false);
    REQUIRE (first != nullptr);
    REQUIRE (second != nullptr);

    phazite::setCrossfade (*first, *second, seconds (0.5), te::TimeDuration::fromSeconds (0.2));
    CHECK (first->getPosition().getEnd().inSeconds() == doctest::Approx (0.6));
    CHECK (second->getPosition().getStart().inSeconds() == doctest::Approx (0.4));
    CHECK (first->getFadeOut().inSeconds() == doctest::Approx (0.2));
    CHECK (second->getFadeIn().inSeconds() == doctest::Approx (0.2));

    // The second clip's audio starts a quarter of a second before it does, so no further.
    phazite::setCrossfade (*first, *second, seconds (0.5), te::TimeDuration::fromSeconds (2.0));
    CHECK (second->getPosition().getStart().inSeconds() == doctest::Approx (0.25));
    CHECK (first->getPosition().getEnd().inSeconds() == doctest::Approx (0.75));

    // A crossfade of nothing puts them back.
    phazite::setCrossfade (*first, *second, seconds (0.5), te::TimeDuration());
    CHECK (first->getPosition().getEnd().inSeconds() == doctest::Approx (0.5));
    CHECK (second->getPosition().getStart().inSeconds() == doctest::Approx (0.5));
    CHECK (second->getFadeIn().inSeconds() == doctest::Approx (0.0));

    phazite::setFadeShape ({ first.get(), second.get() }, te::AudioFadeCurve::sCurve);
    CHECK (first->getFadeOutType() == te::AudioFadeCurve::sCurve);
    CHECK (second->getFadeInType() == te::AudioFadeCurve::sCurve);
}

TEST_CASE ("Duplicated clips go right after them all")
{
    ClipEdit e;
    auto& a = e.addClip (0, 0.0, 4.0);
    auto& b = e.addClip (1, 2.0, 4.0); // together they span beats 0-6

    // Each copy starts 6 beats after its original, on its track; without a selection manager, nothing gets selected.
    auto copies = phazite::duplicateClips ({ &a, &b }, false, nullptr);
    REQUIRE (copies.size() == 2);
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-4.00 6.00-10.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("2.00-6.00 8.00-12.00"));
    CHECK_FALSE (copies.getFirst()->isLinked());
    CHECK (e.selection.getNumObjectsSelected() == 0);

    // As aliases, with a selection manager: the copy is selected instead of what was.
    e.selection.selectOnly (e.tracks[1]);
    copies = phazite::duplicateClips ({ &a }, true, &e.selection);
    REQUIRE (copies.size() == 1);
    CHECK (copies.getFirst()->isLinked());
    CHECK (copies.getFirst()->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (e.selection.isSelected (copies.getFirst()));
    CHECK_FALSE (e.selection.isSelected (e.tracks[1]));

    // No clips: nothing is copied, and the selection stays.
    CHECK (phazite::duplicateClips ({}, false, &e.selection).isEmpty());
    CHECK (e.selection.isSelected (copies.getFirst()));
}

TEST_CASE ("Splitting at a range's edges")
{
    ClipEdit e;
    e.addClip (0, 0.0, 8.0);
    e.addClip (1, 1.0, 6.0); // across the range's edges too, but on a track the range doesn't cover

    phazite::splitClipsAtEdges ({ e.tracks[0] }, { beats (2.0), beats (6.0) });
    CHECK (describeClips (*e.tracks[0]) == juce::String ("0.00-2.00 2.00-6.00 6.00-8.00"));
    CHECK (describeClips (*e.tracks[1]) == juce::String ("1.00-7.00"));

    // A clip that ends inside the range is cut once.
    phazite::splitClipsAtEdges ({ e.tracks[1] }, { beats (5.0), beats (9.0) });
    CHECK (describeClips (*e.tracks[1]) == juce::String ("1.00-5.00 5.00-7.00"));
}

TEST_CASE ("Aliases, audio clips and fade shapes")
{
    ClipEdit e;
    auto& midi = e.addClip (0, 0.0, 4.0);
    CHECK_FALSE (phazite::hasAliases ({ &midi }));
    CHECK_FALSE (phazite::hasAliases ({}));
    CHECK_FALSE (phazite::hasAudioClips ({ &midi }));
    CHECK_FALSE (phazite::hasFadeShape ({ &midi }, te::AudioFadeCurve::linear)); // there is no audio clip

    const auto aliases = phazite::copyClips ({ &midi }, true);
    REQUIRE (aliases.size() == 1);
    CHECK (phazite::hasAliases ({ &midi }));
    CHECK (phazite::hasAliases (aliases));

    // The shape is on when every audio clip fades in and out with it; MIDI clips don't count.
    SilentFile file;
    auto audio = e.tracks[1]->insertWaveClip ("Silence", file.temp.getFile(),
                                              { te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.0)) }, false);
    REQUIRE (audio != nullptr);
    const juce::Array<te::Clip*> both { &midi, audio.get() };
    CHECK (phazite::hasAudioClips (both));

    phazite::setFadeShape (both, te::AudioFadeCurve::convex);
    CHECK (phazite::hasFadeShape (both, te::AudioFadeCurve::convex));
    CHECK_FALSE (phazite::hasFadeShape (both, te::AudioFadeCurve::linear));

    audio->setFadeOutType (te::AudioFadeCurve::linear); // in and out differ
    CHECK_FALSE (phazite::hasFadeShape (both, te::AudioFadeCurve::convex));
}

TEST_CASE ("A double-click's MIDI clip lasts a bar of the time signature there, as one undo step")
{
    ClipEdit e;
    e.edit->tempoSequence.getTimeSigs()[0]->setStringTimeSig ("3/4");
    e.edit->getUndoManager().clearUndoHistory();

    auto* clip = phazite::addMidiClipAt (*e.tracks[0], beats (6.0), &e.selection);
    REQUIRE (clip != nullptr);
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (6.0));
    CHECK (clip->getEndBeat().inBeats() == doctest::Approx (9.0));
    CHECK (phazite::getSelectedClips (e.selection) == juce::Array<te::Clip*> { clip });
    CHECK (e.edit->getUndoManager().getUndoDescription() == juce::String ("Add MIDI Clip"));

    phazite::undo (*e.edit);
    CHECK (e.tracks[0]->getClips().isEmpty());
}

TEST_CASE ("Draw places a copy of the clip used last, or a bar-long MIDI clip on a track with an instrument")
{
    ClipEdit e;
    CHECK (phazite::placeClip (*e.tracks[0], beats (0.0), nullptr) == nullptr); // nothing used yet, and no instrument

    // A MIDI clip's copy is an alias, which stays linked to it.
    auto& source = e.addClip (0, 0.0, 2.0);
    auto* placed = phazite::placeClip (*e.tracks[1], beats (8.0), &source);
    REQUIRE (placed != nullptr);
    CHECK (placed->getTrack() == e.tracks[1]);
    CHECK (placed->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (placed->getLengthInBeats().inBeats() == doctest::Approx (2.0));
    CHECK (phazite::hasAliases ({ placed }));

    // Once the clip used last is gone: an empty MIDI clip a bar long, on a track with an instrument only.
    const te::SafeSelectable<te::Clip> used (&source);
    phazite::deleteClips ({ &source }, false);
    CHECK (phazite::placeClip (*e.tracks[0], beats (4.0), used.get()) == nullptr);

    e.tracks[0]->pluginList.insertPlugin (e.edit->getPluginCache().createNewPlugin (phazite::devices::Synth::typeID, {}), 0, nullptr);
    auto* empty = dynamic_cast<te::MidiClip*> (phazite::placeClip (*e.tracks[0], beats (4.0), used.get()));
    REQUIRE (empty != nullptr);
    CHECK (empty->getSequence().getNumNotes() == 0);
    CHECK (empty->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (empty->getLengthInBeats().inBeats() == doctest::Approx (4.0)); // a bar of 4/4
}

TEST_CASE ("Slip moves what a clip plays, not the clip, and its content can't start after the clip")
{
    ClipEdit e;
    auto& clip = e.addClip (0, 4.0, 4.0);

    phazite::slipClip (clip, te::TimeDuration::fromSeconds (1.0), te::TimeDuration::fromSeconds (0.25));
    CHECK (clip.getPosition().getOffset().inSeconds() == doctest::Approx (0.75));
    CHECK (clip.getStartBeat().inBeats() == doctest::Approx (4.0));

    phazite::slipClip (clip, te::TimeDuration::fromSeconds (1.0), te::TimeDuration::fromSeconds (3.0));
    CHECK (clip.getPosition().getOffset().inSeconds() == doctest::Approx (0.0));
}

TEST_CASE ("Where linked clips play, an editor showing one of them sees its notes playing")
{
    ClipEdit e;
    auto& original = e.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips ({ &original, &original }, true);
    REQUIRE (aliases.size() == 2);
    const auto seconds = [] (double b) { return te::TimePosition::fromSeconds (b / 2.0); }; // 120 BPM

    // One alias later in the song, whole; one that plays only beats 2 to 4 of the notes (a snippet), from beat 16.
    aliases[0]->setPosition ({ { seconds (8.0), seconds (12.0) }, {} });
    aliases[1]->setPosition ({ { seconds (16.0), seconds (18.0) }, te::TimeDuration::fromSeconds (1.0) });
    const juce::Array<te::MidiClip*> shown { &original };

    CHECK (phazite::getLinkedPlayBeat (shown, 9.5) == std::optional<double> (1.5));
    CHECK (phazite::getLinkedPlayBeat (shown, 16.5) == std::optional<double> (2.5));

    // The clip shown plays there itself, or nothing does: nothing to show in it.
    CHECK_FALSE (phazite::getLinkedPlayBeat (shown, 2.0).has_value());
    CHECK_FALSE (phazite::getLinkedPlayBeat (shown, 6.0).has_value());
    CHECK_FALSE (phazite::getLinkedPlayBeat ({ &original, dynamic_cast<te::MidiClip*> (aliases[0]) }, 9.5).has_value());

    // A clip made unique has no linked clips.
    phazite::makeClipsUnique ({ &original });
    CHECK_FALSE (phazite::getLinkedPlayBeat (shown, 9.5).has_value());
}

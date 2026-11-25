// SPDX-License-Identifier: AGPL-3.0-or-later The song's MIDI links as an area of its values (model/base/internal/SongValues.h,
// MidiLinksArea.h; roadmap step S5.20, the MIDI links' switch). What hears the links change hears it from one place
// (Listener::midiLinksChanged()), which the links' area tells: for each edit made through the model, its Undo and its Redo, once
// for each change of the value, and nothing of another area; another area's change tells it nothing. A song's file holds a
// MIDILINKS while it has a link (an empty one is none); a set equal to the links held makes no step; every edit is one exact undo
// step; the MIDI tap follows the links by the next turn of the message loop; the project's unsaved check compares them as one
// value; and the versions give the song another's links, or none, through the links' code, and a merge's clash on a link is
// chosen each way.
//   phazite_tests.exe --test-case="Song values' MIDI links*"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/ParameterHandle.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/MidiTap.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <array>
#include <functional>
#include <vector>

namespace
{
using phazite::test::at;
using phazite::test::lasting;
namespace schema = phazite::schema;
using phazite::ParameterRef;

// How many calls the values' listener heard, of each area.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }
    void chordLaneChanged() override { ++chords; }
    void midiLinksChanged() override { ++links; }

    int notes = 0, clips = 0, tempo = 0, chords = 0, links = 0;
};

// A song in a project of its own, with its versions and a synth on its first track, the values' listener recording what it
// is told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        synth = s.setInstrument (s.firstTrack, phazite::devices::Synth::typeID);
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    struct Heard
    {
        Told told;
        juce::int64 project = 0, versions = 0;
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        change();
        return { told, s.getProject().getChangeCount() - projectBefore, versions->countSongChanges() - versionsBefore };
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    phazite::DeviceID synth;
    Told told;
};

phazite::MidiLinksValue getLinks (const phazite::Song& song)
{
    return phazite::getMidiLinksValue (song);
}

bool hasLinksPlace (const phazite::Song& song)
{
    return phazite::test::copyWholeSong (song).getChildWithName (schema::MIDILINKS).isValid();
}

phazite::AudioIO& getAudioIO()
{
    return phazite::getAudioIO (phazite::test::getEngineContext());
}

// A song of its own (no project) with a synth on its one track.
struct SynthSong
{
    SynthSong()
    {
        track = phazite::ensureTracks (*song, 1).front();
        synth = phazite::test::addOwnDevice (*song, track, phazite::devices::Synth::typeID);
        song->getUndoManager().clearUndoHistory();
    }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::TrackID track;
    phazite::DeviceID synth;

    ParameterRef cutoff() const { return { synth, "cutoff" }; }
    ParameterRef resonance() const { return { synth, "resonance" }; }
};
} // namespace

TEST_CASE ("Song values' MIDI links: each edit through the model tells the links changed once, its Undo and its Redo too, and "
           "nothing of another area")
{
    Watched w;
    auto& song = w.song;
    REQUIRE_FALSE (hasLinksPlace (song));
    const ParameterRef cutoff { w.synth, "cutoff" }, resonance { w.synth, "resonance" };
    const auto volume = w.s.getVolumeParameter (w.s.firstTrack);

    // Each edit heard once by what counts, the first link's too: an empty MIDILINKS is none, and a song's file holds one while
    // the song has a link.
    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());
        const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
        const auto heard = w.hearEach (name, change);
        REQUIRE (heard.size() == moments.size());

        for (size_t i = 0; i < heard.size(); ++i)
        {
            INFO (moments[i]);
            CHECK (heard[i].told.links == 1); // one write of the links' value, whatever the edit
            CHECK (heard[i].told.clips == 0);
            CHECK (heard[i].told.notes == 0);
            CHECK (heard[i].told.tempo == 0);
            CHECK (heard[i].told.chords == 0);
            CHECK (heard[i].project == heard[i].told.links);
            CHECK (heard[i].versions == heard[i].told.links);
        }
    };

    check ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 21 }, cutoff); });
    CHECK (hasLinksPlace (song));
    check ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 22 }, resonance); });
    check ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 21 }, volume); });   // the controller moves
    check ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 2, 7 }, resonance); }); // the parameter's goes
    check ("Remove MIDI Link", [&] { phazite::unlinkMidiController (song, { 2, 7 }); });
    check ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 3, 74 }, cutoff); });
    CHECK (getLinks (song).links.size() == 2);
    phazite::test::checkSong (song);

    // A device removed takes its links along (forgetMidiLinksOfTheGone()): one write of the links.
    for (const auto& heard : w.hearEach ("Remove Device", [&] { phazite::removeDevice (song, w.synth); }))
    {
        CHECK (heard.told.links == 1);
    }

    REQUIRE (getLinks (song).links.size() == 1);
    CHECK (phazite::findLinkedParameter (song, { 1, 21 }) == volume);
    phazite::test::checkSong (song);

    // A link the song holds made again as it is, and a controller unlinked that has no link: no change, nothing told, no step.
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    const auto same = w.hear (
        [&]
        {
            const phazite::UndoStep step (song, "Nothing");
            phazite::linkMidiController (song, { 1, 21 }, volume);
            phazite::unlinkMidiController (song, { 9, 9 });
        });
    CHECK (same.told.links == 0);
    CHECK (same.project == 0);
    CHECK (same.versions == 0);
    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
}

TEST_CASE ("Song values' MIDI links: another area's change tells nothing of the links, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());

    {
        const phazite::UndoStep step (song, "Link MIDI Controller");
        phazite::linkMidiController (song, { 1, 21 }, { w.synth, "cutoff" });
    }

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
        {
            CHECK (heard.told.links == 0);
        }
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Add Effect", [&] { w.s.addEffect (track, phazite::devices::Equalizer::typeID); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' MIDI links: links set equal to those the song holds make no undo step, tell nothing and keep the value held")
{
    SynthSong s;
    auto& song = *s.song;

    {
        const phazite::UndoStep step (song, "Link MIDI Controllers");
        phazite::linkMidiController (song, { 1, 21 }, s.cutoff());
        phazite::linkMidiController (song, { 1, 22 }, s.resonance());
    }

    auto& values = *song.getValues();
    const auto held = values.midiLinks.get();
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    Told told;
    values.addListener (&told);

    // The same links in a new vector: alike, not the same value.
    const phazite::MidiLinksValue alike { immer::flex_vector<phazite::MidiLinkEntry> (held.links.begin(), held.links.end()) };
    REQUIRE_FALSE (alike.isSameAs (held));

    {
        const phazite::UndoStep step (song, "Nothing");
        values.setMidiLinks (song, alike);
    }

    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
    CHECK (told.links == 0);
    CHECK (values.midiLinks.get().isSameAs (held));
    values.removeListener (&told);
}

TEST_CASE ("Song values' MIDI links: the MIDILINKS its first link makes goes with its Undo, and every writer of the links is one "
           "exact undo step")
{
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    const auto synth = t.setInstrument (t.firstTrack, phazite::devices::Synth::typeID);
    const auto other = t.addTrack ("Other");
    const auto bus = t.addBus ("Bus", { other });
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);
    const ParameterRef cutoff { synth, "cutoff" }, resonance { synth, "resonance" };

    const auto edit = [&song, &undo] (const juce::String& name, const std::function<void()>& change)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        change();
                    });
    };

    // The first link: the song's file holds a MIDILINKS while the song has a link; its Undo takes it back, and the song is
    // written without one again.
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 21 }, cutoff); });
    REQUIRE (hasLinksPlace (song));
    phazite::undo (song);
    CHECK_FALSE (hasLinksPlace (song));
    CHECK (getLinks (song) == phazite::MidiLinksValue {});
    phazite::redo (song);
    CHECK (phazite::findLinkedParameter (song, { 1, 21 }) == cutoff);

    // Every writer, each one step.
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 22 }, resonance); });
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 22 }, cutoff); }); // both links' place
    CHECK (getLinks (song).links.size() == 1);
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 2, 1 }, t.getVolumeParameter (bus)); });
    edit ("Remove MIDI Link", [&] { phazite::unlinkMidiController (song, { 1, 22 }); });

    // A folder made a plain one and a bus again: its volume's link follows the fader that takes its place (Buses.cpp).
    edit ("Make Plain Folder", [&] { phazite::makePlainFolder (song, bus); });
    CHECK (phazite::findLinkedParameter (song, { 2, 1 }) == t.getVolumeParameter (bus));
    edit ("Make Bus", [&] { phazite::makeBus (song, bus); });
    CHECK (phazite::findLinkedParameter (song, { 2, 1 }) == t.getVolumeParameter (bus));

    // A device removed takes its links along, in its step, and its Undo brings both back.
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 3, 3 }, resonance); });
    edit ("Remove Device", [&] { phazite::removeDevice (song, synth); });
    CHECK_FALSE (phazite::findLinkedParameter (song, { 3, 3 }).has_value());
    CHECK (getLinks (song).links.size() == 1);
    phazite::undo (song);
    CHECK (phazite::findLinkedParameter (song, { 3, 3 }) == resonance);
    phazite::redo (song);
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' MIDI links: the MIDI tap diverts a link's controller by the next turn of the message loop, and follows its "
           "removal, Undo and Redo")
{
    // The session follows the links' area (PlaybackSession.h: midiLinksChanged(), which only marks, and followMidiLinks() at the
    // next turn of the message loop): no listener reads the links as it is told. A controller sent once its link is followed
    // turns the parameter, never the instrument.
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    const auto synth = t.setInstrument (t.firstTrack, phazite::devices::Synth::typeID);
    auto cutoff = phazite::getDeviceHandle (t.context, synth).getParameter ("cutoff");
    auto& tap = getAudioIO().getMidiTap();
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (tap.isDiverted (5, 30));

    {
        const phazite::UndoStep step (song, "Link MIDI Controller");
        phazite::linkMidiController (song, { 5, 30 }, { synth, "cutoff" });
    }

    CHECK (phazite::test::dispatchMessagesUntil ([&tap] { return tap.isDiverted (5, 30); }));
    CHECK_FALSE (tap.isDiverted (5, 31));
    getAudioIO().takeMidi (juce::MidiMessage::controllerEvent (5, 30, 127)); // as a MIDI input's thread hands it over
    CHECK (phazite::test::dispatchMessagesUntil ([&cutoff] { return cutoff.getValue() == doctest::Approx (cutoff.getRange().end); }));

    if (auto* linker = phazite::findMidiLinker (song))
        linker->endBurst();

    const auto followed = [&tap] (bool diverted)
    { return phazite::test::dispatchMessagesUntil ([&tap, diverted] { return tap.isDiverted (5, 30) == diverted; }); };

    {
        const phazite::UndoStep step (song, "Remove MIDI Link");
        phazite::unlinkMidiController (song, { 5, 30 });
    }

    CHECK (followed (false));
    phazite::undo (song);
    CHECK (followed (true));
    phazite::redo (song);
    CHECK (followed (false));
}

TEST_CASE ("Song values' MIDI links: a link made is an unsaved change, which its Undo takes back, and links changed back are saved "
           "again")
{
    // The links are heard from the song's values (Listener::midiLinksChanged()), compared with the saved copy's as one value
    // (WholeState.h: holdSameMidiLinks(): none is none), as the whole song's comparison says.
    phazite::test::TestSong t;
    auto& song = *phazite::findSong (t.context);
    const auto synth = t.setInstrument (t.firstTrack, phazite::devices::Synth::typeID);
    auto& project = t.getProject();
    phazite::test::dispatchPendingMessages();
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    const auto edit = [&song] (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (song, name);
        change();
    };

    // The first link: unsaved, as saved again once undone (before anything asked, as an unsaved
    // change found stays one until a save).
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 21 }, { synth, "cutoff" }); });
    CHECK_FALSE (project.isWholeAsSaved());
    phazite::undo (song);
    CHECK (project.isWholeAsSaved());
    CHECK_FALSE (project.hasUnsavedChanges());
    phazite::redo (song);
    CHECK (project.hasUnsavedChanges());
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // A link taken away and made again as it was, before anything asked: as saved.
    edit ("Remove MIDI Link", [&] { phazite::unlinkMidiController (song, { 1, 21 }); });
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 21 }, { synth, "cutoff" }); });
    CHECK (project.isWholeAsSaved());
    CHECK_FALSE (project.hasUnsavedChanges());

    // Taken away: unsaved (the saved copy holds the link), the song's file holding no MIDILINKS then (an empty list is none);
    // and saved so, a link made again is a change.
    edit ("Remove MIDI Link", [&] { phazite::unlinkMidiController (song, { 1, 21 }); });
    CHECK_FALSE (hasLinksPlace (song));
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
    REQUIRE (project.save().wasOk());
    edit ("Link MIDI Controller", [&] { phazite::linkMidiController (song, { 1, 21 }, { synth, "resonance" }); });
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
}

TEST_CASE ("Song values' MIDI links: a version's links, and a version without them, applied both ways, each one exact undo step")
{
    // The versions give the song another song's links through the links pass (ApplySong.cpp: makeMidiLinksLike(), through
    // MidiLinks.h's makeMidiLinksAs()), its MIDILINKS added or taken out with them, never leaving its links to the keeper. A
    // restore of the song's own line leaves the links as they are, as it always did.
    SynthSong s;
    auto& song = *s.song;
    phazite::test::UndoCheck undo (song);
    const auto withLinks = phazite::makeSong (phazite::test::copyWholeSong (song));
    REQUIRE (withLinks != nullptr);

    {
        const phazite::UndoStep step (*withLinks, "Link MIDI Controllers");
        phazite::linkMidiController (*withLinks, { 1, 21 }, s.cutoff());
        phazite::linkMidiController (*withLinks, { 1, 22 }, s.resonance());
    }

    const auto other = phazite::test::copyWholeSong (*withLinks);
    const auto without = phazite::test::copyWholeSong (song);

    const auto apply = [&] (const juce::String& name, const juce::ValueTree& from, const std::function<void (const juce::ValueTree&)>& make)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        make (from);
                    });
    };

    const auto songLike = [&song] (const juce::ValueTree& from) { phazite::makeSongLike (song, from); };
    const auto ownLike = [&song] (const juce::ValueTree& from) { phazite::makeSongOwnLike (song, from); };

    apply ("The Version's Song", other, songLike);
    REQUIRE (hasLinksPlace (song));
    CHECK (getLinks (song) == getLinks (*withLinks));
    phazite::test::checkSong (song);

    apply ("The Song Before", without, songLike);
    CHECK_FALSE (hasLinksPlace (song));
    CHECK (getLinks (song) == phazite::MidiLinksValue {});
    phazite::test::checkSong (song);

    // The song's own line: its links stay as they are, either way.
    apply ("The Version's Own", other, ownLike);
    CHECK_FALSE (hasLinksPlace (song));
    apply ("The Version's Song Again", other, songLike);
    apply ("The Own Before", without, ownLike);
    CHECK (getLinks (song) == getLinks (*withLinks));

    // A link that stands in for what its version couldn't read keeps the song's link of its controller, in its place; the
    // rest are the version's.
    {
        const phazite::UndoStep step (song, "Link MIDI Controller");
        phazite::linkMidiController (song, { 1, 21 }, s.resonance()); // the song's (1, 21) now turns the resonance alone
    }

    auto standIn = other.createCopy();
    auto standInLinks = standIn.getChildWithName (schema::MIDILINKS);
    REQUIRE (standInLinks.getNumChildren() == 2);
    standInLinks.getChild (0).setProperty (schema::versions::missing, true, nullptr);
    standInLinks.getChild (1).setProperty (schema::midiLinkParameter, "drive", nullptr);
    apply ("A Stand-in Link", standIn, songLike);
    REQUIRE (getLinks (song).links.size() == 2);
    CHECK (getLinks (song).links[0].number == 21);
    CHECK (phazite::findLinkedParameter (song, { 1, 21 }) == s.resonance());
    CHECK (phazite::findLinkedParameter (song, { 1, 22 }) == ParameterRef { s.synth, "drive" });
    phazite::test::checkSong (song);

    // A MIDILINKS that stands in for what its version couldn't read leaves the song's links as they are, whole.
    auto wholeStandIn = without.createCopy();
    wholeStandIn.appendChild (juce::ValueTree (schema::MIDILINKS, { { schema::versions::missing, true } }), nullptr);
    const auto before = getLinks (song);
    apply ("A Stand-in", wholeStandIn, songLike);
    CHECK (getLinks (song).isSameAs (before));
    CHECK (hasLinksPlace (song));

    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' MIDI links: a merge's clash on a link is chosen each way through the links' value")
{
    // A link merges whole, by its controller: both sides linked the base's controller to other parameters, so the merge
    // reports one clash on that link, whose choice the links' chooser makes (MidiLinks.cpp: chooseMidiLinkClash()), in the
    // choice's step.
    SynthSong s;
    auto& base = *s.song;

    {
        const phazite::UndoStep step (base, "Link MIDI Controller");
        phazite::linkMidiController (base, { 1, 1 }, s.cutoff());
        phazite::linkMidiController (base, { 1, 2 }, { s.synth, "drive" });
    }

    const auto ours = phazite::makeSong (phazite::test::copyWholeSong (base));
    const auto theirs = phazite::makeSong (phazite::test::copyWholeSong (base));
    REQUIRE (ours != nullptr);
    REQUIRE (theirs != nullptr);

    {
        const phazite::UndoStep step (*ours, "Link MIDI Controller");
        phazite::linkMidiController (*ours, { 1, 1 }, s.resonance());
    }

    {
        const phazite::UndoStep step (*theirs, "Link MIDI Controller");
        phazite::linkMidiController (*theirs, { 1, 1 }, { s.synth, "glide" });
    }

    const phazite::MergeSides sides { phazite::test::copyWholeSong (base), phazite::test::copyWholeSong (*ours),
                                      phazite::test::copyWholeSong (*theirs) };
    const auto merged = phazite::mergeSongs (sides.base, sides.before, sides.theirs, [&ours] { return ours->makeID(); });
    const phazite::MergeClash* clash = nullptr;

    for (const auto& each : merged.clashes)
        if (! each.path.isEmpty() && each.path[0].startsWith (schema::MIDILINKS.toString() + ":"))
            clash = &each;

    REQUIRE (clash != nullptr);
    CHECK (clash->path.size() == 2);

    {
        const phazite::UndoStep step (*ours, "Merge");
        phazite::makeSongLike (*ours, merged.song);
    }

    phazite::test::UndoCheck undo (*ours);
    const ParameterRef turned[] = { s.cutoff(), s.resonance(), { s.synth, "glide" } };

    // Where the song holds its link of the controller: a choice puts the link chosen in its place.
    const auto findPlace = [&ours]
    {
        const auto links = getLinks (*ours).links;

        for (size_t i = 0; i < links.size(); ++i)
            if (links[i].channel == 1 && links[i].number == 1)
                return (int) i;

        return -1;
    };

    const auto place = findPlace();
    REQUIRE (place >= 0);

    for (const auto value : { 2, 0, 1 })
    {
        INFO ("value " << value);
        undo.check ("Choose",
                    [&]
                    {
                        const phazite::UndoStep step (*ours, "Choose");
                        CHECK (phazite::chooseMidiLinkClash (*ours, *clash, value, sides) == phazite::AreaChoice::chosen);
                    });
        CHECK (phazite::findLinkedParameter (*ours, { 1, 1 }) == turned[value]);
        CHECK (getLinks (*ours).links.size() == 2);
        CHECK (findPlace() == place);
        phazite::test::checkSong (*ours);
    }

    // A clash elsewhere is none of the links'; one of a property of a link, which a merge never makes, refused.
    auto elsewhere = *clash;
    elsewhere.path = { schema::TEMPOMAP.toString() + ":" + schema::TEMPOMAP.toString() };
    CHECK (phazite::chooseMidiLinkClash (*ours, elsewhere, 0, sides) == phazite::AreaChoice::notTheArea);
    auto ofProperty = *clash;
    ofProperty.property = schema::midiLinkChannel;
    CHECK (phazite::chooseMidiLinkClash (*ours, ofProperty, 0, sides) == phazite::AreaChoice::refused);

    // The side chosen without the link: none.
    auto none = *clash;
    none.theirsElement = {};
    undo.check ("Choose None",
                [&]
                {
                    const phazite::UndoStep step (*ours, "Choose None");
                    CHECK (phazite::chooseMidiLinkClash (*ours, none, 2, sides) == phazite::AreaChoice::chosen);
                });
    CHECK_FALSE (phazite::findLinkedParameter (*ours, { 1, 1 }).has_value());
    CHECK (getLinks (*ours).links.size() == 1);
    phazite::test::checkSong (*ours);
    undo.walkBackAndForth();
}

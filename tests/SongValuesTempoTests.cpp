// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's tempo map change hears it from one place (model/base/internal/SongValues.h:
// Listener::tempoMapChanged()), which the tempo map's area tells (roadmap step S5.20). For each edit of the tempo map made
// through the model, its Undo and its Redo, the listener is told, and told nothing of the clips or their notes; another
// area's change tells it nothing; and what listens to the song's root hears each change once: the project's count of
// changes and the versions' count of the song's changes go up by what the listener was told, and by nothing else; and an
// edit of one property or one entry tells it once, for its one change of the area's value.
//   phazite_tests.exe --test-case="Song values' tempo map*"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/VersionHistory.h"

#include <array>
#include <functional>
#include <vector>

namespace
{
using phazite::test::at;
using phazite::test::lasting;

// How many calls the values' listener heard, of each area.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }

    int notes = 0, clips = 0, tempo = 0;
};

// What a listener of the song's root is told: under the tempo map's element (the TEMPOMAP itself added or taken out too), and
// all else.
struct TreeHeard final : juce::ValueTree::Listener
{
    explicit TreeHeard (const juce::ValueTree& rootToHear)
        : root (rootToHear)
    {
        root.addListener (this);
    }

    ~TreeHeard() override { root.removeListener (this); }

    static bool isUnderTempoMap (juce::ValueTree element)
    {
        for (; element.isValid(); element = element.getParent())
            if (element.hasType (phazite::schema::TEMPOMAP))
                return true;

        return false;
    }

    void hear (const juce::ValueTree& element)
    {
        if (isUnderTempoMap (element))
            ++tempoMap;
        else
            ++other;
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { hear (tree); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        hear (child.hasType (phazite::schema::TEMPOMAP) ? child : parent);
    }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        hear (child.hasType (phazite::schema::TEMPOMAP) ? child : parent);
    }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { hear (parent); }

    juce::ValueTree root;
    int tempoMap = 0, other = 0;
};

// A song in a project of its own, with its versions, the values' listener and a listener of its root recording what they
// are told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    // What a change told: the values' listener, and the root's listener, under the tempo map's element and of everything
    // else; and how far the project's count of changes and the versions' count of the song's changes went up meanwhile.
    struct Heard
    {
        Told told;
        int tempoMap = 0, other = 0;
        juce::int64 project = 0, versions = 0;
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        tree.tempoMap = tree.other = 0;
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        change();
        return { told, tree.tempoMap, tree.other, s.getProject().getChangeCount() - projectBefore,
                 versions->countSongChanges() - versionsBefore };
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
    Told told;
    TreeHeard tree { song.getState() };
};
} // namespace

TEST_CASE ("Song values' tempo map: each edit through the model tells the tempo map changed, its Undo and its Redo too, each once, "
           "and nothing of the clips")
{
    Watched w;
    auto& song = w.song;

    // How often the listener is told for an edit that writes one property of the tempo map, or adds or takes out one of its
    // entries, and for its Undo and its Redo: the tempo map's area tells once for each change of its value, and the tree,
    // whose TEMPOMAP is an empty place for the map, tells nothing under it.
    const auto toldForOneWrite = [] (const Watched::Heard& heard)
    {
        CHECK (heard.tempoMap == 0);
        return 1;
    };

    const auto check = [&w, &toldForOneWrite] (const juce::String& name, const std::function<void()>& change, bool isOneWrite)
    {
        INFO (name.toStdString());
        const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
        const auto heard = w.hearEach (name, change);
        REQUIRE (heard.size() == moments.size());

        for (size_t i = 0; i < heard.size(); ++i)
        {
            INFO (moments[i]);
            CHECK (heard[i].told.tempo > 0);
            CHECK (heard[i].told.clips == 0);
            CHECK (heard[i].told.notes == 0);
            CHECK (heard[i].other == 0);

            // Heard once: what listens to the song's root counts what it was told, and its tree's callbacks nothing more.
            CHECK (heard[i].project == heard[i].told.tempo);
            CHECK (heard[i].versions == heard[i].told.tempo);

            // An edit of one property or one entry: told as often as it changed, no more.
            if (isOneWrite)
                CHECK (heard[i].told.tempo == toldForOneWrite (heard[i]));
        }
    };

    constexpr auto oneWrite = true, more = false;
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); }, oneWrite);
    check ("Change Time Signature", [&] { phazite::setTimeSignature (song, { 3, 4 }); }, more);
    check ("Add Tempo Change", [&] { CHECK (phazite::addTempoChange (song, at (8.0), 140.0) == 1); }, oneWrite);
    check ("Move Tempo Change", [&] { CHECK (phazite::moveTempoChange (song, 1, at (12.0), 150.0) == 1); }, more);
    check ("Change Tempo Ramp", [&] { phazite::setTempoCurve (song, 0, phazite::tempoRamp); }, oneWrite);
    check ("Bend the Ramp", [&] { phazite::setTempoCurve (song, 0, 0.4f); }, oneWrite);
    check ("Change Tempo where a beat is", [&] { phazite::setTempoAt (song, at (16.0), 160.0); }, more);
    check (
        "Take Tempo Changes",
        [&]
        {
            phazite::takeTempoChanges (song, { { at (0.0), 96.0, phazite::tempoJump }, { at (4.0), 128.0, phazite::tempoRamp } },
                                       at (24.0));
        },
        more);
    check ("Insert Tempo Map Time", [&] { phazite::insertTempoMapTime (song, at (6.0), lasting (4.0)); }, more);
    check ("Remove Tempo Map Time", [&] { phazite::removeTempoMapTime (song, { at (14.0), at (18.0) }); }, more);
    check (
        "Change Time Signatures",
        [&] { phazite::setTimeSignatureChanges (song, { { phazite::BeatPosition(), { 4, 4 } }, { at (16.0), { 6, 8 } } }); }, more);
    check ("Delete Tempo Change", [&] { phazite::deleteTempoChange (song, 1); }, oneWrite);
    check ("Remove Tempo Changes", [&] { phazite::removeTempoChanges (song); }, more);
    CHECK_FALSE (phazite::hasTempoChanges (song));

    // The tempo it has already: no change, nothing told.
    const auto same = w.hear ([&] { phazite::setTempo (song, phazite::getTempo (song)); });
    CHECK (same.told.tempo == 0);
    CHECK (same.project == 0);
    CHECK (same.versions == 0);
}

TEST_CASE ("Song values' tempo map: another area's change tells nothing of the tempo map, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
            CHECK (heard.told.tempo == 0);
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Mute Clip", [&] { phazite::toggleClipsMuted (song, { clip }); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Add Effect", [&] { w.s.addEffect (track, phazite::devices::Equalizer::typeID); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' tempo map: the keeper takes in a TEMPOMAP that comes into the song with entries, and an entry added outside "
           "the history, and nothing while the history undoes or redoes")
{
    // Its safety net (SongValues.h), which the versions never leave anything to (ApplySong.cpp, VersionsModel.cpp).
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    auto& values = *song.getValues();
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);
    const auto adoptedBefore = values.countTempoMapAdopted();

    // A TEMPOMAP with entries in the place of the song's, in a step: its entries the song's map, its element empty.
    undo.check (
        "Take In a Tempo Map",
        [&]
        {
            const phazite::UndoStep step (song, "Take In a Tempo Map");
            auto state = song.getState();
            state.removeChild (state.getChildWithName (phazite::schema::TEMPOMAP), &song.getUndoManager());
            juce::ValueTree map (phazite::schema::TEMPOMAP);
            map.appendChild (
                juce::ValueTree (phazite::schema::TEMPO, { { phazite::schema::mapBeat, 8.0 }, { phazite::schema::bpm, 140.0 } }), nullptr);
            map.appendChild (
                juce::ValueTree (phazite::schema::TEMPO, { { phazite::schema::mapBeat, 0.0 }, { phazite::schema::bpm, 90.0 } }), nullptr);
            map.appendChild (juce::ValueTree (phazite::schema::TIMESIGNATURE, { { phazite::schema::mapBeat, 0.0 },
                                                                                { phazite::schema::numerator, 3 },
                                                                                { phazite::schema::denominator, 4 } }),
                             nullptr);

            // What Phazite doesn't know of a map goes with its entries, as holdValues() takes it out of the song's own.
            map.setProperty ("unknownToPhazite", 1, nullptr);
            map.appendChild (juce::ValueTree ("UNKNOWNTOPHAZITE"), nullptr);
            state.appendChild (map, &song.getUndoManager());
        });

    const auto tempos = phazite::getTempoChanges (song);
    REQUIRE (tempos.size() == 2);
    CHECK (tempos[0].bpm == doctest::Approx (90.0));
    CHECK (tempos[1].bpm == doctest::Approx (140.0));
    CHECK (phazite::getTimeSignature (song) == phazite::TimeSignature { 3, 4 });
    CHECK (song.getState().getChildWithName (phazite::schema::TEMPOMAP).getNumChildren() == 0);
    CHECK (song.getState().getChildWithName (phazite::schema::TEMPOMAP).getNumProperties() == 0);
    CHECK (values.countTempoMapAdopted() == adoptedBefore + 3);
    phazite::test::checkSong (song);

    // Undone and redone: the area's own action, nothing taken in again.
    phazite::undo (song);
    CHECK (phazite::getTempoChanges (song).size() == 1);
    CHECK (phazite::getTempo (song) == doctest::Approx (120.0));
    phazite::redo (song);
    CHECK (phazite::getTempoChanges (song).size() == 2);
    CHECK (values.countTempoMapAdopted() == adoptedBefore + 3);
    phazite::test::checkSong (song);

    // An entry added to the song's TEMPOMAP outside the history: in its place by its beat, taken out again.
    {
        const phazite::Song::OutsideHistory outside (song);
        song.getState()
            .getChildWithName (phazite::schema::TEMPOMAP)
            .appendChild (juce::ValueTree (phazite::schema::TEMPO, { { phazite::schema::mapBeat, 12.0 }, { phazite::schema::bpm, 150.0 } }),
                          nullptr);
    }

    const auto taken = phazite::getTempoChanges (song);
    REQUIRE (taken.size() == 3);
    CHECK (taken[1].bpm == doctest::Approx (140.0));
    CHECK (taken[2].beat == phazite::test::at (12.0));
    CHECK (taken[2].bpm == doctest::Approx (150.0));
    CHECK (song.getState().getChildWithName (phazite::schema::TEMPOMAP).getNumChildren() == 0);
    CHECK (values.countTempoMapAdopted() == adoptedBefore + 4);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tempo map: a map set equal to the one the song holds makes no undo step, tells nothing and keeps the value held")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    phazite::addTempoChange (song, at (8.0), 140.0);
    auto& values = *song.getValues();
    const auto held = values.tempoMap.get();
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    Told told;
    values.addListener (&told);

    // The same entries in new vectors: alike, not the same value.
    const phazite::TempoMapValue alike { immer::flex_vector<phazite::TempoEntry> (held.tempos.begin(), held.tempos.end()),
                                         immer::flex_vector<phazite::SignatureEntry> (held.signatures.begin(), held.signatures.end()) };
    REQUIRE_FALSE (alike.isSameAs (held));

    {
        const phazite::UndoStep step (song, "Nothing");
        values.setTempoMap (song, alike);
    }

    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
    CHECK (told.tempo == 0);
    CHECK (values.tempoMap.get().isSameAs (held));
    values.removeListener (&told);
}

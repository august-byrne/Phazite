// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's tempo map change hears it from one place (model/base/internal/SongValues.h:
// Listener::tempoMapChanged()), which the song's keeper tells from the tree's notifications while the tree holds the map,
// and the tempo map's area once it holds it as a value (roadmap step S5.20). For each edit of the tempo map made through the
// model, its Undo and its Redo, the listener is told, and told nothing of the clips or their notes; another area's change
// tells it nothing; and what listens to the song's root hears each change once: the project's count of changes and the
// versions' count of the song's changes go up by what the listener was told, and by nothing else; and an edit of one
// property or one entry tells it as often as it changed, no more.
//   phazite_tests.exe --test-case="Song values' tempo map*"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
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
    // entries, and for its Undo and its Redo: while the song's tree holds the map, once for each of the tree's notifications
    // under its element, which the root's listener counts (so that the keeper telling twice for one shows). THE ONE PLACE TO
    // CHANGE once the tempo map's area holds it as a value (roadmap step S5.20, the tempo map's switch), where each tells
    // once: return 1.
    const auto toldForOneWrite = [] (const Watched::Heard& heard) { return heard.tempoMap; };

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

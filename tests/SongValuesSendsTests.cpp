// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's sends change hears it from one place (model/base/internal/SongValues.h: Listener::sendsChanged()), by
// the sends' IDs (roadmap step S5.20; from the sends' switch on, the song's tree holds none: their area tells). For each edit
// of sends made through the model, its Undo and its Redo, the listener is told every send that differs before and after,
// compared as values (getSendsValue(), independent of what tells it), with the track it was on and is on, the return it went
// to and goes to, the parts that differ among those it was told and its value after as getSendValue() reads it, and the
// song's sends whole after; one told that doesn't differ was taken out and came back (a track moved); what it was told,
// change by change, takes the sends before to those after; and nothing of another area. Each edit tells once, its Undo and
// its Redo once each. Another area's change tells it nothing. What listens to the song's root hears each change once: where
// the tree told nothing but the sends', the project's count of changes and the versions' count of the song's changes go up
// by the calls the listener was told. And what plays takes a level at once, without a plan, and what makes a plan makes
// one, as before; what decides which send sends follows them between plans, sends in another order too; what writes
// automation follows what an undo, a redo or a trial put back set, once the move is whole; the unsaved check and MIDI learn
// follow them too. The keeper's cases under UndoCheck (a track's sends through a deletion, a folder, a duplicate, a return
// removed, a lone send refused, a track that comes in with sends), a version applied each way and a line's, and a merge's
// choices.
//   phazite_tests.exe --test-case="Song values' sends*"
#include "EnginePlayback.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/TracksValue.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/midi/MidiLinkModel.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace
{
using Change = phazite::SongValues::SendChange;
using phazite::DeviceID;
using phazite::SendValue;
using phazite::SongSends;
using phazite::TrackID;
using phazite::test::at;
using phazite::test::lasting;

// What a send's changes told, in all: whether it was there as the first told it and is as the last told it, where it was and
// went to then and is and goes to now, the parts of all of them, and its value after the last.
struct Entry
{
    bool wasThere = false, isThere = false;
    TrackID fromBefore {}, fromAfter {}, toBefore {}, toAfter {};
    std::uint8_t parts = 0;
    std::optional<SendValue> now {};
};

// How many calls the values' listener heard, of each area, and what it was told of the sends, each by its ID.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }

    // The tracks', which a track removed, duplicated or moved comes and goes with since the tracks' switch (no TRACK element
    // in the tree).
    void tracksChanged (const std::vector<phazite::TrackChange>&, const std::vector<phazite::TrackID>&, const phazite::SongTracks&) override
    {
        ++tracksTold;
    }
    void curvesChanged (const std::vector<phazite::ParameterRef>&) override { ++curves; }

    void modulationChanged (const std::vector<phazite::SongValues::ModulationChange>&, const std::vector<phazite::ModulatorPlace>&) override
    {
        ++modulation;
    }

    void sendsChanged (const std::vector<Change>& changes, const std::vector<TrackID>& tracks, const SongSends& now) override
    {
        ++calls;
        CHECK_FALSE ((changes.empty() && tracks.empty()));
        CHECK (phazite::holdsItsLists (now));
        handed = now;

        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            CHECK (std::none_of (changes.begin(), changes.begin() + (std::ptrdiff_t) i,
                                 [&change] (const Change& other) { return other.id == change.id; })); // each once in a call
            CHECK ((change.wasThere() || change.isThere()));
            CHECK ((change.now != nullptr) == change.isThere());
            CHECK (change.parts != 0);

            if (change.now != nullptr)
            {
                CHECK (change.now->id == change.id);
                CHECK (change.now->from == change.fromAfter);
                CHECK (change.now->to == change.toAfter);
                CHECK (now.find (change.id) == change.now); // the value after handed whole, the change's in it
            }
            else
            {
                CHECK (now.find (change.id) == nullptr);
            }

            auto [found, first] = told.try_emplace (change.id.getRaw());
            auto& entry = found->second;

            if (first)
            {
                entry.wasThere = change.wasThere();
                entry.fromBefore = change.fromBefore;
                entry.toBefore = change.toBefore;
            }

            entry.isThere = change.isThere();
            entry.fromAfter = change.fromAfter;
            entry.toAfter = change.toAfter;
            entry.parts = (std::uint8_t) (entry.parts | change.parts);
            entry.now = change.now != nullptr ? std::optional (*change.now) : std::nullopt;

            if (applied.has_value())
                applied = change.now != nullptr ? phazite::withSend (*applied, *change.now) : phazite::withoutSend (*applied, change.id);
        }

        reordered.insert (reordered.end(), tracks.begin(), tracks.end());
    }

    int notes = 0, clips = 0, tempo = 0, curves = 0, tracksTold = 0, modulation = 0, calls = 0;
    std::map<std::uint64_t, Entry> told {};
    std::vector<TrackID> reordered {};
    std::optional<SongSends> applied {}; // where given the song's sends before: each change told applied to it, in turn
    std::optional<SongSends> handed {};  // the song's sends whole as the last call handed them
};

// Whether what each change told did to the song's sends before takes them to the sends after: the same sends, each the same
// value, and each track's list holding the same sends (their order isn't told: a send that comes is put last).
bool takesTo (const SongSends& applied, const SongSends& after)
{
    if (! (applied.sends == after.sends) || applied.tracks.size() != after.tracks.size())
        return false;

    for (const auto& [key, list] : after.tracks)
    {
        const auto* held = applied.tracks.find (key);

        if (held == nullptr)
            return false;

        std::vector<DeviceID> a (held->begin(), held->end()), b (list.begin(), list.end());
        std::sort (a.begin(), a.end());
        std::sort (b.begin(), b.end());

        if (a != b)
            return false;
    }

    return true;
}

// What a listener of the song's root is told, as the test sees it, without asking the song: of a SEND (one added or taken
// out, a property of one), and all else.
struct TreeHeard final : juce::ValueTree::Listener
{
    explicit TreeHeard (const juce::ValueTree& rootToHear)
        : root (rootToHear)
    {
        root.addListener (this);
    }

    ~TreeHeard() override { root.removeListener (this); }

    static bool isUnderSend (juce::ValueTree element)
    {
        for (; element.isValid(); element = element.getParent())
            if (element.hasType (phazite::schema::SEND))
                return true;

        return false;
    }

    void hear (bool ofSend) { ++(ofSend ? sends : other); }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { hear (isUnderSend (tree)); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        hear (child.hasType (phazite::schema::SEND) || isUnderSend (parent));
    }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        hear (child.hasType (phazite::schema::SEND) || isUnderSend (parent));
    }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { hear (isUnderSend (parent)); }

    juce::ValueTree root;
    int sends = 0, other = 0;
};

// The parts in which a send differs between two values of the song's sends, worked out here (independent of the keeper):
// none where it is the same, `made` where it comes or goes or is on another track.
std::uint8_t findDifferingParts (const SendValue* was, const SendValue* is)
{
    if (was == nullptr || is == nullptr)
        return was == is ? 0 : Change::made;

    std::uint8_t parts = 0;
    const auto add = [&parts] (std::uint8_t part) { parts = (std::uint8_t) (parts | part); };

    if (was->from != is->from)
        add (Change::made);

    if (was->to != is->to || was->preFader != is->preFader)
        add (Change::route);

    if (! juce::exactlyEqual (was->level, is->level))
        add (Change::level);

    return parts;
}

// Each ID of either value.
std::vector<std::uint64_t> getAllIDs (const SongSends& before, const SongSends& after)
{
    std::vector<std::uint64_t> ids;

    for (const auto* sends : { &before, &after })
        for (const auto& [id, value] : sends->sends)
            if (std::find (ids.begin(), ids.end(), id) == ids.end())
                ids.push_back (id);

    return ids;
}

// The tracks that hold those they held before in another order: each track's list in both values, the IDs in both, compared.
std::vector<TrackID> findReordered (const SongSends& before, const SongSends& after)
{
    std::vector<TrackID> tracks;

    for (const auto& [key, was] : before.tracks)
    {
        const auto* is = after.tracks.find (key);

        if (is == nullptr)
            continue;

        std::vector<DeviceID> keptBefore, keptAfter;

        for (const auto& each : was)
            if (std::find (is->begin(), is->end(), each) != is->end())
                keptBefore.push_back (each);

        for (const auto& each : *is)
            if (std::find (was.begin(), was.end(), each) != was.end())
                keptAfter.push_back (each);

        if (keptBefore != keptAfter)
            tracks.push_back (TrackID::fromRaw (key));
    }

    return tracks;
}

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

    // What a change told: the values' listener; the root's listener, of the sends and of everything else; how far the
    // project's count of changes and the versions' count of the song's changes went up meanwhile; and the song's sends before
    // it and after.
    struct Heard
    {
        Told told {};
        int sends = 0, other = 0;
        juce::int64 project = 0, versions = 0;
        SongSends before {}, after {};
        std::map<std::uint64_t, std::optional<SendValue>> read {}; // each send told, as getSendValue() reads it after
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        tree.sends = tree.other = 0;
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        auto before = phazite::getSendsValue (song);
        told.applied = before;
        change();
        auto after = phazite::getSendsValue (song);
        Heard heard;
        heard.told = told;
        heard.sends = tree.sends;
        heard.other = tree.other;
        heard.project = s.getProject().getChangeCount() - projectBefore;
        heard.versions = versions->countSongChanges() - versionsBefore;
        heard.before = std::move (before);
        heard.after = std::move (after);

        for (const auto& entry : heard.told.told)
            heard.read[entry.first] = phazite::getSendValue (song, DeviceID::fromRaw (entry.first));

        return heard;
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

// What a change and its Undo and Redo told, each against the song's sends before and after: every send that differs told,
// as it is after (there or not, on which track, to which return, its value as getSendValue() reads it), with at least the
// parts that differ; one told that doesn't differ as it is after too (one taken out and back, as a track JUCE moves); every
// track whose sends came in another order told; nothing of another area unless othersToo; and, where the tree told nothing
// else, the project's and the versions' counts up by the calls the listener was told.
void checkHeard (const juce::String& name, const std::vector<Watched::Heard>& heard, bool othersToo = false)
{
    INFO (name.toStdString());
    const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
    REQUIRE (heard.size() <= moments.size());

    for (size_t i = 0; i < heard.size(); ++i)
    {
        INFO (moments[i]);
        const auto& each = heard[i];
        const auto ids = getAllIDs (each.before, each.after);
        auto differs = false;

        for (const auto id : ids)
        {
            INFO ("send " << id);
            const auto* was = each.before.find (DeviceID::fromRaw (id));
            const auto* is = each.after.find (DeviceID::fromRaw (id));
            const auto parts = findDifferingParts (was, is);
            const auto found = each.told.told.find (id);

            if (parts == 0 && found == each.told.told.end())
                continue;

            differs = differs || parts != 0;
            REQUIRE (found != each.told.told.end());
            const auto& entry = found->second;
            CHECK ((entry.parts & parts) == parts);
            CHECK (entry.isThere == (is != nullptr));

            // One told that doesn't differ was taken out and came back, as it was.
            if (parts == 0)
            {
                CHECK (entry.wasThere);
                CHECK ((entry.parts & Change::made) != 0);
            }

            if (was != nullptr && parts != 0)
            {
                CHECK (entry.fromBefore == was->from);
                CHECK (entry.toBefore == was->to);
            }

            if (is != nullptr)
            {
                CHECK (entry.fromAfter == is->from);
                CHECK (entry.toAfter == is->to);
                REQUIRE (entry.now.has_value());
                CHECK (*entry.now == *is);
                const auto& read = each.read.at (id);
                REQUIRE (read.has_value());
                CHECK (*entry.now == *read);
            }
        }

        // What was told, change by change, takes the sends before to those after; and the last call handed them whole.
        REQUIRE (each.told.applied.has_value());
        CHECK (takesTo (*each.told.applied, each.after));

        if (each.told.handed.has_value())
            CHECK (*each.told.handed == each.after);

        // Nothing told of a send the song had neither before nor after.
        for (const auto& [id, entry] : each.told.told)
            CHECK (std::find (ids.begin(), ids.end(), id) != ids.end());

        for (const auto track : findReordered (each.before, each.after))
        {
            differs = true;
            CHECK_MESSAGE (std::find (each.told.reordered.begin(), each.told.reordered.end(), track) != each.told.reordered.end(),
                           "track " << (juce::int64) track.getRaw() << " not told reordered");
        }

        if (differs)
            CHECK (each.told.calls > 0);

        if (! othersToo)
        {
            CHECK (each.told.clips == 0);
            CHECK (each.told.notes == 0);
            CHECK (each.told.tempo == 0);
            CHECK (each.told.curves == 0);
            CHECK (each.told.modulation == 0);
        }

        // Heard once: what listens to the song's root counts what it was told, and its tree's callbacks nothing more.
        if (each.other == 0 && each.told.tracksTold == 0)
        {
            CHECK (each.project == each.told.calls);
            CHECK (each.versions == each.told.calls);
        }
    }
}

// Whether any change told of a send is of these parts.
bool toldAny (const Watched::Heard& heard, DeviceID id, std::uint8_t parts)
{
    const auto found = heard.told.told.find (id.getRaw());
    return found != heard.told.told.end() && (found->second.parts & parts) != 0;
}

phazite::ParameterRef levelOf (DeviceID send)
{
    return { send, phazite::schema::sendLevelParameter };
}

void setLevel (phazite::Song& song, DeviceID send, float decibels)
{
    const phazite::UndoStep step (song, "Change Send Level");
    phazite::setSendLevel (song, send, decibels);
}
} // namespace

TEST_CASE ("Song values' sends: each edit of a send through the model tells what changed of it, its Undo and its Redo too, "
           "and nothing of another area")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    const auto delay = phazite::addReturnTrack (song, "Delay", nullptr);
    REQUIRE (reverb.isValid());
    REQUIRE (delay.isValid());
    DeviceID send, other;

    const auto added = w.hearEach ("Add Send", [&] { send = phazite::addSend (song, track, reverb, false); });
    checkHeard ("Add Send", added);
    REQUIRE (send.isValid());
    CHECK (toldAny (added[0], send, Change::made));
    CHECK_FALSE (added[0].told.told.at (send.getRaw()).wasThere);
    CHECK (added[0].told.told.at (send.getRaw()).fromAfter == track);
    CHECK (added[0].told.told.at (send.getRaw()).toAfter == reverb);
    CHECK (added[1].told.told.at (send.getRaw()).wasThere);
    CHECK_FALSE (added[1].told.told.at (send.getRaw()).isThere);

    const auto level = w.hearEach ("Change Send Level", [&] { phazite::setSendLevel (song, send, -12.0f); });
    checkHeard ("Change Send Level", level);
    CHECK (toldAny (level[0], send, Change::level));
    CHECK_FALSE (toldAny (level[0], send, (std::uint8_t) (Change::made | Change::route)));

    // To 0 dB, which its element holds as no level at all, and back.
    checkHeard ("Send Level to 0 dB", w.hearEach ("Send Level to 0 dB", [&] { phazite::setSendLevel (song, send, 0.0f); }));
    checkHeard ("Send Level Down", w.hearEach ("Send Level Down", [&] { phazite::setSendLevel (song, send, -3.0f); }));

    const auto preFader = w.hearEach ("Change Send", [&] { phazite::setPreFader (song, send, true); });
    checkHeard ("Change Send", preFader);
    CHECK (toldAny (preFader[0], send, Change::route));
    CHECK_FALSE (toldAny (preFader[0], send, Change::level));

    checkHeard ("Add Another Send", w.hearEach ("Add Another Send", [&] { other = phazite::addSend (song, track, delay, true); }));
    REQUIRE (other.isValid());
    checkHeard ("Remove Send", w.hearEach ("Remove Send", [&] { phazite::removeSend (song, send); }));
    CHECK_FALSE (phazite::getSendValue (song, send).has_value());

    // A level set as it is: no change, nothing told.
    const auto same = w.hear ([&] { setLevel (song, other, phazite::getSendLevel (song, other)); });
    CHECK (same.told.calls == 0);
    CHECK (same.project == 0);
    const auto samePlace = w.hear ([&] { phazite::setPreFader (song, other, true); });
    CHECK (samePlace.told.calls == 0);

    // Each of them told once: the step, its Undo and its Redo, one call each, as the area tells each change of its value
    // (SendsArea.h: SendsTeller); a step that set the level twice told twice, its Undo and Redo once.
    for (const auto& [name, heard] :
         { std::pair ("Add Send", added), std::pair ("Change Send Level", level), std::pair ("Change Send", preFader) })
    {
        INFO (name);

        for (const auto& each : heard)
            CHECK (each.told.calls == 1);
    }

    const auto twice = w.hearEach ("Change Send Level Twice",
                                   [&]
                                   {
                                       phazite::setSendLevel (song, other, -20.0f);
                                       phazite::setSendLevel (song, other, -21.0f);
                                   });
    checkHeard ("Change Send Level Twice", twice);
    CHECK (twice[0].told.calls == 2);
    CHECK (twice[1].told.calls == 1);
    CHECK (twice[2].told.calls == 1);

    // Removed, and what removing it took with it in the same step (nothing named it): one call, its Undo and its Redo one.
    for (const auto& each : w.hearEach ("Remove Another Send", [&] { phazite::removeSend (song, other); }))
        CHECK (each.told.calls == 1);
}

TEST_CASE ("Song values' sends: what takes sends along tells them: a track with its sends removed, duplicated, moved into a "
           "folder, a return removed with the sends to it, a version applied")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto other = w.s.addTrack ("Other");
    const auto reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    const auto delay = phazite::addReturnTrack (song, "Delay", nullptr);
    DeviceID toReverb, toDelay, fromOther;

    {
        const phazite::UndoStep step (song, "Add Sends");
        toReverb = phazite::addSend (song, track, reverb, false);
        toDelay = phazite::addSend (song, track, delay, true);
        fromOther = phazite::addSend (song, other, reverb, false);
        phazite::setSendLevel (song, toDelay, -9.0f);
    }

    REQUIRE (toReverb.isValid());
    REQUIRE (toDelay.isValid());
    REQUIRE (fromOther.isValid());

    // The track with its two: taken out, brought back.
    const auto trackRemoved = w.hearEach ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    checkHeard ("Delete Track", trackRemoved, true);
    CHECK (trackRemoved[0].told.told.size() == 2);
    CHECK (trackRemoved[1].told.told.size() == 2);
    phazite::undo (song); // the track back

    // Duplicated: the copy's, under their new IDs, on the copy.
    std::vector<TrackID> copies;
    const auto duplicated = w.hearEach ("Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); });
    checkHeard ("Duplicate Track", duplicated, true);
    REQUIRE (copies.size() == 1);
    CHECK (duplicated[0].told.told.size() == 2);

    for (const auto& [id, entry] : duplicated[0].told.told)
    {
        CHECK (entry.fromAfter == copies.front());
        CHECK_FALSE (entry.wasThere);
    }

    // Into a folder: the same sends, on the same track, told nothing since the tracks' switch (roadmap step S5.20): a track
    // moved changes its place in the tracks' lists alone (named: JUCE took its element out and put it in again before, and
    // the sends' keeper told its sends taken out and back).
    TrackID folder;

    {
        const phazite::UndoStep step (song, "Add Folder");
        folder = phazite::insertFolder (song, copies.front(), nullptr);
    }

    const auto intoFolder = w.hearEach ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    checkHeard ("Into the Folder", intoFolder, true);
    CHECK (phazite::getParentFolder (song, track) == folder);
    CHECK (intoFolder[0].told.told.count (toReverb.getRaw()) == 0);

    // A return removed: the sends to it go with it, in that step, and come back with it.
    const auto returnRemoved = w.hearEach ("Delete Return", [&] { phazite::deleteTracks (song, { reverb }, nullptr); });
    checkHeard ("Delete Return", returnRemoved, true);
    CHECK_FALSE (returnRemoved[0].told.told.at (toReverb.getRaw()).isThere);
    CHECK_FALSE (returnRemoved[0].told.told.at (fromOther.getRaw()).isThere);
    CHECK (returnRemoved[0].told.told.count (toDelay.getRaw()) == 0);
    CHECK (returnRemoved[1].told.told.at (toReverb.getRaw()).isThere);

    // A version with other sends applied, each way, in a step and outside the history.
    const auto before = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Other Sends");
        phazite::setSendLevel (song, toReverb, -20.0f);
        phazite::setPreFader (song, toDelay, false);
        phazite::removeSend (song, fromOther);
        phazite::addSend (song, other, delay, false);
    }

    const auto after = phazite::test::copyWholeSong (song);
    const auto apply = [&song] (const juce::ValueTree& version)
    {
        phazite::makeSongLike (song, version);
        phazite::removeBrokenReferences (song);
    };

    checkHeard ("Bring Back", w.hearEach ("Bring Back", [&] { apply (before); }), true);
    checkHeard ("Bring Back Again", w.hearEach ("Bring Back Again", [&] { apply (after); }), true);
    checkHeard ("Another Branch", { w.hear ([&] { phazite::makeSongTheSame (song, before); }) }, true);
    checkHeard ("Back on the Branch", { w.hear ([&] { phazite::makeSongTheSame (song, after); }) }, true);
}

TEST_CASE ("Song values' sends: another area's change tells nothing of the sends, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    DeviceID send;

    {
        const phazite::UndoStep step (song, "Add Send");
        send = phazite::addSend (song, track, reverb, false);
    }

    REQUIRE (send.isValid());
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());
    const auto eq = w.s.addEffect (track, phazite::devices::Equalizer::typeID);
    const auto lfo = w.s.addModulator (track, phazite::ModulatorType::lfo);

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
            CHECK (heard.told.calls == 0);
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Add Automation Point", [&] { phazite::addPoint (song, levelOf (send), { at (0.0), 0.5f }); }); // the send's lane
    check ("Modulate Send", [&] { phazite::modulate (w.s.context, lfo, levelOf (send), 0.5f); });
    check ("Change Gain", [&] { phazite::setValueSetByHand (song, { eq, "output" }, -3.0f); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Change Volume", [&] { phazite::setChannelVolume (song, track, -6.0f); });
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Add Effect", [&] { w.s.addEffect (track, phazite::devices::Equalizer::typeID); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' sends: a send's level gives the engine no plan, and a send added, routed or removed one, as before")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    TrackID reverb;

    {
        const phazite::UndoStep step (song, "Add Return");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    }

    REQUIRE (s.getProject().save().wasOk());
    phazite::test::PlanCounter plans (s.context);
    DeviceID send;

    struct Edit
    {
        const char* what = "";
        std::function<void()> make {};
        int plans = 0;
    };

    // In the order they are made, each on the song as the ones before left it.
    const std::vector<Edit> edits {
        { "a send added",
          [&]
          {
              const phazite::UndoStep step (song, "Add Send");
              send = phazite::addSend (song, track, reverb, false);
          },
          1 },
        { "its level set", [&] { setLevel (song, send, -12.0f); }, 0 },
        { "its Undo", [&] { phazite::undo (song); }, 0 },
        { "its Redo", [&] { phazite::redo (song); }, 0 },
        { "its level at 0 dB", [&] { setLevel (song, send, 0.0f); }, 0 },
        { "switched before the fader", [&] { phazite::setPreFader (song, send, true); }, 1 },
        { "its Undo", [&] { phazite::undo (song); }, 1 },
        { "the send removed", [&] { phazite::removeSend (song, send); }, 1 },
        { "its Undo", [&] { phazite::undo (song); }, 1 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
    }
}

TEST_CASE ("Song values' sends: a send's level set while the song plays is heard in the next block, from the value the song's "
           "values tell, and its Undo too")
{
    phazite::test::TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    auto& state = *phazite::findSong (song.context);
    TrackID reverb;

    {
        const phazite::UndoStep step (state, "Add Return");
        reverb = phazite::addReturnTrack (state, "Reverb", nullptr);
    }

    song.addSend (track, reverb, -6.0f);
    const auto send = phazite::getSends (song.context, track).front().id;
    phazite::test::EnginePlayback playback;
    auto& player = phazite::getSession (song.context)->getPlayer();
    song.run ("transport.playStop");
    playback.play (0.1);
    CHECK (player.getPlayedValue (levelOf (send), false).value_or (-1.0f)
           == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)).epsilon (0.001));

    setLevel (state, send, -20.0f);
    playback.play (0.05);
    CHECK (player.getPlayedValue (levelOf (send), false).value_or (-1.0f)
           == doctest::Approx (phazite::decibelsToFaderPosition (-20.0f)).epsilon (0.001));

    phazite::undo (state);
    playback.play (0.05);
    CHECK (player.getPlayedValue (levelOf (send), false).value_or (-1.0f)
           == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)).epsilon (0.001));
    song.run ("transport.playStop");
}

TEST_CASE ("Song values' sends: which sends send while a track is muted follows the sends between plans, from what the player "
           "holds of them: a bus's send switched before its fader, then removed, before the plans they asked for")
{
    // A bus muted feeds what listens to it, a send of its before its fader among them, so the tracks in it play on into it
    // (SongPlayer.cpp: updateAudibility()). A mute or a solo decides that at once, in the tree's callback, from the sends the
    // player holds (SongPlayer::heldSends), which the song's values keep up to date between plans. A bus's send is made by no
    // edit (a send is a track's), so the song is opened from a file that holds one, as the loader keeps it.
    phazite::test::TestSong s;
    const auto inBus = s.firstTrack;
    const auto bus = s.addBus ("Bus", { inBus });
    const auto bystander = s.addTrack ("Bystander");
    auto* song = phazite::findSong (s.context);
    TrackID reverb;

    {
        const phazite::UndoStep step (*song, "Add Return");
        reverb = phazite::addReturnTrack (*song, "Reverb", nullptr);
    }

    auto file = phazite::test::copyWholeSong (*song);
    const auto sendID = song->makeID();
    file.setProperty (phazite::schema::nextID, (juce::int64) song->makeID(), nullptr); // past the send's

    const std::function<juce::ValueTree (const juce::ValueTree&)> findBus = [&findBus, bus] (const juce::ValueTree& parent)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (phazite::schema::TRACK) && phazite::readID (child[phazite::schema::trackID]) == bus.getRaw())
                return child;

            if (const auto found = findBus (child); found.isValid())
                return found;
        }

        return juce::ValueTree();
    };

    auto busInFile = findBus (file.getChildWithName (phazite::schema::TRACKS));
    REQUIRE (busInFile.isValid());
    busInFile.appendChild (phazite::writeSendElement ({ .id = DeviceID::fromRaw (sendID), .from = bus, .to = reverb }), nullptr);
    s.openSong (file);
    song = phazite::findSong (s.context);
    const auto send = DeviceID::fromRaw (sendID);
    REQUIRE (phazite::getSendTrack (*song, send) == bus);
    phazite::test::dispatchPendingMessages();

    auto& player = phazite::getSession (s.context)->getPlayer();
    const auto isHeard = [&player] (TrackID track)
    {
        auto* strip = player.findStrip ((std::int64_t) track.getRaw());
        REQUIRE (strip != nullptr);
        return strip->getChannel().isAudible();
    };

    // A muted bus whose send is after its fader: nothing listens to it, so its track is silent.
    phazite::setTrackMuted (*song, bus, true);
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (isHeard (inBus));

    // Its send switched before the fader, and another track muted before the plan that asked for: its track plays on into it.
    phazite::setPreFader (*song, send, true);
    phazite::setTrackMuted (*song, bystander, true);
    CHECK (isHeard (inBus));
    phazite::test::dispatchPendingMessages(); // the plan: the same
    CHECK (isHeard (inBus));

    // Removed, and another track unmuted before the plan: silent again; its Undo, the same way, plays it again.
    phazite::removeSend (*song, send);
    phazite::setTrackMuted (*song, bystander, false);
    CHECK_FALSE (isHeard (inBus));
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (isHeard (inBus));
    phazite::undo (*song);
    phazite::setTrackMuted (*song, bystander, true);
    CHECK (isHeard (inBus));
    phazite::test::dispatchPendingMessages();
    CHECK (isHeard (inBus));
}

TEST_CASE ("Song values' sends: what writes automation follows a fader, a send's level, a macro and a device's knob that an "
           "Undo or a Redo sets while it writes, once the move is whole; and a drag rolled back with Escape as before")
{
    // Known bug 174's net: the value an undo or a redo sets by hand is what the pass writes from then. The writer follows it
    // once the history's move is whole (PlaybackSession.h: historyMoved()), never while the move's listeners are told.
    phazite::test::TestSong s;
    s.setTempo (120.0);
    const auto track = s.firstTrack;
    s.addMidiClip (track, 0.0, 32.0);
    auto& song = *phazite::findSong (s.context);
    TrackID reverb;

    {
        const phazite::UndoStep step (song, "Add Return");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    }

    s.addSend (track, reverb, -6.0f);
    const auto send = phazite::getSends (s.context, track).front().id;
    const auto macro = s.addModulator (track, phazite::ModulatorType::macro);
    const auto eq = s.addEffect (track, phazite::devices::Equalizer::typeID);
    phazite::setAutomationMode (s.context, track, phazite::AutomationMode::latch);

    // Each in its control's units (ParameterHandle: getRange()), set to three tenths of the way along them; whether the writer
    // writes it (a macro's turns are written by none: Automation.cpp, getParameterTrack(), as before).
    struct Parameter
    {
        const char* what = "";
        phazite::ParameterRef ref {};
        bool written = true;
    };

    const std::vector<Parameter> parameters {
        { "the fader", s.getVolumeParameter (track), true },
        { "the send's level", levelOf (send), true },
        { "the macro", { DeviceID::fromRaw (macro.getRaw()), juce::String (macro.getRaw()) }, false },
        { "the device's knob", { eq, "output" }, true },
    };

    for (const auto& parameter : parameters)
    {
        INFO (std::string (parameter.what));
        auto handle = phazite::getParameter (s.context, parameter.ref);
        REQUIRE (handle.exists());
        const auto range = handle.getRange();
        const auto start = handle.getBaseValue();
        const auto set = range.convertFrom0to1 (0.3f);
        REQUIRE (std::abs (set - start) > 0.01f * (range.end - range.start));

        // Whether the lane holds a point at a value, at or after a beat.
        const auto writes = [&s, &parameter, &range] (float value, double fromBeat)
        {
            const auto position = phazite::toControlPosition (range, value);
            const auto points = phazite::getAutomationPoints (s.context, parameter.ref);
            return std::any_of (points.begin(), points.end(), [=] (const phazite::AutomationPoint& point)
                                { return point.beat.inBeats() >= fromBeat - 0.05 && std::abs (point.position - position) < 0.002f; });
        };

        phazite::test::EnginePlayback playback;
        s.run ("transport.playStop");
        playback.play (0.25);
        handle.setValue (set); // a step of its own, which latch punches in at
        playback.play (0.5);
        REQUIRE (handle.getBaseValue() == doctest::Approx (set).epsilon (0.001));

        // Undo: the value set by hand back, which the pass writes from where the undo was.
        const auto undoneAt = phazite::getPlayheadBeat (s.context).inBeats();
        phazite::undo (song);
        playback.play (0.5);
        CHECK (handle.getBaseValue() == doctest::Approx (start).epsilon (0.001));

        // Redo: the value set again, written from there.
        const auto redoneAt = phazite::getPlayheadBeat (s.context).inBeats();
        phazite::redo (song);
        playback.play (0.5);
        CHECK (handle.getBaseValue() == doctest::Approx (set).epsilon (0.001));

        // A drag rolled back: none of its value written, and what plays is what the pass writes.
        const auto dragged = range.convertFrom0to1 (0.8f);
        handle.beginGesture();
        handle.setValue (dragged);
        playback.play (0.25);
        handle.cancelGesture();
        playback.play (0.25);
        CHECK (handle.getBaseValue() == doctest::Approx (set).epsilon (0.001));
        s.run ("transport.playStop");
        phazite::test::dispatchPendingMessages();

        if (! parameter.written)
        {
            CHECK (phazite::getAutomationPoints (s.context, parameter.ref).empty());
            continue;
        }

        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getAutomationPoints (s.context, parameter.ref).empty(); }));
        CHECK (writes (start, undoneAt));
        CHECK (writes (set, redoneAt));
        CHECK_FALSE (writes (dragged, 0.0));
        const auto points = phazite::getAutomationPoints (s.context, parameter.ref);
        CHECK (points.back().position == doctest::Approx (phazite::toControlPosition (range, set)).epsilon (0.002));
    }
}

TEST_CASE ("Song values' sends: the unsaved check compares the sends it was told of with the saved copy, as the whole "
           "comparison does: changed and changed back is saved again, past mostToCompare of them too")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    TrackID reverb, delay;
    DeviceID send;

    {
        const phazite::UndoStep step (song, "Add Sends");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        delay = phazite::addReturnTrack (song, "Delay", nullptr);
        send = phazite::addSend (song, track, reverb, false);
    }

    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // What the quick check says, against the whole comparison, after the edits.
    const auto check = [&project] (const juce::String& name, bool unsaved, const std::function<void()>& edits)
    {
        INFO (name.toStdString());
        edits();
        CHECK (project.isWholeAsSaved() == ! unsaved);
        CHECK (project.hasUnsavedChanges() == unsaved);

        if (unsaved)
            REQUIRE (project.save().wasOk());
    };

    check ("a level turned and turned back", false,
           [&]
           {
               setLevel (song, send, -12.0f);
               setLevel (song, send, -6.0f);
           });
    check ("switched before the fader and its Undo", false,
           [&]
           {
               phazite::setPreFader (song, send, true);
               phazite::undo (song);
           });
    check ("a send added and its adding undone", false,
           [&]
           {
               phazite::addSendTo (song, track, delay);
               phazite::undo (song);
           });
    check ("a send removed and its removal undone", false,
           [&]
           {
               phazite::removeSend (song, send);
               phazite::undo (song);
           });
    check ("a level turned", true, [&] { setLevel (song, send, -3.0f); });
    check ("switched before the fader", true, [&] { phazite::setPreFader (song, send, true); });
    check ("a send added", true, [&] { phazite::addSendTo (song, track, delay); });

    // More sends than are compared one by one: the tracks compared whole.
    std::vector<DeviceID> many;

    {
        const phazite::UndoStep step (song, "Add Sends");

        for (size_t i = 0; i < phazite::Project::mostToCompare + 6; ++i)
            many.push_back (phazite::addSend (song, phazite::insertTrack (song, track, nullptr), reverb, false));
    }

    REQUIRE (project.save().wasOk());
    const auto setLevels = [&song, &many] (float decibels)
    {
        const phazite::UndoStep step (song, "Change Send Levels");

        for (const auto id : many)
            phazite::setSendLevel (song, id, decibels);
    };

    check ("many changed and changed back", false,
           [&]
           {
               setLevels (-12.0f);
               setLevels (-6.0f);
           });
    check ("many changed", true, [&] { setLevels (-12.0f); });
}

TEST_CASE ("Song values' sends: the unsaved check compares the order of a track's sends where one was taken out and put back "
           "at another place")
{
    // A send taken out of its track and put back last, in two steps before a comparison: the same value, but after the other,
    // which the saved copy has after it. No edit puts one back with its ID (an Undo puts it where it was): the value is taken
    // out and set again through the sends' setters, as a version's pass sets one.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    DeviceID first, second;

    {
        const phazite::UndoStep step (song, "Add Sends");
        const auto reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        const auto delay = phazite::addReturnTrack (song, "Delay", nullptr);
        first = phazite::addSend (song, track, reverb, false);
        second = phazite::addSend (song, track, delay, false);
    }

    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());
    REQUIRE (phazite::getSends (song, track) == std::vector { first, second });

    const auto putBackLast = [&song, first]
    {
        const auto value = phazite::getSendValue (song, first);
        REQUIRE (value.has_value());

        {
            const phazite::UndoStep step (song, "Remove Send");
            REQUIRE (song.getValues()->eraseSend (song, first));
        }

        const phazite::UndoStep step (song, "Add Send");
        REQUIRE (song.getValues()->setSend (song, *value));
    };

    // Both undone: as saved.
    putBackLast();
    REQUIRE (phazite::getSends (song, track) == std::vector { second, first });
    phazite::undo (song);
    phazite::undo (song);
    REQUIRE (phazite::getSends (song, track) == std::vector { first, second });
    CHECK (project.isWholeAsSaved());
    CHECK_FALSE (project.hasUnsavedChanges());

    // Put back last: changed.
    putBackLast();
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
}

TEST_CASE ("Song values' sends: learning a send's level ends as the send goes, in that call, by an undo too, and as its track "
           "goes, and stays while another send goes")
{
    // A send's level ends as the song's values tell the send taken out (SongValues.h: sendsChanged(); MidiLinks.h:
    // MidiLinker), at once.
    phazite::test::TestSong s;
    auto& context = s.context;
    auto& song = *phazite::findSong (context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    DeviceID send, another, onOther;

    {
        const phazite::UndoStep step (song, "Add Sends");
        const auto reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        const auto delay = phazite::addReturnTrack (song, "Delay", nullptr);
        send = phazite::addSend (song, track, reverb, false);
        another = phazite::addSend (song, track, delay, false);
        onOther = phazite::addSend (song, other, reverb, false);
    }

    // Another send goes: still learning.
    const auto level = phazite::getSendLevel (context, send);
    REQUIRE (level.exists());
    phazite::startMidiLearn (level);
    REQUIRE (phazite::isLearningMidi (level));
    phazite::removeSend (song, another);
    CHECK (phazite::isLearningMidi (level));

    // The send goes: learning ends at once; its Undo brings the send back, not the learning.
    phazite::removeSend (song, send);
    CHECK_FALSE (phazite::isLearningMidi (context));
    phazite::undo (song);
    REQUIRE (phazite::getSendValue (song, send).has_value());
    CHECK_FALSE (phazite::isLearningMidi (context));

    // An undo that takes the send away again ends it too.
    const auto again = phazite::getSendLevel (context, send);
    phazite::startMidiLearn (again);
    REQUIRE (phazite::isLearningMidi (again));
    phazite::redo (song);
    CHECK_FALSE (phazite::getSendValue (song, send).has_value());
    CHECK_FALSE (phazite::isLearningMidi (context));

    // A send's level, as its track goes.
    const auto onOtherLevel = phazite::getSendLevel (context, onOther);
    phazite::startMidiLearn (onOtherLevel);
    REQUIRE (phazite::isLearningMidi (onOtherLevel));

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { other }, nullptr);
    }

    CHECK_FALSE (phazite::isLearningMidi (context));
}

TEST_CASE ("Song values' sends: a version that holds a track's sends in another order tells the track reordered, and what "
           "decides which send sends still follows them before the plan")
{
    // No edit puts a track's sends in another order (a send comes last, and an Undo puts one back where it was): a version
    // applied does (ApplySong.cpp: makeSendsLike(), which puts a track's in the other's order). A bus's send is made by no
    // edit (a send is a track's), so the song is opened from a file that holds two, the first before its fader, as the loader
    // keeps them; muted, the bus still feeds that send, so the track in it plays on into it (SongPlayer.cpp:
    // updateAudibility()).
    phazite::test::TestSong s;
    const auto inBus = s.firstTrack;
    const auto bus = s.addBus ("Bus", { inBus });
    const auto bystander = s.addTrack ("Bystander");
    auto* song = phazite::findSong (s.context);
    TrackID reverb, delay;

    {
        const phazite::UndoStep step (*song, "Add Returns");
        reverb = phazite::addReturnTrack (*song, "Reverb", nullptr);
        delay = phazite::addReturnTrack (*song, "Delay", nullptr);
    }

    auto file = phazite::test::copyWholeSong (*song);
    const auto first = DeviceID::fromRaw (song->makeID());
    const auto second = DeviceID::fromRaw (song->makeID());
    file.setProperty (phazite::schema::nextID, (juce::int64) song->makeID(), nullptr); // past the sends'

    for (const auto& child : file.getChildWithName (phazite::schema::TRACKS))
    {
        if (phazite::readID (child[phazite::schema::trackID]) != bus.getRaw())
            continue;

        auto busInFile = child;
        busInFile.appendChild (phazite::writeSendElement ({ .id = first, .from = bus, .to = reverb, .preFader = true }), nullptr);
        busInFile.appendChild (phazite::writeSendElement ({ .id = second, .from = bus, .to = delay }), nullptr);
    }

    s.openSong (file);
    song = phazite::findSong (s.context);
    REQUIRE (phazite::getSends (*song, bus) == std::vector { first, second });
    Told told;
    song->getValues()->addListener (&told);
    phazite::test::dispatchPendingMessages();

    auto& player = phazite::getSession (s.context)->getPlayer();
    const auto isHeard = [&player] (TrackID track)
    {
        auto* strip = player.findStrip ((std::int64_t) track.getRaw());
        REQUIRE (strip != nullptr);
        return strip->getChannel().isAudible();
    };

    phazite::setTrackMuted (*song, bus, true);
    phazite::test::dispatchPendingMessages();
    REQUIRE (isHeard (inBus));
    const auto before = phazite::test::copyWholeSong (*song);

    // The bus's sends put in another order, through the sends' setter, as a version's pass puts them.
    {
        const phazite::UndoStep step (*song, "Move Send");
        REQUIRE (song->getValues()->setTrackSendOrder (*song, bus, { second, first }));
    }

    REQUIRE (phazite::getSends (*song, bus) == std::vector { second, first });
    phazite::test::dispatchPendingMessages();
    REQUIRE (isHeard (inBus));

    // The version applied, its Undo and its Redo: the bus told reordered; another track muted or unmuted before the plan: its
    // track still plays on into it.
    const auto check = [&] (const char* what, const std::vector<DeviceID>& order)
    {
        INFO (what);
        CHECK (phazite::getSends (*song, bus) == order);
        CHECK (std::find (told.reordered.begin(), told.reordered.end(), bus) != told.reordered.end());
        phazite::setTrackMuted (*song, bystander, ! phazite::isTrackMuted (*song, bystander, false));
        CHECK (isHeard (inBus));
        phazite::test::dispatchPendingMessages();
        CHECK (isHeard (inBus));
        told.reordered.clear();
    };

    told.reordered.clear();

    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, before);
        phazite::removeBrokenReferences (*song);
    }

    check ("the version applied", { first, second });
    phazite::undo (*song);
    check ("its Undo", { second, first });
    phazite::redo (*song);
    check ("its Redo", { first, second });
    song->getValues()->removeListener (&told);
}

TEST_CASE ("Song values' sends: what writes automation follows a send's level that a trial put back sets while it writes, once "
           "the move is whole")
{
    // A trial (Swap's, a burst of the wheel's) put back takes its step back alone (Undo.cpp: takeBackAlone()), which tells the
    // history's listeners as an Undo does (Song.h: HistoryListener): the pass writes the value back from there.
    phazite::test::TestSong s;
    s.setTempo (120.0);
    const auto track = s.firstTrack;
    s.addMidiClip (track, 0.0, 32.0);
    auto& song = *phazite::findSong (s.context);
    TrackID reverb;

    {
        const phazite::UndoStep step (song, "Add Return");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    }

    s.addSend (track, reverb, -6.0f);
    const auto send = phazite::getSends (s.context, track).front().id;
    phazite::setAutomationMode (s.context, track, phazite::AutomationMode::latch);
    auto handle = phazite::getParameter (s.context, levelOf (send));
    REQUIRE (handle.exists());
    const auto range = handle.getRange();
    const auto set = range.convertFrom0to1 (0.3f);
    const auto tried = range.convertFrom0to1 (0.8f);

    phazite::test::EnginePlayback playback;
    s.run ("transport.playStop");
    playback.play (0.25);
    handle.setValue (set); // a step of its own, which latch punches in at
    playback.play (0.5);
    double putBackAt = 0.0;

    {
        phazite::Trial trial (song, "Try", nullptr);
        trial.apply ([&handle, tried] { handle.setValue (tried); });
        playback.play (0.25);
        REQUIRE (handle.getBaseValue() == doctest::Approx (tried).epsilon (0.001));
        putBackAt = phazite::getPlayheadBeat (s.context).inBeats();
        trial.putBack();
    }

    playback.play (0.5);
    CHECK (handle.getBaseValue() == doctest::Approx (set).epsilon (0.001));
    s.run ("transport.playStop");
    phazite::test::dispatchPendingMessages();

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getAutomationPoints (s.context, levelOf (send)).empty(); }));
    const auto points = phazite::getAutomationPoints (s.context, levelOf (send));
    const auto position = phazite::toControlPosition (range, set);
    CHECK (std::any_of (points.begin(), points.end(), [=] (const phazite::AutomationPoint& point)
                        { return point.beat.inBeats() >= putBackAt - 0.05 && std::abs (point.position - position) < 0.002f; }));
    CHECK (points.back().position == doctest::Approx (position).epsilon (0.002));
}

namespace
{
// Whether the song's tree holds a SEND anywhere: none, from the sends' switch on (their area holds them).
bool holdsSendElement (const juce::ValueTree& tree)
{
    for (const auto& child : tree)
        if (child.hasType (phazite::schema::SEND) || holdsSendElement (child))
            return true;

    return false;
}
} // namespace

TEST_CASE ("Song values' sends: the keeper keeps a track's sends with its element through a deletion and its Undo, a folder and "
           "a duplicate whose sends' modulated, automated and MIDI-linked levels follow the copy, and a return removed takes the "
           "sends to it, each one exact undo step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    TrackID reverb, delay;
    DeviceID toReverb, toDelay, fromOther;

    {
        const phazite::UndoStep step (song, "Add Sends");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        delay = phazite::addReturnTrack (song, "Delay", nullptr);
        toReverb = phazite::addSend (song, track, reverb, false);
        toDelay = phazite::addSend (song, track, delay, true);
        fromOther = phazite::addSend (song, other, reverb, false);
        phazite::setSendLevel (song, toDelay, -9.0f);
    }

    // Its first send's level modulated by its LFO and linked to a controller, its second's automated.
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    s.modulate (lfo, levelOf (toReverb), 0.5f);

    {
        const phazite::UndoStep step (song, "Automate and Link");
        REQUIRE (phazite::addPoint (song, levelOf (toDelay), { at (0.0), 0.5f }) >= 0);
        phazite::linkMidiController (song, { 1, 21 }, levelOf (toReverb));
    }

    CHECK_FALSE (holdsSendElement (song.getState()));
    const auto before = phazite::getSendsValue (song);
    phazite::test::UndoCheck undo (context);

    // (a) The track with its two deleted: they go with it, the other track's stays; its Undo brings them back as they were.
    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    CHECK (phazite::getSends (song, track).empty());
    CHECK (phazite::getSendValue (song, fromOther).has_value());
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::getSendsValue (song) == before);

    // (a) Moved into a folder and out: the same sends on the same track, the same element given them back each time (JUCE
    // takes a track out and puts it in again).
    TrackID folder;
    undo.checkSeveral ("a folder", [&] { folder = phazite::insertFolder (song, other, nullptr); });
    undo.checkSeveral ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    REQUIRE (phazite::getParentFolder (song, track) == folder);
    CHECK (phazite::getSendsValue (song) == before);
    undo.checkSeveral ("Ungroup Folder", [&] { phazite::ungroupFolder (song, folder); });
    CHECK (phazite::getSendsValue (song) == before);
    CHECK_FALSE (holdsSendElement (song.getState()));

    // Duplicated: the copy's sends under new IDs, to the same returns, as they are; the copy's LFO modulates the copy's first
    // send's level, its second's automation is the copy's too; the MIDI link stays the original's (a controller turns one
    // parameter), as before the sends left the tree.
    std::vector<TrackID> copies;
    undo.checkSeveral ("Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); });
    REQUIRE (copies.size() == 1);
    const auto copied = phazite::getSendValues (song, copies.front());
    REQUIRE (copied.size() == 2);
    CHECK (copied[0].id != toReverb);
    CHECK (copied[1].id != toDelay);
    CHECK (copied[0].to == reverb);
    CHECK (copied[1].to == delay);
    CHECK (copied[1].preFader);
    CHECK (juce::exactlyEqual (copied[1].level, -9.0f));
    CHECK (phazite::getSends (song, track) == std::vector { toReverb, toDelay });

    const auto copiedModulators = phazite::getModulators (song, copies.front());
    REQUIRE (copiedModulators.size() == 1);
    REQUIRE (phazite::getModulations (song, copiedModulators.front()).size() == 1);
    CHECK (phazite::getModulations (song, copiedModulators.front())[0].parameter == levelOf (copied[0].id));
    REQUIRE (phazite::getModulations (song, lfo).size() == 1);
    CHECK (phazite::getModulations (song, lfo)[0].parameter == levelOf (toReverb));
    CHECK (phazite::getAutomationPoints (context, levelOf (copied[1].id)).size() == 1);
    CHECK (phazite::getAutomationPoints (context, levelOf (toDelay)).size() == 1);

    const auto links = phazite::getMidiLinksValue (song).links;
    const auto isLinked = [&links] (DeviceID send)
    {
        return std::any_of (links.begin(), links.end(),
                            [send] (const phazite::MidiLinkEntry& link) { return link.device == send.getRaw(); });
    };
    CHECK (isLinked (toReverb));
    CHECK_FALSE (isLinked (copied[0].id));
    CHECK_FALSE (holdsSendElement (song.getState()));
    phazite::test::checkSong (song);

    // A return removed: the sends to it go in that step (the original's, the copy's and the other track's), with what
    // modulated them; its Undo brings them back.
    undo.checkSeveral ("Delete Return", [&] { phazite::deleteTracks (song, { reverb }, nullptr); });

    for (const auto& [key, send] : phazite::getSendsValue (song).sends)
        CHECK (send.to != reverb);

    CHECK (phazite::getModulations (song, lfo).empty());
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::getSendValue (song, toReverb).has_value());
    CHECK (phazite::getSendValue (song, fromOther).has_value());
    CHECK (phazite::getModulations (song, lfo).size() == 1);
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' sends: a track deleted and brought back by Undo, whose send is then removed, is moved into a folder "
           "without it")
{
    // The keeper remembers a deleted track's sends with its element, and the Undo (which it leaves alone) brings them back
    // itself: what it remembered must not come back with the next move of the same element, once the send is gone.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    DeviceID send;

    {
        const phazite::UndoStep step (song, "Add Send");
        send = phazite::addSend (song, track, phazite::addReturnTrack (song, "Reverb", nullptr), false);
    }

    REQUIRE (send.isValid());
    phazite::test::UndoCheck undo (context);
    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    undo.stepBackOrForth ("Undo Delete Track", [&] { phazite::undo (song); });
    REQUIRE (phazite::getSendValue (song, send).has_value());

    undo.checkSeveral ("Remove Send", [&] { phazite::removeSend (song, send); });
    REQUIRE_FALSE (phazite::getSendValue (song, send).has_value());

    TrackID folder;
    undo.checkSeveral ("a folder", [&] { folder = phazite::insertFolder (song, other, nullptr); });
    undo.checkSeveral ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    REQUIRE (phazite::getParentFolder (song, track) == folder);
    CHECK_FALSE (phazite::getSendValue (song, send).has_value());
    CHECK (phazite::getSends (song, track).empty());
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    CHECK_FALSE (phazite::getSendValue (song, send).has_value());
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' sends: a track that comes in with sends outside the history has them taken in by the tracks' safety net, "
           "one of an ID the song holds dropped")
{
    // The tracks' safety net. Since the tracks' switch the song's tree holds no TRACK, so no lone SEND
    // can be added to one; a TRACK that comes in is the tracks' part of the keeper's (TracksArea.h), which reads its sends
    // through the sends' reader of a track (SendsArea.h: takeTrackSendsIn()), outside the history alone.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto& values = *song.getValues();
    const auto track = s.firstTrack;
    TrackID reverb;
    DeviceID held, other;

    {
        const phazite::UndoStep step (song, "Add Sends");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        held = phazite::addSend (song, track, reverb, false);
        other = phazite::addSend (song, track, phazite::addReturnTrack (song, "Delay", nullptr), true);
    }

    REQUIRE (other.isValid());

    // A track that comes in whole with its SEND elements (as a merge's choice of a whole track brings one), outside the
    // history: its own taken in, counted (by the sends' count, and as a track taken in), the one under an ID the song holds
    // dropped (the song's kept, on its track), none left in the tree.
    auto incoming = phazite::test::copyTrack (song, track);
    phazite::giveNewIDs (song, incoming);
    auto sendsIn = 0;

    for (auto child : incoming)
        if (child.hasType (phazite::schema::SEND) && sendsIn++ == 0)
            child.setProperty (phazite::schema::sendID, (juce::int64) held.getRaw(), nullptr); // the song's send's ID

    REQUIRE (sendsIn == 2);
    const auto came = phazite::readTrackElementID (incoming);
    const auto tracksBefore = values.countTracksAdopted();

    {
        const phazite::Song::OutsideHistory outside (song);
        song.getState().getChildWithName (phazite::schema::TRACKS).appendChild (incoming, nullptr);
    }

    CHECK (values.countTracksAdopted() == tracksBefore + 1);
    REQUIRE (phazite::getSends (song, came).size() == 1);
    CHECK (phazite::getSendTrack (song, held) == track);
    CHECK_FALSE (holdsSendElement (song.getState()));
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' sends: a track deleted, then its line brought back from a version, comes back with its sends as they "
           "were, and Undo and Redo take them back and forth")
{
    // What the keeper does with a track that comes in with its sends (the case above), as someone does it, through the model:
    // so that it holds unchanged once the tracks leave the song's tree (roadmap step S5.20).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    DeviceID toReverb, toDelay;

    {
        const phazite::UndoStep step (song, "Add Sends");
        toReverb = phazite::addSend (song, track, phazite::addReturnTrack (song, "Reverb", nullptr), false);
        toDelay = phazite::addSend (song, track, phazite::addReturnTrack (song, "Delay", nullptr), true);
        phazite::setSendLevel (song, toDelay, -9.0f);
    }

    REQUIRE (phazite::getSends (song, track) == std::vector { toReverb, toDelay });
    const auto before = phazite::getSendsValue (song);
    const auto version = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (s.context);

    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    CHECK_FALSE (phazite::getSendValue (song, toReverb).has_value());
    CHECK_FALSE (phazite::getSendValue (song, toDelay).has_value());
    phazite::test::checkSong (song);

    undo.checkSeveral ("Restore Track",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Restore Track");
                           REQUIRE (phazite::makeTrackLike (song, track, version));
                           phazite::removeBrokenReferences (song);
                       });
    CHECK (phazite::getSendsValue (song) == before);
    CHECK_FALSE (holdsSendElement (song.getState()));
    phazite::test::checkSong (song);

    undo.stepBackOrForth ("Undo Restore Track", [&] { phazite::undo (song); });
    CHECK_FALSE (phazite::getSendValue (song, toReverb).has_value());
    undo.stepBackOrForth ("Redo Restore Track", [&] { phazite::redo (song); });
    CHECK (phazite::getSendsValue (song) == before);
    undo.walkBackAndForth();
    CHECK (phazite::getSendsValue (song) == before);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' sends: a set equal to what the song holds makes no undo step and tells nothing; the unsaved check "
           "agrees with the whole comparison after an edit and its Undo")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    DeviceID first, second;

    {
        const phazite::UndoStep step (song, "Add Sends");
        first = phazite::addSend (song, track, phazite::addReturnTrack (song, "Reverb", nullptr), false);
        second = phazite::addSend (song, track, phazite::addReturnTrack (song, "Delay", nullptr), true);
    }

    REQUIRE (project.save().wasOk());
    Told told;
    song.getValues()->addListener (&told);

    {
        const phazite::UndoStep step (song, "Nothing");
        auto* values = song.getValues();
        CHECK_FALSE (values->setSend (song, *phazite::getSendValue (song, first)));
        CHECK_FALSE (values->setAllSends (song, phazite::getSendsValue (song)));
        CHECK_FALSE (values->setTrackSendOrder (song, track, { first, second }));
        CHECK_FALSE (values->eraseSend (song, DeviceID()));
        phazite::setSendLevel (song, first, phazite::getSendLevel (song, first));
        phazite::setSendLevel (song, first, phazite::getSendLevel (song, first) + 1.0e-6f); // the same to a ten-thousandth
        phazite::setPreFader (song, second, true);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    CHECK (told.calls == 0);
    song.getValues()->removeListener (&told);

    // (The quick check asked only after both: once it finds an unsaved change, a save clears it, not an Undo.)
    setLevel (song, first, -20.0f);
    CHECK_FALSE (project.isWholeAsSaved());
    phazite::undo (song);
    CHECK_FALSE (project.hasUnsavedChanges());
    CHECK (project.isWholeAsSaved());
}

TEST_CASE ("Song values' sends: a version with other sends applied each way, a track's line and a folder's line brought back, and "
           "the song as copied, which writes nothing")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto inFolder = s.addTrack ("In Folder");
    TrackID reverb, delay, folder;
    DeviceID toReverb, fromFolder;

    {
        const phazite::UndoStep step (song, "Add Sends");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        delay = phazite::addReturnTrack (song, "Delay", nullptr);
        toReverb = phazite::addSend (song, track, reverb, false);
        fromFolder = phazite::addSend (song, inFolder, delay, false);
        folder = phazite::insertFolder (song, inFolder, nullptr);
        phazite::moveTrackIntoFolder (song, inFolder, folder);
    }

    REQUIRE (phazite::getParentFolder (song, inFolder) == folder);
    const auto version = phazite::test::copyWholeSong (song);
    DeviceID added;

    {
        const phazite::UndoStep step (song, "Other Sends");
        phazite::setSendLevel (song, toReverb, -20.0f);
        phazite::setPreFader (song, toReverb, true);
        phazite::setSendLevel (song, fromFolder, -1.5f);
        added = phazite::addSend (song, track, delay, false);
    }

    const auto since = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (context);

    // The whole song, each way, in a step.
    for (const auto* other : { &version, &since, &version })
    {
        undo.checkSeveral ("Apply Version",
                           [&]
                           {
                               const phazite::UndoStep step (song, "Apply Version");
                               phazite::makeSongLike (song, *other);
                               phazite::removeBrokenReferences (song);
                           });
        CHECK (phazite::readSendsValue (*other) == phazite::getSendsValue (song));
        CHECK_FALSE (holdsSendElement (song.getState()));
        phazite::test::checkSong (song);
    }

    // The song as it was since, then the track's line brought back: its sends as the version holds them, the folder's track's
    // as they are.
    undo.checkSeveral ("Apply Since",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Apply Since");
                           phazite::makeSongLike (song, since);
                           phazite::removeBrokenReferences (song);
                       });
    undo.checkSeveral ("Restore Track",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Restore Track");
                           REQUIRE (phazite::makeTrackLike (song, track, version));
                           phazite::removeBrokenReferences (song);
                       });
    const auto inVersion = phazite::readSendsValue (version);
    CHECK (phazite::getSends (song, track) == std::vector { toReverb });
    CHECK (phazite::getSendValue (song, toReverb) == *inVersion.find (toReverb));
    CHECK_FALSE (phazite::getSendValue (song, added).has_value());
    CHECK (juce::exactlyEqual (phazite::getSendLevel (song, fromFolder), -1.5f));

    // The folder deleted with its track, then its line brought back: the track in it comes back with it, and with its send
    // as the version holds it.
    undo.checkSeveral ("Delete Folder", [&] { phazite::deleteTracks (song, { folder }, nullptr); });
    REQUIRE_FALSE (phazite::getSendValue (song, fromFolder).has_value());
    undo.checkSeveral ("Restore Folder",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Restore Folder");
                           REQUIRE (phazite::makeTrackLike (song, folder, version));
                           phazite::removeBrokenReferences (song);
                       });
    REQUIRE (phazite::getSendValue (song, fromFolder).has_value());
    CHECK (phazite::getSendValue (song, fromFolder) == *inVersion.find (fromFolder));
    CHECK (phazite::getSendTrack (song, fromFolder) == inFolder);
    phazite::test::checkSong (song);

    // The song as copied writes nothing (asserted in Debug builds: ApplySong.cpp), its sends the value they were.
    const auto held = phazite::getSendsValue (song);
    phazite::makeSongAsCopied (song, phazite::test::copyWholeSong (song));
    CHECK (phazite::getSendsValue (song).isSameAs (held));
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

namespace
{
// The path a merge's clash names an element by, from the song down (SongMerge.h: findByPath()).
juce::StringArray pathOf (const juce::ValueTree& element)
{
    juce::StringArray path;

    for (auto each = element; each.isValid() && each.getParent().isValid(); each = each.getParent())
        path.insert (0, each.getType().toString() + ":" + phazite::schema::getElementKey (each));

    return path;
}

// A send's element in a song whole, by its ID.
juce::ValueTree findSendIn (const juce::ValueTree& tree, DeviceID send)
{
    for (const auto& child : tree)
    {
        if (child.hasType (phazite::schema::SEND) && phazite::readID (child[phazite::schema::sendID]) == send.getRaw())
            return child;

        if (const auto found = findSendIn (child, send); found.isValid())
            return found;
    }

    return {};
}
} // namespace

TEST_CASE ("Song values' sends: a merge's clash on a send is chosen through the sends' setters, each way; a send both lines made "
           "under one ID goes for what was before (known bug 239's shape); what the song can't hold is refused")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    TrackID reverb;
    DeviceID send;

    {
        const phazite::UndoStep step (song, "Add Send");
        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        send = phazite::addSend (song, track, reverb, false);
    }

    // Each line turns the send's level its own way: base, theirs, then ours, which the song holds after.
    const auto base = phazite::test::copyWholeSong (song);
    setLevel (song, send, -12.0f);
    const auto theirs = phazite::test::copyWholeSong (song);
    phazite::undo (song);
    setLevel (song, send, -3.0f);
    const auto ours = phazite::test::copyWholeSong (song);
    const phazite::MergeSides sides { base, ours, theirs };
    const auto clashes = phazite::mergeSongs (base, ours, theirs, [&song] { return song.makeID(); }).clashes;
    const phazite::MergeClash* level = nullptr;

    for (const auto& clash : clashes)
        if (clash.property == phazite::schema::sendLevel)
            level = &clash;

    REQUIRE (level != nullptr);
    phazite::test::UndoCheck undo (context);

    // The clash chosen with this value in a step of its own, checked, then undone where it wrote.
    const auto choose = [&] (const phazite::MergeClash& clash, int value, const std::function<void()>& check)
    {
        const auto before = phazite::test::copyWholeSong (song);
        auto choice = phazite::AreaChoice::notTheArea;
        undo.check ("Choose",
                    [&]
                    {
                        const phazite::UndoStep step (song, "Choose");
                        choice = phazite::chooseSendClash (song, clash, value, sides);
                    });
        REQUIRE (choice == phazite::AreaChoice::chosen);
        check();
        phazite::test::checkSong (song);

        if (! phazite::test::copyWholeSong (song).isEquivalentTo (before))
            phazite::undo (song);
    };

    for (const auto& [value, expected] : { std::pair { 0, -6.0f }, std::pair { 1, -3.0f }, std::pair { 2, -12.0f } })
    {
        const auto wanted = expected;
        choose (*level, value, [&song, send, wanted] { CHECK (juce::exactlyEqual (phazite::getSendLevel (song, send), wanted)); });
    }

    // A whole send: the song's (ours) taken out on one side, each side's element made the song's, or none.
    phazite::MergeClash whole;
    whole.line = "Track";
    whole.path = pathOf (findSendIn (base, send));
    whole.baseElement = findSendIn (base, send);
    whole.theirsElement = findSendIn (theirs, send);
    REQUIRE (whole.baseElement.isValid());
    choose (whole, 0, [&] { CHECK (juce::exactlyEqual (phazite::getSendLevel (song, send), -6.0f)); });
    choose (whole, 1, [&] { CHECK_FALSE (phazite::getSendValue (song, send).has_value()); });
    choose (whole, 2, [&] { CHECK (juce::exactlyEqual (phazite::getSendLevel (song, send), -12.0f)); });
    CHECK (phazite::getSends (song, track) == std::vector { send });

    // A send both lines made under one ID (only a damaged commit's song holds one: the merge renumbers what both add), its
    // level's clash with no value before: what was before takes it away rather than leaving it at 0 dB.
    DeviceID made;

    {
        const phazite::UndoStep step (song, "Made");
        made = phazite::addSend (song, other, reverb, false);
    }

    const auto madeOurs = phazite::test::copyWholeSong (song);
    auto madeTheirs = madeOurs.createCopy();
    findSendIn (madeTheirs, made).setProperty (phazite::schema::sendLevel, -18.0f, nullptr);
    phazite::MergeClash both;
    both.line = "Other";
    both.path = pathOf (findSendIn (madeOurs, made));
    both.property = phazite::schema::sendLevel;
    both.ours = -6.0f;
    both.theirs = -18.0f;
    const phazite::MergeSides madeSides { base, madeOurs, madeTheirs };

    for (const auto& [value, expected] : { std::pair { 0, 0.0f }, std::pair { 1, -6.0f }, std::pair { 2, -18.0f } })
    {
        INFO ("value " << value);
        const auto heldBefore = phazite::getSendsValue (song);

        {
            const phazite::UndoStep step (song, "Choose Made");
            REQUIRE (phazite::chooseSendClash (song, both, value, madeSides) == phazite::AreaChoice::chosen);
        }

        if (value == 0)
            CHECK_FALSE (phazite::getSendValue (song, made).has_value());
        else
            CHECK (juce::exactlyEqual (phazite::getSendLevel (song, made), expected));

        phazite::test::checkSong (song);

        if (! (phazite::getSendsValue (song) == heldBefore))
            phazite::undo (song);
    }

    // Taken away, then the other line's chosen: put back as it holds it.
    {
        const phazite::UndoStep step (song, "Before");
        REQUIRE (phazite::chooseSendClash (song, both, 0, madeSides) == phazite::AreaChoice::chosen);
    }

    REQUIRE_FALSE (phazite::getSendValue (song, made).has_value());

    {
        const phazite::UndoStep step (song, "Theirs");
        REQUIRE (phazite::chooseSendClash (song, both, 2, madeSides) == phazite::AreaChoice::chosen);
    }

    CHECK (juce::exactlyEqual (phazite::getSendLevel (song, made), -18.0f));
    CHECK (phazite::getSendTrack (song, made) == other);
    phazite::undo (song);
    phazite::undo (song);

    // Refused, nothing written: a send the song hasn't, its ID, a property that isn't a send's, a level finer than the song
    // holds, a whole send of another ID than its path's, and a send's path through another track than the one it is on.
    auto nowhere = *level;
    nowhere.path.set (nowhere.path.size() - 1, phazite::schema::SEND.toString() + ":" + juce::String ((juce::int64) song.makeID()));
    auto byID = *level;
    byID.property = phazite::schema::sendID;
    auto unknown = *level;
    unknown.property = "nothing";
    auto finer = *level;
    finer.theirs = "-6.123456";
    auto otherID = whole;
    otherID.theirsElement = findSendIn (madeTheirs, made);
    auto otherTrack = *level;
    otherTrack.path.set (otherTrack.path.size() - 1, both.path[both.path.size() - 1]);

    for (const auto* refused : { &nowhere, &byID, &unknown, &finer, &otherID, &otherTrack })
    {
        const phazite::UndoStep step (song, "Refused");
        CHECK (phazite::chooseSendClash (song, *refused, 2, { base, ours, theirs }) == phazite::AreaChoice::refused);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }
}

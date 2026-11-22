// SPDX-License-Identifier: AGPL-3.0-or-later
// The changes since the last version worked out on the history's thread (docs/design/versions.md: Off the message thread;
// roadmap S4.5): from copies of what the song's notifications said changed, a round at a time, the last list kept until the
// next is in, whose listeners are told then; never a list of another song's, nor an older one last.
#include "TestClips.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ChangesWorker.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/VersionHistory.h"

#include <functional>
#include <optional>

namespace
{
using namespace phazite::test;

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name = {})
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
    return phazite::getLastVersion (context)->id;
}

// What the versions' listeners were told, and the changes each time.
struct Told final : phazite::ProjectContext::Listener
{
    explicit Told (phazite::ProjectContext& c)
        : context (c)
    {
        context.addListener (this);
    }

    ~Told() override { context.removeListener (this); }

    void projectChanged() override {}

    void versionsChanged() override
    {
        ++times;
        lists.push_back (phazite::countChanges (context));
    }

    phazite::ProjectContext& context;
    int times = 0;
    std::vector<int> lists;
};

// The history's thread held at a read of its own, so that what is given it after waits behind it, until let go.
struct ThreadHeld
{
    explicit ThreadHeld (phazite::ProjectContext& context)
    {
        phazite::VersionHistory::holdReadsForTests = true;
        auto& history = *phazite::getVersionHistory (context);
        history.getVersionChanges (phazite::getLastVersion (context)->id); // a read, held where it starts
        REQUIRE (history.isReadingVersionChanges (phazite::getLastVersion (context)->id));
    }

    ~ThreadHeld() { letGo(); }

    void letGo() { phazite::VersionHistory::holdReadsForTests = false; }
};

// A line of the changes as text: its kind, where it is, its name and what changed.
juce::String describe (const phazite::VersionChange& line)
{
    const auto kind = line.kind == phazite::VersionChange::Kind::added     ? "added"
                      : line.kind == phazite::VersionChange::Kind::removed ? "removed"
                                                                           : "changed";
    const auto where = line.isSong     ? juce::String ("song")
                       : line.isMaster ? juce::String ("master")
                                       : "track " + juce::String (line.track.getRaw());
    return juce::String (kind) + " " + where + " \"" + line.name + "\": " + line.what;
}

juce::String describe (const phazite::SongChange& change)
{
    phazite::VersionChange line;
    line.kind = change.kind == phazite::SongChange::Kind::added     ? phazite::VersionChange::Kind::added
                : change.kind == phazite::SongChange::Kind::removed ? phazite::VersionChange::Kind::removed
                                                                    : phazite::VersionChange::Kind::changed;
    line.isMaster = change.where == phazite::SongChange::Where::master;
    line.isSong = change.where == phazite::SongChange::Where::song;
    line.track = line.isSong ? phazite::TrackID() : phazite::TrackID::fromRaw (change.trackID);
    line.name = change.name;
    line.what = change.what.joinIntoString (", ");
    return describe (line);
}

// The changes since the last version, as the history worked them out from what the song's notifications said changed,
// against those of a comparison of the last version's song with the song as it is, whole (SongDiff.h: findChanges()): the
// same lines, in the same order.
void checkChangesAreWhole (phazite::ProjectContext& context, const juce::String& after)
{
    INFO ("after " << after);
    juce::StringArray worked, whole;

    for (const auto& line : phazite::getChangesSinceLastVersion (withChangesIn (context)))
        worked.add (describe (line));

    const auto& history = *phazite::getVersionHistory (context);
    const auto version = history.readSong (phazite::getLastVersion (context)->id);
    REQUIRE (version.isValid());

    for (const auto& change : phazite::findChanges (version, phazite::test::copyWholeSong (*phazite::findSong (context))))
        whole.add (describe (change));

    CHECK_MESSAGE (worked == whole, "worked out from what changed:\n  " << worked.joinIntoString ("\n  ")
                                                                        << "\nthe whole songs compared:\n  "
                                                                        << whole.joinIntoString ("\n  "));
}
} // namespace

TEST_CASE ("The changes since the last version, worked out from what changed, are those of the whole songs compared, after note "
           "and clip edits and their Undo")
{
    // A lone clip on the Keys; on the Bass a clip X with an alias Y on the Keys.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto keys = s.addTrack ("Keys");
    const auto lone = s.addMidiClip (keys, 0.0, 4.0);
    s.addNote (lone, 60, 0.0, 1.0);
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    s.addNote (clip, 40, 2.0, 1.0);
    const auto alias = s.copyClip (clip, 8.0, true);
    auto& song = *phazite::findSong (s.context);
    phazite::moveClip (song, alias, at (8.0), keys);
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);

    const auto edit = [&] (const juce::String& name, const std::function<void()>& change)
    {
        {
            const phazite::UndoStep step (song, name);
            change();
        }

        checkChangesAreWhole (s.context, name);
    };

    edit ("the lone clip muted", [&] { phazite::toggleClipsMuted (song, { lone }); });
    edit ("a lone clip's note moved", [&] { phazite::moveNote (song, phazite::getClipNotes (song, lone).front().getRef(), at (1.0), 62); });
    edit ("a note of the alias made quieter",
          [&] { phazite::setNoteVelocities (song, { phazite::getClipNotes (song, alias).back().getRef() }, 40); });
    edit ("a note added through the clip", [&] { phazite::addNote (song, clip, 43, at (3.0), lasting (0.5), 90); });
    edit ("the lone clip moved onto the Bass", [&] { phazite::moveClip (song, lone, at (12.0), bass); });
    edit ("the alias moved onto the Bass", [&] { phazite::moveClip (song, alias, at (20.0), bass); });
    edit ("the clip deleted", [&] { phazite::deleteClips (song, { clip }, false); });

    for (const auto* step : { "the clip deleted", "the alias moved", "the lone clip moved", "the note added", "the velocity" })
    {
        phazite::undo (song);
        checkChangesAreWhole (s.context, juce::String ("Undo of ") + step);
    }

    phazite::redo (song);
    checkChangesAreWhole (s.context, "Redo of the velocity");
}

TEST_CASE ("A tempo change and nothing else changes the song's own line alone, and a tempo set back by another edit leaves no change")
{
    // What the song's own line is worked out from (ChangesWorker.cpp: makeSkeleton()) holds the tempo map whole, as the song
    // does: one that held none, or an empty one, would leave the song's own line changed with the tempo back where the version
    // has it.
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);
    auto& song = *phazite::findSong (s.context);
    const auto tempo = phazite::getTempo (song);

    phazite::setTempo (song, 100.0);
    const auto lines = phazite::getChangesSinceLastVersion (withChangesIn (s.context));
    REQUIRE (lines.size() == 1);
    CHECK (lines.front().isSong);
    checkChangesAreWhole (s.context, "the tempo changed");

    phazite::setTempo (song, tempo);
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 0);
    checkChangesAreWhole (s.context, "the tempo set back");
}

TEST_CASE ("A curve edited and nothing else changes its device's track's line alone, and the edit undone leaves no change")
{
    // What the changes are worked out from takes each curve that changed from the song whole (ChangesWorker.cpp: take(),
    // Automation.h: copyWholeCurve()): a curve it didn't find there would count as gone, so that the track's line would name
    // its automation as changed with the curve back as the version has it.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto keys = s.addTrack ("Keys");
    auto& song = *phazite::findSong (s.context);
    const auto volume = s.getVolumeParameter (bass);
    phazite::addAutomationPoint (s.context, volume, { at (0.0), 0.5f });
    phazite::addAutomationPoint (s.context, s.getVolumeParameter (keys), { at (0.0), 0.5f });
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);

    phazite::addAutomationPoint (s.context, volume, { at (4.0), 0.25f });
    const auto lines = phazite::getChangesSinceLastVersion (withChangesIn (s.context));
    REQUIRE (lines.size() == 1);
    CHECK (lines.front().track == bass);
    CHECK_FALSE (lines.front().isSong);
    checkChangesAreWhole (s.context, "a point added");

    phazite::undo (song);
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 0);
    checkChangesAreWhole (s.context, "the point's Undo");

    phazite::redo (song);
    REQUIRE (phazite::getChangesSinceLastVersion (withChangesIn (s.context)).size() == 1);
    checkChangesAreWhole (s.context, "the point's Redo");
}

TEST_CASE ("The changes follow an edit once worked out in the background, the listeners told as they come in")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);
    Told told (s.context);

    // Edited: the song may have changes at once (what Save Version and Discard All Changes go by), and the list follows
    // without a test asking, as the views' ticks ask.
    s.setVolume (bass, -6.0f);
    CHECK (phazite::mayHaveChangesSinceLastVersion (s.context));
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::countChanges (s.context) == 1; }, versionHistoryTimeout));
    CHECK (phazite::areChangesWorkedOut (s.context));
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    REQUIRE (dispatchMessagesUntil ([&told] { return ! told.lists.empty() && told.lists.back() == 1; }));
    CHECK (phazite::getChangesSinceLastVersion (s.context).front().what == "volume");

    // Undone: back to the version's state, no changes left behind ("a change put back leaves nothing behind").
    phazite::undo (*phazite::findSong (s.context));
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 0);
    CHECK_FALSE (phazite::mayHaveChangesSinceLastVersion (s.context));
    REQUIRE (dispatchMessagesUntil ([&told] { return told.lists.back() == 0; }));
}

TEST_CASE ("An edit while a round is worked out gives the newer list, never the older one last")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);
    Told told (s.context);

    {
        ThreadHeld held (s.context);

        // A round for Bass, given while the thread is held; Drums edited meanwhile, for the next.
        s.setVolume (bass, -6.0f);
        phazite::getChangesSinceLastVersion (s.context);
        s.setVolume (drums, -3.0f);
        phazite::getChangesSinceLastVersion (s.context);
        dispatchPendingMessages();
        CHECK_FALSE (phazite::areChangesWorkedOut (s.context));
        CHECK (phazite::countChanges (s.context) == 0); // the last list, until the next is in
        held.letGo();
    }

    CHECK (phazite::countChanges (withChangesIn (s.context)) == 2);
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE_FALSE (told.lists.empty());
    CHECK (told.lists.back() == 2);
}

TEST_CASE ("A round under way as the project closes is let go of, and one of another version's song is never taken in")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    s.saveWithPluginStates();

    // Closed with a round given behind a held read: nothing waits, nothing reaches the song opened after.
    {
        ThreadHeld held (s.context);
        s.setVolume (bass, -6.0f);
        phazite::getChangesSinceLastVersion (s.context);
        auto& song = s.getProject().getSong();
        const auto number = s.context.getProjectNumber();
        phazite::setSong (s.context, nullptr);
        held.letGo();
        REQUIRE (dispatchMessagesUntil ([] { return phazite::countVersionsBeingFinished() == 0; }, versionHistoryTimeout));
        dispatchPendingMessages();

        phazite::setSong (s.context, &song);
        CHECK (s.context.getProjectNumber() != number);
        REQUIRE (waitForVersionHistory (s.context));
        CHECK (phazite::countChanges (withChangesIn (s.context)) == 1); // the volume, as this song's own round says
    }

    // A round of a version's song that was replaced as it ran (Undo Last Version, a switch, a version saved) is dropped:
    // the worker's own rule, which the history follows.
    phazite::ChangesWorker worker;
    juce::ValueTree song (phazite::schema::PHAZITE), other (phazite::schema::PHAZITE);
    song.appendChild (juce::ValueTree (phazite::schema::TRACKS), nullptr);
    other.appendChild (juce::ValueTree (phazite::schema::TRACKS), nullptr);
    other.setProperty (phazite::schema::outline, "pop", nullptr);
    auto& open = s.getProject().getSong();
    const auto none = [] (const std::set<std::uint64_t>&) { return phazite::PluginParameterValues(); };

    worker.setReference (other, {}, true);
    const auto request = worker.take (open, none, none, {});
    REQUIRE (request != nullptr);
    CHECK (worker.isWorking());
    worker.setReference (song, {}, true); // another version's song meanwhile
    CHECK_FALSE (worker.takeResult (phazite::ChangesWorker::work (*request)));
    CHECK (worker.getChanges().empty());
    CHECK (worker.isDirty()); // everything again, against the new one
    CHECK_FALSE (worker.isWorking());
}

TEST_CASE ("Save Version names the version by what changed in the song as it is, worked out from its copy")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");

    // Saved at once after the edit, before any list of its changes is in: named from the song.
    s.setVolume (bass, -6.0f);
    phazite::setVersionDraft (s.context, {}, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    CHECK (phazite::getLastVersion (s.context)->name == "Update Bass");
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 0);

    // Discard All Changes acts on the song as it is: there the moment the song changed, before its list is in.
    s.setVolume (bass, -9.0f);
    CHECK (phazite::mayHaveChangesSinceLastVersion (s.context));
    REQUIRE (phazite::discardAllChanges (s.context));
    CHECK (phazite::getChannelVolume (*phazite::findSong (s.context), bass) == doctest::Approx (-6.0f));
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 0);
}

TEST_CASE ("A note edited and a version saved at once leave no changes since the version, without another ask")
{
    // The clips whose notes were heard to change are marked as the baseline moves to the version saved (VersionHistory.cpp:
    // baselineChanged()), not left for the next ask, which kept the history saying it may have changes and hadn't worked
    // them out.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);
    auto& song = *phazite::findSong (s.context);

    {
        const phazite::UndoStep step (song, "Set Velocity");
        phazite::setNoteVelocities (song, { phazite::getClipNotes (song, clip).front().getRef() }, 40);
    }

    phazite::setVersionDraft (s.context, "Quieter", {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (phazite::getLastVersion (s.context)->name == "Quieter");

    CHECK (phazite::areChangesWorkedOut (s.context)); // asks nothing
    CHECK_FALSE (phazite::mayHaveChangesSinceLastVersion (s.context));
    CHECK (phazite::countChanges (s.context) == 0);
}

TEST_CASE ("Compare with the Song lists what differs from another version, worked out in the background as the song changes")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.setVolume (bass, -6.0f);
    keepVersion (s.context, "Quieter");

    REQUIRE (finished (s.context, [&] { return phazite::compareWithVersion (s.context, first); }));
    CHECK (phazite::getListedChanges (withChangesIn (s.context)).size() == 1);

    // The song back as First had it: nothing differs from First, while the changes since Quieter say the volume.
    s.setVolume (bass, 0.0f);
    CHECK (phazite::getListedChanges (withChangesIn (s.context)).empty());
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 1);
}

TEST_CASE ("Save Version writes nothing for a song with no change since its version, though its changes weren't worked out yet")
{
    // The review of S4.5: Save Version is there while the song may have changes, before the round after an edit is in; an
    // edit and its Undo, or Discard All Changes, then Ctrl+Alt+S, saved a version of nothing, "No changes".
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    const auto versions = phazite::getVersions (s.context).size();

    const auto save = [&s]
    {
        std::optional<juce::Result> result;
        REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
        REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
        REQUIRE (waitForVersionHistory (s.context));
        return *result;
    };

    // An edit and its Undo, saved at once.
    s.setVolume (bass, -6.0f);
    phazite::undo (*phazite::findSong (s.context));
    phazite::setVersionDraft (s.context, "Half time", "the bass down");
    auto result = save();
    CHECK (phazite::getVersionDraftName (s.context) == "Half time"); // the name typed, back in the field
    CHECK (phazite::getVersionDraftNote (s.context) == "the bass down");
    phazite::setVersionDraft (s.context, {}, {});
    CHECK (result.failed());
    CHECK (result.getErrorMessage() == "Nothing changed since the last version.");
    CHECK (phazite::getSaveProblem (s.context) == "Nothing changed since the last version.");
    CHECK (phazite::getVersions (s.context).size() == versions);

    // Discard All Changes, then saved.
    s.setVolume (bass, -6.0f);
    REQUIRE (phazite::discardAllChanges (s.context));
    result = save();
    CHECK (result.failed());
    CHECK (phazite::getVersions (s.context).size() == versions);

    // A change: saved, and the problem said no more.
    s.setVolume (bass, -3.0f);
    CHECK (save().wasOk());
    CHECK (phazite::getSaveProblem (s.context).isEmpty());
    CHECK (phazite::getVersions (s.context).size() == versions + 1);
}

TEST_CASE ("A drag is compared once it is committed, and rounds come at most once a tick, never back to back")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    REQUIRE (phazite::countChanges (withChangesIn (s.context)) == 0);
    auto& song = *phazite::findSong (s.context);
    const auto copied = phazite::ChangesWorker::countTracksCopied();

    // A fader dragged through many moves, the badge and the view asking all along: no round until it is committed.
    {
        phazite::Gesture drag (song, "Change Volume");

        for (int i = 0; i < 40; ++i)
        {
            s.setVolume (bass, -0.25f * (float) (i + 1));
            dispatchPendingMessages();
            phazite::countChanges (s.context);
        }

        CHECK (phazite::ChangesWorker::countTracksCopied() == copied);
        CHECK_FALSE (phazite::areChangesWorkedOut (s.context));
        drag.commit();
    }

    CHECK (phazite::countChanges (withChangesIn (s.context)) == 1);
    CHECK (phazite::ChangesWorker::countTracksCopied() - copied == 1);
}

TEST_CASE ("A round of a reference let go of since it was taken works nothing out, and counts nothing")
{
    // The review of S4.5: a round taken while a version was being saved ran after the save's write, against the version before,
    // its list dropped, and a performance gate counted its comparisons late. None is taken while a version is saved now
    // (VersionHistory::askForChanges()), and one of a reference let go of stops.
    TestSong s;
    s.addTrack ("Bass");
    s.addTrack ("Drums");
    const auto& song = *phazite::findSong (s.context);
    const auto noKnobs = [] (const std::set<std::uint64_t>&) { return phazite::PluginParameterValues(); };

    for (const auto letGo : { false, true })
    {
        CAPTURE (letGo);
        phazite::ChangesWorker worker;
        worker.setReference (juce::ValueTree (phazite::schema::PHAZITE), {}, true);
        const auto request = worker.take (song, noKnobs, noKnobs, {});
        REQUIRE (request != nullptr);

        if (letGo)
            worker.setReference (phazite::test::copyWholeSong (song), {}, true, true);

        const auto before = phazite::ChangesWorker::countTracksCompared();
        const auto result = phazite::ChangesWorker::work (*request);
        CHECK ((phazite::ChangesWorker::countTracksCompared() == before) == letGo);
        CHECK (result.changes.empty() == letGo);
        CHECK (worker.takeResult (result) == ! letGo);
    }
}

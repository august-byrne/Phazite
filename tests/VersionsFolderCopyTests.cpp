// SPDX-License-Identifier: AGPL-3.0-or-later
// A first save's copy of the Versions folder (model/versions/internal/VersionHistory.h: copyFolder()), as the history sees it:
// nothing writes into the folder while it is copied (a merge taken is refused, saying why, and every write into a held
// folder asserts: model/base/WholeFile.h, holdFolder()); what waited for the copy starts after it, unless the project closes
// first; and a copy that never began, as the history goes, is told so.
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/WholeFile.h"
#include "model/commands/CommandRegistry.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

namespace
{
using namespace phazite::test;

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
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

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

// A copy of the folder made slow, as a first save gives it to the history (makeVersionsCopier()): it holds the folder from its
// start (copyFolder()) until it is let go, as a long history's copy does; let go of as this goes too, so that a check that
// fails leaves no job waiting for ever. What the job and the callback share outlives this.
struct SlowCopy
{
    struct State
    {
        std::atomic<bool> started { false }, letGo { false };
        std::optional<juce::Result> copied; // message thread
    };

    const std::shared_ptr<State> state = std::make_shared<State>();

    ~SlowCopy() { state->letGo = true; }

    bool started() const { return state->started.load(); }
    void letGo() { state->letGo = true; }
    const std::optional<juce::Result>& copied() const { return state->copied; }

    void start (phazite::ProjectContext& context)
    {
        phazite::makeVersionsCopier (context).copy (
            [shared = state] (const auto&)
            {
                shared->started = true;

                while (! shared->letGo.load())
                    juce::Thread::sleep (1);

                return juce::Result::ok();
            },
            [shared = state] (const juce::Result& result) { shared->copied = result; });
    }
};
} // namespace

TEST_CASE ("Merge waits while a first save copies the versions: refused, saying why, and nothing is written into them meanwhile")
{
    // A reviewer, of known bug 169's fix: Merge wrote the song before it into the store on the message thread while the
    // folder was being copied, and the commit it waits for was lost with the folder the project left.
    TestSong s;
    auto& context = s.context;
    const auto bass = s.addTrack ("Bass");
    keepVersion (context, "First");
    REQUIRE (phazite::newBranch (context, {}, s.save));
    s.setVolume (bass, -12.0f);
    const auto theirs = keepVersion (context, "Quieter");
    REQUIRE (phazite::switchBranch (context, findBranch (context, "main"), s.save));
    REQUIRE (waitForVersionHistory (context));
    s.addTrack ("Lead"); // not committed: the song before the merge, which Abort Merge needs, is written into the store

    REQUIRE (phazite::startMerge (context, theirs));
    REQUIRE_FALSE (phazite::getMergeLines (context).empty());
    REQUIRE (phazite::canFinishMerge (context));

    auto* history = phazite::getVersionHistory (context);
    SlowCopy copy;
    copy.start (context);
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.started(); }));
    CHECK (phazite::isInHeldFolder (phazite::getVersionsFolder (*phazite::findSong (context)).getChildFile ("current")));

    // Greyed, and refused if run all the same: the list stays, and the view says why.
    CHECK (phazite::isSavingProjectFolder (context));
    CHECK_FALSE (phazite::canFinishMerge (context));
    CHECK_FALSE (s.commands.invoke ("version.finishMerge"));
    phazite::finishTickedMerge (context);
    CHECK_FALSE (phazite::isMerging (context));
    CHECK_FALSE (phazite::getMergeLines (context).empty());
    CHECK (phazite::getMergeProblem (context).contains ("saving the project"));

    // Once the copy is over, Merge takes the lines.
    copy.letGo();
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.copied().has_value(); }));
    CHECK (copy.copied()->wasOk());
    history->endFolderCopy();
    CHECK_FALSE (phazite::isInHeldFolder (phazite::getVersionsFolder (*phazite::findSong (context)).getChildFile ("current")));
    REQUIRE (waitForVersionHistory (context));
    CHECK (phazite::canFinishMerge (context));
    phazite::finishTickedMerge (context);
    CHECK (phazite::isMerging (context));
}

TEST_CASE ("What waited for a first save's copy starts after it, and not when the project closes first")
{
    // A reviewer, of known bug 169's fix: as the copy ended, the history started the snapshot (and the initial commit) that
    // had waited for it into the folder of a project closing, which was deleted moments later.
    TestSong s;
    auto& context = s.context;
    const auto versions = phazite::getVersionsFolder (*phazite::findSong (context));

    // This computer's Local History, as its file holds it.
    const auto readLocalHistory = [&versions]
    {
        juce::String text;

        for (const auto& file : versions.getChildFile ("local").findChildFiles (juce::File::findFiles, true, "history"))
            text << file.loadFileAsString();

        return text;
    };

    // A first save's copy, and a snapshot as the project opens, which waits for it; nothing started as it ends.
    const auto copyAndEnd = [&context]
    {
        auto* history = phazite::getVersionHistory (context);
        SlowCopy copy;
        copy.letGo();
        copy.start (context);
        REQUIRE (dispatchMessagesUntil ([&copy] { return copy.copied().has_value(); }));
        REQUIRE (waitForVersionHistory (context));
        history->takeSnapshot ("open");
        history->endFolderCopy();
        CHECK_FALSE (history->isBusy());
    };

    // Closed before the next turn of the message loop: nothing is written, at that turn or after.
    s.addTrack ("Bass"); // a change since the last commit, which the snapshot keeps
    const auto before = readLocalHistory();
    copyAndEnd();
    phazite::setSong (context, nullptr);
    bool turned = false;
    juce::MessageManager::callAsync ([&turned] { turned = true; });
    REQUIRE (dispatchMessagesUntil ([&turned] { return turned; }));
    CHECK (readLocalHistory() == before);

    // Left open: the snapshot is taken then.
    phazite::setSong (context, &s.getProject().getSong());
    REQUIRE (waitForVersionHistory (context));
    s.addTrack ("Drums"); // the song otherwise than the snapshot taken as it opened again
    const auto taken = phazite::getVersionHistory (context)->getSnapshots().size();
    copyAndEnd();
    REQUIRE (dispatchMessagesUntil ([&context, taken] { return phazite::getVersionHistory (context)->getSnapshots().size() > taken; },
                                    versionHistoryTimeout));
    CHECK (phazite::getVersionHistory (context)->getSnapshots().front().reason == "open");
}

TEST_CASE ("A first save's copy that waits behind the history's writes as the history goes is told it didn't copy")
{
    // A reviewer, of known bug 169's fix: the history going (setSong()) dropped the copy waiting in its thread's queue,
    // which was never told, so that the save's progress window never finished. Now its thread, with the commit it writes,
    // is handed on as it goes (FinishingHistories), and the copy behind the commit runs there: told it didn't copy, and
    // copying nothing for a project that went.
    TestSong s;
    auto& context = s.context;
    std::atomic<bool> holding { true };
    const phazite::WholeFileHook held (
        [&holding] (const juce::File& file)
        {
            while (holding.load() && file.getFullPathName().contains ("Versions"))
                juce::Thread::sleep (1);

            return true;
        });

    // A commit being written, held, and the copy waiting behind it.
    s.addTrack ("Bass");
    REQUIRE (phazite::saveVersion (context));
    SlowCopy copy;
    copy.letGo();
    copy.start (context);

    // The history goes, waiting for neither; then the commit is let go of, and finished.
    phazite::setSong (context, nullptr);
    holding = false;
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.copied().has_value(); }));
    CHECK (copy.copied()->failed());
    CHECK_FALSE (copy.started());
    phazite::waitForVersionsFinished();
    phazite::setSong (context, &s.getProject().getSong());
}

TEST_CASE ("A first save's copy waits for what a history of the same folder that went before still writes into it")
{
    // The branch's FinishingHistories, met: a history that closed with a commit to write finishes it on its own thread,
    // into the folder the next history copies. The copy holds the folder (WholeFile.h: holdFolder()) only once those
    // writes are done, so that none of them is lost with the folder the project leaves, nor asserts as it is written.
    TestSong s;
    auto& context = s.context;
    std::atomic<bool> holding { true };
    const phazite::WholeFileHook held (
        [&holding] (const juce::File& file)
        {
            while (holding.load() && file.getFullPathName().contains ("Versions"))
                juce::Thread::sleep (1);

            return true;
        });

    s.addTrack ("Bass");
    REQUIRE (phazite::saveVersion (context));
    const auto before = phazite::getVersions (context).size();
    phazite::setSong (context, nullptr); // its commit finishes on the thread handed on
    phazite::setSong (context, &s.getProject().getSong());
    REQUIRE (phazite::countVersionsBeingFinished() > 0);

    SlowCopy copy;
    copy.start (context);
    holding = false;
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.started(); }));
    CHECK (phazite::countVersionsBeingFinished() == 0); // written before the copy began
    copy.letGo();
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.copied().has_value(); }));
    CHECK (copy.copied()->wasOk());
    phazite::getVersionHistory (context)->endFolderCopy();
    REQUIRE (waitForVersionHistory (context));
    CHECK (phazite::getVersions (context).size() == before + 1); // the commit the closed history finished
}
TEST_CASE ("A bounce of an old commit that a first save's copy of the versions stops starts again after it")
{
    // A reviewer, of known bug 169's fix: the copy stopped a bounce under way without a word, and it never came.
    TestSong s;
    auto& context = s.context;
    const auto track = s.addTrack ("Tone");
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Tone.wav"), 90.0), 0.0);
    const auto old = keepVersion (context, "Old");
    s.setVolume (track, -6.0f);
    keepVersion (context, "New");
    REQUIRE (phazite::startBackgroundBounce (context, old));

    auto* history = phazite::getVersionHistory (context);
    SlowCopy copy;
    copy.start (context);
    CHECK_FALSE (phazite::getBounceProgress (context, old).has_value()); // stopped
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.started(); }));
    copy.letGo();
    REQUIRE (dispatchMessagesUntil ([&copy] { return copy.copied().has_value(); }));
    history->endFolderCopy();
    REQUIRE (dispatchMessagesUntil ([&context, &old] { return phazite::getBounceProgress (context, old).has_value(); }));
}
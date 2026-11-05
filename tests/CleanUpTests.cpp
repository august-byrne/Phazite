// SPDX-License-Identifier: AGPL-3.0-or-later
// Clean Up Unused Audio... (model/project/UnusedAudio.h and its window, ui/shell/CleanUpPanel.h): the audio files in a
// project's Audio and Generated folders that neither its song, nor a version, nor a branch's song as it was left plays, and
// those ticked deleted.
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "ui/shell/CleanUpPanel.h"

namespace
{
using namespace phazite::test;

void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isBusy();
        }));
}

void keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    waitForVersions (context);
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }));
    REQUIRE (result->wasOk());
    waitForVersions (context);
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

juce::StringArray getNames (const std::vector<phazite::UnusedAudioFile>& files)
{
    juce::StringArray names;

    for (const auto& file : files)
        names.add (file.name);

    return names;
}

// A song that plays one file, a version that plays another, a branch left with a third; and two files nothing plays, in
// Audio and in Generated, beside a file that isn't audio.
struct Files
{
    TestSong s;
    TemporaryFolder sounds;
    phazite::TrackID track = s.addTrack ("Audio");
    const phazite::OpenSong open = [this] (const juce::ValueTree& song)
    {
        s.openSong (song);
        waitForVersions (s.context);
    };

    Files()
    {
        s.addAudioClip (track, writeSound (sounds.folder.getChildFile ("Played.wav"), 0.5), 0.0);
        const auto kept = s.addAudioClip (track, writeSound (sounds.folder.getChildFile ("Kept.wav"), 0.5), 4.0);
        keepVersion (s.context, "Both");

        REQUIRE (phazite::newBranch (s.context, {}, open));
        s.addAudioClip (s.firstTrack, writeSound (sounds.folder.getChildFile ("Tried.wav"), 0.5), 8.0);
        REQUIRE (phazite::switchBranch (s.context, findBranch (s.context, "Main"), open)); // Tried: on the branch, as it was left

        s.select (kept);
        s.run ("edit.delete"); // Kept: in the version alone

        writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
        writeSound (s.getProject().getGeneratedFolder().getChildFile ("Leftover.wav"), 0.5);
        s.getProject().getAudioFolder().getChildFile ("Notes.txt").replaceWithText ("not audio");
    }
};
} // namespace

TEST_CASE ("Clean Up lists the audio nothing plays: not the song's, nor a version's, nor a branch's as it was left")
{
    Files p;
    REQUIRE (phazite::canCleanUpAudio (p.s.context));
    const auto unused = phazite::findUnusedAudio (p.s.context);
    CHECK (getNames (unused) == juce::StringArray ("Audio/Old take.wav", "Generated/Leftover.wav"));
    CHECK (unused[0].size == unused[0].file.getSize());

    // Those given that are still unused go; one the song plays stays, even if asked for.
    const auto played = p.s.getProject().getAudioFolder().getChildFile ("Played.wav");
    REQUIRE (played.existsAsFile());
    CHECK (phazite::deleteUnusedAudio (p.s.context, { unused[0].file, played }, phazite::Removal::outright) == 1);
    CHECK_FALSE (unused[0].file.existsAsFile());
    CHECK (unused[1].file.existsAsFile());
    CHECK (played.existsAsFile());
    CHECK (getNames (phazite::findUnusedAudio (p.s.context)) == juce::StringArray ("Generated/Leftover.wav"));
}

TEST_CASE ("The Clean Up window lists the files ticked, a click keeps one, and Delete deletes the others and says so")
{
    Files p;
    phazite::ui::CleanUpPanel panel (p.s.context, phazite::Removal::outright);
    TestWindow window (panel, phazite::ui::CleanUpPanel::width, phazite::ui::CleanUpPanel::height);
    REQUIRE (panel.getNumFiles() == 2);
    CHECK (panel.describeFile (0).startsWith ("Audio/Old take.wav, "));
    CHECK (panel.describeFile (0).endsWith (", to delete"));
    CHECK (panel.getSummary().startsWith ("2 of 2 ticked"));

    // A click on the second keeps it.
    auto& list = panel.getList();
    Mouse mouse (list);
    mouse.click (list.getRowPosition (1, true).getCentre());
    dispatchPendingMessages();
    CHECK_FALSE (panel.isTicked (1));
    CHECK (panel.describeFile (1).endsWith (", kept"));
    CHECK (panel.getSummary().startsWith ("1 of 2 ticked"));

    Mouse buttons (panel);
    buttons.click (panel.getDeleteButton().getBounds().getCentre());
    dispatchPendingMessages();
    CHECK (panel.getSummary().startsWith ("Deleted 1 file."));
    CHECK (panel.getNumFiles() == 1);
    CHECK (panel.describeFile (0).startsWith ("Generated/Leftover.wav"));
    CHECK (panel.getCancelButton().getButtonText() == "Close");
    CHECK_FALSE (p.s.getProject().getAudioFolder().getChildFile ("Old take.wav").existsAsFile());
}

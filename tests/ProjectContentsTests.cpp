// SPDX-License-Identifier: AGPL-3.0-or-later
// What the open project holds, as the Project view shows it (src/model/project/ProjectContents.h): its notes, the audio files
// its song plays and its plug-ins, by the project's context.
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/base/internal/Schema.h"
#include "model/commands/SongGesture.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

TEST_CASE ("The project's notes are the song's, each change an undo step, a gesture's changes one, and kept in its file")
{
    phazite::test::TestSong test;
    auto& song = *findSong (test.context);
    auto& undo = song.getUndoManager();
    CHECK (phazite::getProjectNotes (test.context).isEmpty());

    phazite::setProjectNotes (test.context, "Verse needs a lift");
    CHECK (phazite::getProjectNotes (test.context) == juce::String ("Verse needs a lift"));
    CHECK (undo.getUndoDescription() == juce::String ("Change Notes"));
    CHECK (test.getProject().hasUnsavedChanges());

    // A burst of typing, as the Project view makes one: one step.
    {
        phazite::SongGesture typing (test.context, "Change Notes");
        phazite::setProjectNotes (test.context, "Verse needs a lift\nT");
        phazite::setProjectNotes (test.context, "Verse needs a lift\nTry a key change");
    }

    phazite::undo (song);
    CHECK (phazite::getProjectNotes (test.context) == juce::String ("Verse needs a lift"));
    phazite::undo (song);
    CHECK (phazite::getProjectNotes (test.context).isEmpty());
    CHECK_FALSE (song.getState().hasProperty (phazite::schema::notes)); // none written while there are none

    // In the project's file.
    phazite::setProjectNotes (test.context, "Lyrics:\nfirst line");
    test.reopen();
    CHECK (phazite::getProjectNotes (test.context) == juce::String ("Lyrics:\nfirst line"));
}

TEST_CASE ("The project's audio files: each once, with the tracks that play it; a missing one so marked, and found again")
{
    phazite::test::TestSong test;
    const auto sounds = test.getSoundsFolder();
    const auto kick = phazite::test::writeSound (sounds.getChildFile ("Kick.wav"), 0.5);
    const auto loop = phazite::test::writeSound (sounds.getChildFile ("Loop.wav"), 2.0);
    const auto drums = test.firstTrack;
    test.rename (drums, "Drums");
    const auto bass = test.addTrack ("Bass");
    test.addAudioClip (drums, kick, 0.0);
    test.addAudioClip (drums, kick, 4.0);
    test.addAudioClip (bass, kick, 8.0);
    test.addAudioClip (bass, loop, 0.0);

    // The project's copies, in its Audio folder, in the order the tracks play them first.
    auto files = phazite::getProjectAudioFiles (test.context);
    REQUIRE (files.size() == 2);
    CHECK (files[0].path == juce::String ("Audio/Kick.wav"));
    CHECK (files[0].channels == juce::StringArray { "Drums", "Bass" });
    CHECK (files[0].uses == 3);
    CHECK_FALSE (files[0].missing);
    CHECK (files[1].path == juce::String ("Audio/Loop.wav"));
    CHECK (files[1].channels == juce::StringArray { "Bass" });
    CHECK (files[1].file.existsAsFile());

    // The copy gone: missing, as a check of the disk finds it in the background (none without one).
    REQUIRE (files[1].file.deleteFile());
    CHECK_FALSE (phazite::getProjectAudioFiles (test.context)[1].missing);
    phazite::MissingAudioCheck check;
    check.check ({ files[0].file, files[1].file });
    CHECK (check.isChecking());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&check] { return ! check.isChecking(); }));
    CHECK (check.getMissing() == std::set<juce::File> { files[1].file });
    files = phazite::getProjectAudioFiles (test.context, check.getMissing());
    CHECK_FALSE (files[0].missing);
    CHECK (files[1].missing);

    // Found again elsewhere: the project's copy of what was found plays in its place, as one undo step.
    auto& song = *findSong (test.context);
    const auto found = phazite::test::writeSound (sounds.getChildFile ("Found/Loop take 2.wav"), 2.0);
    CHECK (phazite::locateAudioFile (test.context, "Audio/Loop.wav", found));
    phazite::test::waitForAudioWork (test.context); // copied into the project in the background (known bug 123)
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Locate Audio File"));
    files = phazite::getProjectAudioFiles (test.context, check.getMissing());
    REQUIRE (files.size() == 2);
    CHECK (files[1].path == juce::String ("Audio/Loop take 2.wav"));
    CHECK_FALSE (files[1].missing);
    CHECK (files[1].uses == 1);

    phazite::undo (song);
    CHECK (phazite::getProjectAudioFiles (test.context)[1].path == juce::String ("Audio/Loop.wav"));

    // A path the song doesn't play, or a file that isn't there, changes nothing.
    CHECK_FALSE (phazite::locateAudioFile (test.context, "Audio/Nothing.wav", found));
    CHECK_FALSE (phazite::locateAudioFile (test.context, "Audio/Loop.wav", sounds.getChildFile ("Gone.wav")));
}

TEST_CASE ("One check of the missing files at a time: those asked for while one is held start one more, the newest")
{
    // The audit's review, 2026-11-20: each edit started one more check, and with a file on a share that had stopped
    // answering, every new check stuck on that same file.
    phazite::test::TemporaryFolder temp;
    const auto there = phazite::test::writeSound (temp.folder.getChildFile ("There.wav"), 0.1);
    const auto gone = temp.folder.getChildFile ("Gone.wav");

    const auto going = std::make_shared<std::atomic<bool>> (false);
    const auto held = std::make_shared<std::atomic<int>> (0);
    const juce::ScopeGuard letGo { [going] { *going = true; } };
    phazite::MissingAudioCheck check;
    check.setBeforeChecking (
        [going, held] (const juce::File&)
        {
            ++*held;
            const auto since = juce::Time::getMillisecondCounterHiRes();

            while (! going->load() && juce::Time::getMillisecondCounterHiRes() - since < phazite::test::hangTimeout)
                juce::Thread::sleep (1);
        });

    check.check ({ there });
    REQUIRE (phazite::test::dispatchMessagesUntil ([held] { return held->load() == 1; }));

    for (int i = 0; i < 5; ++i)
        check.check ({ there, gone });

    phazite::test::dispatchPendingMessages();
    CHECK (check.getChecksStarted() == 1);
    CHECK (check.isChecking());

    // Let go: the one held stops, and the newest runs, once.
    *going = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&check] { return ! check.isChecking(); }));
    CHECK (check.getChecksStarted() == 2);
    CHECK (check.getMissing() == std::set<juce::File> { gone });
    CHECK (phazite::test::dispatchMessagesUntil ([] { return phazite::MissingAudioCheck::getChecksUnderWay() == 0; }));
}

TEST_CASE ("The project's plug-ins: each on its channel and as it plays; one the plug-in host doesn't know is missing")
{
    phazite::test::TestPlugin plugin; // the host knows it, before the songs that hold it

    {
        phazite::test::TestSong test;
        auto& song = *findSong (test.context);
        test.rename (test.firstTrack, "Lead");
        phazite::DeviceID device;

        {
            const phazite::UndoStep step (song, "Add Device");
            device = phazite::test::TestPlugin::add (song, test.firstTrack);
        }

        const auto plugins = phazite::getProjectPlugins (test.context);
        REQUIRE (plugins.size() == 1);
        CHECK (plugins[0].device == device);
        CHECK (plugins[0].channel == test.firstTrack);
        CHECK (plugins[0].channelName == juce::String ("Lead"));
        CHECK (plugins[0].name == juce::String ("Test Effect"));
        CHECK (plugins[0].maker == juce::String ("Phazite Tests"));
        CHECK_FALSE (plugins[0].isInstrument);
        CHECK (plugins[0].state == phazite::PluginState::playing);
    }
}

TEST_CASE ("A plug-in that isn't installed shows as missing among the project's plug-ins, and Phazite's own devices don't show")
{
    phazite::test::TestSong test; // no stand-in alive: the host doesn't know the plug-in
    auto& song = *findSong (test.context);
    test.setInstrument (test.firstTrack, "phaziteSynth");

    {
        const phazite::UndoStep step (song, "Add Device");
        phazite::test::TestPlugin::add (song, test.firstTrack);
    }

    const auto plugins = phazite::getProjectPlugins (test.context);
    REQUIRE (plugins.size() == 1);
    CHECK (plugins[0].state == phazite::PluginState::missing);
}

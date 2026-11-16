// SPDX-License-Identifier: AGPL-3.0-or-later
// The work of the commands that read much of an audio file (src/model/arrangement/internal/AudioWork.h; known bug 123):
// an import, Normalize and Quantize Audio read in the background, the window usable meanwhile, and make their undo steps
// once what they read is in; nothing changes, and the user is told, when the song no longer fits what they read; a job
// shows its progress window after a while, whose Cancel stops it; and a job's step never joins a drag's.
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/InputHold.h"
#include "devices/drums/DrumSampler.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/arrangement/internal/Transients.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/SwapModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/project/internal/UnsavedProjects.h"

using phazite::test::at;

namespace
{
phazite::Song& getSong (phazite::test::TestSong& s)
{
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    return *song;
}

// What the model tells the user, as the app would show it.
struct Problems
{
    explicit Problems (phazite::ProjectContext& c)
        : context (c)
    {
        phazite::setProblemReporter (context,
                                     [this] (const juce::String& title, const juce::String& message)
                                     {
                                         told.add (title + ": " + message);
                                         ++count;
                                     });
    }

    ~Problems() { phazite::setProblemReporter (context, nullptr); }

    phazite::ProjectContext& context;
    juce::StringArray told;
    std::atomic<int> count { 0 }; // as told, for another thread to look at
};

// A job of the test's own: it reads until it is let go (or stopped), and says what came of it.
struct HeldJob final : phazite::AudioWork::Job
{
    struct Outcome
    {
        std::atomic<bool> letGo { false }, readDone { false };
        bool finished = false, discarded = false;
    };

    explicit HeldJob (std::shared_ptr<Outcome> o)
        : outcome (std::move (o))
    {
    }

    juce::String getTitle() const override { return "Holding"; }

    void read (const phazite::BackgroundJobs::ShouldStop& shouldStop, std::atomic<float>& progress) override
    {
        progress = 0.5f;

        while (! outcome->letGo && ! shouldStop())
            juce::Thread::sleep (1);

        outcome->readDone = true;
    }

    void finish (phazite::ProjectContext& context) override
    {
        auto& song = *phazite::findSong (context);
        const phazite::UndoStep step (song, "Held Change");
        phazite::setTrackName (song, phazite::getTrackIDs (song).front(), "Changed");
        outcome->finished = true;
    }

    void discard (phazite::ProjectContext&) override { outcome->discarded = true; }
    void abandon (phazite::ProjectContext&) override { outcome->discarded = true; } // given up, its read not waited for

    const std::shared_ptr<Outcome> outcome;
};
} // namespace

TEST_CASE ("A dropped file is read in the background, and its clip and its one undo step come once it is in the project")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto take = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 2.0);

    // A file of the same name in the project already, the same: imported before, so no copy is made.
    const auto audioFolder = s.getProject().getAudioFolder();
    REQUIRE (audioFolder.createDirectory());
    REQUIRE (take.copyFileTo (audioFolder.getChildFile ("Take.wav")));
    song.getUndoManager().clearUndoHistory();

    std::vector<phazite::ClipID> added;
    REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, s.firstTrack, at (4.0),
                                               [&added] (const std::vector<phazite::ClipID>& clips) { added = clips; }));

    // Nothing in the call: the message thread doesn't read it (known bug 123).
    CHECK (phazite::isReadingAudio (s.context));
    CHECK (phazite::getClips (song, s.firstTrack).empty());
    CHECK_FALSE (phazite::canUndo (song));

    phazite::test::waitForAudioWork (s.context);
    const auto clips = phazite::getClips (song, s.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (added == clips);
    CHECK (phazite::getClipRange (song, clips[0]).getStart().inBeats() == doctest::Approx (4.0));
    CHECK (phazite::getClipFile (song, phazite::findClipState (song, clips[0])) == audioFolder.getChildFile ("Take.wav"));
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Add File"));
    CHECK (audioFolder.findChildFiles (juce::File::findFiles, false).size() == 1);

    // Another sound of the same name is copied in under a name of its own.
    const auto other = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Other/Take.wav"), 1.0, 440.0);
    REQUIRE (phazite::importFilesInBackground (s.context, { other.getFullPathName() }, s.firstTrack, at (16.0)));
    phazite::test::waitForAudioWork (s.context);
    CHECK (phazite::getClips (song, s.firstTrack).size() == 2);
    CHECK (audioFolder.findChildFiles (juce::File::findFiles, false).size() == 2);

    // Files that can't be imported start nothing.
    const auto text = s.getSoundsFolder().getChildFile ("Notes.txt");
    REQUIRE (text.replaceWithText ("not a sound"));
    CHECK_FALSE (phazite::importFilesInBackground (s.context, { text.getFullPathName() }, s.firstTrack, at (0.0)));
}

TEST_CASE ("When the song changes so that what was read no longer fits, nothing changes and the user is told")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    Problems problems (s.context);
    const auto take = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 2.0);

    SUBCASE ("An import whose track goes meanwhile, its copy deleted")
    {
        const auto track = s.addTrack ("Gone");
        REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, track, at (0.0)));
        phazite::deleteTracks (song, { track }, nullptr);
        const auto steps = phazite::describeUndo (&song);
        phazite::test::waitForAudioWork (s.context);

        CHECK (phazite::describeUndo (&song) == steps);
        CHECK (phazite::getTrackIDs (song).size() == 1);
        CHECK_FALSE (s.getProject().getAudioFolder().getChildFile ("Take.wav").exists());
        REQUIRE (problems.told.size() == 1);
        CHECK (problems.told[0].startsWith ("Nothing was imported: The track Take.wav was dropped on went"));
    }

    SUBCASE ("Normalize of a clip trimmed meanwhile")
    {
        const auto clip = s.addAudioClip (s.firstTrack, take, 0.0);
        song.getUndoManager().clearUndoHistory();
        phazite::normalizeClip (s.context, clip);
        phazite::trimClipStart (s.context, clip, at (1.0));
        phazite::test::waitForAudioWork (s.context);

        CHECK (phazite::getAudioClip (s.context, clip)->gain == doctest::Approx (0.0f));
        CHECK (phazite::describeUndo (&song) != juce::String ("Undo Normalize Clip"));
        REQUIRE (problems.told.size() == 1);
        CHECK (problems.told[0].startsWith ("Nothing was normalized"));
    }

    SUBCASE ("Quantize Audio of a clip deleted meanwhile")
    {
        const auto clip = s.addAudioClip (s.firstTrack, phazite::test::writeClicks (s.getSoundsFolder(), 4, 120.0), 0.0);
        phazite::quantizeAudio (s.context, { clip }, phazite::test::lasting (1.0));
        phazite::deleteClips (song, { clip }, false);
        const auto steps = phazite::describeUndo (&song);
        phazite::test::waitForAudioWork (s.context);

        CHECK (phazite::describeUndo (&song) == steps);
        REQUIRE (problems.told.size() == 1);
        CHECK (problems.told[0].startsWith ("Nothing was quantized"));
    }
}

TEST_CASE ("Work that reads audio for one project changes nothing once another is open, and says so")
{
    const auto first = phazite::createSong();
    const auto second = phazite::createSong();
    phazite::ProjectContext context (phazite::test::getEngineContext());
    Problems problems (context);
    phazite::test::TemporaryFolder temp;
    setSong (context, first.get());
    const auto track = phazite::getTrackIDs (*first).front();
    const auto clips = phazite::importFiles (
        context, { phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 1.0).getFullPathName() }, track, at (0.0));
    REQUIRE (clips.size() == 1);

    // A read that would go on until it is stopped: the project changing stops it at once, and nothing more is read for it.
    auto& work = phazite::getAudioWork (context);
    std::atomic<int> steps { 0 };
    work.setOnReadStep (
        [&steps] (int, const phazite::BackgroundJobs::ShouldStop& stop)
        {
            ++steps;

            while (! stop())
                juce::Thread::sleep (1);
        });

    phazite::normalizeClip (context, clips[0]);
    REQUIRE (phazite::test::runTimersUntil ([&steps] { return steps > 0; }));
    setSong (context, second.get());
    CHECK_FALSE (phazite::isReadingAudio (context)); // given up in the change itself, its read not waited for
    REQUIRE (phazite::test::runTimersUntil ([&work] { return work.countReadsGivenUp() == 0; })); // it stops as asked
    work.setOnReadStep (nullptr);

    CHECK (phazite::getClipGain (*first, clips[0]) == doctest::Approx (0.0f));
    REQUIRE (problems.told.size() == 1);
    CHECK (problems.told[0]
           == juce::String ("Normalizing Tone stopped: Another project was opened before it was done, so nothing was changed."));
    setSong (context, nullptr);
}

TEST_CASE ("A job that reads for long shows its progress window, whose Cancel stops it and changes nothing")
{
    phazite::test::TestSong s;
    auto& work = phazite::getAudioWork (s.context);
    phazite::test::RenderHost window;
    work.setHost (&window);
    work.setShowAfter (0);

    // Cancelled: the window goes while it reads.
    auto cancelled = std::make_shared<HeldJob::Outcome>();
    work.add (std::make_unique<HeldJob> (cancelled));
    REQUIRE (phazite::test::runTimersUntil ([&window] { return window.render != nullptr; }));
    CHECK (window.render->getTitle() == juce::String ("Holding"));
    // As its read says, once its thread has begun (the window may show before it, at 0).
    CHECK (phazite::test::runTimersUntil ([&window] { return window.render->getProgress() == 0.5f; }));
    window.render->cancel(); // as its Cancel does, before its window deletes it
    window.render = nullptr;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    CHECK (cancelled->discarded);
    CHECK_FALSE (cancelled->finished);
    CHECK (window.finished.isEmpty());
    CHECK (phazite::getTrackName (getSong (s), s.firstTrack) != juce::String ("Changed"));

    // Its window gone without its Cancel (another render closed it): it reads on, unseen no longer than its next turn to
    // show, and changes what it was to.
    auto closed = std::make_shared<HeldJob::Outcome>();
    work.add (std::make_unique<HeldJob> (closed));
    REQUIRE (phazite::test::runTimersUntil ([&window] { return window.render != nullptr; }));
    window.render = nullptr;
    REQUIRE (phazite::test::runTimersUntil ([&window] { return window.render != nullptr; })); // shown again
    CHECK_FALSE (closed->discarded);
    closed->letGo = true;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    CHECK (closed->finished);
    window.finished.clear();

    // Done while it shows: the window closes, and the change is made.
    auto done = std::make_shared<HeldJob::Outcome>();
    work.add (std::make_unique<HeldJob> (done));
    REQUIRE (phazite::test::runTimersUntil ([&window] { return window.render != nullptr; }));
    done->letGo = true;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    CHECK (done->finished);
    CHECK (window.render == nullptr);
    CHECK (window.finished == juce::StringArray { "Holding: ok" });

    // The cancelled one's read, given up, returned too: no thread of this work is left to the next case.
    REQUIRE (phazite::test::runTimersUntil ([&work] { return work.countReadsGivenUp() == 0; }));
    work.setHost (nullptr);
    work.setShowAfter (phazite::AudioWork::showAfterMilliseconds);
}

TEST_CASE ("A job's change waits for a drag in progress to end, and is an undo step of its own")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    auto& work = phazite::getAudioWork (s.context);
    const auto other = s.addTrack ("Other");
    song.getUndoManager().clearUndoHistory();

    phazite::Gesture drag (song, "Drag");
    phazite::setTrackName (song, other, "Dragged");

    auto outcome = std::make_shared<HeldJob::Outcome>();
    outcome->letGo = true;
    work.add (std::make_unique<HeldJob> (outcome));
    REQUIRE (phazite::test::runTimersUntil ([&work] { return work.isWaitingForGesture(); }));
    CHECK_FALSE (outcome->finished);

    drag.commit();
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    CHECK (outcome->finished);
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Held Change"));
    phazite::undo (song);
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Drag"));
    CHECK (phazite::getTrackName (song, other) == juce::String ("Dragged"));
}

TEST_CASE ("While input is held, keys reach no window; once it ends, each window hears its keys changed, so a key let go is let go")
{
    // What listens to the window's keys: how often it heard that they changed, and whether a key went down.
    struct Keys final : juce::KeyListener
    {
        bool keyPressed (const juce::KeyPress&, juce::Component*) override
        {
            ++pressed;
            return true;
        }

        bool keyStateChanged (bool isKeyDown, juce::Component*) override
        {
            ++(isKeyDown ? down : up);
            return false;
        }

        int pressed = 0, down = 0, up = 0;
    } keys;

    juce::Component content;
    phazite::test::TestWindow window (content, 200, 100);
    window.addKeyListener (&keys);

    phazite::InputHold hold;
    hold.holdInput();
    CHECK (phazite::InputHold::isHold (juce::Component::getCurrentlyModalComponent()));

    // A key pressed and let go meanwhile: nothing hears either.
    window.pressKey (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
    window.releaseKeys();
    CHECK (keys.pressed == 0);
    CHECK (keys.up == 0);

    // As it ends, the window hears that its keys changed, and asks which are still down: none.
    hold.releaseInput();
    CHECK (juce::Component::getCurrentlyModalComponent() == nullptr);
    CHECK (keys.up == 1);
    window.pressKey (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
    CHECK (keys.pressed == 1);
    window.removeKeyListener (&keys);
}

TEST_CASE ("A job holds input from its start; a Swap's trial open then is kept first, and one opened meanwhile is waited for")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    auto& work = phazite::getAudioWork (s.context);

    // What holds input, as the app's does: from the job's start to its end.
    struct Holder final : phazite::InputHolder
    {
        void holdInput() override { ++holds; }
        void releaseInput() override { --holds; }
        int holds = 0;
    } holder;

    work.setInputHolder (&holder);
    const auto other = s.addTrack ("Other");
    song.getUndoManager().clearUndoHistory();

    // A trial open as it starts is kept first, as a step of its own, as an edit made at once would keep it.
    phazite::Trial open (song, "Try", nullptr);
    open.apply ([&song, other] { phazite::setTrackName (song, other, "Tried"); });

    auto outcome = std::make_shared<HeldJob::Outcome>();
    work.add (std::make_unique<HeldJob> (outcome));
    CHECK (holder.holds == 1);
    CHECK_FALSE (open.isOpen());

    // A trial opened while it reads (by something other than the user, who can't): its change waits until it ends.
    auto trial = std::make_unique<phazite::Trial> (song, "Swap Pad 1", nullptr);
    trial->apply ([&song, other] { phazite::setTrackName (song, other, "Swapped"); });
    outcome->letGo = true;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return work.isWaitingForGesture(); }));
    CHECK_FALSE (outcome->finished);
    CHECK (holder.holds == 1);

    trial->putBack();
    trial = nullptr;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    CHECK (outcome->finished);
    CHECK (holder.holds == 0);
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Held Change"));
    phazite::undo (song);
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Try"));
    work.setInputHolder (nullptr);
}

TEST_CASE ("A compressed sound loud only at its end after long silence is read to its end, and what a header that lies keeps is bounded")
{
    phazite::test::TemporaryFolder temp;

    // A minute of silence, then a loud tenth of a second: as FLAC, the silence takes a few bytes a block, far less than a
    // byte for 16 frames (where the reads stopped before).
    const auto file = temp.folder.getChildFile ("Mostly silence.flac");
    {
        constexpr double rate = 44100.0;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::FlacAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> audio (1, (int) rate);
        audio.clear();

        for (int second = 0; second < 60; ++second)
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());

        for (int i = 0; i < 4410; ++i)
            audio.setSample (0, i, 0.8f * (float) std::sin (0.2 * i));

        writer->writeFromAudioSampleBuffer (audio, 0, 4410);
    }

    REQUIRE (file.getSize() * 16 < (juce::int64) (60.0 * 44100.0));
    const auto peak = phazite::readPeak (file, { 0.0, 60.1 }, nullptr, nullptr).peak;
    REQUIRE (peak.has_value());
    CHECK (*peak == doctest::Approx (0.8f).epsilon (0.01));

    const auto hits = phazite::Transients::analyse (file);
    REQUIRE (hits.has_value());
    REQUIRE_FALSE (hits->empty());
    CHECK (hits->front() == doctest::Approx (60.0).epsilon (0.001)); // its first sound, past the silence

    // A WAV whose header says it holds billions of frames, of a second's sound: what is kept is that second.
    const auto liar = phazite::test::writeSound (temp.folder.getChildFile ("Liar.wav"), 1.0);
    {
        juce::MemoryBlock bytes;
        REQUIRE (liar.loadFileAsData (bytes));
        auto* data = static_cast<char*> (bytes.getData());
        const unsigned char huge[] = { 0xf0, 0xff, 0xff, 0x7f };

        for (size_t i = 12; i + 8 <= bytes.getSize(); ++i)
            if (std::memcmp (data + i, "data", 4) == 0)
                std::memcpy (data + i + 4, huge, 4);

        REQUIRE (liar.replaceWithData (bytes.getData(), bytes.getSize()));
    }

    juce::int64 kept = 0;
    REQUIRE (phazite::Transients::analyse (liar, nullptr, nullptr, &kept).has_value());
    CHECK (kept <= (juce::int64) (2.0 * 44100.0));
}

TEST_CASE ("A copy's name is kept for it as it is chosen, the next job reads after the one before has made its change, and what the "
           "song names is never deleted")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    auto& work = phazite::getAudioWork (s.context);
    const auto audioFolder = s.getProject().getAudioFolder();
    const auto take = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 30.0);

    // An import held as its copy is written: its name is there, kept, before the copy is in.
    std::atomic<bool> letGo { false };
    work.setOnReadStep (
        [&letGo, audioFolder] (int, const phazite::BackgroundJobs::ShouldStop& stop)
        {
            if (audioFolder.getChildFile ("Take.wav").existsAsFile())
                while (! letGo && ! stop())
                    juce::Thread::sleep (1);
        });

    REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, s.firstTrack, at (0.0)));
    REQUIRE (phazite::test::runTimersUntil ([&audioFolder] { return audioFolder.getChildFile ("Take.wav").existsAsFile(); }));

    // Meanwhile a sound of the same name copied in at once (a pad's, say) takes a name of its own.
    const auto padSound = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Pad/Take.wav"), 0.5, 880.0);
    const auto padCopy = phazite::copyIntoProject (song, padSound);
    CHECK (padCopy != audioFolder.getChildFile ("Take.wav"));
    CHECK (padCopy.getParentDirectory() == audioFolder);

    letGo = true;
    phazite::test::waitForAudioWork (s.context);
    work.setOnReadStep (nullptr);
    const auto clips = phazite::getClips (song, s.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (phazite::getClipFile (song, phazite::findClipState (song, clips[0])) == audioFolder.getChildFile ("Take.wav"));
    CHECK (audioFolder.getChildFile ("Take.wav").getSize() == take.getSize());
    CHECK (padCopy.getSize() == padSound.getSize()); // as it was written

    // Two imports of a new sound, the first refused at its end (its track went): the second reads only after the first has
    // made its change (refused, its copy deleted, the user told), copies it again, and its clip's file is there.
    const auto loop = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 2.0, 330.0);
    const auto gone = s.addTrack ("Gone");
    const auto kept = s.addTrack ("Kept");
    Problems problems (s.context);
    std::atomic<int> lastJob { 0 }, toldAtSecondsStart { -1 }; // what the user had been told as each job began to read
    work.setOnReadStep (
        [&problems, &lastJob, &toldAtSecondsStart] (int job, const phazite::BackgroundJobs::ShouldStop&)
        {
            if (job > lastJob.load())
            {
                lastJob = job;
                toldAtSecondsStart = problems.count.load();
            }
        });
    REQUIRE (phazite::importFilesInBackground (s.context, { loop.getFullPathName() }, gone, at (0.0)));
    REQUIRE (phazite::importFilesInBackground (s.context, { loop.getFullPathName() }, kept, at (0.0)));
    phazite::deleteTracks (song, { gone }, nullptr);
    phazite::test::waitForAudioWork (s.context);
    work.setOnReadStep (nullptr);
    CHECK (toldAtSecondsStart.load() == 1); // the first's refusal came before the second began to read
    const auto keptClips = phazite::getClips (song, kept);
    REQUIRE (keptClips.size() == 1);
    CHECK (phazite::getClipFile (song, phazite::findClipState (song, keptClips[0])).existsAsFile());

    // A copy a refused import wrote that the song names meanwhile (taken as its own by a sound copied in at once) stays.
    const auto beat = s.addTrack ("Beat");
    const auto refused = s.addTrack ("Refused");
    const auto kick = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"), 0.3, 50.0);
    {
        phazite::Gesture drag (song, "Drag"); // holds the import's change once it has read
        REQUIRE (phazite::importFilesInBackground (s.context, { kick.getFullPathName() }, refused, at (0.0)));
        REQUIRE (phazite::test::runTimersUntil ([&work] { return work.isWaitingForGesture(); }));
        REQUIRE (phazite::importFile (song, beat, kick, at (0.0)).isValid()); // which finds the copy made, and takes it
        phazite::deleteTracks (song, { refused }, nullptr);
        drag.commit();
    }

    phazite::test::waitForAudioWork (s.context);
    const auto beatClips = phazite::getClips (song, beat);
    REQUIRE (beatClips.size() == 1);
    CHECK (phazite::getClipFile (song, phazite::findClipState (song, beatClips[0])).existsAsFile());
}

TEST_CASE ("A read stuck in the system is given up: Cancel lets go of input at once, another project opens at once, and what it "
           "left goes once it returns")
{
    phazite::test::TestSong s;
    auto& work = phazite::getAudioWork (s.context);
    phazite::test::RenderHost window;
    work.setHost (&window);
    work.setShowAfter (0);

    struct Holder final : phazite::InputHolder
    {
        void holdInput() override { ++holds; }
        void releaseInput() override { --holds; }
        int holds = 0;
    } holder;

    work.setInputHolder (&holder);

    // An import's read that, once its copy's name is kept, never returns until the test lets it, as one from a network share
    // that went away: it doesn't ask whether it should stop.
    const auto audioFolder = s.getProject().getAudioFolder();
    const auto reserved = audioFolder.getChildFile ("Take.wav");
    std::atomic<bool> letGo { false };
    work.setOnReadStep (
        [&letGo, reserved] (int, const phazite::BackgroundJobs::ShouldStop&)
        {
            if (reserved.existsAsFile())
                while (! letGo)
                    juce::Thread::sleep (1);
        });

    const auto take = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 30.0);
    REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, s.firstTrack, at (0.0)));
    CHECK (holder.holds == 1);
    REQUIRE (phazite::test::runTimersUntil ([&window, &reserved] { return window.render != nullptr && reserved.existsAsFile(); }));

    // Cancel: the input is let go at once, the read still stuck.
    window.render->cancel();
    window.render = nullptr;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    CHECK (holder.holds == 0);
    CHECK (reserved.existsAsFile());
    CHECK (phazite::getClips (getSong (s), s.firstTrack).empty());

    // The project's folder isn't deleted while it may still write there; once let go, its kept name goes, and then the folder
    // may.
    auto deletable = false;
    phazite::AudioWork::whenNoneWritesInto (s.getProject().getFolder(), [&deletable] { deletable = true; });
    CHECK_FALSE (deletable);
    letGo = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&deletable, &reserved] { return deletable && ! reserved.exists(); }));
    work.setOnReadStep (nullptr);
    work.setInputHolder (nullptr);
    work.setHost (nullptr);
    work.setShowAfter (phazite::AudioWork::showAfterMilliseconds);
}

TEST_CASE ("Another project opens at once while a read is stuck in the system, and nothing waits for it")
{
    const auto first = phazite::createSong();
    const auto second = phazite::createSong();
    phazite::ProjectContext context (phazite::test::getEngineContext());
    phazite::test::TemporaryFolder temp;
    setSong (context, first.get());
    const auto clips =
        phazite::importFiles (context, { phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 1.0).getFullPathName() },
                              phazite::getTrackIDs (*first).front(), at (0.0));
    REQUIRE (clips.size() == 1);

    auto& work = phazite::getAudioWork (context);
    std::atomic<bool> stuck { false }, letGo { false };
    work.setOnReadStep (
        [&stuck, &letGo] (int, const phazite::BackgroundJobs::ShouldStop&)
        {
            stuck = true;

            while (! letGo)
                juce::Thread::sleep (1);
        });

    phazite::normalizeClip (context, clips[0]);
    REQUIRE (phazite::test::runTimersUntil ([&stuck] { return stuck.load(); }));
    setSong (context, second.get()); // returns, the read still stuck
    CHECK_FALSE (phazite::isReadingAudio (context));
    CHECK (phazite::getClipGain (*first, clips[0]) == doctest::Approx (0.0f));
    letGo = true;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return work.countReadsGivenUp() == 0; })); // returned, and gone
    work.setOnReadStep (nullptr);
    setSong (context, nullptr);
}

TEST_CASE ("A project's work that goes while a read it gave up is still under way keeps that read's thread only until it "
           "returns, then lets it go")
{
    const auto song = phazite::createSong();
    phazite::test::TemporaryFolder temp;
    std::atomic<bool> under { false }, letGo { false };
    REQUIRE (phazite::AudioWork::countThreadsLeftBehind() == 0);

    {
        phazite::ProjectContext context (phazite::test::getEngineContext());
        setSong (context, song.get());
        const auto clips =
            phazite::importFiles (context, { phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 1.0).getFullPathName() },
                                  phazite::getTrackIDs (*song).front(), at (0.0));
        REQUIRE (clips.size() == 1);

        // A read that returns only once let go, as one slowed by a loaded machine: its work goes before it returns, as a
        // test's context ends or the app's as Phazite quits.
        phazite::getAudioWork (context).setOnReadStep (
            [&under, &letGo] (int, const phazite::BackgroundJobs::ShouldStop&)
            {
                under = true;

                while (! letGo)
                    juce::Thread::sleep (1);
            });

        phazite::normalizeClip (context, clips[0]);
        REQUIRE (phazite::test::runTimersUntil ([&under] { return under.load(); }));
        setSong (context, nullptr);
    }

    CHECK (phazite::AudioWork::countThreadsLeftBehind() == 1); // not waited for
    letGo = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return phazite::AudioWork::countThreadsLeftBehind() == 0; }));
}

TEST_CASE ("A copy remembered from an import is taken again only while it is as it was written, not one written over since")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto sound = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Hit.wav"), 0.5, 200.0);
    const auto copy = phazite::copyIntoProject (song, sound);
    REQUIRE (copy != sound);
    CHECK (phazite::copyIntoProject (song, sound) == copy); // remembered: not read again

    // The copy written over with other audio of the same size, later: no longer the one remembered, nor the same sound.
    phazite::test::writeSound (copy, 0.5, 900.0);
    REQUIRE (copy.getSize() == sound.getSize());
    REQUIRE (copy.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (5.0)));
    const auto again = phazite::copyIntoProject (song, sound);
    CHECK (again != copy);
    CHECK (again.getParentDirectory() == copy.getParentDirectory());
}

TEST_CASE ("Quantize Audio of a file longer than the hits are looked for in says so")
{
    phazite::test::TestSong s;
    Problems problems (s.context);

    // Eleven minutes, mono at 8 kHz, a click each second.
    const auto file = s.getSoundsFolder().getChildFile ("Long take.wav");
    {
        file.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (8000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> second (1, 8000);
        second.clear();

        for (int i = 0; i < 40; ++i)
            second.setSample (0, i, 0.5f);

        for (int i = 0; i < 11 * 60; ++i)
            writer->writeFromAudioSampleBuffer (second, 0, 8000);
    }

    const auto clip = s.addAudioClip (s.firstTrack, file, 0.0);
    phazite::quantizeAudio (s.context, { clip }, phazite::test::lasting (1.0));
    phazite::test::waitForAudioWork (s.context);
    REQUIRE (problems.told.size() == 1);
    CHECK (problems.told[0].startsWith ("Quantize Audio quantized the first 10 minutes: Long take is longer than 10 minutes"));
}

TEST_CASE ("While a Swap's keep copies its sound in, no other swap opens for the song, and the sound is copied once")
{
    phazite::test::TestSong s;
    const auto drums = s.setInstrument (s.firstTrack, phazite::devices::DrumSampler::typeID);
    const auto kick = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Kick One.wav"), 0.3);
    const auto audioFolder = s.getProject().getAudioFolder();
    auto& song = getSong (s);

    auto first = std::make_unique<phazite::Swap> (s.context, drums, 36);
    REQUIRE (first->tryFile (kick));
    auto kept = false;
    REQUIRE (first->keep ([&kept] { kept = true; })); // its sound copied in first, in the background
    first = nullptr;

    // Meanwhile another pad's swap opens closed, and a change beginning elsewhere doesn't keep the first a second time.
    phazite::Swap second (s.context, drums, 38);
    CHECK_FALSE (second.isOpen());
    phazite::Trial::keepOpenTrial (song.getUndoManager());

    phazite::test::waitForAudioWork (s.context);
    CHECK (kept);
    CHECK (audioFolder.findChildFiles (juce::File::findFiles, false).size() == 1);
    CHECK (audioFolder.getChildFile ("Kick One.wav").getSize() == kick.getSize());
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Put Kick One on Pad 36"));

    phazite::Swap third (s.context, drums, 38);
    CHECK (third.isOpen());
}

TEST_CASE ("A job goes on the message thread, which owns it, even when its read's thread lets go of its task last")
{
    phazite::test::TestSong s;
    auto& work = phazite::getAudioWork (s.context);

    // Where it went: 1 on the message thread, 2 elsewhere.
    struct Tracked final : phazite::AudioWork::Job
    {
        explicit Tracked (std::shared_ptr<std::atomic<int>> w)
            : where (std::move (w))
        {
        }

        ~Tracked() override { *where = juce::MessageManager::existsAndIsCurrentThread() ? 1 : 2; }

        juce::String getTitle() const override { return "Tracked"; }
        void read (const phazite::BackgroundJobs::ShouldStop&, std::atomic<float>&) override {}
        void finish (phazite::ProjectContext&) override {}

        const std::shared_ptr<std::atomic<int>> where;
    };

    auto where = std::make_shared<std::atomic<int>> (0);
    std::atomic<bool> letGo { false };
    work.setAfterEachRead (
        [&where, &letGo]
        {
            while (where->load() == 0 && ! letGo)
                juce::Thread::sleep (1);
        });

    work.add (std::make_unique<Tracked> (where));
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));
    letGo = true;
    REQUIRE (phazite::test::runTimersUntil ([&where] { return where->load() != 0; }));
    work.setAfterEachRead (nullptr);
    CHECK (where->load() == 1);
}

TEST_CASE ("Jobs given up before their reads began are discarded at once, and the project's folder is free once the one that "
           "read returns")
{
    phazite::test::TestSong s;
    auto& work = phazite::getAudioWork (s.context);
    const auto take = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 2.0);
    const auto other = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Other.wav"), 2.0, 440.0);

    // The first reads until it is asked to stop; the second waits its turn behind it.
    std::atomic<bool> reading { false };
    work.setOnReadStep (
        [&reading] (int job, const phazite::BackgroundJobs::ShouldStop& stop)
        {
            if (job != 1)
                return;

            reading = true;

            while (! stop())
                juce::Thread::sleep (1);
        });

    const auto folder = s.getProject().getFolder();
    REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, s.firstTrack, at (0.0)));
    REQUIRE (phazite::importFilesInBackground (s.context, { other.getFullPathName() }, s.firstTrack, at (8.0)));
    REQUIRE (phazite::test::runTimersUntil ([&reading] { return reading.load(); }));

    // Another project: both given up at once; the one that never began is no read to wait for.
    phazite::getAudioWork (s.context).songChanged (nullptr);
    CHECK_FALSE (work.isBusy());
    CHECK (work.countReadsGivenUp() <= 1);

    auto deletable = false;
    phazite::AudioWork::whenNoneWritesInto (folder, [&deletable] { deletable = true; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&deletable, &work] { return deletable && work.countReadsGivenUp() == 0; }));
    work.setOnReadStep (nullptr);
    work.songChanged (phazite::findSong (s.context)); // as the context has it again
}

TEST_CASE ("A copy a given-up read made is no other job's to take: the next import of the same file makes its own")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);
    auto& work = phazite::getAudioWork (s.context);
    phazite::test::RenderHost window;
    work.setHost (&window);
    work.setShowAfter (0);
    const auto take = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 2.0);
    const auto copied = s.getProject().getAudioFolder().getChildFile ("Take.wav");

    // The first import held once its copy is whole, at its last look at whether it should stop: given up there.
    std::atomic<bool> letGo { false };
    work.setOnReadStep (
        [&letGo, copied, size = take.getSize()] (int job, const phazite::BackgroundJobs::ShouldStop&)
        {
            if (job == 1 && copied.getSize() == size)
                while (! letGo)
                    juce::Thread::sleep (1);
        });

    REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, s.firstTrack, at (0.0)));
    REQUIRE (phazite::test::runTimersUntil ([&window, &copied, &take]
                                            { return window.render != nullptr && copied.getSize() == take.getSize(); }));
    window.render->cancel();
    window.render = nullptr;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy(); }));

    // The same file again: not the given-up one's copy, which it deletes once let go.
    REQUIRE (phazite::importFilesInBackground (s.context, { take.getFullPathName() }, s.firstTrack, at (8.0)));
    phazite::test::waitForAudioWork (s.context);
    letGo = true;
    REQUIRE (phazite::test::runTimersUntil ([&work] { return work.countReadsGivenUp() == 0; }));
    work.setOnReadStep (nullptr);

    const auto clips = phazite::getClips (song, s.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto file = phazite::getClipFile (song, phazite::findClipState (song, clips[0]));
    CHECK (file != copied);
    CHECK (file.existsAsFile());
    work.setHost (nullptr);
    work.setShowAfter (phazite::AudioWork::showAfterMilliseconds);
}

TEST_CASE ("The window's hold takes the keys but not from a field being typed in, and gives them back only where nothing else "
           "took them")
{
    juce::Component content;
    juce::TextButton first ("First"), second ("Second");
    juce::TextEditor field;
    first.setWantsKeyboardFocus (true);
    second.setWantsKeyboardFocus (true);

    for (auto* c : std::initializer_list<juce::Component*> { &first, &second, &field })
        content.addAndMakeVisible (c);

    first.setBounds (0, 0, 80, 20);
    second.setBounds (0, 30, 80, 20);
    field.setBounds (0, 60, 80, 20);
    phazite::test::TestWindow window (content, 200, 100);
    phazite::InputHold hold;
    juce::Array<juce::KeyPress> passed; // the keys the hold let through
    hold.setWindow (&content,
                    [&passed] (const juce::KeyPress& key)
                    {
                        passed.add (key);
                        return true;
                    });

    // Given back where it was.
    first.grabKeyboardFocus();
    hold.holdInput();
    CHECK_FALSE (first.hasKeyboardFocus (true));
    CHECK_FALSE (juce::Component::getCurrentlyModalComponent()->isAccessible()); // nothing said for a screen reader
    hold.releaseInput();
    CHECK (first.hasKeyboardFocus (true));

    // Something else took the keys meanwhile (a swap that started as a keep landed): they stay there.
    hold.holdInput();
    second.grabKeyboardFocus();
    hold.releaseInput();
    CHECK (second.hasKeyboardFocus (true));

    // A value half typed into a field: the field keeps the keys and its edit, which nothing commits.
    struct Lost final : juce::TextEditor::Listener
    {
        void textEditorFocusLost (juce::TextEditor&) override { ++count; }
        int count = 0;
    } lost;

    field.addListener (&lost);
    field.grabKeyboardFocus();
    field.setText ("-1", false);
    hold.holdInput();
    CHECK (field.hasKeyboardFocus (true));

    // Space meanwhile does nothing: not typed into the field, nor let through to the transport.
    window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    CHECK (passed.isEmpty());
    CHECK (field.getText() == juce::String ("-1"));
    hold.releaseInput();
    CHECK (field.hasKeyboardFocus (true));
    CHECK (lost.count == 0);
    CHECK (field.getText() == juce::String ("-1"));
    field.removeListener (&lost);
}

TEST_CASE ("An unsaved project discarded while it couldn't be deleted is deleted at the next start, not offered back; and what a "
           "copy left behind isn't recovered as audio")
{
    phazite::test::TemporaryFolder temp;
    const auto parent = temp.folder.getChildFile ("Unsaved");
    juce::File projectFile;

    {
        auto unsaved = phazite::UnsavedFolder::create (parent);
        REQUIRE (unsaved != nullptr);
        juce::String error;
        auto project = phazite::Project::create (unsaved->getFolder(), "Untitled", error);
        REQUIRE (project != nullptr);
        projectFile = project->getFile();
        REQUIRE (phazite::test::writeSound (project->getAudioFolder().getChildFile ("Take.wav"), 0.5).existsAsFile());
        project = nullptr;

        // A file held open as it is closed (as a read given up still writes one): the folder stays, marked.
        juce::FileOutputStream open (projectFile.getSiblingFile ("Audio").getChildFile ("Take_temp1a2b3c.wav"));
        REQUIRE (open.openedOk());
        unsaved = nullptr;
#if JUCE_WINDOWS
        CHECK (projectFile.getParentDirectory().getParentDirectory().getChildFile ("Discarded").existsAsFile());
#endif
    }

    CHECK (phazite::UnsavedFolder::findProjectToRestore (parent) == juce::File());
    CHECK_FALSE (projectFile.getParentDirectory().getParentDirectory().exists());

    // In a project's Audio folder, an empty file (a name kept for a copy that never came) and a copy's temporary file are no
    // audio to recover; a sound nothing plays is.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto audio = s.getProject().getAudioFolder();
    REQUIRE (audio.createDirectory());
    REQUIRE (audio.getChildFile ("Kept.wav").create().wasOk());
    phazite::test::writeSound (audio.getChildFile ("Take_temp9f8e.wav"), 0.3);
    phazite::test::writeSound (audio.getChildFile ("Unplayed.wav"), 0.3);
    CHECK (phazite::UnsavedFolder::recoverUnusedAudio (song) == 1);
}

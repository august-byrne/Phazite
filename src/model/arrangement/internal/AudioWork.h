// SPDX-License-Identifier: AGPL-3.0-or-later
// The work of the commands that read much of an audio file (known bug 123): an import's copy of a file into the project's
// Audio folder (and its look at a file of the same name there), the copies of the sounds put on a drum pad, the sampler or
// a wavetable, kept by Swap or found again for a missing file (bringFilesIntoProject(), Import.h), Normalize's peaks and
// Quantize Audio's hits. Each command takes what it needs of the song on the message thread as it starts (the files, the
// parts, the IDs: a job never reads the song, whose juce::ValueTree isn't thread-safe), reads on a thread of its own here,
// and makes its undo step on the message thread once what it read is in, one job at a time, in the order they came. So the
// message thread reads none of their audio (noteRead() counts what it would, and the performance gates hold it to none).
//
// To the user they behave as they did when they read at once: from the moment a job starts, the window takes no input (the
// InputHolder, the app's), so that nothing the user does lands between the command and its step (an undo, a drag, a
// gain turned, a selection made); a short job ends before anyone notices, and one that takes longer than a second shows
// its progress with Cancel, in the window a render shows (TrackRenderHost: the app's progress window), which only its
// Cancel cancels. Cancel stops it and leaves the project as it was. A job's change is made on the message thread, as one
// named undo step of its own, never inside a drag in progress (a Gesture) or a Swap's open trial (a Trial, which it keeps
// first as it starts, as an edit made at once would); if the song changed meanwhile so that what it read no longer fits
// (something other than the user can change it: a plug-in, a recording ending; its clip or its track gone, its clip
// playing another file, another part or at another gain, the project moved), nothing is changed, and the user is told why
// (reportProblem(), model/project/internal/ProjectContext.h). Another project opened stops every job at once.
#pragma once

#include "model/base/BackgroundJobs.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
class ProjectContext;
class Song;
class TrackRenderHost;

// What holds the window's input while work runs: the app's (src/app/InputHold.h); none in most tests. Each holdInput() is
// matched by a releaseInput().
class InputHolder
{
public:
    virtual ~InputHolder() = default;
    virtual void holdInput() = 0;
    virtual void releaseInput() = 0;
};

class AudioWork final : private juce::Timer
{
public:
    class Job
    {
    public:
        virtual ~Job() = default;

        // What it does, as its progress window says it: "Importing Take.wav".
        virtual juce::String getTitle() const = 0;

        // On the work's thread: reads what it needs, never the song, setting how far it is (from 0 to 1) as it goes, and
        // looking at whether it should stop between its pieces (a megabyte or a second of audio).
        virtual void read (const BackgroundJobs::ShouldStop&, std::atomic<float>& progress) = 0;

        // On the message thread, once read, in its project, while no drag or trial is open, before the next job reads: its
        // change, as one named undo step of its own, if the song still fits what it read; else nothing, and it tells the user
        // why (reportProblem()) and leaves nothing behind.
        virtual void finish (ProjectContext&) = 0;

        // What it made that nothing uses when it changes nothing (cancelled, or another project open): a copy it wrote, to
        // delete, never a file the song names or one that was there before it. Message thread.
        virtual void discard (ProjectContext&) {}

        // Whether it finishes the Swap's trial open now (its keep, whose sounds it brings in): that trial isn't kept as it
        // starts, nor waited for as it ends.
        virtual bool isForTheOpenTrial() const { return false; }

        // Its read given up while it may still be under way (cancelled, another project opened, Phazite quitting), as a read
        // stuck in the system (a network share that went) can't be waited for: on the message thread, what it made so far
        // goes (never what the song names), and what it would have called goes with it (nothing it holds may call into the
        // song from its thread). Whatever its read still makes, it deletes itself as the read returns, on its thread.
        virtual void abandon (ProjectContext&) {}

        // The project's folder it writes into, if it writes: an unsaved project's isn't deleted while a read given up may
        // still write there (whenNoneWritesInto()).
        virtual juce::File getFolder() const { return {}; }
    };

    explicit AudioWork (ProjectContext&);
    ~AudioWork() override; // every job given up, none waited for; none changes anything

    // A job, started once those before it have ended: input is held from now on. Message thread.
    void add (std::unique_ptr<Job>);

    // Whether any job waits, reads or is still to make its change.
    bool isBusy() const noexcept { return ! jobs.empty(); }

    // Whether the first job has read what it needs and waits for a drag or a trial to end before it makes its change.
    bool isWaitingForGesture() const noexcept { return waitingForGesture; }

    // The open song, as the project context sets it (setSong()): another gives up every job of the one before, at once,
    // waiting for none of their reads (abandon()); the user is told.
    void songChanged (Song*);

    // The work for a song, while it is the open song of a context: for what holds a song alone (a device's handle).
    static AudioWork* find (const Song&);
    static bool isAnyBusy();       // of any context: what tests wait for
    int countReadsGivenUp() const; // given up, and not returned yet

    // The threads of works that went while a read they had given up was still under way, kept until it returns (all
    // works'): none once those reads have returned and the message thread has heard so. Message thread.
    static int countThreadsLeftBehind();

    // Runs an action (an unsaved project's folder deleted) once no read given up still writes into a folder or one inside it:
    // at once if none does. Message thread.
    static void whenNoneWritesInto (const juce::File& folder, std::function<void()> action);

    // What shows a job's progress window and its Cancel, and what holds input, the app's (none in most tests), and how long a
    // job reads before it is shown.
    void setHost (TrackRenderHost* host) noexcept { renders = host; }
    void setInputHolder (InputHolder*);
    void setShowAfter (int milliseconds) noexcept { showAfter = milliseconds; }
    static constexpr int showAfterMilliseconds = 1000;

    // Tests: what the work's thread does each time a job asks whether it should stop, as it starts and between its pieces (a
    // read made slow, or held at a point until let go, or stuck, as in the system), given the job's number (from 1, in the
    // order added to this work) and what says whether it should stop.
    using ReadStep = std::function<void (int job, const BackgroundJobs::ShouldStop&)>;
    void setOnReadStep (ReadStep step) { onReadStep = std::move (step); }

    // Tests: what the work's thread does once a read has returned and been handed back, as the last of its task.
    void setAfterEachRead (std::function<void()> after) { afterEachRead = std::move (after); }

    // The bytes of audio a reader of these jobs read: counted when it reads on the message thread, which they never do (the
    // performance gates check that). Any thread.
    static void noteRead (juce::int64 bytes);
    static juce::int64 getMessageThreadReads() noexcept { return messageThreadReads.load(); }

private:
    // What a job's thread, its progress window and the work share.
    struct State
    {
        // What its read is: under way, done, or given up (by the work, on the message thread: whichever comes first of the
        // read's end and the giving up decides, as each exchanges the phase).
        enum Phase
        {
            queued, // its read not begun (it may never be)
            reading,
            done,
            givenUp
        };

        std::atomic<int> phase { queued };
        std::shared_ptr<Job> orphan; // a job given up while it read: its read's thread lets go of it as the read returns
        std::atomic<float> progress { 0.0f };
        std::atomic<bool> cancelled { false };
        bool shown = false, over = false;
        double started = 0.0;
        int projectNumber = 0;
        int number = 0; // in the order added
        juce::String title;
        juce::File folder;

        bool isRead() const noexcept { return phase.load() == done; }
    };

    struct Shown; // the job as its progress window shows it

    struct Entry
    {
        std::shared_ptr<Job> job; // the message thread's alone, unless it is given up while it reads (State::orphan)
        std::shared_ptr<State> state;
    };

    ProjectContext& context;
    Song* song = nullptr;
    TrackRenderHost* renders = nullptr;
    InputHolder* holder = nullptr;
    bool holding = false;
    int showAfter = showAfterMilliseconds;
    bool waitingForGesture = false;
    ReadStep onReadStep;
    std::function<void()> afterEachRead;
    int jobsAdded = 0;
    std::deque<Entry> jobs;                      // the first reads, or is to make its change
    std::vector<std::shared_ptr<State>> givenUp; // reads given up, until they return
    // The threads the reads run on: one reads at a time, but a read given up may hold its thread for a while (one stuck in
    // the system for ever), so there are a few. A work that goes while a read it gave up is still under way leaves them
    // behind (LeftBehind), not waited for.
    std::unique_ptr<BackgroundJobs> threads = std::make_unique<BackgroundJobs> ("Phazite audio work", 8, juce::Thread::Priority::normal);
    static inline std::atomic<juce::int64> messageThreadReads { 0 };

    // The threads of a work that went, and its reads given up that hadn't returned: deleted on the message thread once each
    // has (as each read returns, its thread asks the message thread to look: freeLeftBehind()). One stuck in the system as
    // Phazite quits keeps them to the end: the process's end takes that thread, never ended by force nor waited for, and
    // what it touches if it returns meanwhile is its own or never goes (Import.cpp's names and copies); a Debug build's
    // JUCE leak check reports its thread's object then.
    struct LeftBehind
    {
        std::unique_ptr<BackgroundJobs> threads;
        std::vector<std::shared_ptr<State>> reads;
    };

    static std::vector<LeftBehind>& getLeftBehind(); // message thread; never destroyed
    static void freeLeftBehind();

    void startFirst();
    void endFirst();
    void giveUp (Entry&); // its read let go of, the job told (abandon()), message thread
    void hold (bool);
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE (AudioWork)
};
} // namespace phazite

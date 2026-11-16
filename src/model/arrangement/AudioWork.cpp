// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioWork.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Freeze.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace phazite
{
namespace
{
    // The work of each song open in a context (message thread).
    std::map<const Song*, AudioWork*>& getWorks()
    {
        static std::map<const Song*, AudioWork*> works;
        return works;
    }
} // namespace

// The job as its progress window shows it: how far it is, and what it does. Only its Cancel cancels it: the window going
// otherwise (another closing it) leaves the job reading, and it shows again.
struct AudioWork::Shown final : TrackRender::Impl
{
    explicit Shown (std::shared_ptr<State> s)
        : state (std::move (s))
    {
        state->shown = true;
    }

    ~Shown() override { state->shown = false; }

    float getProgress() const override { return juce::jlimit (0.0f, 1.0f, state->progress.load()); }
    juce::String getTitle() const override { return state->title; }

    void cancel() override
    {
        if (! state->over)
            state->cancelled = true;
    }

    bool isHold() const override { return true; } // Escape is its Cancel, as a versions read's is

    const std::shared_ptr<State> state;
};

AudioWork::AudioWork (ProjectContext& c)
    : context (c)
{
}

AudioWork::~AudioWork()
{
    stopTimer();
    songChanged (nullptr);
    hold (false);

    // A read given up that hasn't returned yet (stopping as asked, or stuck in the system): its threads are left behind until
    // it has, not waited for (LeftBehind). Otherwise they go with the work, each waited for as it ends what it runs.
    std::erase_if (givenUp, [] (const std::shared_ptr<State>& state) { return state->phase.load() == State::done; });

    if (! givenUp.empty())
        getLeftBehind().push_back ({ std::move (threads), std::move (givenUp) });
}

std::vector<AudioWork::LeftBehind>& AudioWork::getLeftBehind()
{
    // Never destroyed, so that nothing of it goes at the process's end under a thread still in it.
    static auto& left = *new std::vector<LeftBehind>();
    return left;
}

void AudioWork::freeLeftBehind()
{
    // Each work's threads once all its reads have returned: deleting them waits for each to end the task it runs, which has
    // only to let go of what it held.
    std::erase_if (getLeftBehind(),
                   [] (const LeftBehind& each)
                   {
                       return std::all_of (each.reads.begin(), each.reads.end(),
                                           [] (const std::shared_ptr<State>& state) { return state->phase.load() == State::done; });
                   });
}

int AudioWork::countThreadsLeftBehind()
{
    return (int) getLeftBehind().size();
}

void AudioWork::noteRead (juce::int64 bytes)
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        messageThreadReads += bytes;
}

int AudioWork::countReadsGivenUp() const
{
    return (int) std::count_if (givenUp.begin(), givenUp.end(),
                                [] (const std::shared_ptr<State>& state) { return state->phase.load() != State::done; });
}

bool AudioWork::isAnyBusy()
{
    for (const auto& [song, work] : getWorks())
        if (work->isBusy())
            return true;

    return false;
}

AudioWork* AudioWork::find (const Song& s)
{
    const auto& works = getWorks();
    const auto found = works.find (&s);
    return found != works.end() ? found->second : nullptr;
}

namespace
{
    // The folders reads given up may still write into, each once for each such read, and the actions waiting for none to
    // (message thread).
    std::multiset<juce::String>& getFoldersWritten()
    {
        static std::multiset<juce::String> folders;
        return folders;
    }

    std::vector<std::pair<juce::File, std::function<void()>>>& getWaitingForFolders()
    {
        static std::vector<std::pair<juce::File, std::function<void()>>> waiting;
        return waiting;
    }

    bool isWrittenInto (const juce::File& folder)
    {
        for (const auto& written : getFoldersWritten())
            if (const juce::File file (written); file == folder || file.isAChildOf (folder))
                return true;

        return false;
    }

    // A read given up has returned: what waited for its folder runs, if no other given up writes there.
    void noLongerWritten (const juce::String& folder)
    {
        auto& folders = getFoldersWritten();

        if (const auto found = folders.find (folder); found != folders.end())
            folders.erase (found);

        auto& waiting = getWaitingForFolders();
        std::vector<std::function<void()>> ready;

        for (auto it = waiting.begin(); it != waiting.end();)
        {
            if (! isWrittenInto (it->first))
            {
                ready.push_back (std::move (it->second));
                it = waiting.erase (it);
            }
            else
            {
                ++it;
            }
        }

        for (auto& action : ready)
            action();
    }
} // namespace

void AudioWork::whenNoneWritesInto (const juce::File& folder, std::function<void()> action)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (isWrittenInto (folder))
        getWaitingForFolders().emplace_back (folder, std::move (action));
    else
        action();
}

void AudioWork::songChanged (Song* newSong)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (newSong == song)
        return;

    if (song != nullptr)
        getWorks().erase (song);

    song = newSong;

    if (song != nullptr)
        getWorks()[song] = this;

    // Every job of the project before given up at once, none waited for (a read may be stuck in the system for ever):
    // nothing of theirs changes the song, and what they made goes.
    giveUpAll (newSong != nullptr ? juce::String ("Another project was opened before it was done, so nothing was changed.")
                                  : juce::String());
}

void AudioWork::giveUpAll (const juce::String& why)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (jobs.empty())
        return;

    auto stopped = std::move (jobs);
    jobs.clear();
    waitingForGesture = false;
    stopTimer();

    for (auto& entry : stopped)
    {
        giveUp (entry);

        if (entry.state->shown && renders != nullptr)
            renders->renderFinished (entry.state->title, juce::Result::ok());
    }

    hold (false);

    if (why.isNotEmpty())
        reportProblem (context, stopped.front().state->title + " stopped", why);
}

bool AudioWork::cancelFirst()
{
    // As its progress window's Cancel cancels it (Shown::cancel()): given up at the next tick.
    if (jobs.empty() || jobs.front().state->over)
        return false;

    jobs.front().state->cancelled = true;
    return true;
}

void AudioWork::giveUp (Entry& entry)
{
    auto& state = *entry.state;
    state.cancelled = true;
    state.over = true;

    // Whichever comes first, the read's start or end or this, decides. A job whose read never began, or is done, is
    // discarded at once, on the message thread, which owns it. One that reads is handed over to its read's thread, which
    // lets go of it as the read returns: told first (abandon()), so that nothing it holds calls into the song from there.
    if (state.folder != juce::File())
        getFoldersWritten().insert (state.folder.getFullPathName());

    entry.job->abandon (context);
    state.orphan = entry.job; // before the exchange, which hands it over

    if (const auto old = state.phase.exchange (State::givenUp); old == State::queued || old == State::done)
    {
        state.orphan = nullptr;

        if (state.folder != juce::File())
            noLongerWritten (state.folder.getFullPathName());

        entry.job->discard (context);
        return;
    }

    givenUp.push_back (entry.state);
}

void AudioWork::setInputHolder (InputHolder* newHolder)
{
    const auto wasHolding = holding;
    hold (false);
    holder = newHolder;
    hold (wasHolding);
}

void AudioWork::hold (bool on)
{
    if (on == holding)
        return;

    holding = on;

    if (holder != nullptr)
    {
        if (on)
            holder->holdInput();
        else
            holder->releaseInput();
    }
}

void AudioWork::add (std::unique_ptr<Job> job)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (job == nullptr)
        return;

    // As an edit made at once would: a Swap's trial open keeps what it tried first, now, as a step of its own.
    if (auto* open = findSong (context); open != nullptr && ! job->isForTheOpenTrial())
        Trial::keepOpenTrial (open->getUndoManager());

    auto state = std::make_shared<State>();
    state->title = job->getTitle();
    state->folder = job->getFolder();
    state->projectNumber = context.getProjectNumber();
    state->number = ++jobsAdded;
    jobs.push_back ({ std::shared_ptr<Job> (std::move (job)), std::move (state) });
    hold (true); // from the start: nothing the user does lands between the command and its step

    if (jobs.size() == 1)
        startFirst();
}

void AudioWork::startFirst()
{
    // Those given up that have returned are forgotten.
    std::erase_if (givenUp, [] (const std::shared_ptr<State>& state) { return state->phase.load() == State::done; });

    if (jobs.empty())
    {
        stopTimer();
        hold (false);
        return;
    }

    auto& first = jobs.front();
    first.state->started = juce::Time::getMillisecondCounterHiRes();
    threads->add (
        [job = first.job.get(), state = first.state, step = onReadStep,
         afterRead = afterEachRead] (const BackgroundJobs::ShouldStop& shouldStop)
        {
            // Given up before it began: the message thread discarded it (it may be gone), and nothing is read.
            if (auto expected = (int) State::queued; ! state->phase.compare_exchange_strong (expected, State::reading))
                return;

            const BackgroundJobs::ShouldStop asked = [&shouldStop, &state] { return shouldStop() || state->cancelled.load(); };
            const BackgroundJobs::ShouldStop stop = [&asked, &step, number = state->number]
            {
                if (step != nullptr)
                    step (number, asked);

                return asked();
            };

            if (! stop())
                job->read (stop, state->progress);

            // Done: the message thread owns the job, and makes its change. Given up meanwhile: the job is let go of here (what
            // it held that touches the song went with abandon(); what it still holds is its own, and the names and copies of
            // Import.cpp, which never go), and the message thread hears so: the folder it may have written into is free, and
            // the threads of a work gone since are let go of once their reads have all returned. The job isn't touched after
            // the exchange. Once Phazite has quit, the message is dropped (no message manager), and nothing else is touched.
            if (state->phase.exchange (State::done) == State::givenUp)
            {
                const auto orphan = std::move (state->orphan);
                juce::MessageManager::callAsync (
                    [folder = state->folder.getFullPathName()]
                    {
                        if (folder.isNotEmpty())
                            noLongerWritten (folder);

                        freeLeftBehind();
                    });
            }

            if (afterRead != nullptr)
                afterRead();
        });

    startTimerHz (30);
}

void AudioWork::endFirst()
{
    auto first = std::move (jobs.front());
    jobs.pop_front();
    waitingForGesture = false;
    first.state->over = true;

    // Its window goes: the host deletes it, which cancels nothing.
    if (first.state->shown && renders != nullptr)
        renders->renderFinished (first.state->title, juce::Result::ok());

    startFirst();
}

void AudioWork::timerCallback()
{
    if (jobs.empty())
    {
        stopTimer();
        return;
    }

    auto& first = jobs.front();
    auto& state = *first.state;

    if (! state.isRead())
    {
        // Cancelled: given up at once, its read not waited for (it may be stuck in the system), the hold let go.
        if (state.cancelled)
        {
            giveUp (first);
            endFirst();
            return;
        }

        // A second of reading, then its window, unless another render's shows: it waits its turn (one at a time).
        if (! state.shown && renders != nullptr && ! renders->isShowingRender()
            && juce::Time::getMillisecondCounterHiRes() - state.started >= showAfter)
            renders->runRender (std::make_unique<TrackRender> (std::make_unique<Shown> (first.state)));

        return;
    }

    if (state.cancelled || context.getProjectNumber() != state.projectNumber || findSong (context) == nullptr)
    {
        first.job->discard (context);
        endFirst();
        return;
    }

    // Not into a drag's step or a trial's, which a step begun meanwhile would join or keep: once they end.
    auto& open = *findSong (context);
    waitingForGesture = Gesture::isInProgress (open) || (Trial::isInProgress (open) && ! first.job->isForTheOpenTrial());

    if (waitingForGesture)
        return;

    // Its change before the next job reads, so that the next finds what this one made (a copy it brought in).
    first.job->finish (context);
    endFirst();
}
} // namespace phazite

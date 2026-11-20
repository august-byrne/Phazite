// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ClipRenders.h"
#include "devices/core/Processor.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AudioFiles.h"
#include "model/playback/internal/PlaybackSession.h"

#include <algorithm>
#include <set>
#include <span>
#include <utility>

namespace phazite
{
namespace
{
    // How long the clips must stay as they are before renders start; and how often the sources of renders that failed are
    // looked at.
    constexpr int quietMilliseconds = 300, failedCheckMilliseconds = 2000;

    // What a render is made from.
    struct Recipe
    {
        juce::File source;
        stretch::Settings settings;
    };

    // Whether renders can be written into a folder: it is one, or can be made, and may be written in.
    bool canWriteInto (const juce::File& folder)
    {
        return folder.createDirectory().wasOk() && folder.isDirectory() && folder.hasWriteAccess();
    }

    // The renders the song's clips play, each with what it is made from: named for their sources as last asked about
    // (getRenderFile()), or as the disk says now if asked to ask.
    std::map<juce::File, Recipe> getNeededRenders (const Song& song, bool askSources = false)
    {
        std::map<juce::File, Recipe> needed;
        std::set<juce::String> asked;

        for (const auto& clipState : getClipStates (song))
        {
            if (! isAudioState (clipState))
                continue;

            if (const auto settings = getClipStretch (song, clipState); ! stretch::isUnchanged (settings))
            {
                const auto source = getClipFile (song, clipState);

                if (askSources && asked.insert (source.getFullPathName()).second)
                    askFileIdentity (source);

                needed.emplace (getPlayedFile (song, clipState), Recipe { source, settings });
            }
        }

        return needed;
    }

    // How many renders renderClipAudio() made, on the message thread and elsewhere (countClipRendersMade()).
    std::atomic<int> madeOnMessageThread { 0 }, madeElsewhere { 0 };

    // What the tests have called as the stretcher asks whether to stop (setClipRenderStopSeam()).
    std::function<void (const std::function<bool()>&)> clipRenderStopSeam;
} // namespace

void setClipRenderStopSeam (std::function<void (const std::function<bool()>&)> seam)
{
    clipRenderStopSeam = std::move (seam);
}

void announceClipRenders (const Song& song, const juce::Array<juce::File>& made)
{
    if (made.isEmpty() || ! juce::MessageManager::existsAndIsCurrentThread())
        return;

    if (auto* session = findSession (song))
        session->update();
}

ClipRendersMade countClipRendersMade()
{
    return { madeOnMessageThread.load(), madeElsewhere.load() };
}

bool renderClipAudio (const juce::File& source, const stretch::Settings& settings, const juce::File& render,
                      const std::function<bool()>& shouldStop, std::atomic<juce::int64>* framesWritten)
{
    if (render.existsAsFile())
        return true;

    std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (source));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return false;

    // What isn't sound in a file of floats (NaN, an infinity) read as silence, sample by sample, as the engine reads it
    // (engine/audio/AudioFile.h): the stretcher would spread it over the whole render.
    const stretch::Source in { (int) reader->numChannels, reader->sampleRate, reader->lengthInSamples,
                               [&reader] (juce::AudioBuffer<float>& buffer, juce::int64 start, int frames)
                               {
                                   reader->read (&buffer, 0, frames, start, true, true);

                                   for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                                       for (auto& sample : std::span (buffer.getWritePointer (channel), (size_t) frames))
                                           sample = devices::soundOf (sample);
                               } };

    render.getParentDirectory().createDirectory();
    const auto part = render.getSiblingFile (render.getFileNameWithoutExtension() + "_"
                                             + juce::String::toHexString (juce::Random().nextInt64()) + ".part");
    auto rendered = false;

    if (auto file = std::make_unique<juce::FileOutputStream> (part); file->openedOk())
    {
        std::unique_ptr<juce::OutputStream> stream = std::move (file);
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate (in.sampleRate)
                                 .withNumChannels (in.numChannels)
                                 .withBitsPerSample (32)
                                 .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

        if (auto writer = juce::WavAudioFormat().createWriterFor (stream, options))
            rendered = stretch::render (
                in, settings,
                [&writer, framesWritten] (const juce::AudioBuffer<float>& block, int frames)
                {
                    if (framesWritten != nullptr)
                        framesWritten->fetch_add (frames, std::memory_order_relaxed);

                    return writer->writeFromAudioSampleBuffer (block, 0, frames);
                },
                shouldStop);
    }

    // Someone else may have made it meanwhile: theirs is the same.
    if (rendered && ! render.existsAsFile())
    {
        rendered = part.moveFileTo (render);

        if (rendered)
            ++(juce::MessageManager::existsAndIsCurrentThread() ? madeOnMessageThread : madeElsewhere);
    }

    part.deleteFile();
    return render.existsAsFile();
}

std::vector<ClipRenderToMake> findMissingClipRenders (const Song& song)
{
    std::vector<ClipRenderToMake> missing;

    for (const auto& [render, recipe] : getNeededRenders (song, true))
        if (! render.existsAsFile())
            missing.push_back ({ render, recipe.source, recipe.settings, getAudioFileInfo (recipe.source).lengthInSamples });

    return missing;
}

juce::Array<juce::File> makeClipRenders (const std::vector<ClipRenderToMake>& renders, const std::function<bool()>& shouldStop,
                                         const std::function<void (double)>& progress)
{
    juce::Array<juce::File> made;
    std::vector<juce::int64> lengths;
    juce::int64 all = 0, before = 0;

    for (const auto& toMake : renders)
    {
        lengths.push_back (std::max<juce::int64> (1, stretch::getStretchedLength (toMake.sourceLength, toMake.settings)));
        all += lengths.back();
    }

    for (size_t i = 0; i < renders.size(); ++i)
    {
        if (shouldStop != nullptr && shouldStop())
            break;

        // How far it has come told as the stretcher asks whether to stop, between its blocks.
        std::atomic<juce::int64> written { 0 };
        const auto asking = [&]
        {
            if (progress != nullptr)
                progress (juce::jlimit (0.0, 1.0, (double) (before + std::min (written.load(), lengths[i])) / (double) all));

            if (clipRenderStopSeam != nullptr)
                clipRenderStopSeam (shouldStop);

            return shouldStop != nullptr && shouldStop();
        };

        if (renderClipAudio (renders[i].source, renders[i].settings, renders[i].render, asking, &written))
            made.add (renders[i].render);

        before += lengths[i];
    }

    return made;
}

bool renderMissingClipAudio (const Song& song, const std::function<bool()>& shouldStop)
{
    const auto missing = findMissingClipRenders (song);
    const auto made = makeClipRenders (missing, shouldStop);
    announceClipRenders (song, made);
    return made.size() == (int) missing.size() && ! (shouldStop != nullptr && shouldStop());
}

void purgeClipRenders (const Song& song)
{
    juce::Array<juce::File> played;

    for (const auto& [render, recipe] : getNeededRenders (song))
        played.add (render);

    for (const auto& entry : juce::RangedDirectoryIterator (song.getCacheFolder(), false, juce::String (renderFilePrefix) + "*"))
    {
        if (played.contains (entry.getFile()))
            continue;

        entry.getFile().deleteFile();
    }
}

//==============================================================================
ClipRenders::ClipRenders (const Song& s)
    : song (s),
      state (s.getState()),
      jobs ("Phazite clip renders", 2, juce::Thread::Priority::low)
{
    state.addListener (this);
    addFileIdentityListener (*this);
    update();
}

ClipRenders::~ClipRenders()
{
    state.removeListener (this);
    removeFileIdentityListener (*this);
    stopTimer();
    jobs.removeAll (true); // each stops between blocks, and is waited for
    cancelPendingUpdate();
}

bool ClipRenders::isRendering() const
{
    for (const auto& [render, recipe] : getNeededRenders (song))
        if (! render.existsAsFile() && ! hasFailed (render))
            return true;

    return false;
}

bool ClipRenders::hasFailed (const juce::File& render) const
{
    return failed.find (render) != failed.end();
}

ClipRenders::Progress ClipRenders::getProgress() const
{
    Progress progress;
    auto written = framesMade;

    for (const auto& [render, made] : running)
    {
        if (made->stopped.load())
            continue;

        ++progress.renders;
        written += std::min (made->written.load (std::memory_order_relaxed), made->length);
    }

    if (progress.renders > 0 && framesAll > 0)
        progress.done = juce::jlimit (0.0, 1.0, (double) written / (double) framesAll);

    return progress;
}

std::optional<double> ClipRenders::getProgressOf (const juce::File& render) const
{
    const auto made = running.find (render);

    if (made == running.end() || made->second->stopped.load())
        return std::nullopt;

    return juce::jlimit (0.0, 1.0, (double) made->second->written.load (std::memory_order_relaxed) / (double) made->second->length);
}

void ClipRenders::update()
{
    const auto needed = getNeededRenders (song);

    // Those no clip plays any more stop, and leave the progress.
    for (const auto& [render, made] : running)
    {
        if (needed.find (render) == needed.end() && ! made->stopped.load())
        {
            made->stopped.store (true);
            framesAll -= made->length;
        }
    }

    // Failures no clip needs are forgotten: a clip that needs one again (by Undo) tries it again.
    for (auto i = failed.begin(); i != failed.end();)
        i = needed.find (i->first) == needed.end() ? failed.erase (i) : std::next (i);

    for (const auto& [render, recipe] : needed)
    {
        if (render.existsAsFile() || running.find (render) != running.end())
            continue;

        // One that failed waits for its source to change, which names another render, or, for one that failed as its folder
        // couldn't be written, for the folder to be one that can (which a check in the background finds:
        // checkFailedAgain(), and forgets it then).
        if (failed.contains (render))
            continue;

        ++rendersStarted;
        auto made = std::make_shared<Render>();
        made->length =
            std::max<juce::int64> (1, stretch::getStretchedLength (getAudioFileInfo (recipe.source).lengthInSamples, recipe.settings));
        framesAll += made->length;
        running[render] = made;
        jobs.add (
            [this, toMake = recipe, file = render, shared = std::move (made)] (const BackgroundJobs::ShouldStop& shouldStop)
            {
                const auto stopping = [&] { return shouldStop() || shared->stopped.load(); };
                const auto rendered = renderClipAudio (toMake.source, toMake.settings, file, stopping, &shared->written);
                const auto failedNow = ! rendered && ! stopping();
                jobFinished ({ file, failedNow, failedNow && ! canWriteInto (file.getParentDirectory()) });
            });
    }

    findFailedClips();
}

void ClipRenders::findFailedClips()
{
    std::vector<ClipID> clips;

    if (! failed.empty())
        for (const auto clip : getAudioClips (song))
            if (isRendered (song, clip) && hasFailed (getPlayedFile (song, findClipState (song, clip))))
                clips.push_back (clip);

    failedClips = std::move (clips);
}

void ClipRenders::checkFailedAgain()
{
    stopTimer();

    // The folders of those that failed as they couldn't be written, looked at off the message thread (a drive that is slow
    // or gone mustn't hold the window), one check at a time: those that can be written now are tried again as it ends.
    if (! checkingFolders)
    {
        std::vector<juce::File> folders;

        for (const auto& [render, failure] : failed)
            if (failure.folderShut)
                if (const auto folder = render.getParentDirectory(); std::find (folders.begin(), folders.end(), folder) == folders.end())
                    folders.push_back (folder);

        if (! folders.empty())
        {
            checkingFolders = true;
            jobs.add (
                [this, folders] (const BackgroundJobs::ShouldStop&)
                {
                    std::vector<juce::File> open;

                    for (const auto& folder : folders)
                        if (canWriteInto (folder))
                            open.push_back (folder);

                    {
                        const juce::ScopedLock sl (finishedLock);
                        foldersChecked = std::move (open);
                        folderCheckEnded = true;
                    }

                    triggerAsyncUpdate();
                });
        }
    }

    update();

    // While some failed, their sources are looked at now and then: one that changed is tried again.
    if (! failed.empty())
        startTimer (failedCheckMilliseconds);
}

void ClipRenders::jobFinished (Finished job)
{
    {
        const juce::ScopedLock sl (finishedLock);
        finished.push_back (std::move (job));
    }

    triggerAsyncUpdate();
}

void ClipRenders::timerCallback()
{
    checkFailedAgain();
}

void ClipRenders::handleAsyncUpdate()
{
    std::vector<Finished> done;
    juce::Array<juce::File> made;
    std::vector<juce::File> open;
    auto checked = false;

    {
        const juce::ScopedLock sl (finishedLock);
        done.swap (finished);
        open.swap (foldersChecked);
        checked = std::exchange (folderCheckEnded, false);
    }

    // Those whose folders can be written now are forgotten, so that update() tries them again.
    if (checked)
    {
        checkingFolders = false;

        for (auto i = failed.begin(); i != failed.end();)
            i = i->second.folderShut && std::find (open.begin(), open.end(), i->first.getParentDirectory()) != open.end() ? failed.erase (i)
                                                                                                                          : std::next (i);
    }

    for (const auto& job : done)
    {
        if (const auto found = running.find (job.render); found != running.end())
        {
            if (! found->second->stopped.load())
                framesMade += found->second->length;

            running.erase (found);
        }

        if (job.render.existsAsFile())
            made.add (job.render);
        else if (job.failed)
            failed[job.render] = { job.folderShut }; // not tried again until its source or its folder changes
    }

    announceClipRenders (song, made);
    update(); // starts what waited for one that stopped

    if (! failed.empty() && ! isTimerRunning())
        startTimer (failedCheckMilliseconds);

    // None being made: the next progress starts over.
    if (running.empty())
        framesMade = framesAll = 0;
}

void ClipRenders::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    // A clip's sound, or the tempo it follows.
    if (tree.hasType (schema::CLIP) || tree.hasType (schema::TEMPO))
        clipsChanged();
}

void ClipRenders::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)
{
    clipsChanged(); // a clip, or a track with clips, back from undo, say
}

void ClipRenders::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
    // A tempo change taken away, or a clip's last warp marker: the clips under it play at another speed now, from a render
    // of their own.
    clipsChanged();
}

void ClipRenders::fileIdentityChanged (const juce::File&)
{
    // Whichever file it is: the renders the clips need are found again as they would be after a change of the clips.
    clipsChanged();
}

void ClipRenders::clipsChanged()
{
    startTimer (quietMilliseconds);

    // The clips whose renders failed at once, while there are any: not one deleted, or one that needs another render now.
    if (! failed.empty() || ! failedClips.empty())
        findFailedClips();
}
} // namespace phazite

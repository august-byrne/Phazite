// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ClipRenders.h"
#include "internal/AudioClips.h"
#include "model/base/internal/Schema.h"

namespace phazite
{
namespace
{
    // How long the clips must stay as they are before renders start.
    constexpr int quietMilliseconds = 300;

    // What a render is made from.
    struct Recipe
    {
        juce::File source;
        stretch::Settings settings;
    };

    // The renders the Edit's clips play, each with what it is made from.
    std::map<juce::File, Recipe> getNeededRenders (te::Edit& edit)
    {
        std::map<juce::File, Recipe> needed;

        for (auto* clip : getAudioClips (edit))
            if (isRendered (*clip))
                needed.emplace (clip->getSourceFileReference().getFile(), Recipe { getClipFile (*clip), getClipStretch (*clip) });

        return needed;
    }

    // Tells Tracktion that a file it may have looked for already is there now.
    void announce (te::Engine& engine, const juce::File& file)
    {
        engine.getAudioFileManager().checkFileForChanges (te::AudioFile (engine, file));
    }
} // namespace

bool renderClipAudio (te::Engine& engine, const juce::File& source, const stretch::Settings& settings, const juce::File& render,
                      const std::function<bool()>& shouldStop, std::atomic<juce::int64>* framesWritten)
{
    if (render.existsAsFile())
        return true;

    std::unique_ptr<juce::AudioFormatReader> reader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (source));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return false;

    const stretch::Source in { (int) reader->numChannels, reader->sampleRate, reader->lengthInSamples,
                               [&reader] (juce::AudioBuffer<float>& buffer, juce::int64 start, int frames)
                               { reader->read (&buffer, 0, frames, start, true, true); } };

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
        rendered = part.moveFileTo (render);

    part.deleteFile();
    return render.existsAsFile();
}

bool renderMissingClipAudio (te::Edit& edit, const std::function<bool()>& shouldStop)
{
    auto allRendered = true, anyNew = false;

    for (const auto& [render, recipe] : getNeededRenders (edit))
    {
        if (render.existsAsFile())
            continue;

        if (shouldStop != nullptr && shouldStop())
            return false;

        if (renderClipAudio (edit.engine, recipe.source, recipe.settings, render, shouldStop))
        {
            announce (edit.engine, render);
            anyNew = true;
        }
        else
        {
            allRendered = false;
        }
    }

    if (anyNew && juce::MessageManager::existsAndIsCurrentThread())
        edit.restartPlayback();

    return allRendered;
}

void purgeClipRenders (te::Edit& edit)
{
    juce::Array<juce::File> played;

    for (auto* clip : getAudioClips (edit))
        if (isRendered (*clip))
            played.addIfNotAlreadyThere (clip->getSourceFileReference().getFile());

    for (const auto& entry : juce::RangedDirectoryIterator (edit.getTempDirectory (false), false, juce::String (renderFilePrefix) + "*"))
        if (! played.contains (entry.getFile()))
            te::AudioFile (edit.engine, entry.getFile()).deleteFile();
}

//==============================================================================
class ClipRenders::Job : public juce::ThreadPoolJob
{
public:
    Job (ClipRenders& o, te::Engine& e, Recipe r, juce::File file, std::shared_ptr<Render> shared)
        : juce::ThreadPoolJob ("Clip render"),
          owner (o),
          engine (e),
          recipe (std::move (r)),
          render (std::move (file)),
          made (std::move (shared))
    {
    }

    JobStatus runJob() override
    {
        renderClipAudio (
            engine, recipe.source, recipe.settings, render, [this] { return shouldExit() || made->stopped.load(); }, &made->written);
        owner.jobFinished (render);
        return jobHasFinished;
    }

private:
    ClipRenders& owner;
    te::Engine& engine;
    const Recipe recipe;
    const juce::File render;
    const std::shared_ptr<Render> made;
};

ClipRenders::ClipRenders (te::Edit& e)
    : edit (e),
      state (e.state),
      pool (juce::ThreadPoolOptions {}
                .withThreadName ("Phazite clip renders")
                .withNumberOfThreads (2)
                .withDesiredThreadPriority (juce::Thread::Priority::low))
{
    state.addListener (this);
    update();
}

ClipRenders::~ClipRenders()
{
    state.removeListener (this);
    stopTimer();
    pool.removeAllJobs (true, 10000); // each stops between blocks
    cancelPendingUpdate();
}

bool ClipRenders::isRendering() const
{
    for (const auto& [render, recipe] : getNeededRenders (edit))
        if (! render.existsAsFile())
            return true;

    return false;
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
    const auto needed = getNeededRenders (edit);

    // Those no clip plays any more stop, and leave the progress.
    for (const auto& [render, made] : running)
    {
        if (needed.find (render) == needed.end() && ! made->stopped.load())
        {
            made->stopped.store (true);
            framesAll -= made->length;
        }
    }

    for (const auto& [render, recipe] : needed)
    {
        if (render.existsAsFile() || running.find (render) != running.end())
            continue;

        auto made = std::make_shared<Render>();
        made->length = std::max<juce::int64> (
            1, stretch::getStretchedLength (te::AudioFile (edit.engine, recipe.source).getLengthInSamples(), recipe.settings));
        framesAll += made->length;
        running[render] = made;
        pool.addJob (new Job (*this, edit.engine, recipe, render, std::move (made)), true);
    }
}

void ClipRenders::jobFinished (const juce::File& render)
{
    {
        const juce::ScopedLock sl (finishedLock);
        finished.add (render);
    }

    triggerAsyncUpdate();
}

void ClipRenders::timerCallback()
{
    stopTimer();
    update();
}

void ClipRenders::handleAsyncUpdate()
{
    juce::Array<juce::File> done;

    {
        const juce::ScopedLock sl (finishedLock);
        done.swapWith (finished);
    }

    auto anyNew = false;

    for (const auto& render : done)
    {
        if (const auto made = running.find (render); made != running.end())
        {
            if (! made->second->stopped.load())
                framesMade += made->second->length;

            running.erase (made);
        }

        if (render.existsAsFile())
        {
            announce (edit.engine, render);
            anyNew = true;
        }
    }

    if (anyNew)
        edit.restartPlayback();

    update(); // starts what waited for one that stopped

    // None being made: the next progress starts over.
    if (running.empty())
        framesMade = framesAll = 0;
}

void ClipRenders::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (te::IDs::AUDIOCLIP) && (property == te::IDs::source || property == schema::phaziteSource))
        startTimer (quietMilliseconds);
}

void ClipRenders::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)
{
    startTimer (quietMilliseconds); // a clip, or a track with clips, back from undo, say
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Preview.h"
#include "devices/synth/Synth.h"
#include "internal/Preview.h"
#include "internal/PreviewLoop.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
struct Previewer::Impl : private juce::Timer
{
    explicit Impl (te::Engine& e)
        : engine (e)
    {
    }

    ~Impl() override { stopTimer(); }

    // Stops the sound and lets its playback graph go, which Tracktion would otherwise keep at work for the rest of the
    // session (docs/performance-audit.md, A3), and then the loop it played, if it was one.
    void release()
    {
        stopTimer();
        starting = false;

        if (edit != nullptr)
        {
            edit->getTransport().stop (false, false);
            edit->getTransport().freePlaybackContext();
            clearPreviewLoop (*edit);
        }

        loop.reset();
    }

    void start()
    {
        starting = false;
        edit->getTransport().play (false);
        startTimer (250);
    }

    // A loop starts once its start is stretched (StretchedLoop::isReadyToPlay()), a moment after it is chosen; a sound
    // that plays once is released once it has played to its end, as Tracktion would play silence after it.
    void timerCallback() override
    {
        if (starting)
        {
            if (loop != nullptr && loop->isReadyToPlay())
                start();
        }
        else if (edit == nullptr || ! edit->getTransport().isPlaying())
        {
            stopTimer();
        }
        else if (! repeating && edit->getTransport().getPosition() >= te::toPosition (edit->getLength()))
        {
            release();
        }
    }

    void startWhenReady()
    {
        starting = true;
        startTimer (5);
    }

    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<StretchedLoop> loop; // a loop's sound, stretched to the project's tempo, while it plays
    juce::File file;
    bool inTime = false;
    bool repeating = false;
    bool starting = false; // a loop waiting for its start to be stretched
    bool synced = false;   // to the project, while it plays
    float volume = te::decibelsToVolumeFaderPosition (-6.0f);
};

std::unique_ptr<Previewer> createPreviewer (te::Engine& engine)
{
    return std::make_unique<Previewer> (std::make_unique<Previewer::Impl> (engine));
}

std::unique_ptr<te::Edit> createPreviewEdit (te::Engine& engine, const juce::File& file, const te::Edit* project, bool& inTime,
                                             std::unique_ptr<te::Edit> previous)
{
    engine.getAudioFileManager().checkFileForChanges (te::AudioFile (engine, file));

    // Loops keep their own pitch: Tracktion would pull every loop with a known root toward the song's pitch sequence, which
    // is C in a song without a key, and it ignores major and minor (an A minor loop would go up to C in C major).
    auto matched = false;
    const auto synth = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, devices::Synth::typeID);
    auto edit =
        te::Edit::createEditForPreviewingFile (engine, file, project, true, false, &matched, synth, {}, false, std::move (previous));
    inTime = edit != nullptr && matched && project != nullptr;
    return edit;
}

Previewer::Previewer (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

Previewer::~Previewer()
{
    stop();
    impl->edit = nullptr; // on the message thread, before the Engine goes
}

bool Previewer::play (const juce::File& newFile, const ProjectContext& context, bool repeat)
{
    stop();

    if (! newFile.existsAsFile())
        return false;

    // Made from the last sound's Edit, which Tracktion empties and fills again. A MIDI file plays on Phazite's synth, at its
    // defaults.
    impl->edit = createPreviewEdit (impl->engine, newFile, getEdit (context), impl->inTime, std::move (impl->edit));

    if (impl->edit == nullptr)
        return false;

    impl->file = newFile;
    impl->synced = false;
    impl->edit->setMasterVolumeSliderPos (impl->volume);

    // A loop in time plays stretched as a clip of it would be, by Rubber Band, rather than by the clip Tracktion made of
    // it, which would stretch it with SoundTouch as it plays (internal/PreviewLoop.h).
    if (impl->inTime)
        impl->loop = stretchPreviewLoop (*impl->edit, newFile);

    // A loop repeats until another sound plays or the preview stops; a one-shot plays once.
    auto& transport = impl->edit->getTransport();
    impl->repeating = impl->inTime && repeat;
    transport.looping = impl->repeating;
    transport.setPosition (te::TimePosition());

    if (impl->loop != nullptr)
        impl->startWhenReady();
    else
        impl->start();

    keepInTime (context);
    return true;
}

void Previewer::keepInTime (const ProjectContext& context)
{
    auto* project = getEdit (context);

    if (impl->edit == nullptr || ! impl->inTime || project == nullptr)
        return;

    // Once each time the project starts: Tracktion then moves the loop to start on one of the project's bars.
    if (! project->getTransport().isPlaying())
    {
        impl->synced = false;
    }
    else if (! impl->synced && impl->edit->getTransport().isPlaying())
    {
        impl->edit->getTransport().syncToEdit (project, false);
        impl->synced = true;
    }
}

void Previewer::stop()
{
    impl->release();
    impl->file = juce::File();
    impl->inTime = false;
    impl->repeating = false;
    impl->synced = false;
}

bool Previewer::isPlaying() const
{
    if (impl->starting)
        return true;

    if (impl->edit == nullptr || ! impl->edit->getTransport().isPlaying())
        return false;

    // Anything that doesn't repeat has finished once the playhead passes its end.
    return impl->repeating || impl->edit->getTransport().getPosition() < te::toPosition (impl->edit->getLength());
}

juce::File Previewer::getFile() const
{
    return impl->file;
}

bool Previewer::isInTime() const noexcept
{
    return impl->inTime;
}

void Previewer::setVolume (float faderPosition)
{
    impl->volume = juce::jlimit (0.0f, 1.0f, faderPosition);

    if (impl->edit != nullptr)
        impl->edit->setMasterVolumeSliderPos (impl->volume);
}

float Previewer::getVolume() const noexcept
{
    return impl->volume;
}
} // namespace phazite

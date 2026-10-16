// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Preview.h"
#include "devices/synth/Synth.h"
#include "internal/Preview.h"
#include "model/project/ProjectContext.h"

namespace phazite
{
struct Previewer::Impl
{
    explicit Impl (te::Engine& e)
        : engine (e)
    {
    }

    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;
    juce::File file;
    bool inTime = false;
    bool repeating = false;
    bool synced = false; // to the project, while it plays
    float volume = te::decibelsToVolumeFaderPosition (-6.0f);
};

std::unique_ptr<Previewer> createPreviewer (te::Engine& engine)
{
    return std::make_unique<Previewer> (std::make_unique<Previewer::Impl> (engine));
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
    // defaults. Loops keep their own pitch: Tracktion would pull every loop with a known root toward the song's pitch
    // sequence, which is C in a song without a key, and it ignores major and minor (an A minor loop would go up to C in
    // C major).
    auto* project = context.getEdit();
    auto matched = false;
    const auto synth = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, devices::Synth::typeID);
    impl->edit = te::Edit::createEditForPreviewingFile (impl->engine, newFile, project, true, false, &matched, synth, {}, false,
                                                        std::move (impl->edit));

    if (impl->edit == nullptr)
        return false;

    impl->file = newFile;
    impl->inTime = matched && project != nullptr;
    impl->synced = false;
    impl->edit->setMasterVolumeSliderPos (impl->volume);

    // A loop repeats until another sound plays or the preview stops; a one-shot plays once.
    auto& transport = impl->edit->getTransport();
    impl->repeating = impl->inTime && repeat;
    transport.looping = impl->repeating;
    transport.setPosition (te::TimePosition());
    transport.play (false);
    keepInTime (context);
    return true;
}

void Previewer::keepInTime (const ProjectContext& context)
{
    auto* project = context.getEdit();

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
    if (impl->edit != nullptr)
        impl->edit->getTransport().stop (false, false);

    impl->file = juce::File();
    impl->inTime = false;
    impl->repeating = false;
    impl->synced = false;
}

bool Previewer::isPlaying() const
{
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

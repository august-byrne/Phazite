// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Preview.h"

namespace phazite
{
Previewer::Previewer (te::Engine& e)
    : engine (e)
{
}

Previewer::~Previewer()
{
    stop();
    edit = nullptr; // on the message thread, before the Engine goes
}

bool Previewer::play (const juce::File& newFile, te::Edit* project, bool repeat)
{
    stop();

    if (! newFile.existsAsFile())
        return false;

    // Made from the last sound's Edit, which Tracktion empties and fills again. A MIDI file plays on the 4OSC synth,
    // the one built-in instrument for now (it breaks the real-time rules, but only while a preview plays). Loops keep
    // their own pitch: Tracktion would pull every loop with a known root toward the song's pitch sequence, which is C
    // in a song without a key, and it ignores major and minor (an A minor loop would go up to C in C major).
    auto matched = false;
    const auto synth = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, te::FourOscPlugin::xmlTypeName);
    edit = te::Edit::createEditForPreviewingFile (engine, newFile, project, true, false, &matched, synth, {}, false, std::move (edit));

    if (edit == nullptr)
        return false;

    file = newFile;
    inTime = matched && project != nullptr;
    synced = false;
    edit->setMasterVolumeSliderPos (volume);

    // A loop repeats until another sound plays or the preview stops; a one-shot plays once.
    auto& transport = edit->getTransport();
    repeating = inTime && repeat;
    transport.looping = repeating;
    transport.setPosition (te::TimePosition());
    transport.play (false);
    keepInTime (project);
    return true;
}

void Previewer::keepInTime (te::Edit* project)
{
    if (edit == nullptr || ! inTime || project == nullptr)
        return;

    // Once each time the project starts: Tracktion then moves the loop to start on one of the project's bars.
    if (! project->getTransport().isPlaying())
    {
        synced = false;
    }
    else if (! synced && edit->getTransport().isPlaying())
    {
        edit->getTransport().syncToEdit (project, false);
        synced = true;
    }
}

void Previewer::stop()
{
    if (edit != nullptr)
        edit->getTransport().stop (false, false);

    file = juce::File();
    inTime = false;
    repeating = false;
    synced = false;
}

bool Previewer::isPlaying() const
{
    if (edit == nullptr || ! edit->getTransport().isPlaying())
        return false;

    // Anything that doesn't repeat has finished once the playhead passes its end.
    return repeating || edit->getTransport().getPosition() < te::toPosition (edit->getLength());
}

juce::File Previewer::getFile() const
{
    return file;
}

void Previewer::setVolume (float faderPosition)
{
    volume = juce::jlimit (0.0f, 1.0f, faderPosition);

    if (edit != nullptr)
        edit->setMasterVolumeSliderPos (volume);
}
} // namespace phazite

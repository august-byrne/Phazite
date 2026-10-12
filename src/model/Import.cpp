// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Import.h"
#include "Clips.h"
#include "Tracks.h"

namespace phazite
{
FileKind getFileKind (te::Engine& engine, const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();

    if (extension == ".mid" || extension == ".midi")
        return FileKind::midi;

    if (extension.isNotEmpty() && engine.getAudioFileFormatManager().readFormatManager.findFormatForFileExtension (extension) != nullptr)
        return FileKind::audio;

    return FileKind::other;
}

juce::File copyIntoProject (te::Edit& edit, const juce::File& file)
{
    const auto projectFile = edit.editFileRetriever != nullptr ? edit.editFileRetriever() : juce::File();

    if (projectFile == juce::File() || file.isAChildOf (projectFile.getParentDirectory()))
        return file;

    const auto audioFolder = projectFile.getSiblingFile ("Audio");
    auto target = audioFolder.getChildFile (file.getFileName());

    if (target.existsAsFile() && target.hasIdenticalContentTo (file))
        return target; // imported before

    if (target.exists())
        target = audioFolder.getNonexistentChildFile (file.getFileNameWithoutExtension(), file.getFileExtension(), false);

    if (! audioFolder.createDirectory() || ! file.copyFileTo (target))
        return file;

    return target;
}

std::optional<juce::Colour> getColourForName (const juce::String& name)
{
    struct Family
    {
        const char* colourName; // in getTrackColours()
        juce::StringArray words;
    };

    // The first family with a word in the name wins, so the more specific words come first.
    static const Family families[] = {
        { "Blue", { "bass", "808", "sub" } },
        { "Vermilion",
          { "kick", "snare", "clap", "hihat", "hi-hat", "hat", "perc", "drum", "cymbal", "crash", "shaker", "conga", "bongo" } },
        { "Reddish purple", { "vocal", "vox", "voice", "choir", "acapella", "adlib" } },
        { "Yellow", { "fx", "riser", "impact", "sweep", "noise", "uplifter", "downlifter", "whoosh" } },
        { "Bluish green", { "guitar", "gtr", "string", "violin", "cello", "brass", "horn", "flute" } },
        { "Sky blue", { "synth", "lead", "pad", "chord", "keys", "piano", "pluck", "arp", "bell", "organ" } },
    };

    const auto lower = name.toLowerCase();

    for (const auto& family : families)
        for (const auto& word : family.words)
            if (lower.contains (word))
                for (const auto& colour : getTrackColours())
                    if (juce::String (colour.name) == family.colourName)
                        return colour.colour;

    return std::nullopt;
}

te::Clip* importFile (te::ClipTrack& track, const juce::File& file, te::BeatPosition start)
{
    auto& edit = track.edit;

    switch (getFileKind (edit.engine, file))
    {
        case FileKind::audio:
        {
            const auto copy = copyIntoProject (edit, file);
            const te::AudioFile audio (edit.engine, copy);
            const auto length = te::TimeDuration::fromSeconds (audio.getLength());

            if (length <= te::TimeDuration())
                return nullptr;

            const auto time = edit.tempoSequence.toTime (start);
            return track.insertWaveClip (file.getFileNameWithoutExtension(), copy, { te::TimeRange (time, time + length) }, false).get();
        }

        case FileKind::midi:
        {
            auto clip = te::createClipFromFile (file, track, false);

            if (clip == nullptr)
                return nullptr;

            moveClip (*clip, start, track);
            return clip.get();
        }

        case FileKind::other:
        default:
            return nullptr;
    }
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SamplerSound.h"
#include "devices/sampler/FactorySamples.h"
#include "internal/DeviceFiles.h"
#include "internal/Parameters.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <tuple>

namespace phazite
{
namespace
{
    using devices::Sampler;

    constexpr float smallestFine = 3.0f; // cents a sound's pitch is off its note before Fine takes it back

    const devices::ParameterInfo& findInfo (std::string_view id)
    {
        return Sampler::info.parameters[(size_t) devices::findParameter (Sampler::info, id)];
    }

    // The parameters that belong to the sound it plays, set again as another sound is put on it: its root key and fine, its
    // markers on the whole sound, its loop (the file's own, or off where it starts), and no slices changed by hand.
    void setSoundSettings (Song& song, DeviceID id, int root, float fine, std::optional<std::pair<float, float>> loop)
    {
        const auto set = [&song, id] (const char* parameter, float value) { setValueSetByHand (song, { id, parameter }, value); };
        set ("root", (float) root);
        set ("fine", fine);
        set ("start", 0.0f);
        set ("end", 1.0f);
        set ("loopOn", loop.has_value() ? 1.0f : 0.0f);
        set ("loopStart", loop.has_value() ? loop->first : findInfo ("loopStart").defaultValue);
        set ("loopEnd", loop.has_value() ? loop->second : findInfo ("loopEnd").defaultValue);
        setDeviceSound (song, id, schema::samplerSlices, {});
        setDeviceSound (song, id, schema::samplerSlicesRemoved, {});
    }

    juce::ValueTree findSamplerSounds (const Song& song, DeviceID id)
    {
        const auto device = findDeviceState (song, id);
        return isSampler (device) ? device.getChildWithName (schema::SOUNDS) : juce::ValueTree();
    }

    juce::File getSampleFile (const Song& song, const juce::ValueTree& sounds)
    {
        return findProjectFile (song, sounds[schema::samplerSampleFile].toString());
    }

    int getFactoryIndex (const juce::ValueTree& sounds)
    {
        return std::max (0, devices::findFactorySample (sounds[schema::samplerSample].toString().toStdString()));
    }

    std::vector<float> parseSlices (const juce::String& text)
    {
        std::vector<float> positions;

        for (const auto& word : juce::StringArray::fromTokens (text, " ", ""))
            if (word.isNotEmpty())
                positions.push_back (juce::jlimit (0.0f, 1.0f, word.getFloatValue()));

        std::sort (positions.begin(), positions.end());
        return positions;
    }

    juce::String writeSlices (std::vector<float> positions)
    {
        std::sort (positions.begin(), positions.end());
        juce::StringArray words;

        for (const auto position : positions)
            words.add (juce::String (position, 6).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));

        return words.joinIntoString (" ");
    }

    Sampler::ManualSlices readManualSlices (const juce::ValueTree& sounds)
    {
        return { parseSlices (sounds[schema::samplerSlices].toString()), parseSlices (sounds[schema::samplerSlicesRemoved].toString()) };
    }

    // The sound it plays, as loaded for it: its file's, or its factory sound.
    std::shared_ptr<const Sampler::Sound> findSound (const Song& song, DeviceID id)
    {
        const auto sounds = findSamplerSounds (song, id);

        if (const auto file = getSampleFile (song, sounds); file.existsAsFile())
            if (auto sound = readSampleSound (file))
                return sound;

        return devices::getFactorySample (getFactoryIndex (sounds));
    }

    // Its parameters' values as set by hand, in the order of its description.
    std::vector<float> getValues (const Song& song, DeviceID id)
    {
        std::vector<float> values;

        for (const auto& parameter : Sampler::info.parameters)
            values.push_back (getValueSetByHand (song, { id, juce::String (parameter.id.data(), parameter.id.size()) }));

        return values;
    }

    // The sound's slices at the song's tempo, a bar of them as long as the bar where playback starts, where Slices to Clip
    // writes them.
    Sampler::Slices findSlices (const Song& song, DeviceID id, const Sampler::Sound& sound)
    {
        const auto values = getValues (song, id);
        const auto manual = readManualSlices (findSamplerSounds (song, id));
        const auto barLength = getBars (song).getBarLength (getPlayStartBeat (song)).inBeats();
        return Sampler::getSlices (sound, &manual, values, getTempo (song), barLength);
    }

    // A slice of the sound, counted from 1, and where it starts as a part of the sound's length; none for one that isn't.
    struct FoundSlice
    {
        float position = 0.0f;
        bool byHand = false;
    };

    std::optional<FoundSlice> findSlice (const Song& song, DeviceID id, int slice)
    {
        const auto sound = isSampler (song, id) ? findSound (song, id) : nullptr;

        if (sound == nullptr || sound->audio.getNumSamples() == 0)
            return {};

        const auto slices = findSlices (song, id, *sound);

        if (slice < 1 || slice >= slices.count)
            return {};

        return FoundSlice { (float) slices.bounds[(size_t) slice] / (float) sound->audio.getNumSamples(), slices.byHand[(size_t) slice] };
    }

    // The slice put in by hand nearest a place, in a list of them.
    std::vector<float>::iterator findNearest (std::vector<float>& positions, float position)
    {
        return std::min_element (positions.begin(), positions.end(),
                                 [position] (float a, float b) { return std::abs (a - position) < std::abs (b - position); });
    }

    void writeManualSlices (Song& song, DeviceID id, const Sampler::ManualSlices& slices)
    {
        setDeviceSound (song, id, schema::samplerSlices, writeSlices (slices.added));
        setDeviceSound (song, id, schema::samplerSlicesRemoved, writeSlices (slices.removed));
    }
} // namespace

bool isSampler (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::DEVICE) && deviceState[schema::deviceType].toString() == Sampler::typeID;
}

bool isSampler (const Song& song, DeviceID id)
{
    return isSampler (findDeviceState (song, id));
}

juce::String getSampleName (const Song& song, DeviceID id)
{
    if (! isSampler (song, id))
        return {};

    const auto sounds = findSamplerSounds (song, id);

    if (const auto file = getSampleFile (song, sounds); file != juce::File())
        return file.getFileNameWithoutExtension();

    const auto name = devices::getFactorySampleNames()[(size_t) getFactoryIndex (sounds)];
    return juce::String (name.data(), name.size());
}

bool hasSampleFile (const Song& song, DeviceID id)
{
    return isSampler (song, id) && getSampleFile (song, findSamplerSounds (song, id)) != juce::File();
}

int getFactorySampleIndex (const Song& song, DeviceID id)
{
    return isSampler (song, id) && ! hasSampleFile (song, id) ? getFactoryIndex (findSamplerSounds (song, id)) : -1;
}

std::optional<SampleSettings> readSampleSettings (const juce::File& file)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};

    SampleSettings settings;
    const auto& data = reader->metadataValues;

    // Its own loop, from its smpl chunk, where it has one that holds together.
    if (data.getValue ("NumSampleLoops", "0").getIntValue() > 0)
    {
        const auto loopFrom = data.getValue ("Loop0Start", "-1").getLargeIntValue();
        const auto loopTo = data.getValue ("Loop0End", "-1").getLargeIntValue() + 1; // the loop's last sample, inclusive

        if (loopFrom >= 0 && loopTo > loopFrom && loopTo <= reader->lengthInSamples)
            settings.loop = std::pair { (float) ((double) loopFrom / (double) reader->lengthInSamples),
                                        (float) ((double) loopTo / (double) reader->lengthInSamples) };
    }

    // Its pitch, from its first seconds.
    std::optional<double> pitch;
    const auto length = (int) std::min (reader->lengthInSamples, (juce::int64) (4.0 * reader->sampleRate));
    juce::AudioBuffer<float> opening (juce::jlimit (1, 2, (int) reader->numChannels), length);

    if (reader->read (&opening, 0, length, 0, true, true))
        pitch = devices::detectPitch (opening, reader->sampleRate);

    // The note its own data name; else a note in its name, in the octave its pitch is in, or from middle C; else its pitch.
    const auto unity = data.getValue ("MidiUnityNote", "").getIntValue();

    if (data.containsKey ("MidiUnityNote") && unity > 0 && unity < 128)
    {
        settings.root = unity;
        settings.rootFrom = SampleSettings::RootFrom::file;
    }
    else if (const auto key = getKeyFromName (file.getFileNameWithoutExtension()))
    {
        const auto near = pitch.has_value() ? *pitch : 65.5; // middle C's octave, from C4 up to B4
        settings.root = juce::jlimit (0, 127, key->root + 12 * (int) std::lround ((near - key->root) / 12.0));
        settings.rootFrom = SampleSettings::RootFrom::name;
    }
    else if (pitch.has_value())
    {
        settings.root = juce::jlimit (0, 127, (int) std::lround (*pitch));
        settings.rootFrom = SampleSettings::RootFrom::pitch;
    }

    // Fine takes back how far its pitch is off the note, where it is within half a semitone of it.
    if (pitch.has_value())
        if (const auto off = (float) ((*pitch - settings.root) * 100.0); std::abs (off) <= 50.0f && std::abs (off) >= smallestFine)
            settings.fine = -std::round (off);

    return settings;
}

bool setSampleFile (Song& song, DeviceID id, const juce::File& file)
{
    if (! isSampler (song, id) || ! file.existsAsFile() || getFileKind (file) != FileKind::audio)
        return false;

    const auto settings = readSampleSettings (file);

    if (! settings.has_value())
        return false;

    const auto copy = copyIntoProject (song, file);
    const UndoStep step (song, "Put " + copy.getFileNameWithoutExtension() + " on the Sampler");
    setDeviceSound (song, id, schema::samplerSampleFile, getProjectPath (song, copy));
    setSoundSettings (song, id, settings->root, settings->fine, settings->loop);
    return true;
}

void setFactorySample (Song& song, DeviceID id, int index)
{
    const auto names = devices::getFactorySampleNames();

    if (! isSampler (song, id) || ! juce::isPositiveAndBelow (index, (int) names.size()) || getFactorySampleIndex (song, id) == index)
        return;

    const auto name = juce::String (names[(size_t) index].data(), names[(size_t) index].size());
    const UndoStep step (song, "Put " + name + " on the Sampler");
    setDeviceSound (song, id, schema::samplerSampleFile, {});
    setDeviceSound (song, id, schema::samplerSample, index == 0 ? juce::String() : name);
    setSoundSettings (song, id, devices::getFactorySampleRoot (index), 0.0f, devices::getFactorySampleLoop (index));
}

void findRootKey (Song& song, DeviceID id)
{
    if (! isSampler (song, id))
        return;

    if (const auto file = getSampleFile (song, findSamplerSounds (song, id)); file.existsAsFile())
    {
        if (const auto settings = readSampleSettings (file); settings.has_value() && settings->rootFrom != SampleSettings::RootFrom::none)
        {
            const UndoStep step (song, "Find Root Key");
            setValueSetByHand (song, { id, "root" }, (float) settings->root);
            setValueSetByHand (song, { id, "fine" }, settings->fine);
        }

        return;
    }

    const UndoStep step (song, "Find Root Key");
    setValueSetByHand (song, { id, "root" }, (float) devices::getFactorySampleRoot (getFactoryIndex (findSamplerSounds (song, id))));
    setValueSetByHand (song, { id, "fine" }, 0.0f);
}

std::shared_ptr<const Sampler::Sound> readSampleSound (const juce::File& file)
{
    // Each file read once, while anything holds what was read: by its path, its size and when it was last written.
    struct FileKey
    {
        juce::String path;
        juce::int64 size = 0, written = 0;

        bool operator< (const FileKey& other) const
        {
            return std::tie (path, size, written) < std::tie (other.path, other.size, other.written);
        }
    };

    static std::map<FileKey, std::weak_ptr<const Sampler::Sound>> read;
    const FileKey key { file.getFullPathName(), file.getSize(), file.getLastModificationTime().toMilliseconds() };

    if (const auto found = read.find (key); found != read.end())
        if (auto sound = found->second.lock())
            return sound;

    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};

    auto sound = std::make_shared<Sampler::Sound>();
    const auto length = (int) std::min (reader->lengthInSamples, (juce::int64) (longestSampleSeconds * reader->sampleRate));
    sound->audio.setSize (juce::jlimit (1, 2, (int) reader->numChannels), length);
    sound->sampleRate = reader->sampleRate;

    if (! reader->read (&sound->audio, 0, length, 0, true, true))
        return {};

    sound->onsets = devices::detectOnsets (sound->audio, sound->sampleRate);
    sound->beats = readAudioFileInfo (*reader, file.getFileNameWithoutExtension()).loopBeats;
    Sampler::makeOctaves (*sound);

    // Forget what nothing holds any more.
    for (auto each = read.begin(); each != read.end();)
        each = each->second.expired() ? read.erase (each) : std::next (each);

    read[key] = sound;
    return sound;
}

std::shared_ptr<const Sampler::Sound> getSampleSound (const Song& song, DeviceID id)
{
    return isSampler (song, id) ? findSound (song, id) : nullptr;
}

Sampler::ManualSlices getManualSlices (const Song& song, DeviceID id)
{
    return isSampler (song, id) ? readManualSlices (findSamplerSounds (song, id)) : Sampler::ManualSlices();
}

std::vector<float> getSlicePositions (const Song& song, DeviceID id)
{
    std::vector<float> positions;
    const auto sound = isSampler (song, id) ? findSound (song, id) : nullptr;
    const auto frames = sound != nullptr ? sound->audio.getNumSamples() : 0;

    if (frames <= 0)
        return positions;

    const auto slices = findSlices (song, id, *sound);

    for (int i = 1; i < slices.count; ++i)
        positions.push_back ((float) slices.bounds[(size_t) i] / (float) frames);

    return positions;
}

void moveSlice (Song& song, DeviceID id, int slice, float position)
{
    const auto found = findSlice (song, id, slice);

    if (! found.has_value())
        return;

    // One put in by hand moves; one found is taken away from where it was and put in by hand where it goes.
    auto manual = readManualSlices (findSamplerSounds (song, id));
    const UndoStep step (song, "Move Slice");

    if (found->byHand && ! manual.added.empty())
        *findNearest (manual.added, found->position) = juce::jlimit (0.0f, 1.0f, position);
    else
    {
        manual.removed.push_back (found->position);
        manual.added.push_back (juce::jlimit (0.0f, 1.0f, position));
    }

    writeManualSlices (song, id, manual);
}

void addSlice (Song& song, DeviceID id, float position)
{
    if (! isSampler (song, id) || (int) getSlicePositions (song, id).size() + 1 >= Sampler::maxSlices)
        return;

    auto manual = readManualSlices (findSamplerSounds (song, id));
    const UndoStep step (song, "Add Slice");
    manual.added.push_back (juce::jlimit (0.0f, 1.0f, position));

    // Put in by hand where one was taken away, it is no longer taken away.
    const auto frames = (float) std::max (1, findSound (song, id)->audio.getNumSamples());
    manual.removed.erase (std::remove_if (manual.removed.begin(), manual.removed.end(), [position, frames] (float removed)
                                          { return std::abs (removed - position) * frames < (float) Sampler::samplesApart; }),
                          manual.removed.end());
    writeManualSlices (song, id, manual);
}

void removeSlice (Song& song, DeviceID id, int slice)
{
    const auto found = findSlice (song, id, slice);

    if (! found.has_value())
        return;

    auto manual = readManualSlices (findSamplerSounds (song, id));
    const UndoStep step (song, "Remove Slice");

    if (found->byHand && ! manual.added.empty())
        manual.added.erase (findNearest (manual.added, found->position));
    else
        manual.removed.push_back (found->position);

    writeManualSlices (song, id, manual);
}

ClipID writeSliceNotes (Song& song, DeviceID id, Selection* selection)
{
    const auto track = getDeviceChannel (song, id);
    const auto sound = isSampler (song, id) ? findSound (song, id) : nullptr;

    if (! track.isValid() || sound == nullptr || sound->audio.getNumSamples() == 0)
        return {};

    const auto slices = findSlices (song, id, *sound);

    if (slices.count == 0)
        return {};

    // Each slice where it plays in the sound, in beats: at the sound's own tempo where it says how many beats it lasts,
    // which then follows the song's; else at the song's.
    const auto frames = sound->audio.getNumSamples();
    const auto beatsPerSample = sound->beats > 0.0 ? sound->beats / frames : getTempo (song) / 60.0 / sound->sampleRate;
    const auto first = slices.bounds[0];
    const auto beatOf = [&] (int sample) { return (double) (sample - first) * beatsPerSample; };

    // Whole bars, as long as the time signature makes them, at the play start, or after the track's last clip, on a bar,
    // where one is in the way.
    const auto bars = getBars (song);
    auto start = getPlayStartBeat (song).inBeats();
    const auto bar = bars.getBarLength (BeatPosition::fromBeats (start)).inBeats();
    const auto length = std::max (bar, std::ceil (beatOf (slices.bounds[(size_t) slices.count]) / bar) * bar);
    const auto clips = getClips (song, track);
    const auto inTheWay = [&] (ClipID clip)
    { return getClipRange (song, clip).overlaps (BeatRange (BeatPosition::fromBeats (start), BeatDuration::fromBeats (length))); };

    if (std::any_of (clips.begin(), clips.end(), inTheWay))
        for (const auto& clip : clips)
        {
            const auto end = getClipRange (song, clip).getEnd();
            const auto endBar = bars.getBarAt (end);
            const auto barLine = bars.getBarStart (endBar) >= end ? bars.getBarStart (endBar) : bars.getBarStart (endBar + 1);
            start = std::max (start, barLine.inBeats());
        }

    const UndoStep step (song, "Slices to Clip");
    const auto clip = insertMidiClip (song, track, BeatRange (BeatPosition::fromBeats (start), BeatDuration::fromBeats (length)), selection,
                                      getSampleName (song, id));

    for (int i = 0; i < slices.count; ++i)
    {
        const auto from = beatOf (slices.bounds[(size_t) i]), to = beatOf (slices.bounds[(size_t) i + 1]);
        addNote (song, clip, Sampler::firstSliceNote + i, BeatPosition::fromBeats (start + from), BeatDuration::fromBeats (to - from), 100);
    }

    return clip;
}

void loadSamplerSound (Sampler& sampler, const Song& song, const juce::ValueTree& sounds)
{
    // A file that is gone or can't be read leaves it with its factory sound.
    if (const auto file = getSampleFile (song, sounds); file.existsAsFile())
    {
        if (auto sound = readSampleSound (file))
        {
            sampler.setSound (std::move (sound));
            return;
        }
    }

    sampler.setSound (devices::getFactorySample (getFactoryIndex (sounds)));
}

void loadSamplerSlices (Sampler& sampler, const juce::ValueTree& sounds)
{
    if (sounds[schema::samplerSlices].toString().isEmpty() && sounds[schema::samplerSlicesRemoved].toString().isEmpty())
    {
        sampler.setManualSlices (nullptr);
        return;
    }

    sampler.setManualSlices (std::make_shared<Sampler::ManualSlices> (readManualSlices (sounds)));
}
} // namespace phazite

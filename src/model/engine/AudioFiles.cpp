// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioFiles.h"

#include <cmath>
#include <map>
#include <memory>
#include <utility>

namespace phazite
{
namespace
{
    // A name's words and numbers, in lower case: "LoopBreak_120BPM" is "loop", "break", "120" and "bpm".
    juce::StringArray getWords (const juce::String& name)
    {
        juce::StringArray words;
        juce::String word;
        juce::juce_wchar previous = 0;

        for (const auto c : name)
        {
            const auto isPart = juce::CharacterFunctions::isLetterOrDigit (c);
            const auto startsWord = isPart && previous != 0
                                    && (juce::CharacterFunctions::isDigit (c) != juce::CharacterFunctions::isDigit (previous)
                                        || (juce::CharacterFunctions::isUpperCase (c) && juce::CharacterFunctions::isLowerCase (previous)));

            if ((! isPart || startsWord) && word.isNotEmpty())
            {
                words.add (word.toLowerCase());
                word.clear();
            }

            if (isPart)
                word << juce::String::charToString (c);

            previous = isPart ? c : 0;
        }

        if (word.isNotEmpty())
            words.add (word.toLowerCase());

        return words;
    }

    // What was read of a file, as it was then.
    struct Read
    {
        juce::int64 size = 0;
        juce::Time modified;
        AudioFileInfo info;
    };
} // namespace

std::optional<double> getTempoFromName (const juce::String& name)
{
    const auto words = getWords (name);

    for (int i = 0; i < words.size(); ++i)
    {
        if (words[i] != "bpm")
            continue;

        for (const auto next : { i - 1, i + 1 })
            if (juce::isPositiveAndBelow (next, words.size()) && words[next].containsOnly ("0123456789"))
                if (const auto tempo = words[next].getIntValue(); tempo >= 50 && tempo <= 250)
                    return (double) tempo;
    }

    return std::nullopt;
}

AudioFileInfo readAudioFileInfo (const juce::AudioFormatReader& reader, const juce::String& name)
{
    AudioFileInfo info { reader.lengthInSamples, reader.sampleRate, (int) reader.numChannels };
    const auto& data = reader.metadataValues;

    // Its loop data, which says how many beats it lasts, or that it plays once.
    for (const auto& [beats, once] : { std::pair (juce::WavAudioFormat::acidBeats, juce::WavAudioFormat::acidOneShot),
                                       std::pair (juce::AiffAudioFormat::appleBeats, juce::AiffAudioFormat::appleOneShot) })
    {
        if (data.containsKey (beats))
        {
            if (data[once] != "1")
                info.loopBeats = std::max (0.0, data[beats].getDoubleValue());

            return info;
        }
    }

    // Or a tempo in its name, at which it lasts whole bars of four beats, within a tenth of a beat.
    if (const auto seconds = info.getSeconds(); seconds >= 1.0 && seconds <= 60.0)
    {
        if (const auto tempo = getTempoFromName (name))
        {
            const auto beats = *tempo / 60.0 * seconds;
            const auto bars = std::round (beats / 4.0);

            if (bars >= 1.0 && std::abs (beats - bars * 4.0) <= 0.1)
                info.loopBeats = beats;
        }
    }

    return info;
}

juce::AudioFormatManager& getAudioFormats()
{
    static juce::AudioFormatManager formats;

    if (formats.getNumKnownFormats() == 0)
        formats.registerBasicFormats();

    return formats;
}

AudioFileInfo getAudioFileInfo (const juce::File& file)
{
    static std::map<juce::String, Read> read;
    const auto size = file.getSize();
    const auto modified = file.getLastModificationTime();
    auto& entry = read[file.getFullPathName()];

    if (entry.size != size || entry.modified != modified || ! entry.info.isValid())
    {
        entry = { size, modified, {} };

        if (const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file)); reader != nullptr)
            entry.info = readAudioFileInfo (*reader, file.getFileNameWithoutExtension());
    }

    return entry.info;
}
} // namespace phazite

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioFiles.h"

#include <cmath>
#include <map>
#include <memory>
#include <utility>
#include <vector>

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

    // A tempo in a name's words, in small letters: "bpm", and a number from 50 to 250 just before or after it; and where the
    // two are among the words.
    struct FoundTempo
    {
        int tempo = 0;
        int bpm = 0, number = 0;
    };

    std::optional<FoundTempo> findTempo (const juce::StringArray& words)
    {
        for (int i = 0; i < words.size(); ++i)
        {
            if (words[i] != "bpm")
                continue;

            for (const auto next : { i - 1, i + 1 })
                if (juce::isPositiveAndBelow (next, words.size()) && words[next].containsOnly ("0123456789"))
                    if (const auto tempo = words[next].getIntValue(); tempo >= 50 && tempo <= 250)
                        return FoundTempo { tempo, i, next };
        }

        return std::nullopt;
    }

    // What was read of a file, as it was then.
    struct Read
    {
        juce::int64 size = 0;
        juce::Time modified;
        AudioFileInfo info;
    };

    //==============================================================================
    // Keys in names.
    constexpr juce::juce_wchar sharpSign = 0x266F, flatSign = 0x266D; // ♯ and ♭

    bool isAccidentalSign (juce::juce_wchar c)
    {
        return c == '#' || c == sharpSign || c == flatSign;
    }

    // A word of a name as written, its sharps and flats kept with it, and whether it is all that is between brackets.
    struct KeyWord
    {
        juce::String text;
        bool bracketed = false;
    };

    // The words of a name, as written: runs of letters and digits, with a sharp or a flat kept on the letter before it
    // ("F#m"); a word also ends between letters and digits ("Cmaj7", "120Am") and before a capital after small letters
    // ("PadAm").
    std::vector<KeyWord> getKeyWords (const juce::String& name)
    {
        const auto length = name.length();
        std::vector<KeyWord> words;
        int start = -1;
        juce::juce_wchar previous = 0;

        // Whether the nearest character before the word, and after it, past spaces, is an opening and a closing bracket.
        const auto isBracketed = [&name, length] (int from, int to)
        {
            auto before = from - 1, after = to;

            while (before >= 0 && name[before] == ' ')
                --before;

            while (after < length && name[after] == ' ')
                ++after;

            return before >= 0 && after < length && (name[before] == '(' || name[before] == '[')
                   && (name[after] == ')' || name[after] == ']');
        };

        const auto end = [&] (int at)
        {
            if (start >= 0)
                words.push_back ({ name.substring (start, at), isBracketed (start, at) });

            start = -1;
        };

        for (int i = 0; i < length; ++i)
        {
            const auto c = name[i];
            const auto isPart = juce::CharacterFunctions::isLetterOrDigit (c)
                                || (isAccidentalSign (c) && start >= 0 && ! juce::CharacterFunctions::isDigit (previous));

            if (! isPart)
            {
                end (i);
                previous = 0;
                continue;
            }

            // A capital after small letters starts a word ("PadAm"), but after a single one ("dB", decibels, never D and B).
            const auto isDigit = juce::CharacterFunctions::isDigit (c);
            const auto startsWord =
                start >= 0
                && (isDigit != juce::CharacterFunctions::isDigit (previous)
                    || (juce::CharacterFunctions::isUpperCase (c) && juce::CharacterFunctions::isLowerCase (previous) && i - start >= 2));

            if (startsWord)
                end (i);

            if (start < 0)
                start = i;

            previous = c;
        }

        end (length);
        return words;
    }

    // How a name is written: in small letters only, in capitals only, or both.
    enum class Letters
    {
        mixed,
        small,
        capitals
    };

    Letters getLetters (const juce::String& name)
    {
        auto small = false, capitals = false;

        for (const auto c : name)
        {
            small = small || juce::CharacterFunctions::isLowerCase (c);
            capitals = capitals || juce::CharacterFunctions::isUpperCase (c);
        }

        return small && capitals ? Letters::mixed : small ? Letters::small : Letters::capitals;
    }

    // A word that is a key on its own: a note, a sharp or a flat, and minor or major. Nothing for one that merely starts
    // with a note ("Bass", "Ambient"), for "FM" (a capital M is no minor, but after a sharp or a flat in a name in capitals),
    // or for a note with a flat and nothing after it in a name in small letters ("db" is more often decibels).
    struct KeyInWord
    {
        SoundKey key;
        bool hasAccidental = false;
    };

    std::optional<KeyInWord> readKeyWord (const juce::String& word, Letters letters)
    {
        static const juce::String notes ("CDEFGAB");
        static constexpr int roots[] = { 0, 2, 4, 5, 7, 9, 11 };

        if (word.isEmpty())
            return {};

        const auto first = word[0];
        const auto noteIndex = notes.indexOfChar (juce::CharacterFunctions::toUpperCase (first));

        if (noteIndex < 0 || (juce::CharacterFunctions::isLowerCase (first) && letters != Letters::small))
            return {};

        KeyInWord found;
        auto root = roots[noteIndex];
        auto rest = word.substring (1);
        auto accidental = 0;

        if (rest.isNotEmpty())
        {
            const auto sign = rest[0];

            if (sign == '#' || sign == sharpSign)
                accidental = 1;
            else if (sign == flatSign || (sign == 'b' && letters != Letters::capitals))
                accidental = -1;
            else if (sign == 'B' && letters == Letters::capitals && rest.length() > 1) // "EBMIN", never "EB" alone
                accidental = -1;

            if (accidental != 0)
                rest = rest.substring (1);
        }

        const auto suffix = rest.toLowerCase();

        if (rest.isEmpty())
            found.key.mode = SoundKey::Mode::note;
        else if (rest == "m" || (rest == "M" && accidental != 0 && letters == Letters::capitals) || suffix == "min" || suffix == "minor")
            found.key.mode = SoundKey::Mode::minor;
        else if (suffix == "maj" || suffix == "major")
            found.key.mode = SoundKey::Mode::major;
        else
            return {};

        // A flat note alone, in small letters: "db" is more often decibels than D flat.
        if (letters == Letters::small && accidental < 0 && found.key.mode == SoundKey::Mode::note)
            return {};

        found.key.root = (root + accidental + 12) % 12;
        found.hasAccidental = accidental != 0;
        return found;
    }

    std::optional<SoundKey::Mode> readModeWord (const juce::String& word)
    {
        const auto lower = word.toLowerCase();

        if (lower == "minor" || lower == "min" || word == "m")
            return SoundKey::Mode::minor;

        if (lower == "major" || lower == "maj")
            return SoundKey::Mode::major;

        return std::nullopt;
    }

    // The key a name's words give, as getKeyFromName() reads it (the first key with minor or major, else the first note alone
    // where the name sets it apart), and the words that say it: from first to the one before end.
    struct FoundKey
    {
        SoundKey key;
        size_t first = 0, end = 0;
    };

    std::optional<FoundKey> findKey (const std::vector<KeyWord>& words, Letters letters, bool notesAlone)
    {
        std::optional<FoundKey> firstNote;

        // "Key of G", "Root C": a note after either, within two words.
        const auto followsKeyWord = [&words] (size_t i)
        {
            for (size_t back = 1; back <= 2 && back <= i; ++back)
                if (const auto word = words[i - back].text.toLowerCase(); word == "key" || word == "root")
                    return true;

            return false;
        };

        for (size_t i = 0; i < words.size(); ++i)
        {
            auto found = readKeyWord (words[i].text, letters);

            if (! found.has_value())
                continue;

            auto& key = found->key;
            auto next = i + 1;

            // "F Sharp Minor": a sharp or a flat, then minor or major, as the words after a note alone.
            if (key.mode == SoundKey::Mode::note && ! found->hasAccidental && next < words.size())
            {
                if (const auto word = words[next].text.toLowerCase(); word == "sharp" || word == "flat")
                {
                    key.root = (key.root + (word == "sharp" ? 1 : 11)) % 12;
                    found->hasAccidental = true;
                    ++next;
                }
            }

            if (key.mode == SoundKey::Mode::note && next < words.size())
            {
                if (const auto mode = readModeWord (words[next].text))
                {
                    key.mode = *mode;
                    ++next;
                }
            }

            if (key.mode != SoundKey::Mode::note)
                return FoundKey { key, i, next };

            // A note alone, where the name sets it apart.
            const auto setApart = found->hasAccidental || words[i].bracketed || next == words.size() || followsKeyWord (i);

            if (notesAlone && setApart && ! firstNote.has_value())
                firstNote = FoundKey { key, i, next };
        }

        return firstNote;
    }

    //==============================================================================
    // What a name says of a sound, as areNamesAlike() compares names: all its words in small letters, the rest of them
    // besides those that give its tempo and its key, and the tempo and the key.
    struct NameWords
    {
        juce::StringArray all, rest;
        std::optional<int> tempo;
        std::optional<SoundKey> key;
    };

    NameWords readNameWords (const juce::String& name)
    {
        const auto words = getKeyWords (name);
        NameWords read;

        for (const auto& word : words)
            read.all.add (word.text.toLowerCase());

        std::vector<bool> given (words.size()); // the words that give its tempo or its key

        if (const auto tempo = findTempo (read.all))
        {
            read.tempo = tempo->tempo;
            given[(size_t) tempo->bpm] = true;
            given[(size_t) tempo->number] = true;
        }

        if (const auto key = findKey (words, getLetters (name), true))
        {
            read.key = key->key;

            for (auto i = key->first; i < key->end; ++i)
                given[i] = true;
        }

        for (size_t i = 0; i < words.size(); ++i)
            if (! given[i])
                read.rest.add (read.all[(int) i]);

        return read;
    }
} // namespace

//==============================================================================
juce::String SoundKey::describe() const
{
    const auto note = juce::MidiMessage::getMidiNoteName (root, true, false, 0);

    switch (mode)
    {
        case Mode::major:
            return note + " major";
        case Mode::minor:
            return note + " minor";
        case Mode::note:
            break;
    }

    return note;
}

juce::String SoundKey::getShortName() const
{
    const auto note = juce::MidiMessage::getMidiNoteName (root, true, false, 0);
    return mode == Mode::minor ? note + "m" : mode == Mode::major ? note + "maj" : note;
}

std::optional<double> getTempoFromName (const juce::String& name)
{
    if (const auto found = findTempo (getWords (name)))
        return (double) found->tempo;

    return std::nullopt;
}

std::optional<SoundKey> getKeyFromName (const juce::String& name, bool notesAlone)
{
    if (const auto found = findKey (getKeyWords (name), getLetters (name), notesAlone))
        return found->key;

    return std::nullopt;
}

bool areNamesAlike (const juce::String& name, const juce::String& other)
{
    if (name == other)
        return true;

    const auto one = readNameWords (name), two = readNameWords (other);
    const auto alike = [] (const auto& a, const auto& b) { return ! a.has_value() || ! b.has_value() || *a == *b; };

    return one.all == two.all || (! one.rest.isEmpty() && one.rest == two.rest && alike (one.tempo, two.tempo) && alike (one.key, two.key));
}

AudioFileInfo readAudioFileInfo (const juce::AudioFormatReader& reader, const juce::String& name)
{
    AudioFileInfo info { reader.lengthInSamples, reader.sampleRate, (int) reader.numChannels };
    const auto& data = reader.metadataValues;

    // Its loop data, which say how many beats it lasts, or that it plays once; and the root note they name, if they do,
    // with Apple loop data's minor or major.
    struct LoopKeys
    {
        const char *beats, *once, *rootSet, *rootNote, *scale;
    };

    for (const auto& keys :
         { LoopKeys { juce::WavAudioFormat::acidBeats, juce::WavAudioFormat::acidOneShot, juce::WavAudioFormat::acidRootSet,
                      juce::WavAudioFormat::acidRootNote, nullptr },
           LoopKeys { juce::AiffAudioFormat::appleBeats, juce::AiffAudioFormat::appleOneShot, juce::AiffAudioFormat::appleRootSet,
                      juce::AiffAudioFormat::appleRootNote, juce::AiffAudioFormat::appleKey } })
    {
        if (! data.containsKey (keys.beats))
            continue;

        if (data[keys.once] == "1")
        {
            info.loopData = AudioFileInfo::LoopData::oneShot;
        }
        else if (const auto beats = data[keys.beats].getDoubleValue(); beats > 0.0)
        {
            info.loopData = AudioFileInfo::LoopData::loop;
            info.loopBeats = beats;
        }

        if (data[keys.rootSet] == "1" && data.containsKey (keys.rootNote))
        {
            SoundKey key;
            key.root = ((data[keys.rootNote].getIntValue() % 12) + 12) % 12;

            if (keys.scale != nullptr)
                key.mode = data[keys.scale] == "minor"   ? SoundKey::Mode::minor
                           : data[keys.scale] == "major" ? SoundKey::Mode::major
                                                         : SoundKey::Mode::note;

            info.key = key;
        }

        return info;
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
    // Registered once, on whichever thread asks first (renders read files on threads of their own).
    struct Formats
    {
        Formats() { manager.registerBasicFormats(); }
        juce::AudioFormatManager manager;
    };

    static Formats formats;
    return formats.manager;
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

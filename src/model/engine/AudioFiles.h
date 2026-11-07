// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite reads of an audio file without playing it: how long it is, at what rate and in how many channels, whether it
// is a loop that knows its beats, and its key; with the readers of the formats it reads. Message thread only, but for
// readAudioFileInfo(), getTempoFromName(), getKeyFromName() and getAudioFormats().
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

namespace phazite
{
// A sound's key, as its name or its loop data say (docs/design.md, section 11): a root, and minor or major where they say;
// a one-shot tuned to a note has the note alone ("Kick (F)").
struct SoundKey
{
    enum class Mode
    {
        note, // the root alone
        major,
        minor
    };

    int root = 0; // 0 is C, 1 C#, ..., 11 B
    Mode mode = Mode::note;

    bool operator== (const SoundKey&) const = default;
    bool operator< (const SoundKey& other) const noexcept { return root != other.root ? root < other.root : mode < other.mode; }

    juce::String describe() const;     // "F", "F major", "F# minor", in sharps as Phazite names keys (NoteValues.h)
    juce::String getShortName() const; // "F", "Fmaj", "F#m": in a row, beside a sound's name
};

struct AudioFileInfo
{
    // What a file's loop data (a WAV file's acid chunk, an AIFF file's Apple loop data) say of it, if it has them: that it
    // is a loop of loopBeats, or that it plays once.
    enum class LoopData
    {
        none,
        loop,
        oneShot
    };

    juce::int64 lengthInSamples = 0;
    double sampleRate = 0.0;
    int numChannels = 0;
    double loopBeats = 0.0; // as a loop, how many beats it lasts; 0 for a sound that isn't one
    LoopData loopData = LoopData::none;
    std::optional<SoundKey> key {}; // the root note its loop data name, with Apple loop data's minor or major

    bool isValid() const noexcept { return sampleRate > 0.0; }
    double getSeconds() const noexcept { return sampleRate > 0.0 ? (double) lengthInSamples / sampleRate : 0.0; }
};

// A file's, read once for each time it was written (by its path, its size and when it was last changed); one that isn't
// there, or that no format reads, is invalid.
AudioFileInfo getAudioFileInfo (const juce::File&);

// What a reader tells of its file, with the file's name: its beats as a loop come from its loop data (the beats a WAV file's
// acid chunk or an AIFF file's Apple loop data say, unless they say it plays once), or else from a tempo in its name at
// which it lasts whole bars of four beats, when it lasts from one second to a minute; and the key its loop data say. From
// any thread.
AudioFileInfo readAudioFileInfo (const juce::AudioFormatReader&, const juce::String& name);

// A tempo in a name ("Break 120 BPM", "loop_128bpm"): a number from 50 to 250 just before or after "bpm".
std::optional<double> getTempoFromName (const juce::String&);

// A key in a name, as sample packs write them: a note from A to G with a sharp or a flat ("F#", "Bb"), and "m", "min",
// "minor", "maj" or "major" after it or as the next word ("Am", "C#min", "F# Minor", "Cmaj7"). A note alone counts where the
// name sets it apart: as its last word ("808 Long F"), in brackets ("Kick (F)"), with a sharp or a flat, or after "key" or
// "root" ("Key of G"); notesAlone false counts none of those, for a folder's name. A capital M is no minor ("FM Bass"), nor
// is a word that merely starts with a note ("Ambient", "Bass"). The first key with minor or major wins, else the first note.
std::optional<SoundKey> getKeyFromName (const juce::String&, bool notesAlone = true);

// Whether two names say the same of a sound: the same words, whatever their capitals and whatever is between them
// ("Shaker_Loop", "shaker loop"); or the same words but for a tempo (getTempoFromName()) and a key (getKeyFromName()) that
// only one of them gives, or both alike ("Shaker 82 BPM", "Shaker"), with words besides those. Names with a word the other
// hasn't ("Shaker 2", "Shaker") or with different tempos or keys ("Vox Am", "Vox Em") aren't alike.
bool areNamesAlike (const juce::String&, const juce::String&);

// The formats Phazite reads audio files in: JUCE's (WAV, AIFF, FLAC, Ogg Vorbis, MP3 and what the system adds), from any
// thread.
juce::AudioFormatManager& getAudioFormats();
} // namespace phazite

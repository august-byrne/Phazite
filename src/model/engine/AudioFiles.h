// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite reads of an audio file without playing it: how long it is, at what rate and in how many channels, and
// whether it is a loop that knows its beats; with the readers of the formats it reads. Message thread only.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

namespace phazite
{
struct AudioFileInfo
{
    juce::int64 lengthInSamples = 0;
    double sampleRate = 0.0;
    int numChannels = 0;
    double loopBeats = 0.0; // as a loop, how many beats it lasts; 0 for a sound that isn't one

    bool isValid() const noexcept { return sampleRate > 0.0; }
    double getSeconds() const noexcept { return sampleRate > 0.0 ? (double) lengthInSamples / sampleRate : 0.0; }
};

// A file's, read once for each time it was written (by its path, its size and when it was last changed); one that isn't
// there, or that no format reads, is invalid.
AudioFileInfo getAudioFileInfo (const juce::File&);

// What a reader tells of its file, with the file's name: its beats as a loop come from its loop data (the beats a WAV file's
// acid chunk or an AIFF file's Apple loop data say, unless they say it plays once), or else from a tempo in its name at
// which it lasts whole bars of four beats, when it lasts from one second to a minute.
AudioFileInfo readAudioFileInfo (const juce::AudioFormatReader&, const juce::String& name);

// A tempo in a name ("Break 120 BPM", "loop_128bpm"): a number from 50 to 250 just before or after "bpm".
std::optional<double> getTempoFromName (const juce::String&);

// The formats Phazite reads audio files in: JUCE's (WAV, AIFF, FLAC, Ogg Vorbis, MP3 and what the system adds), from any
// thread.
juce::AudioFormatManager& getAudioFormats();
} // namespace phazite

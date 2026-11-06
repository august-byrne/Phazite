// SPDX-License-Identifier: AGPL-3.0-or-later
// Hearing a sound before it goes into the song, from the browser (docs/design.md, section 11). A loop plays in time
// with the project, at its tempo, stretched as a clip of it would be, by Rubber Band (internal/PreviewLoop.h), and at its
// own pitch (in key later). A one-shot plays as it is. A loop is known by its loop data, or by a tempo in its name ("Break
// 120 BPM") when it lasts whole bars at that tempo. Each sound plays as a plan of its own on Phazite's engine for
// previews, whose sound is added to the song's (model/playback/internal/PlaybackHost.h);
// while the project plays, a loop keeps in step with its bars, and starting or stopping the project stops it.
// A MIDI file plays on Phazite's synth, in time, as a loop over whole bars. It also plays Phazite's startup sound
// (StartupSound.h). The app makes it (internal/Preview.h).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>

namespace phazite
{
class ProjectContext;

class Previewer
{
public:
    ~Previewer();

    // Plays a sound, in time with the open project (or as it is, without one), in place of the sound playing before: a
    // loop over and over, unless repeat is false (a riser made for some bars plays once, in time). Returns false if the
    // file can't be played.
    bool play (const juce::File&, const ProjectContext&, bool repeat = true);

    // Plays a sound made in memory, at a sample rate, once and as it is, in place of the sound playing before: Phazite's
    // startup sound (StartupSound.h). Returns false if it is empty.
    bool playSound (juce::AudioBuffer<float>, double sampleRate);

    // The sample rate previews play at: the audio output's, once one has opened.
    double getSampleRate() const noexcept;

    void stop();

    bool isPlaying() const;
    juce::File getFile() const; // the sound playing, or none

    // Whether the sound is a loop that keeps the project's tempo.
    bool isInTime() const noexcept;

    // Keeps a loop started while the project plays on its bars, and stops the sound once the project starts or stops (the
    // transport plays the song). Call it regularly, from a timer.
    void keepInTime (const ProjectContext&);

    // How loud previews play, as a fader position from 0 to 1.
    void setVolume (float faderPosition);
    float getVolume() const noexcept;

    struct Impl; // what plays the previews (internal/Preview.h)
    explicit Previewer (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Previewer)
};
} // namespace phazite

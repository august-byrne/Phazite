// SPDX-License-Identifier: AGPL-3.0-or-later
// Hearing a sound before it goes into the song, from the browser (docs/design.md, section 11). A loop plays in time
// with the project, at its tempo, stretched as a clip of it would be, by Rubber Band (internal/PreviewLoop.h), and at its
// own pitch (in key later). A one-shot plays as it is. Tracktion knows a loop by its loop data, or by a tempo in its name
// ("Break 120 BPM") when it lasts whole bars at that tempo. Each sound plays in a small Edit of its own (Tracktion's
// preview Edit), which the engine mixes with the project's; while the project plays, a loop keeps in step with its bars.
// A MIDI file plays on Phazite's synth, in time, as a loop. Without Tracktion here (architecture phase A3): the app makes
// it (internal/Preview.h).
#pragma once

#include <juce_core/juce_core.h>

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
    void stop();

    bool isPlaying() const;
    juce::File getFile() const; // the sound playing, or none

    // Whether the sound is a loop that keeps the project's tempo.
    bool isInTime() const noexcept;

    // Keeps a loop on the open project's bars once the project starts playing. Call it regularly, from a timer.
    void keepInTime (const ProjectContext&);

    // How loud previews play, as a fader position from 0 to 1.
    void setVolume (float faderPosition);
    float getVolume() const noexcept;

    struct Impl; // the Engine, and the preview Edit (internal/Preview.h)
    explicit Previewer (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Previewer)
};
} // namespace phazite

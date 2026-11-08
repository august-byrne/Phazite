// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's sampler (roadmap step 4.6; docs/design.md, section 9): one sound played across the keys, as Ableton's Simpler,
// Logic's Quick Sampler and FL Studio's Sampler and Slicex play one. Three modes:
// - Classic: each note plays the sound at its pitch against the root key, from its start marker to its end, for as long as
//   it is held, through an amp envelope; a loop, crossfaded so that its seam doesn't click, holds it on.
// - One-Shot: each note plays it from its start to its end, however short the note (Trigger) or while it is held (Gate),
//   with a fade in and a fade out, as a drum pad does.
// - Slice: the sound cut where its hits begin (by its transients, a sensitivity choosing how many), into beats, into equal
//   parts, or only where the user put slices; slices put by hand stay whatever slices it, and those it found that were
//   taken away stay away. Each slice is on a note of its own from note 36 up, played at its own pitch, with the fades,
//   triggered or gated, to its end or on through the sound, each cutting off the one before unless choke is off.
// Then a resonant filter of any of the types of dsp/MultimodeFilter.h with an envelope of its own, one LFO, and 16 voices,
// or one in mono mode, legato, gliding between the keys held without starting the sound again, as an 808 played as a
// bass line glides. The sound reads between its samples by a windowed sinc whose cutoff follows its pitch
// (dsp/SincReader.h), forward or reversed. The model loads the sound, with where its hits begin found as it loads
// (SampleAnalysis.h), and the sampler swaps it in without the audio thread ever waiting or freeing memory.
#pragma once

#include "SampleAnalysis.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/SincReader.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace phazite::devices
{
class Sampler final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteSampler";
    static constexpr int maxVoices = 16;
    static constexpr int firstSliceNote = 36, maxSlices = 64; // slices on the notes from 36 up, as a drum sampler's pads

    // In the order of the description's parameters, as the editor shows them. Positions in the sound (the markers) are parts
    // of its length, from 0 to 1.
    enum Parameter
    {
        mode, // a Mode
        root, // the note that plays the sound at its own pitch
        transpose,
        fine,
        start,
        end,
        reverse,
        loopOn,
        loopStart,
        loopEnd,
        loopCrossfade, // a part of the loop's length, crossfaded at its seam
        playback,      // One-Shot's and Slice's: to the end (Trigger), or while the note is held (Gate)
        fadeIn,        // One-Shot's and Slice's
        fadeOut,       // before the end, and as a gated note lets go
        sliceBy,       // a SliceBy
        sensitivity,   // how many of its transients slice it, from the strongest only to all of them
        sliceDivision, // a note value (dsp/NoteValues.h), slicing by beats
        sliceCount,    // slicing into equal parts
        playThrough,   // a slice plays on past its end, to the end of the sound
        choke,         // a slice cuts off the one playing
        attack,        // Classic's amp envelope
        decay,
        sustain,
        release,
        attackCurve, // how each stage bends (dsp::Adsr::setCurves())
        decayCurve,
        releaseCurve,
        filterOn,
        filterType, // a dsp::FilterMode
        cutoff,
        resonance,
        filterSlope,
        keyTracking,
        filterEnvelope,
        filterEnvelopeOn,
        filterAttack,
        filterDecay,
        filterSustain,
        filterRelease,
        filterAttackCurve,
        filterDecayCurve,
        filterReleaseCurve,
        lfoOn,
        lfoShape,
        lfoTarget,
        lfoRate,
        lfoDepth,
        voiceMode,
        glide,
        bendRange,
        velocity,
        level
    };

    enum class Mode
    {
        classic,
        oneShot,
        slice
    };

    enum class SliceBy
    {
        transients,
        beats,
        equal,
        manual // only the slices put by hand
    };

    enum class LfoTarget
    {
        pitch,  // up to a semitone either way
        cutoff, // up to two octaves either way
        level   // tremolo
    };

    static const DeviceInfo info;

    // A sound it plays, at its own sample rate, in mono or stereo, with where its hits begin (SampleAnalysis.h) and, for a
    // loop that says, how many beats it lasts (0 for one that doesn't); and the sound an octave, two and three octaves down,
    // each at half the rate of the one before (dsp::SincReader::halve()), which a voice pitched up an octave or more reads,
    // stepping over fewer samples for the same sound (makeOctaves()).
    static constexpr int numOctaves = 3;

    struct Sound
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 48000.0;
        std::vector<Onset> onsets;
        double beats = 0.0;
        std::array<juce::AudioBuffer<float>, numOctaves> octaves;
    };

    // Makes a sound's octaves from its audio, before anything plays it. Not on the audio thread: a minute of stereo sound
    // takes about 90 ms, less than half the time detectOnsets() takes.
    static void makeOctaves (Sound&);

    // The slices the user put in by hand, and where the slices it found were taken away, as parts of the sound's length from
    // 0 to 1, each in order.
    struct ManualSlices
    {
        std::vector<float> added, removed;
    };

    // Where the sound's slices are, in its samples: count slices, each from bounds[i] to bounds[i + 1]; and which were put
    // in by hand.
    struct Slices
    {
        std::array<int, maxSlices + 1> bounds {};
        std::array<bool, maxSlices + 1> byHand {};
        int count = 0;
    };

    // The slices of what a sound plays (from its start marker to its end) for the parameters' values: at the transients as
    // strong as the sensitivity takes, each beat division at the sound's own tempo (or the song's, for a sound that doesn't
    // say how many beats it lasts), in equal parts or none, less those taken away, with those put in by hand; at most
    // maxSlices, the first at the start. Two closer than samplesApart are one. Real-time safe: the audio thread finds a
    // note's slice with it, the editor draws them and the model edits and writes them.
    static Slices getSlices (const Sound&, const ManualSlices*, std::span<const float> parameters, double tempo) noexcept;
    static constexpr int samplesApart = 64;

    Sampler();
    ~Sampler() override;

    // Message thread. What it plays: a sound (nullptr: its default, the first of its factory sounds, FactorySamples.h, which
    // a new sampler plays), and the slices changed by hand (nullptr: none). What it no longer plays is let go of by a later
    // call, once the audio thread can't be using it.
    void setSound (std::shared_ptr<const Sound>);
    void setManualSlices (std::shared_ptr<const ManualSlices>);

    // Message thread: the sound and the slices changed by hand it plays now, for drawing them; the slices nullptr for none.
    const Sound* getSound() const noexcept { return playingSound.get(); }
    const ManualSlices* getManualSlices() const noexcept { return playingSlices.get(); }

    // Plays a note at the next block as if it came in, and lets it go: for the editor (a slice clicked, the sound played).
    // Message thread, lock-free. One that waited longer than longestAuditionWait for a block is dropped, as the drum
    // sampler's pads are.
    void audition (int note, float velocity);
    void stopAudition (int note);
    bool isWaitingToPlay() const noexcept override;

    // How many voices are sounding, for tests and meters. Any thread.
    int getActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

    // What its editor shows playing, once a block (devices/core/LiveValue.h): where each voice is in the sound (from 0 to 1,
    // below 0 for a voice that plays nothing); the newest voice's amp and filter envelopes (from 0 to 1), the cutoff it
    // plays (in hertz; 0 while none plays or the filter is off) and its slice (below 0 for none); where the LFO is in its
    // cycle; and the song's tempo, which slicing by beats goes by for a sound that doesn't say its own.
    struct Shown
    {
        std::array<LiveValue, maxVoices> positions;
        LiveValue amp, filterEnvelope, cutoff, slice { -1.0f }, lfoPhase, tempo { 120.0f };

        Shown()
        {
            for (auto& position : positions)
                position.set (-1.0f);
        }
    };

    const Shown& getShown() const noexcept { return shown; }

    // The filter's response at a frequency, as a gain, for the parameters' values with its cutoff at a frequency: for its
    // editor.
    static double getFilterMagnitude (std::span<const float> parameters, double cutoffHertz, double frequency, double sampleRate);

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override;

private:
    static constexpr int controlStep = 16; // samples between updates of pitch, filter and LFO
    static constexpr int maxHeld = 16;     // keys mono mode remembers, to go back to

    struct Voice
    {
        int note = -1;
        bool held = false;      // its key is down
        bool sustained = false; // its key is up but the sustain pedal holds it
        bool oneShot = false;   // plays to its end, whatever its note does (One-Shot's or a slice's Trigger)
        bool fades = false;     // its fades shape it rather than the amp envelope (One-Shot, Slice)
        const Sound* sound = nullptr;
        int slice = -1; // the slice it plays, in Slice mode

        // What it plays, in samples of its sound counted the way it plays them (from the end back, reversed): from its
        // start to its end, the loop between loopStart and loopEnd, its seam crossfaded over fadeLength before loopEnd.
        bool reversed = false, loops = false;
        double position = 0.0, regionStart = 0.0, regionEnd = 0.0;
        double loopStartAt = 0.0, loopEndAt = 0.0, fadeLength = 0.0;
        double step = 1.0; // samples of its sound for each one it plays, set each control step
        int fadeIn = 0;    // samples of a short fade at a start in the middle of the sound, so that it doesn't click

        float velocityGain = 1.0f;
        float pitch = 60.0f, target = 60.0f; // in semitones, gliding to the target
        float slidePerSample = 0.0f;         // while it slides there evenly (a slide or portamento note): semitones a sample
        std::uint64_t started = 0;           // when it began, so that the oldest is taken first
        dsp::Adsr amp, filterEnvelope;
        std::array<dsp::MultimodeFilter, 2> filters;
        dsp::FilterSettings filterSettings {};
        bool filterIsSet = false;
        float filterEnvelopeLevel = 0.0f;
        float fade = 1.0f, fadeStep = 0.0f; // fading out: taken for another note, its sound swapped, or cut as playback stopped
        int age = 0;                        // the samples since its note began (up to a limit)
    };

    struct Retired
    {
        std::shared_ptr<const void> held;
        std::uint64_t safeAfter = 0; // blocks done after which the audio thread can't be using it
    };

    double sampleRate = 44100.0;
    static constexpr double stealFadeSeconds = 0.003; // a voice taken for another note fades out over this
    static constexpr double stopFadeSeconds = 0.01;   // a note that had only just begun when playback stopped, over this
    static constexpr double justBegunSeconds = 0.1;   // how young that is
    static constexpr double portamentoSeconds = 0.1;  // a portamento note's glide while Glide is off
    static constexpr int declickSamples = 32;         // a fade at a start or an end in the middle of the sound

    // The last fades out what a voice played when another note took it (stealVoice()); notes never start on it.
    std::array<Voice, maxVoices + 1> voices;
    std::array<int, maxHeld> heldKeys {}; // mono mode's keys down, oldest first
    int numHeld = 0;
    std::uint64_t notesPlayed = 0;
    float lastNote = -1.0f; // where a new note glides from
    float bend = 0.0f;
    bool pedalDown = false, songPlaying = false;
    int shownMode = -1;       // the mode and the voices' mode as the last block played
    double songTempo = 120.0; // as the block being processed began
    dsp::Lfo lfo;
    dsp::SincReader reader;
    std::atomic<float> tailSeconds { 0.05f };
    std::atomic<int> activeVoices { 0 };
    Shown shown;

    // What the audio thread plays, and the message thread's own of it, with what it let go of until the audio thread can't
    // be using it.
    std::atomic<const Sound*> sound { nullptr };
    std::atomic<const ManualSlices*> manualSlices { nullptr };
    std::atomic<std::uint64_t> blocksDone { 0 };
    std::shared_ptr<const Sound> playingSound;
    std::shared_ptr<const ManualSlices> playingSlices;
    std::vector<Retired> retired;

    // The notes the editor played and let go of, each with when (juce::Time::getMillisecondCounter()).
    struct Audition
    {
        int note = -1;
        float velocity = 0.0f; // 0 lets it go
        juce::uint32 playedAt = 0;
    };

    static constexpr int auditionSlots = 64;
    static constexpr int longestAuditionWait = 250; // ms: longer than an audio device's longest blocks last
    juce::AbstractFifo auditionFifo { auditionSlots };
    std::array<Audition, auditionSlots> auditions {};

    void freeRetired();
    void handle (const NoteEvent&, std::span<const float> parameters);
    void startNote (int note, float velocity, std::span<const float> parameters, bool portamento = false);
    void startSlice (int note, float velocity, std::span<const float> parameters);
    void placeVoice (Voice&, const Sound&, int from, int to, std::span<const float> parameters, bool loops) const noexcept;
    void startVoice (Voice&, float velocity, std::span<const float> parameters);
    void slideTo (int note, float seconds);
    void stopNote (int note, std::span<const float> parameters);
    void releaseVoice (Voice&);
    Voice& findVoiceFor (int note);
    void stealVoice (Voice&);
    void readAt (const Voice&, double position, float& left, float& right) const noexcept;
    void render (float* left, float* right, int numSamples, std::span<const float> parameters);
};
} // namespace phazite::devices

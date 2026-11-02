// SPDX-License-Identifier: AGPL-3.0-or-later
// How tests hear a song, and tell two engines apart (docs/tracktion-migration.md, T0): the song rendered offline, always
// the same way, through an engine; the render's fingerprint, the RMS and peak level of each 100 ms block on each side; and
// fingerprints compared, with a golden one (tests/golden/renders/) and with another engine's. Levels may be toleranceDb
// apart, and a level below floorDb counts as floorDb: that absorbs the differences between compilers' maths libraries,
// and where a reverb's tail is cut off. A fingerprint doesn't notice a change of phase only, or of frequency at the same
// level: a golden render's check (GoldenRender) looks at what it shows. Golden renders never use anything random
// (Phazite's devices seed their noise and random phases). GoldenFiles.h says how to regenerate the golden files.
#pragma once

#include "GoldenFiles.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
class ProjectContext;
}

namespace phazite::test
{
inline constexpr float toleranceDb = 0.05f; // how far a level may be from the golden one
inline constexpr float floorDb = -70.0f;    // a lower level counts as this: the fingerprints don't compare what is quieter
inline constexpr int renderSampleRate = 44100;
inline constexpr int renderBlockSize = 512;
inline constexpr int fingerprintBlockSamples = 4410; // 100 ms
inline constexpr int renderChannels = 2;
inline constexpr int maxListedDifferences = 20; // the most lines a comparison lists

//==============================================================================
// The RMS and peak level of a block, in dBFS, on each side (left, right).
struct BlockLevels
{
    std::array<float, renderChannels> rms {}, peak {};
};

// A render's fingerprint: the levels of each block, in order.
using Fingerprint = std::vector<BlockLevels>;

// The fingerprint of stereo audio, in blocks of so many samples (100 ms at its rate).
Fingerprint getFingerprint (const juce::AudioBuffer<float>&, int blockSamples = fingerprintBlockSamples);

// A fingerprint as its golden file holds it: JSON, with what it plays, its rate and block size, and one block on each
// line, its levels with two decimals.
juce::String writeFingerprint (const Fingerprint&, const juce::String& about, int sampleRate = renderSampleRate,
                               int blockSamples = fingerprintBlockSamples);

// The differences between the texts of two fingerprints, the golden one (or the reference engine's) and another, at most
// maxListedDifferences lines:
//   sampleRate: expected 44100, got 48000
//   1.20-1.30 s, left RMS: expected -14.20 dBFS, got -13.10 dBFS
// Two levels are the same if they are at most toleranceDb apart, a level below floorDb counting as floorDb. Fingerprints
// that differ in their sample rate, block size, channels or number of blocks aren't compared block by block.
juce::StringArray compareFingerprints (const juce::String& expectedText, const juce::String& actualText);

// A level as fingerprints write it, in dBFS with two decimals; and a block's two sides, "left -20.00 dBFS, right ...".
juce::String formatLevel (double level);
juce::String describeLevels (const std::array<float, renderChannels>&);

// The loudest RMS of a block's two sides, and of a range of blocks [first, end).
float getLouder (const BlockLevels&);
float getLoudest (const Fingerprint&, size_t first, size_t end);

// Whether two fingerprints of the same length differ somewhere by more than so many decibels (a block's RMS on a side,
// each level below floorDb counting as floorDb).
bool differSomewhere (const Fingerprint&, const Fingerprint&, float decibels);

// The level of both sides of a block together, in dBFS: what panning moves from one side to the other doesn't change it.
float getPower (const BlockLevels&);

// For a golden render's check: one CHECK_MESSAGE each, which says why the render should sound so and what it does. That
// one block is louder than another by at least so many decibels (both sides together); that a block's level is within a
// range; that the levels of a range of blocks [first, end) swing by at least so many decibels.
bool isLouder (const Fingerprint&, size_t louder, size_t quieter, float decibels, const juce::String& why);
bool isBetween (const Fingerprint&, size_t block, float low, float high, const juce::String& why);
bool swings (const Fingerprint&, size_t first, size_t end, float decibels, const juce::String& why);

// How far apart two renders are, sample by sample: the loudest difference between them on either side, in dBFS, -100 for
// none (which a fingerprint can't tell: a shift of a few samples, a change of phase), and where it is, in seconds. Renders
// of different lengths differ at the shorter one's end.
struct Difference
{
    float peakDb = -100.0f;
    double seconds = 0.0;
};

Difference getDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, double sampleRate);

// The frequency of a tone in a stretch of a side of audio, in hertz, from how often it crosses zero.
double estimateFrequency (const juce::AudioBuffer<float>&, int channel, double sampleRate, double startSeconds, double seconds);

// Keeps audio that doesn't sound as it should, or doesn't match its golden file, as
// <temp>/PhaziteGoldenActual/renders/<name>.wav, to listen to.
void keepRender (const juce::AudioBuffer<float>&, double sampleRate, const juce::String& name);

//==============================================================================
// An engine that plays the open project's song, offline: Phazite's.
class RenderEngine
{
public:
    virtual ~RenderEngine() = default;
    virtual juce::String getName() const = 0;

    // The song's first seconds (a whole number of 100 ms blocks): at renderSampleRate, in stereo, in blocks of
    // renderBlockSize samples, through the master's devices, as 32-bit floats without dither.
    virtual juce::AudioBuffer<float> render (ProjectContext&, double seconds) = 0;
};

// The open project's song rendered by the reference engine, as a fingerprint: what a golden render's check can compare a
// change with ("the modulator moves its sound").
Fingerprint renderFingerprint (ProjectContext&, double seconds);

// The two renders the harness compares, both through Phazite's engine (model/playback/internal/PlaybackSession.h:
// renderWithOwnEngine()): the reference, of the open project, whose renders the golden files hold; and the candidate, of
// a project opened afresh from a copy of the song alone, as a save holds it, in a context of its own, which shows that the
// song alone makes what it plays, and that nothing a render leaves behind (where a modulator was) plays in the next.
std::unique_ptr<RenderEngine> makeReferenceEngine();
std::unique_ptr<RenderEngine> makeCandidateEngine();

//==============================================================================
// A golden render: the open project's song rendered for some seconds, whose render is checked before it is compared with
// its golden file, so that a broken render (silent, or with the feature it shows not working) fails there and never
// becomes the golden file: check() says whether it sounds as it should, with CHECK_MESSAGEs that say why not.
struct GoldenRender
{
    juce::String name;  // its golden file: tests/golden/renders/<name>.json
    juce::String about; // what it plays, which the golden file says
    double seconds = 4.0;
    std::function<bool (const Fingerprint&, const juce::AudioBuffer<float>&)> check;
};

// Renders the song through the reference engine, checks the render and compares it with the golden file (which
// PHAZITE_UPDATE_GOLDEN=1 rewrites); then through the candidate engine, checks that render and compares it with the
// reference's. A render that fails is kept (keepRender()), named after the golden file and the engine.
void checkGoldenRender (ProjectContext&, const GoldenRender&);

// The same for a golden fingerprint of audio made otherwise (a render in real time, an export's file): checks it and
// compares it with tests/golden/renders/<name>.json.
void checkGoldenAudio (const juce::AudioBuffer<float>&, double sampleRate, const GoldenRender&);

// A golden render of a change: renders the song as it is (with the reference engine), makes the change, and checks the
// song as a golden render whose check is given the fingerprints from before and after the change, so that it can ask the
// change to do what it should ("the modulator moves the sound", "the effect changes it").
using ChangeCheck = std::function<bool (const Fingerprint& before, const Fingerprint& after)>;
void checkGoldenChange (ProjectContext&, const std::function<void()>& change, GoldenRender, const ChangeCheck&);

// For a change's check: that a block is louder by at least so many decibels than it was before the change (or, with a
// negative number, quieter by at least as many), both sides together.
bool changesLevel (const Fingerprint& before, const Fingerprint& after, size_t block, float decibels, const juce::String& why);
} // namespace phazite::test

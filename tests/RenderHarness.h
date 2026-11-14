// SPDX-License-Identifier: AGPL-3.0-or-later
// How tests hear a song: the song rendered offline, always the same way, through Phazite's engine, and played live
// through the app's engine a block at a time (EnginePlayback.h), which must sound as the render does; what was played as
// a fingerprint; and fingerprints compared, with a golden one (tests/golden/renders/) and with another's.
//
// A fingerprint holds, for each 100 ms block:
//   rms, peak   the level of each side (left, right), in dBFS
//   bands       the level of both sides together in each of ten bands, in dBFS: below 44 Hz, the eight octaves around
//               62.5 Hz, 125 Hz... 8 kHz, and above 11.3 kHz (each band's edges 24 dB an octave), so that a filter that
//               moved, or a sound an octave out, shows where the level as a whole stays the same
//   hertz       the frequency each side's power lies around (the root of its mean square frequency: a tone's own
//               frequency, and for any other sound a number that goes up and down with its pitch and its brightness)
//   centre      where in the block each side's power lies, in milliseconds from the block's start: a hit that comes
//               early or late moves it
// How far two fingerprints may be apart: levels and bands toleranceDb, a level below floorDb counting as floorDb; and
// where a side is above floorDb in both, its frequency toleranceHertzShare of itself and toleranceHertz, and its centre
// toleranceMilliseconds. That absorbs the differences between compilers' maths libraries, and where a reverb's tail is cut
// off. A fingerprint doesn't notice a change of phase only (a side upside down), a steady sound a few samples late, or a
// pitch that moves less than a tenth of a semitone: a golden render's check (GoldenRender) looks at what it shows. Golden
// renders never use anything random (Phazite's devices seed their noise and random phases). GoldenFiles.h says how to
// regenerate the golden files.
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
inline constexpr float toleranceDb = 0.05f;           // how far a level may be from the golden one
inline constexpr float floorDb = -70.0f;              // a lower level counts as this: the fingerprints don't compare what is quieter
inline constexpr double toleranceHertzShare = 0.005;  // how far a frequency may be from the golden one: half a percent of it
inline constexpr double toleranceHertz = 0.1;         // and what writing it with one decimal does to it
inline constexpr double toleranceMilliseconds = 0.03; // how far a centre may be: a little more than a sample at 44.1 kHz
inline constexpr int renderSampleRate = 44100;
inline constexpr int renderBlockSize = 512;
inline constexpr int fingerprintBlockSamples = 4410; // 100 ms
inline constexpr int renderChannels = 2;
inline constexpr int numBands = 10;
inline constexpr int maxListedDifferences = 20; // the most lines a comparison lists

// Where each band ends and the next starts, in hertz: half an octave under 62.5 Hz, and each octave up from there.
std::array<double, numBands - 1> getBandEdges();

//==============================================================================
// A block's levels in dBFS: the RMS and peak of each side (left, right) and each band's of both sides together; and each
// side's frequency, in hertz, and centre, in milliseconds (both 0 for a silent side).
struct BlockLevels
{
    std::array<float, renderChannels> rms {}, peak {};
    std::array<float, numBands> bands {};
    std::array<float, renderChannels> hertz {}, centre {};
};

// A render's fingerprint: the levels of each block, in order.
using Fingerprint = std::vector<BlockLevels>;

// The fingerprint of stereo audio, in blocks of so many samples, 100 ms at its rate (which is ten times as many).
Fingerprint getFingerprint (const juce::AudioBuffer<float>&, int blockSamples = fingerprintBlockSamples);

// A fingerprint as its golden file holds it: JSON, with what it plays, its rate and block size, and one block on each
// line, its levels with two decimals.
juce::String writeFingerprint (const Fingerprint&, const juce::String& about, int sampleRate = renderSampleRate,
                               int blockSamples = fingerprintBlockSamples);

// The differences between the texts of two fingerprints, the golden one (or the reference engine's) and another, at most
// maxListedDifferences lines:
//   sampleRate: expected 44100, got 48000
//   1.20-1.30 s, left RMS: expected -14.20 dBFS, got -13.10 dBFS
//   1.20-1.30 s, 8 kHz band: expected -41.20 dBFS, got -38.35 dBFS
//   1.20-1.30 s, left frequency: expected 220.0 Hz, got 233.1 Hz
//   1.20-1.30 s, left centre: expected 12.40 ms, got 12.52 ms
// Two levels are the same if they are at most toleranceDb apart, a level below floorDb counting as floorDb; frequencies
// and centres are compared where the side's RMS is above floorDb in both, within their tolerances. Fingerprints that
// differ in their sample rate, block size, channels, bands or number of blocks aren't compared block by block.
juce::StringArray compareFingerprints (const juce::String& expectedText, const juce::String& actualText);

// The differences between what two stretches of audio at a rate sound like, as their fingerprints tell: the first what is
// expected ("a render"), the second what should sound as it does ("the song played live").
juce::StringArray compareAudio (const juce::AudioBuffer<float>& expected, const juce::AudioBuffer<float>& actual, double sampleRate);

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

// Writes a WAV file (and the folders it is in) of a hit, and returns it: 50 ms of a 440 Hz tone at half of full scale, in and
// out over 5 ms, and nothing after it. For a song that shows what rings on after a sound: what follows the hit is the
// song's devices' alone.
juce::File writeHit (const juce::File&);

// Keeps audio that doesn't sound as it should, or doesn't match its golden file, as
// <temp>/PhaziteGoldenActual/renders/<name>.wav, to listen to.
void keepRender (const juce::AudioBuffer<float>&, double sampleRate, const juce::String& name);

//==============================================================================
// The open project's song from its start, for so many seconds, in stereo, at a rate and in blocks of so many samples:
//
// rendered offline, as an export renders it (model/playback/internal/PlaybackSession.h: renderWithOwnEngine()), the
// clips' renders made first;
//
// and played live through the app's engine, a block at a time, as an audio device would drive it but not in real time
// (EnginePlayback.h), so the same every time: the engine has played half a second while stopped, as the app's has when
// Play is pressed (its devices asleep), then plays the song from its start, what comes out later than a render's taken
// off its front (the song's delay compensation, which a render plays ahead by). Live, devices sleep through silence, the
// files are read ahead by the reading thread and plug-ins keep a deadline, none of which a render does: what plays live
// should sound as the render does all the same. The song mustn't be looping, nor its metronome on (a render plays
// neither), and its project must be the one the engine plays.
juce::AudioBuffer<float> renderOffline (ProjectContext&, double seconds, double sampleRate = renderSampleRate,
                                        int blockSize = renderBlockSize);
juce::AudioBuffer<float> playLive (ProjectContext&, double seconds, double sampleRate = renderSampleRate, int blockSize = renderBlockSize);

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
// reference's; then plays the song live (playLive()), checks what played and compares it with the reference's render too:
// the golden file is the live path's as much as the render's, within the same tolerance. What fails is kept
// (keepRender()), named after the golden file and what played it (-reference, -candidate, -live).
//
// A song that can't sound live as its render does says why (liveDiffers): what it plays live is checked as the renders
// are, but not compared with them. Each is an exception to name where the song is made, never a way round a difference.
void checkGoldenRender (ProjectContext&, const GoldenRender&, const juce::String& liveDiffers = {});

// The same for a golden fingerprint of audio made otherwise (the song played live in a way a render can't play it: looped,
// stopped and started again, with its metronome; an export's file): checks it and compares it with
// tests/golden/renders/<name>.json.
void checkGoldenAudio (const juce::AudioBuffer<float>&, double sampleRate, const GoldenRender&);

// A golden render of a change: renders the song as it is (with the reference engine), makes the change, and checks the
// song as a golden render whose check is given the fingerprints from before and after the change, so that it can ask the
// change to do what it should ("the modulator moves the sound", "the effect changes it").
using ChangeCheck = std::function<bool (const Fingerprint& before, const Fingerprint& after)>;
void checkGoldenChange (ProjectContext&, const std::function<void()>& change, GoldenRender, const ChangeCheck&,
                        const juce::String& liveDiffers = {});

// For a change's check: that a block is louder by at least so many decibels than it was before the change (or, with a
// negative number, quieter by at least as many), both sides together.
bool changesLevel (const Fingerprint& before, const Fingerprint& after, size_t block, float decibels, const juce::String& why);
} // namespace phazite::test

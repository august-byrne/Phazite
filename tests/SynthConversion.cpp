// SPDX-License-Identifier: AGPL-3.0-or-later
// The old Synth's presets on the Wavetable's engine (roadmap step S6.7, part B): a tool, not a test. It converts each preset
// of resources/presets/Synth/ into the Wavetable's by formulas (convert(), each step's comment saying why), writes it as Save
// Preset writes one into resources/presets/Wavetable/<kind>/, and measures the pair, the old preset on the old Synth against
// the new on the Wavetable, both through Phazite's engine as a track plays them (EnginePlayback.h), at 48 kHz, on the same
// phrases: a note held 2 s at C2, C3, C4 and C5 at velocity 100 and let ring until silent; C3 at velocity 50; a chord (C3, E3,
// G3, B3); two legato notes a fifth apart (for presets in Mono or with glide); C3 with the mod wheel at full (for presets
// whose LFO the wheel brings in). Each engine plays them sixteen times, each time from other random phases (its unison's,
// drift's and noise's, which short notes before the phrases move on). For each phrase: the level (RMS and peak, in dB) and
// the long-term spectrum in third octaves from 30 Hz to 16 kHz (the largest difference among the bands within 40 dB of the
// loudest), each averaged over the sixteen; and from the first of them, the level in 10 ms frames (where either is within 40
// dB of the phrase's loudest frame), the spectral centroid in 50 ms frames (in octaves) and the pitch in 50 ms frames by
// the normalized autocorrelation (in cents, where both are steady enough to read). The old Synth against itself, its first
// play against each of the others for the frames, and for the averages 2.5 standard errors of a difference of two averages
// from its sixteen plays' scatter, is its spread: how far random phases alone move each measure, which a difference within its
// bound and that spread is put down to ("unsteady"), not to the conversion. A band counts where it holds a harmonic of a note
// played (every band, for noise) within 40 dB of the old Synth's loudest; under a fundamental of 100 Hz, the level in time
// is in frames of two of its periods. It writes report.md (and with
// PHAZITE_SYNTH_CONVERSION_WAVS set, each pair's renders) to the folder PHAZITE_SYNTH_CONVERSION names, else a temporary
// one, which it names; PHAZITE_PRESET_FILTER limits it to presets whose path holds a text ("Bass/Acid"). Run it from a build
// in the repository with
//   phazite_tests.exe --test-suite=synth-conversion --no-skip --test-case="The old Synth*"
// And the example songs that play the old Synth's presets, measured as their golden render measures a song (their mix and
// each stem: integrated loudness, and the fingerprints' ten bands over the whole song, RenderHarness.h), into
// songs-<PHAZITE_SONGS_LABEL>.json in the same folder ("before" or "after"); with both there, the movement of each:
//   phazite_tests.exe --test-suite=synth-conversion --no-skip --test-case="*songs*"
// ctest and CI skip both.
#include "EnginePlayback.h"
#include "RenderHarness.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/dsp/Oscillators.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ExampleSongs.h"
#include "model/project/Export.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace
{
namespace devices = phazite::devices;
namespace dsp = phazite::devices::dsp;
namespace keys = phazite::schema::preset;
using Old = devices::Synth;
using New = devices::WavetableSynth;
using Audio = juce::AudioBuffer<float>;

constexpr double rate = phazite::test::EnginePlayback::sampleRate; // 48 kHz
constexpr double pi = juce::MathConstants<double>::pi;

juce::String toString (std::string_view text)
{
    return juce::String (text.data(), text.size());
}

std::vector<float> readValues (const devices::DeviceInfo& info, const juce::var& device)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    if (const auto* set = device[keys::parameters].getDynamicObject())
        for (const auto& [id, value] : set->getProperties())
            if (const auto index = devices::findParameter (info, id.toString().toStdString()); index >= 0)
                values[(size_t) index] =
                    devices::constrain (info.parameters[(size_t) index], phazite::readSavedValue (info.parameters[(size_t) index], value));

    return values;
}

double toDecibels (double gain)
{
    return gain > 1.0e-12 ? 20.0 * std::log10 (gain) : -240.0;
}

//==============================================================================
// The conversion.

// The first harmonic's amplitude of what an oscillator of the old Synth plays (devices/dsp/Oscillators.h): a sine from -1 to
// 1, the triangle and the saw as band-limited copies of those from -1 to 1, and the pulse as two saws a width apart.
double getOldFirstHarmonic (dsp::Waveform wave, float width)
{
    switch (wave)
    {
        case dsp::Waveform::sine:
            return 1.0;
        case dsp::Waveform::triangle:
            return 8.0 / (pi * pi);
        case dsp::Waveform::saw:
            return 2.0 / pi;
        case dsp::Waveform::square:
            return 4.0 / pi * std::sin (pi * std::clamp ((double) width, 0.05, 0.95));
    }

    return 1.0;
}

// The first harmonic's amplitude of what a table plays at a position, its two nearest frames crossfaded as the Wavetable
// crossfades them (WavetableSynth::render()), from the copy with all its harmonics.
double getTableFirstHarmonic (const devices::Wavetable& table, float position)
{
    const auto lastFrame = table.getNumFrames() - 1;
    const auto frame = std::clamp (position, 0.0f, 1.0f) * (float) lastFrame;
    const auto first = std::min ((int) frame, lastFrame), next = std::min (first + 1, lastFrame);
    const auto morph = (double) (frame - (float) first);
    const auto a = table.getLevel (first, 0), b = table.getLevel (next, 0);
    std::complex<double> sum;

    for (int i = 0; i < a.length; ++i)
        sum += ((1.0 - morph) * a.samples[i] + morph * b.samples[i]) * std::polar (1.0, -2.0 * pi * i / a.length);

    return 2.0 * std::abs (sum) / a.length;
}

// What a wave of the old Synth's becomes: a frame of Basic Shapes (sine, triangle, saw, square), or for a pulse other than a
// square, or one whose width the LFO moves, the Pulse Width table at the frame of its width; and how much louder the old
// oscillator's first harmonic is than the table's there, which the oscillator's level makes up.
struct OscillatorChoice
{
    juce::String table;
    float position = 0.0f;
    double gain = 1.0;
};

OscillatorChoice chooseOscillator (int wave, float width, bool widthMoves)
{
    OscillatorChoice choice;
    const auto waveform = (dsp::Waveform) wave;

    // Even a pulse within a hair of 50 %: Basic Shapes' square has none of its even harmonics, whose lowest stand alone in
    // their bands (its 4th, 24 dB under the fundamental at 50.5 %, 18 to 34 dB off there); the table's crossfade between its
    // frames 1.5 % apart is right up to about the 30th harmonic (2.5 dB off in a band above it at 50.5 %).
    if (waveform == dsp::Waveform::square && (widthMoves || std::abs (width - 0.5f) > 1.0e-4f))
    {
        choice.table = "Pulse Width";
        choice.position = std::clamp (std::abs (width - 0.5f) / 0.48f, 0.0f, 1.0f); // its frames: widths from 50 % to 2 %
    }
    else
    {
        choice.table = "Basic Shapes";
        choice.position = (float) wave / 3.0f;
    }

    const auto table = devices::getFactoryWavetable (devices::findFactoryWavetable (choice.table.toStdString()));
    choice.gain = getOldFirstHarmonic (waveform, width) / getTableFirstHarmonic (*table, choice.position);
    return choice;
}

struct Conversion
{
    std::vector<float> values; // the Wavetable's, in the order of its parameters
    std::array<juce::String, New::numOscillators> tables;
    juce::StringArray notes; // what it carries over only approximately, and why
    double g = 1.0;          // how far the oscillators' levels were divided down, and the Level raised
};

Conversion convert (const std::vector<float>& old)
{
    Conversion c;

    for (const auto& parameter : New::info.parameters)
        c.values.push_back (parameter.defaultValue);

    const auto get = [&old] (Old::Parameter p) { return old[(size_t) p]; };
    const auto isOn = [&old] (Old::Parameter p) { return old[(size_t) p] > 0.5f; };
    const auto set = [&c] (int p, float value) { c.values[(size_t) p] = value; };
    const auto choice = [&old] (Old::Parameter p) { return (int) std::lround (old[(size_t) p]); };

    const auto lfoOn = isOn (Old::lfoOn), filterOn = isOn (Old::filterOn), envelopeOn = isOn (Old::filterEnvelopeOn);
    const auto lfoTarget = (Old::LfoTarget) choice (Old::lfoTarget);
    const auto depth = get (Old::lfoDepth);
    const auto width = get (Old::pulseWidth);
    const auto widthMoves = lfoOn && lfoTarget == Old::LfoTarget::pulseWidth;
    const auto mix = get (Old::oscMix);

    // The oscillators, on the tables that play their waves, at levels that keep the old mix: each oscillator's level is its
    // share of the mix times how much louder the old wave is than the table's; where that comes over 100 %, every level is
    // divided by the largest, G, and the synth's Level raised by as much. The sub and the noise, after the oscillators' levels
    // in the old voice, are divided by G too.
    const std::array waves { choice (Old::osc1Wave), choice (Old::osc2Wave) };
    const std::array oscillators { chooseOscillator (waves[0], width, widthMoves), chooseOscillator (waves[1], width, widthMoves) };
    const std::array shares { (1.0 - mix) * oscillators[0].gain, (double) mix * oscillators[1].gain };
    const auto g = std::max ({ shares[0], shares[1], 1.0 });
    const auto gDecibels = (float) toDecibels (g);
    c.g = g;

    for (int osc = 0; osc < New::numOscillators; ++osc)
    {
        c.tables[(size_t) osc] = oscillators[(size_t) osc].table;
        set (New::getParameter (osc, New::oscOn), shares[(size_t) osc] > 0.0 ? 1.0f : 0.0f);
        set (New::getParameter (osc, New::oscPosition), oscillators[(size_t) osc].position);
        set (New::getParameter (osc, New::oscLevel), (float) (shares[(size_t) osc] / g));
        set (New::getParameter (osc, New::oscPitch), 0.0f);
        set (New::getParameter (osc, New::oscFine), 0.0f);
        set (New::getParameter (osc, New::oscWarp), (float) New::Warp::none);
        set (New::getParameter (osc, New::oscWarpAmount), 0.0f);
    }

    // Osc 2's pitch, or synced, as far as its sync reads it: the Wavetable's Sync reads the wave 2^(3 a) times a cycle, so a
    // ratio of n semitones is an amount of n / 36, with the oscillator itself on the note (its own cycle is the one synced).
    const auto synced = isOn (Old::osc2Sync);
    const auto osc2Semitones = get (Old::osc2Pitch) + 0.01f * get (Old::osc2Detune);

    if (synced)
    {
        set (New::osc2Warp, (float) New::Warp::sync);
        set (New::osc2WarpAmount, std::clamp (osc2Semitones / 36.0f, 0.0f, 1.0f));

        if (osc2Semitones < 0.0f)
            c.notes.add ("osc 2 synced below osc 1, which the Wavetable's Sync can't read (it reads at least once a cycle)");
    }
    else
    {
        set (New::osc2Pitch, get (Old::osc2Pitch));
        set (New::osc2Fine, get (Old::osc2Detune));
    }

    set (New::subLevel, (float) (get (Old::subLevel) / g));
    set (New::subOctave, get (Old::subOctave));
    set (New::subShape, get (Old::subShape));
    set (New::noiseLevel, (float) (get (Old::noiseLevel) / g));

    // The unison, the same formulas; off is one voice. Its copies don't drift on their own: the old Synth's moved with their
    // oscillator, whose Drift is a slot of its own below.
    set (New::unisonVoices, isOn (Old::unisonOn) ? get (Old::unisonVoices) : 0.0f);
    set (New::unisonDetune, get (Old::unisonDetune));
    set (New::unisonSpread, get (Old::unisonSpread));
    set (New::unisonPosition, 0.0f);
    set (New::unisonStack, (float) New::Stack::off);
    set (New::unisonDrift, 0.0f);

    // The filter. Filter Velocity is a slot from Velocity to the cutoff at half of it (96 semitones a slot against the old
    // 48 at full) with the cutoff lowered by as much as velocity 1 raises it, so that velocity 1 plays the cutoff as set and
    // a softer note closes it as the old one did. The drive is raised by G, which lowered what goes into it.
    const auto filterVelocity = filterOn ? get (Old::filterVelocity) : 0.0f;
    const auto cutoff = get (Old::cutoff) * std::exp2 (-4.0f * filterVelocity);
    set (New::filterOn, filterOn ? 1.0f : 0.0f);
    set (New::filterType, get (Old::filterType));
    set (New::cutoff, std::max (20.0f, cutoff));
    set (New::resonance, get (Old::resonance)); // the same curve (dsp::getSynthResonanceQ())
    set (New::filterSlope, get (Old::filterSlope));
    set (New::drive, get (Old::drive) > 0.0f ? std::min (24.0f, get (Old::drive) + gDecibels) : 0.0f);
    set (New::keyTracking, get (Old::keyTracking));

    if (cutoff < 20.0f)
        c.notes.add ("cutoff under 20 Hz once Filter Velocity lowers it");

    // Envelope 1 is the amp, Envelope 2 the filter's envelope, with the same times and curves.
    const std::array<Old::Parameter, New::envFields> amp { Old::attack,      Old::decay,      Old::sustain,     Old::release,
                                                           Old::attackCurve, Old::decayCurve, Old::releaseCurve };
    const std::array<Old::Parameter, New::envFields> filterEnvelope { Old::filterAttack,      Old::filterDecay,       Old::filterSustain,
                                                                      Old::filterRelease,     Old::filterAttackCurve, Old::filterDecayCurve,
                                                                      Old::filterReleaseCurve };

    for (int field = 0; field < New::envFields; ++field)
    {
        set (New::getEnvelopeParameter (0, (New::EnvelopeField) field), get (amp[(size_t) field]));
        set (New::getEnvelopeParameter (1, (New::EnvelopeField) field), get (filterEnvelope[(size_t) field]));
    }

    // LFO 1 is the old LFO, running free (the old one never starts again with a note), coming into each note over its Fade In.
    set (New::lfo1Shape, get (Old::lfoShape));
    set (New::lfo1Rate, get (Old::lfoRate));
    set (New::lfo1Sync, 0.0f);
    set (New::lfo1Retrigger, 0.0f);
    set (New::lfo1FadeIn, get (Old::lfoFade));
    set (New::lfo2Retrigger, 0.0f);

    set (New::voiceMode, get (Old::voiceMode));
    set (New::glide, get (Old::glide));
    set (New::glideMode, get (Old::glideMode));
    // The old Synth starts its oscillators from their waves' starts at each note in Poly, and lets them run on in Mono.
    set (New::phase, (float) (std::lround (get (Old::voiceMode)) == 1 ? New::Phase::free : New::Phase::reset));
    set (New::bendRange, get (Old::bendRange));
    set (New::velocity, get (Old::velocity));
    set (New::level, std::min (6.0f, get (Old::level) + gDecibels));

    if (get (Old::level) + gDecibels > 6.0f)
        c.notes.add ("Level over +6 dB once raised by G");

    // The slots, in this order.
    auto slot = 0;
    const auto addSlot = [&] (New::ModSource source, New::ModTarget target, float amount)
    {
        if (slot >= New::numModSlots)
        {
            c.notes.add ("more than eight slots");
            return -1;
        }

        set (New::getModParameter (slot, New::modSource), (float) source);
        set (New::getModParameter (slot, New::modTarget), (float) target);
        set (New::getModParameter (slot, New::modAmount), std::clamp (amount, -1.0f, 1.0f));
        return slot++;
    };

    // As many slots as an amount beyond one slot's reach takes.
    const auto addSlots = [&] (New::ModSource source, New::ModTarget target, float amount)
    {
        while (std::abs (amount) > 1.0e-6f)
        {
            const auto part = std::clamp (amount, -1.0f, 1.0f);
            addSlot (source, target, part);
            amount -= part;
        }
    };

    for (int s = 0; s < New::numModSlots; ++s)
    {
        set (New::getModParameter (s, New::modSource), (float) New::ModSource::none);
        set (New::getModParameter (s, New::modTarget), (float) New::ModTarget::none);
        set (New::getModParameter (s, New::modAmount), 0.0f);
    }

    // Envelope 3 does one job: the pitch envelope, or a level that comes in over the LFO's Fade In and holds (for an LFO on
    // the level or across a pulse's width, whose middle moves with its depth).
    enum class Envelope3
    {
        unused,
        pitch,
        hold
    };

    auto envelope3 = Envelope3::unused;
    const auto useEnvelope3 = [&] (Envelope3 use)
    {
        if (envelope3 != Envelope3::unused && envelope3 != use)
        {
            c.notes.add ("Envelope 3 wanted for the pitch envelope and the LFO at once");
            return false;
        }

        envelope3 = use;
        return true;
    };

    // The filter envelope on the cutoff, in semitones of the old envelope: a slot's 100 % is 96.
    if (filterOn && envelopeOn && std::abs (get (Old::filterEnvelope)) > 0.0f)
        addSlots (New::ModSource::envelope2, New::ModTarget::cutoff, get (Old::filterEnvelope) / 96.0f);

    // The filter envelope on osc 2: synced, on its warp (36 semitones a slot), otherwise on its pitch (24).
    if (envelopeOn && std::abs (get (Old::osc2Envelope)) > 0.0f)
    {
        if (synced)
            addSlots (New::ModSource::envelope2, New::ModTarget::osc2Warp, get (Old::osc2Envelope) / 36.0f);
        else
            addSlots (New::ModSource::envelope2, New::ModTarget::osc2Pitch, get (Old::osc2Envelope) / 24.0f);
    }

    if (filterVelocity > 0.0f)
        addSlot (New::ModSource::velocity, New::ModTarget::cutoff, filterVelocity / 2.0f);

    // Drift: the old Synth's three wanders, each its own (Synth::moveDrift()), at one pace, toward new places at the same
    // moments: oscillator 1's pitch and oscillator 2's, up to 8 cents each at full (of a slot's 24 semitones), and the cutoff's,
    // up to 2 semitones (of its 96). Each slot from Drift wanders on its own, at that pace. The old sub followed oscillator
    // 1's drift; the Wavetable's sub follows Pitch, which would move both oscillators with it, so it stays on the note.
    if (const auto drift = get (Old::drift); drift > 0.0f)
    {
        if (shares[0] > 0.0)
            addSlot (New::ModSource::drift, New::ModTarget::osc1Pitch, drift * 0.08f / 24.0f);

        if (shares[1] > 0.0 && ! synced)
            addSlot (New::ModSource::drift, New::ModTarget::osc2Pitch, drift * 0.08f / 24.0f);
        else if (shares[1] > 0.0)
            c.notes.add ("synced osc 2's own drift left out (its sync reads from osc 1's cycle)");

        if (filterOn)
            addSlot (New::ModSource::drift, New::ModTarget::cutoff, drift * 2.0f / 96.0f);

        if (get (Old::subLevel) > 0.0f)
            c.notes.add ("the sub stays on the note, where the old one followed osc 1's drift");
    }

    // The pitch envelope: the old one falls exponentially, e^(-t / tau), tau its Pitch Decay. Envelope 3's decay at a curve of
    // -100 % is e^(-10 t / D) less a hair (dsp::Adsr: aiming 4.5e-5 past its target, 87 dB down), landing on 0 at D: so
    // D = 10 tau at -100 % is the old fall within 0.4 dB over its first 60 dB, the least-squares fit (whose best curve is the
    // knob's end); its release, from wherever the key lets go, the same.
    if (std::abs (get (Old::pitchAmount)) > 0.0f && useEnvelope3 (Envelope3::pitch))
    {
        const auto decay = 10.0f * get (Old::pitchDecay);
        set (New::env3Attack, 16.0f / 48.0f); // one control step, so that its first step plays the top, as the old one's does
        set (New::env3Decay, std::min (10000.0f, decay));
        set (New::env3Sustain, 0.0f);
        set (New::env3Release, std::min (10000.0f, decay));
        set (New::env3DecayCurve, -1.0f);
        set (New::env3ReleaseCurve, -1.0f);
        addSlots (New::ModSource::envelope3, New::ModTarget::pitch, get (Old::pitchAmount) / 24.0f);

        if (decay > 10000.0f)
            c.notes.add ("Pitch Decay over 1 s, which Envelope 3's 10 s can't fall over");
    }

    // A level that comes in over the LFO's Fade In, straight, as the old LFO's depth does, and holds: through the release
    // too, its release curve at +100 % keeping it within 0.01 % of its level over the first 10 % of its 10 s.
    const auto holdEnvelope3 = [&]
    {
        if (! useEnvelope3 (Envelope3::hold))
            return false;

        set (New::env3Attack, get (Old::lfoFade));
        set (New::env3Decay, 0.0f);
        set (New::env3Sustain, 1.0f);
        set (New::env3Release, 10000.0f);
        set (New::env3AttackCurve, 0.0f);
        set (New::env3DecayCurve, 0.0f);
        set (New::env3ReleaseCurve, 1.0f);
        return true;
    };

    // The LFO, at its depth, and the mod wheel adding to that depth: the old Synth's depth and the wheel together go up to
    // full, so the wheel's slot moves the LFO's slot's amount by what is left to full, (1 - depth) of the LFO's scale.
    if (lfoOn)
    {
        const auto addWheel = [&] (int lfoSlot, float scale)
        {
            if (lfoSlot >= 0 && depth < 1.0f)
                addSlot (New::ModSource::modWheel, New::getAmountTarget (lfoSlot), (1.0f - depth) * scale);
        };

        switch (lfoTarget)
        {
            case Old::LfoTarget::pitch: // a semitone at full: 1/24 of a slot
                addWheel (addSlot (New::ModSource::lfo1, New::ModTarget::pitch, depth / 24.0f), 1.0f / 24.0f);
                break;

            case Old::LfoTarget::cutoff: // 24 semitones at full: a quarter of a slot
                if (filterOn)
                    addWheel (addSlot (New::ModSource::lfo1, New::ModTarget::cutoff, depth / 4.0f), 0.25f);

                break;

            case Old::LfoTarget::level:
                // The old gain is 1 - d/2 - (d/2) lfo: on the Volume, a held -d/2 from Envelope 3 and the LFO at -d/2, each
                // coming in over the fade, and the wheel taking each to -1/2.
                if (holdEnvelope3())
                {
                    addWheel (addSlot (New::ModSource::envelope3, New::ModTarget::volume, -0.5f * depth), -0.5f);
                    addWheel (addSlot (New::ModSource::lfo1, New::ModTarget::volume, -0.5f * depth), -0.5f);
                }

                break;

            case Old::LfoTarget::pulseWidth:
                // The width w + 0.45 d lfo, on the Pulse Width table's position |w - 0.5| / 0.48, 0.9375 d of a slot either way.
                // A sweep that crosses 50 % plays |w - 0.5| folded: from a square at the middle of a triangle LFO, its fold
                // is a triangle at twice the rate, 1 at the start of the LFO's cycle, so the position is 0.46875 d (1 - tri2)
                // with LFO 2 at twice the rate: a held 0.46875 d from Envelope 3 and LFO 2 at -0.46875 d.
                for (int osc = 0; osc < New::numOscillators; ++osc)
                {
                    if ((dsp::Waveform) waves[(size_t) osc] != dsp::Waveform::square || shares[(size_t) osc] <= 0.0)
                        continue;

                    const auto position = osc == 0 ? New::ModTarget::osc1Position : New::ModTarget::osc2Position;
                    const auto crosses = std::abs (width - 0.5f) < 0.45f * depth;
                    const auto folds =
                        crosses && std::abs (width - 0.5f) < 1.0e-4f && (dsp::LfoShape) choice (Old::lfoShape) == dsp::LfoShape::triangle;

                    if (folds && holdEnvelope3())
                    {
                        set (New::lfo2Shape, (float) dsp::LfoShape::triangle);
                        set (New::lfo2Rate, std::min (20.0f, 2.0f * get (Old::lfoRate)));
                        set (New::lfo2Sync, 0.0f);
                        set (New::lfo2FadeIn, get (Old::lfoFade));
                        addWheel (addSlot (New::ModSource::envelope3, position, 0.46875f * depth), 0.46875f);
                        addWheel (addSlot (New::ModSource::lfo2, position, -0.46875f * depth), -0.46875f);

                        if (2.0f * get (Old::lfoRate) > 20.0f)
                            c.notes.add ("the pulse's sweep folded at twice the LFO's rate, over 20 Hz");

                        continue;
                    }

                    if (crosses)
                        c.notes.add ("the pulse's width swept across 50 %, which the table's position can't follow");

                    const auto side = width < 0.5f ? -0.9375f : 0.9375f;
                    addWheel (addSlot (New::ModSource::lfo1, position, side * depth), side);
                }

                break;
        }

        if ((dsp::LfoShape) choice (Old::lfoShape) == dsp::LfoShape::random)
            c.notes.add ("a random LFO, whose values are drawn from another seed");
    }

    return c;
}

// The conversion as a preset, as Save Preset writes one: put into a device of a song of its own and saved from there.
juce::var makePreset (const Conversion& c)
{
    auto* parameters = new juce::DynamicObject();

    for (size_t i = 0; i < New::info.parameters.size(); ++i)
    {
        const auto& parameter = New::info.parameters[i];

        if (parameter.unit == devices::Unit::choice)
            parameters->setProperty (toString (parameter.id), toString (parameter.choices[(size_t) std::lround (c.values[i])]));
        else
            parameters->setProperty (toString (parameter.id), c.values[i]);
    }

    auto* sounds = new juce::DynamicObject();

    for (int osc = 0; osc < New::numOscillators; ++osc)
        sounds->setProperty (phazite::schema::getOscillatorWavetable (osc), c.tables[(size_t) osc]);

    auto* device = new juce::DynamicObject();
    device->setProperty (keys::name, toString (New::info.name));
    device->setProperty (keys::instrument, true);
    device->setProperty (keys::type, juce::String (New::typeID));
    device->setProperty (keys::parameters, juce::var (parameters));
    device->setProperty (keys::sounds, juce::var (sounds));

    auto* preset = new juce::DynamicObject();
    preset->setProperty (keys::kind, juce::String (keys::deviceKind));
    preset->setProperty (keys::device, juce::var (device));

    const auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song)[0];
    const auto added = phazite::addDeviceFromPreset (*song, phazite::getPluginHost (phazite::test::getEngineContext()), track,
                                                     juce::var (preset), nullptr);
    REQUIRE (added.isValid());
    const auto saved = phazite::makeDevicePreset (*song, added);
    phazite::removeDevice (*song, added);
    return saved;
}

//==============================================================================
// The phrases, at 120 BPM.
struct Note
{
    int pitch;
    double start, length; // in seconds from the phrase's start
    int velocity;
};

struct Phrase
{
    juce::String name;
    std::vector<Note> notes;
    bool pitched = true; // one note at a time, whose pitch can be read
    bool wheel = false;  // the mod wheel at full through it
    double start = 0.0;  // in seconds from the song's start
};

constexpr double held = 2.0;
constexpr int redrawings = 15; // how many times each engine plays again from other random phases, for averages and the spread

std::vector<Phrase> getPhrases (const std::vector<float>& old)
{
    std::vector<Phrase> phrases;

    for (const auto& [name, pitch] : { std::pair { "C2", 36 }, { "C3", 48 }, { "C4", 60 }, { "C5", 72 } })
        phrases.push_back ({ name, { { pitch, 0.0, held, 100 } } });

    phrases.push_back ({ "C3 v50", { { 48, 0.0, held, 50 } } });
    phrases.push_back (
        { "Chord", { { 48, 0.0, held, 100 }, { 52, 0.0, held, 100 }, { 55, 0.0, held, 100 }, { 59, 0.0, held, 100 } }, false });

    if (std::lround (old[Old::voiceMode]) == 1 || old[Old::glide] > 0.0f)
        phrases.push_back ({ "Legato", { { 48, 0.0, 1.0, 100 }, { 55, 0.75, held - 0.75, 100 } } });

    if (old[Old::lfoOn] > 0.5f)
        phrases.push_back ({ "Wheel", { { 48, 0.0, held, 100 } }, true, true });

    return phrases;
}

// Each phrase's length: the notes, the release and a beat to spare, in whole beats.
double getPhraseSeconds (const std::vector<float>& old)
{
    return std::ceil ((held + old[Old::release] * 0.001 + 0.5) / 0.5) * 0.5;
}

// The phrases played from the song's start by an instrument with a preset, on a track as the app plays it; drawn again, with short
// notes first, before the phrases (one more for each redraw), which move on what each of its voices draws from its seeds (its unison's phases,
// its drift, its noise), so that each phrase starts from other random phases while all else is as it was (a free LFO too).
Audio play (const char* typeID, const juce::File& preset, const std::vector<Phrase>& phrases, double phraseSeconds, int redraws = 0)
{
    phazite::test::TestSong song;
    song.setTempo (120.0);
    const auto device = song.setInstrument (song.firstTrack, typeID);
    REQUIRE (phazite::loadPresetInto (song.context, device, preset));
    const auto total = phrases.back().start + phraseSeconds;
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, total * 2.0);
    auto& s = *phazite::findSong (song.context);
    const auto beat = [] (double seconds) { return phazite::BeatPosition::fromBeats (seconds * 2.0); };
    phazite::addControl (s, clip, beat (0.0), phazite::ControlKind::controller, 1, 0);

    for (int each = 0; each < redraws; ++each)
        song.addNote (clip, phrases.front().notes.front().pitch, 0.3 * each, 0.2, 100);

    for (const auto& phrase : phrases)
    {
        for (const auto& note : phrase.notes)
            song.addNote (clip, note.pitch, (phrase.start + note.start) * 2.0, note.length * 2.0, note.velocity);

        if (phrase.wheel)
        {
            phazite::addControl (s, clip, beat (phrase.start - 0.1), phazite::ControlKind::controller, 1, 127);
            phazite::addControl (s, clip, beat (phrase.start + phraseSeconds - 0.1), phazite::ControlKind::controller, 1, 0);
        }
    }

    phazite::test::EnginePlayback playback (phazite::test::EnginePlayback::Input::silence, phazite::test::EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    auto audio = playback.play (total);
    song.run ("transport.playStop");
    return audio;
}

//==============================================================================
// The measures.
struct Measures
{
    double rms = 0.0, peak = 0.0;            // the new less the old, in dB
    double spectrum = 0.0, spectrumAt = 0.0; // the largest difference of a band, in dB, and the band's centre in hertz
    double level = 0.0, levelAt = 0.0;       // the largest difference of a 10 ms frame, in dB, and when, in seconds
    double centroid = 0.0, centroidAt = 0.0; // the largest difference of a 50 ms frame's centroid, in octaves, and when
    std::optional<double> pitch;             // the largest difference of a 50 ms frame's pitch, in cents
    double pitchAt = 0.0;
    int pitchFrames = 0; // the frames steady enough in both to read their pitch

    // How far apart random phases alone leave two averages of the sixteen plays, for the RMS, the peak and the band compared
    // (2.5 standard errors of a difference of two means, from the old Synth's sixteen plays' own scatter).
    std::array<double, 3> averagedSpread {};

    static constexpr double rmsBound = 0.5, peakBound = 1.5, spectrumBound = 2.0, levelBound = 3.0, centroidBound = 1.0 / 3.0,
                            pitchBound = 15.0;

    // Each measure as a size, in this order: RMS, peak, band, level, centroid, pitch.
    std::array<double, 6> getSizes() const { return { std::abs (rms), std::abs (peak), spectrum, level, centroid, pitch.value_or (0.0) }; }

    // The worst measure against what it may be (1 at that): its bound, and given the old Synth's difference from itself
    // (its spread), the bound and that; and which it is.
    std::pair<double, juce::String> getWorst (const Measures* spread = nullptr) const
    {
        // The level, peak and spectrum are averaged over each engine's plays (measure()), with their own spread; the frames
        // are the first plays', against the old Synth's first play against its others.
        constexpr std::array bounds { rmsBound, peakBound, spectrumBound, levelBound, centroidBound, pitchBound };
        std::array<double, 6> allowed {};

        for (size_t i = 0; i < allowed.size(); ++i)
            allowed[i] = bounds[i] + (spread == nullptr ? 0.0 : (i < 3 ? averagedSpread[i] : spread->getSizes()[i]));

        std::vector<std::pair<double, juce::String>> each {
            { std::abs (rms) / allowed[0], "RMS " + juce::String (rms, 2) + " dB" },
            { std::abs (peak) / allowed[1], "peak " + juce::String (peak, 2) + " dB" },
            { spectrum / allowed[2],
              "band " + juce::String (spectrum, 1) + " dB at " + juce::String (juce::roundToInt (spectrumAt)) + " Hz" },
            { level / allowed[3], "level " + juce::String (level, 1) + " dB at " + juce::String (levelAt, 2) + " s" },
            { centroid / allowed[4], "centroid " + juce::String (centroid, 2) + " oct at " + juce::String (centroidAt, 2) + " s" },
        };

        if (pitch.has_value())
            each.push_back ({ *pitch / allowed[5], "pitch " + juce::String (*pitch, 1) + " cents at " + juce::String (pitchAt, 2) + " s" });

        return *std::max_element (each.begin(), each.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    }

    bool passes (const Measures* spread = nullptr) const { return getWorst (spread).first <= 1.0; }
};

// The larger of two measures, each measure on its own: of two spreads, the wider.
Measures getLargest (const Measures& a, const Measures& b)
{
    auto m = a;
    m.rms = std::abs (b.rms) > std::abs (a.rms) ? b.rms : a.rms;
    m.peak = std::abs (b.peak) > std::abs (a.peak) ? b.peak : a.peak;

    if (b.spectrum > a.spectrum)
        std::tie (m.spectrum, m.spectrumAt) = std::pair (b.spectrum, b.spectrumAt);

    if (b.level > a.level)
        std::tie (m.level, m.levelAt) = std::pair (b.level, b.levelAt);

    if (b.centroid > a.centroid)
        std::tie (m.centroid, m.centroidAt) = std::pair (b.centroid, b.centroidAt);

    if (b.pitch.value_or (-1.0) > a.pitch.value_or (-1.0))
        std::tie (m.pitch, m.pitchAt) = std::pair (b.pitch, b.pitchAt);

    return m;
}

// A copy of a stretch of audio.
Audio slice (const Audio& audio, int start, int length)
{
    Audio part (audio.getNumChannels(), length);

    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        part.copyFrom (channel, 0, audio, channel, start, length);

    return part;
}

double getPower (const Audio& audio, int start, int length)
{
    auto energy = 0.0;

    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int i = start; i < start + length; ++i)
            energy += (double) audio.getSample (channel, i) * audio.getSample (channel, i);

    return energy / (audio.getNumChannels() * (double) length);
}

// The long-term spectrum: the power in each third octave from 31.25 Hz to 16 kHz, from 16384-sample Hann frames at half
// overlap, both sides together.
std::vector<double> getThirdOctaves (const Audio& audio)
{
    constexpr int order = 14, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> bins ((size_t) size / 2, 0.0);
    std::vector<float> data ((size_t) size * 2);

    for (int start = 0; start + size <= audio.getNumSamples(); start += size / 2)
    {
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < size; ++i)
                data[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / size)) * audio.getSample (channel, start + i);

            fft.performFrequencyOnlyForwardTransform (data.data());

            for (size_t bin = 0; bin < bins.size(); ++bin)
                bins[bin] += (double) data[bin] * data[bin];
        }
    }

    std::vector<double> bands;

    for (int n = -15; n <= 12; ++n)
    {
        const auto centre = 1000.0 * std::exp2 (n / 3.0);
        auto power = 0.0;

        for (size_t bin = 1; bin < bins.size(); ++bin)
            if (const auto frequency = (double) bin * rate / size;
                frequency >= centre * std::exp2 (-1.0 / 6.0) && frequency < centre * std::exp2 (1.0 / 6.0))
                power += bins[bin];

        bands.push_back (power);
    }

    return bands;
}

// Where a frame's spectrum's weight is, in hertz: 2400 samples (50 ms) from a sample, Hann, both sides together.
double getCentroid (const Audio& audio, int start, int length)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2, 0.0f);

    for (int i = 0; i < length; ++i)
        data[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / (length - 1))) * 0.5f
                           * (audio.getSample (0, start + i) + audio.getSample (1, start + i));

    fft.performFrequencyOnlyForwardTransform (data.data());
    auto weighted = 0.0, total = 0.0;

    for (int bin = 1; bin < size / 2; ++bin)
    {
        if (const auto frequency = bin * rate / size; frequency >= 20.0 && frequency <= 20000.0)
        {
            weighted += frequency * data[(size_t) bin];
            total += data[(size_t) bin];
        }
    }

    return total > 1.0e-12 ? weighted / total : 0.0;
}

// A frame's pitch by the normalized square difference function (McLeod and Wyvill's): its peaks after the first dip, the
// first within 90 % of the highest, or, given a lag, the peak nearest it among those within 60 % of the highest. In
// samples (between them by a parabola), and how high the peak is: 1 for a sound that repeats exactly.
struct Period
{
    double lag = 0.0, clarity = 0.0;
};

Period findPeriod (const Audio& audio, int start, int length, double near = 0.0)
{
    constexpr int order = 13, size = 1 << order, shortest = 10, longest = 1200; // 4.8 kHz to 40 Hz
    juce::dsp::FFT fft (order);
    std::vector<juce::dsp::Complex<float>> in ((size_t) size), out ((size_t) size);
    std::vector<double> squares ((size_t) length + 1, 0.0);

    for (int i = 0; i < length; ++i)
    {
        const auto x = 0.5f * (audio.getSample (0, start + i) + audio.getSample (1, start + i));
        in[(size_t) i] = x;
        squares[(size_t) i + 1] = squares[(size_t) i] + (double) x * x;
    }

    if (squares.back() < 1.0e-12)
        return {};

    fft.perform (in.data(), out.data(), false);

    for (auto& value : out)
        value = std::norm (value);

    fft.perform (out.data(), in.data(), true);
    const auto scale = squares.back() / (double) in[0].real();
    std::vector<double> nsdf ((size_t) longest + 2, 0.0);

    for (int lag = 0; lag < (int) nsdf.size(); ++lag)
    {
        const auto m = squares[(size_t) (length - lag)] + (squares.back() - squares[(size_t) lag]);
        nsdf[(size_t) lag] = m > 1.0e-12 ? 2.0 * scale * in[(size_t) lag].real() / m : 0.0;
    }

    std::vector<std::pair<int, double>> peaks;
    auto lag = shortest;

    while (lag < longest && nsdf[(size_t) lag] > 0.0) // past the first dip
        ++lag;

    while (lag < longest)
    {
        while (lag < longest && nsdf[(size_t) lag] <= 0.0)
            ++lag;

        auto best = lag;

        while (lag < longest && nsdf[(size_t) lag] > 0.0)
        {
            if (nsdf[(size_t) lag] > nsdf[(size_t) best])
                best = lag;

            ++lag;
        }

        if (best < longest && nsdf[(size_t) best] > 0.0)
            peaks.push_back ({ best, nsdf[(size_t) best] });
    }

    if (peaks.empty())
        return {};

    const auto highest =
        std::max_element (peaks.begin(), peaks.end(), [] (const auto& a, const auto& b) { return a.second < b.second; })->second;
    auto chosen = std::find_if (peaks.begin(), peaks.end(), [highest] (const auto& p) { return p.second >= 0.9 * highest; });

    if (near > 0.0)
    {
        auto nearest = peaks.end();

        for (auto it = peaks.begin(); it != peaks.end(); ++it)
            if (it->second >= 0.6 * highest && (nearest == peaks.end() || std::abs (it->first - near) < std::abs (nearest->first - near)))
                nearest = it;

        if (nearest != peaks.end())
            chosen = nearest;
    }

    const auto at = chosen->first;
    const auto a = nsdf[(size_t) at - 1], b = nsdf[(size_t) at], c = nsdf[(size_t) at + 1];
    const auto bend = a - 2.0 * b + c;
    const auto shift = std::abs (bend) > 1.0e-12 ? 0.5 * (a - c) / bend : 0.0;
    return { at + std::clamp (shift, -0.5, 0.5), b };
}

// What a phrase's notes play, for the third-octave bands that hold their harmonics and the frames that hold a cycle: each
// partial series' base frequency, from where to where it goes (a pitch envelope sweeps it), in hertz; how far its harmonics may
// stray, in semitones; and whether every band counts (noise, which has no harmonics).
struct Partials
{
    std::vector<std::pair<double, double>> bases;
    double straySemitones = 0.5;
    bool everyBand = false;
    double lowest = 1000.0; // the lowest note's fundamental
};

Partials getPartials (const std::vector<float>& old, const Phrase& phrase)
{
    Partials partials;
    partials.everyBand = old[Old::noiseLevel] >= 0.5f;
    const auto lfoOnPitch = old[Old::lfoOn] > 0.5f && std::lround (old[Old::lfoTarget]) == (int) Old::LfoTarget::pitch;
    partials.straySemitones = 0.5 + (lfoOnPitch ? (phrase.wheel ? 1.0 : (double) old[Old::lfoDepth]) : 0.0)
                              + (old[Old::unisonOn] > 0.5f ? 0.005 * old[Old::unisonDetune] : 0.0);
    const auto sweep = std::exp2 (old[Old::pitchAmount] / 12.0);
    const auto add = [&partials, sweep] (double frequency)
    { partials.bases.push_back ({ std::min (frequency, frequency * sweep), std::max (frequency, frequency * sweep) }); };

    for (const auto& note : phrase.notes)
    {
        const auto frequency = 440.0 * std::exp2 ((note.pitch - 69) / 12.0);
        partials.lowest = std::min (partials.lowest, frequency);

        if (old[Old::oscMix] < 1.0f)
            add (frequency);

        if (old[Old::oscMix] > 0.0f)
            add (old[Old::osc2Sync] > 0.5f ? frequency
                                           : frequency * std::exp2 ((old[Old::osc2Pitch] + 0.01 * old[Old::osc2Detune]) / 12.0));

        if (old[Old::subLevel] > 0.0f)
            add (frequency * (std::lround (old[Old::subOctave]) == 1 ? 0.25 : 0.5));
    }

    return partials;
}

// Whether a third-octave band (by its centre) holds a harmonic of the partials.
bool holdsHarmonic (const Partials& partials, double centre)
{
    const auto low = centre * std::exp2 (-1.0 / 6.0), high = centre * std::exp2 (1.0 / 6.0);
    const auto stray = std::exp2 (partials.straySemitones / 12.0);

    for (const auto& [from, to] : partials.bases)
        for (auto h = 1; h * from / stray < high && h <= 2000; ++h)
            if (h * to * stray >= low)
                return true;

    return false;
}

// The measures of a phrase, the old Synth's plays against the new: the level, the peak and the spectrum over all their draws
// (each draw's power, peak in dB and bands averaged), which keeps what random phases do to one draw out of them; the level
// and centroid in time and the pitch from the first draws.
Measures measure (const std::vector<Audio>& olds, const std::vector<Audio>& nows, int start, int length, std::optional<int> pitch,
                  const Partials& partials, juce::StringArray* leftOut = nullptr)
{
    Measures m;
    const auto& old = olds.front();
    const auto& now = nows.front();
    const auto peakOf = [&] (const Audio& a)
    {
        auto peak = 0.0f;

        for (int channel = 0; channel < 2; ++channel)
            peak = std::max (peak, a.getMagnitude (channel, start, length));

        return (double) peak;
    };

    // Each play's power, peak and bands, in decibels.
    struct Play
    {
        double power = 0.0, peak = 0.0;
        std::vector<double> bands;
    };

    const auto measurePlays = [&] (const std::vector<Audio>& draws)
    {
        std::vector<Play> plays;

        for (const auto& draw : draws)
        {
            Play play;
            play.power = 10.0 * std::log10 (getPower (draw, start, length) + 1.0e-30);
            play.peak = toDecibels (peakOf (draw));

            for (const auto band : getThirdOctaves (slice (draw, start, length)))
                play.bands.push_back (10.0 * std::log10 (band + 1.0e-30));

            plays.push_back (std::move (play));
        }

        return plays;
    };

    // The average over plays of a value (its power averaged, as a power), and how far two such averages may lie apart from
    // random phases alone: 2.5 standard errors of the difference of two means, from the old plays' scatter.
    const auto playsOld = measurePlays (olds), playsNew = measurePlays (nows);
    const auto averagePower = [] (const std::vector<Play>& plays, auto&& of)
    {
        auto sum = 0.0;

        for (const auto& play : plays)
            sum += std::pow (10.0, of (play) / 10.0);

        return 10.0 * std::log10 (sum / (double) plays.size() + 1.0e-30);
    };

    const auto averageOf = [] (const std::vector<Play>& plays, auto&& of)
    {
        auto sum = 0.0;

        for (const auto& play : plays)
            sum += of (play);

        return sum / (double) plays.size();
    };

    const auto spreadOf = [&playsOld, &averageOf] (auto&& of)
    {
        if (playsOld.size() < 2)
            return 0.0;

        const auto mean = averageOf (playsOld, of);
        auto squares = 0.0;

        for (const auto& play : playsOld)
            squares += (of (play) - mean) * (of (play) - mean);

        const auto deviation = std::sqrt (squares / (double) (playsOld.size() - 1));
        return 2.5 * deviation * std::sqrt (2.0 / (double) playsOld.size());
    };

    const auto power = [] (const Play& play) { return play.power; };
    const auto peak = [] (const Play& play) { return play.peak; };
    m.rms = averagePower (playsNew, power) - averagePower (playsOld, power);
    m.peak = averageOf (playsNew, peak) - averageOf (playsOld, peak);
    m.averagedSpread[0] = spreadOf (power);
    m.averagedSpread[1] = spreadOf (peak);

    // The bands that hold a harmonic of a note played (any, for noise) within 40 dB of the old Synth's loudest band; those
    // within 40 dB of either's loudest left out for holding none, named. The worst of them against its bound and its own spread.
    const auto numBands = playsOld.front().bands.size();
    std::vector<double> bandsOld, bandsNew;

    for (size_t band = 0; band < numBands; ++band)
    {
        bandsOld.push_back (averagePower (playsOld, [band] (const Play& play) { return play.bands[band]; }));
        bandsNew.push_back (averagePower (playsNew, [band] (const Play& play) { return play.bands[band]; }));
    }

    const auto loudestOld = *std::max_element (bandsOld.begin(), bandsOld.end());
    const auto loudest = std::max (loudestOld, *std::max_element (bandsNew.begin(), bandsNew.end()));
    auto worst = -1.0;

    for (size_t band = 0; band < numBands; ++band)
    {
        const auto centre = 1000.0 * std::exp2 (((int) band - 15) / 3.0);
        const auto harmonic = partials.everyBand || holdsHarmonic (partials, centre);

        if (! harmonic && leftOut != nullptr && std::max (bandsOld[band], bandsNew[band]) >= loudest - 40.0)
            leftOut->addIfNotAlreadyThere (juce::String (juce::roundToInt (centre)));

        if (! harmonic || bandsOld[band] < loudestOld - 40.0)
            continue;

        const auto difference = std::abs (bandsNew[band] - bandsOld[band]);
        const auto spread = spreadOf ([band] (const Play& play) { return play.bands[band]; });

        if (const auto ratio = difference / (Measures::spectrumBound + spread); ratio > worst)
        {
            worst = ratio;
            m.spectrum = difference;
            m.spectrumAt = centre;
            m.averagedSpread[2] = spread;
        }
    }

    // The level in 10 ms frames, or under a fundamental of 100 Hz frames of two of its periods, where either is within 40 dB of
    // the louder's loudest frame.
    const auto frame = partials.lowest < 100.0 ? std::max (480, (int) std::ceil (2.0 * rate / partials.lowest)) : 480;
    std::vector<double> levelsOld, levelsNew;

    for (int at = 0; at + frame <= length; at += frame)
    {
        levelsOld.push_back (10.0 * std::log10 (getPower (old, start + at, frame) + 1.0e-24));
        levelsNew.push_back (10.0 * std::log10 (getPower (now, start + at, frame) + 1.0e-24));
    }

    const auto top =
        std::max (*std::max_element (levelsOld.begin(), levelsOld.end()), *std::max_element (levelsNew.begin(), levelsNew.end()));

    for (size_t i = 0; i < levelsOld.size(); ++i)
    {
        if (std::max (levelsOld[i], levelsNew[i]) < top - 40.0)
            continue;

        if (const auto difference = std::abs (levelsNew[i] - levelsOld[i]); difference > m.level)
        {
            m.level = difference;
            m.levelAt = (double) i * frame / rate;
        }
    }

    // The centroid and the pitch in 50 ms frames, where both are within 40 dB (30 dB for the pitch) of that loudest frame.
    constexpr int window = 2400;
    auto followed = pitch.has_value() ? rate / (440.0 * std::exp2 ((*pitch - 69) / 12.0)) : 0.0;

    for (int at = 0; at + window <= length; at += window)
    {
        const auto levelOld = 10.0 * std::log10 (getPower (old, start + at, window) + 1.0e-24);
        const auto levelNew = 10.0 * std::log10 (getPower (now, start + at, window) + 1.0e-24);

        if (std::min (levelOld, levelNew) < top - 40.0)
            continue;

        const auto centroidOld = getCentroid (old, start + at, window), centroidNew = getCentroid (now, start + at, window);

        if (centroidOld > 0.0 && centroidNew > 0.0)
        {
            if (const auto difference = std::abs (std::log2 (centroidNew / centroidOld)); difference > m.centroid)
            {
                m.centroid = difference;
                m.centroidAt = (double) at / rate;
            }
        }

        if (! pitch.has_value() || std::min (levelOld, levelNew) < top - 30.0)
            continue;

        // The old one followed from the note played, frame by frame, and the new one read at the period the old one is read
        // at: a sound whose period can be read at several lags (a sub an octave down, a pitch envelope high above the note)
        // is read at the same in both, which keeps their ratio.
        const auto periodOld = findPeriod (old, start + at, window, followed);
        const auto periodNew = findPeriod (now, start + at, window, periodOld.lag);

        if (periodOld.clarity < 0.8 || periodNew.clarity < 0.8)
            continue;

        followed = periodOld.lag;

        ++m.pitchFrames;

        if (const auto cents = std::abs (1200.0 * std::log2 (periodOld.lag / periodNew.lag)); cents > m.pitch.value_or (-1.0))
        {
            m.pitch = cents;
            m.pitchAt = (double) at / rate;
        }
    }

    return m;
}

// The peak of the Wavetable with a conversion's values and tables, played as tests/FactoryPresetTests.cpp plays a preset:
// notes at velocity 100 from the start, let go after some seconds, and three seconds more.
float getPeak (const Conversion& c, std::initializer_list<int> notes, double seconds)
{
    auto synth = std::make_unique<New>();
    synth->prepare (rate, 256);

    for (int osc = 0; osc < New::numOscillators; ++osc)
        synth->setTable (osc, devices::getFactoryWavetable (devices::findFactoryWavetable (c.tables[(size_t) osc].toStdString())));

    const auto release = (int) (seconds * rate), numSamples = release + (int) (3.0 * rate);
    Audio buffer (2, 256);
    auto peak = 0.0f;

    for (int done = 0; done < numSamples; done += 256)
    {
        std::vector<devices::NoteEvent> events;

        for (const auto note : notes)
        {
            if (done == 0)
                events.push_back ({ devices::NoteEvent::Type::noteOn, 0, note, 0.8f });

            if (done <= release && done + 256 > release)
                events.push_back ({ devices::NoteEvent::Type::noteOff, release - done, note, 0.0f });
        }

        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = 256;
        block.parameters = c.values;
        block.events = events;
        synth->process (block);
        peak = std::max ({ peak, buffer.getMagnitude (0, 0, 256), buffer.getMagnitude (1, 0, 256) });
    }

    return peak;
}

// A conversion leveled as docs/presets.md says every preset is (FactoryPresetTests checks it): its note at its kind's pitch,
// and for one that plays chords its chord, no higher than -1 dBFS, and a chord over a bass note under +6 dBFS. Where the
// Wavetable plays it louder than the old Synth did (an oscillator whose wave is the old one's turned over, adding where the
// old ones took away), its Level goes down by as much as that takes, in tenths of a decibel, and the conversion says so.
void level (Conversion& c, const juce::String& kind)
{
    const auto decibels = [] (float gain) { return (double) juce::Decibels::gainToDecibels (gain, -200.0f); };
    const auto note = kind == "Bass" || kind == "Percussion" ? 36 : (kind == "Pad" || kind == "Strings" ? 48 : 60);
    auto over = decibels (getPeak (c, { note }, 2.0)) + 1.0;

    if (std::lround (c.values[New::voiceMode]) != 1 && kind != "Bass" && kind != "Percussion" && kind != "FX")
        over = std::max (over, decibels (getPeak (c, { 36, 48, 52, 55 }, 0.5)) + 1.0);

    over = std::max (over, decibels (getPeak (c, { 36, 48, 52, 55 }, 0.5)) - 6.0 + 0.1);

    if (over <= 0.0)
        return;

    const auto down = (float) (std::ceil (over * 10.0) / 10.0);
    c.values[New::level] -= down;
    c.notes.add ("Level " + juce::String (down, 1) + " dB lower than the formula's, to keep its peaks within the leveling rule");
}

void writeWav (const Audio& audio, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

juce::File getOutputFolder()
{
    const juce::File folder (juce::SystemStats::getEnvironmentVariable ("PHAZITE_SYNTH_CONVERSION", {}));
    const auto chosen = folder.getFullPathName().isNotEmpty()
                            ? folder
                            : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PhaziteSynthConversion");
    chosen.createDirectory();
    return chosen;
}
} // namespace

TEST_CASE ("The old Synth's presets converted to the Wavetable's engine, and measured against the old Synth"
           * doctest::test_suite ("synth-conversion") * doctest::skip())
{
    const auto presets = phazite::test::getFactoryPresetsFolder();
    const auto filter = juce::SystemStats::getEnvironmentVariable ("PHAZITE_PRESET_FILTER", {});
    const auto withWavs = juce::SystemStats::getEnvironmentVariable ("PHAZITE_SYNTH_CONVERSION_WAVS", {}).isNotEmpty();
    const auto folder = getOutputFolder();
    auto files = presets.getChildFile ("Synth").findChildFiles (juce::File::findFiles, true, "*" + juce::String (phazite::presetExtension));
    files.sort();

    juce::String table, details, notesText;
    auto failing = 0, measured = 0;

    for (const auto& file : files)
    {
        const auto path =
            file.getRelativePathFrom (presets.getChildFile ("Synth")).replace ("\\", "/").upToLastOccurrenceOf (".", false, false);

        if (filter.isNotEmpty() && ! ("Synth/" + path).contains (filter))
            continue;

        CAPTURE (path.toStdString());
        const auto device = phazite::readPreset (file)[keys::device];
        const auto old = readValues (Old::info, device);
        auto conversion = convert (old);
        level (conversion, file.getParentDirectory().getFileName());
        const auto converted = presets.getChildFile ("Wavetable").getChildFile (path + phazite::presetExtension);
        converted.getParentDirectory().createDirectory();
        REQUIRE (phazite::writePreset (makePreset (conversion), converted));

        // The phrases after time for the short note that draws again to ring out.
        auto phrases = getPhrases (old);
        const auto phraseSeconds = getPhraseSeconds (old);
        const auto leadIn = phraseSeconds - held + 0.5 + 0.3 * redrawings;

        for (size_t i = 0; i < phrases.size(); ++i)
            phrases[i].start = leadIn + (double) i * phraseSeconds;

        // Each engine drawn redrawings + 1 times.
        std::vector<Audio> olds, nows;

        for (int redraw = 0; redraw <= redrawings; ++redraw)
        {
            olds.push_back (play (Old::typeID, file, phrases, phraseSeconds, redraw));
            nows.push_back (play (New::typeID, converted, phrases, phraseSeconds, redraw));
        }

        const auto& played = olds.front();
        const auto& playedNew = nows.front();
        const auto length = (int) std::lround (phraseSeconds * rate);
        const auto sampleAt = [] (const Phrase& phrase) { return (int) std::lround (phrase.start * rate); };

        if (withWavs)
        {
            writeWav (played, folder.getChildFile ("wav").getChildFile (path + " - Synth.wav"));
            writeWav (playedNew, folder.getChildFile ("wav").getChildFile (path + " - Wavetable.wav"));
        }

        juce::String row = "| " + path + " |";
        juce::StringArray leftOut; // the bands within 40 dB left out for holding no harmonic of a note played
        details << "\n### " << path << "\n\nG " << juce::String (conversion.g, 3) << " (" << juce::String (toDecibels (conversion.g), 2)
                << " dB).\n\n"
                << "| Phrase | RMS dB | Peak dB | Band dB | Level dB | Centroid oct | Pitch cents |\n|---|---|---|---|---|---|---|\n";
        auto passes = true;

        const auto describe = [] (const Measures& m)
        {
            return juce::String (m.rms, 2) + " | " + juce::String (m.peak, 2) + " | " + juce::String (m.spectrum, 2) + " @ "
                   + juce::String (juce::roundToInt (m.spectrumAt)) + " Hz | " + juce::String (m.level, 2) + " @ "
                   + juce::String (m.levelAt, 2) + " s | " + juce::String (m.centroid, 3) + " @ " + juce::String (m.centroidAt, 2) + " s | "
                   + (m.pitch.has_value() ? juce::String (*m.pitch, 1) + " @ " + juce::String (m.pitchAt, 2) + " s ("
                                                + juce::String (m.pitchFrames) + " frames)"
                                          : juce::String ("-"));
        };

        for (const auto& name : { "C2", "C3", "C4", "C5", "C3 v50", "Chord", "Legato", "Wheel" })
        {
            const auto phrase = std::find_if (phrases.begin(), phrases.end(), [name] (const Phrase& p) { return p.name == name; });

            if (phrase == phrases.end())
            {
                row << " - |";
                continue;
            }

            // Against its bound alone; where only the old Synth's own spread lets it through, said so, with that.
            const auto pitch = phrase->pitched ? std::optional<int> (phrase->notes.front().pitch) : std::nullopt;
            // And the old Synth's spread: its first draw against each of the others, one at a time, the widest.
            const auto partials = getPartials (old, *phrase);
            const auto m = measure (olds, nows, sampleAt (*phrase), length, pitch, partials, &leftOut);
            auto spread = measure ({ olds[0] }, { olds[1] }, sampleAt (*phrase), length, pitch, partials);

            for (size_t redraw = 2; redraw < olds.size(); ++redraw)
                spread = getLargest (spread, measure ({ olds[0] }, { olds[redraw] }, sampleAt (*phrase), length, pitch, partials));

            passes = passes && m.passes (&spread);

            if (m.passes())
                row << " pass |";
            else if (m.passes (&spread))
                row << " unsteady: " << m.getWorst().second << " (old " << spread.getWorst().second << ") |";
            else
                row << " **" << m.getWorst (&spread).second << "** |";

            details << "| " << name << " | " << describe (m) << " |\n| (old vs old) | " << describe (spread) << " |\n";
        }

        row << " " << (leftOut.isEmpty() ? juce::String ("-") : leftOut.joinIntoString (", ") + " Hz") << " |";
        table << row << "\n";
        ++measured;
        failing += passes ? 0 : 1;

        if (! conversion.notes.isEmpty())
            notesText << "- " << path << ": " << conversion.notes.joinIntoString ("; ") << "\n";

        MESSAGE (path << (passes ? ": passes" : ": fails"));
    }

    juce::String report;
    report
        << "# The old Synth's presets on the Wavetable\n\n"
        << measured << " measured, " << failing << " failing.\n\n"
        << "Each phrase's worst measure where it fails its bound (RMS 0.5 dB, peak 1.5 dB, a third-octave band 2 dB, a 10 ms "
           "frame's level 3 dB, a 50 ms frame's centroid 1/3 octave, pitch 15 cents), against the bound and the old Synth's "
           "own spread there: for the RMS, the peak and the bands, averaged over sixteen plays from other random phases, 2.5 "
           "standard errors of a difference of two such averages, from the old Synth's sixteen plays' scatter; for the frames, the "
           "old Synth's first play against its others ('unsteady' where only that spread lets it through). A band counts "
           "where it holds a harmonic of a note played (every band, for the noise presets) within 40 dB of the old Synth's loudest; the last column names those within 40 dB left out for "
           "holding none. Under a fundamental of 100 Hz, the level is measured in frames of two of its periods.\n\n"
        << "| Preset | C2 | C3 | C4 | C5 | C3 v50 | Chord | Legato | Wheel | Bands left out |\n"
           "|---|---|---|---|---|---|---|---|---|---|\n"
        << table << "\n## What the conversion carries over only approximately\n\n"
        << (notesText.isEmpty() ? juce::String ("Nothing.\n") : notesText) << "\n## Every measure\n"
        << details;
    const auto reportFile = folder.getChildFile ("report.md");
    REQUIRE (reportFile.replaceWithText (report));
    MESSAGE ("The report: " << reportFile.getFullPathName() << " (" << failing << " of " << measured << " failing)");
}

//==============================================================================
namespace
{
// A song's sound as its golden render measures one, over the whole song: integrated loudness (EBU R128) and the
// fingerprint's ten bands, each its power over every block.
struct SongSound
{
    double loudness = -100.0;
    std::array<double, phazite::test::numBands> bands {};
};

SongSound measureSong (const Audio& audio, double sampleRate)
{
    SongSound sound;
    phazite::engine::LoudnessMeter meter;
    meter.prepare (sampleRate, 512);

    for (int done = 0; done < audio.getNumSamples(); done += 512)
    {
        const float* channels[] = { audio.getReadPointer (0, done), audio.getReadPointer (1, done) };
        meter.process (channels, 2, std::min (512, audio.getNumSamples() - done));
    }

    sound.loudness = meter.getReadings().integrated.value_or (-100.0f);
    const auto fingerprint = phazite::test::getFingerprint (audio, (int) std::lround (sampleRate / 10.0));

    for (size_t band = 0; band < sound.bands.size(); ++band)
    {
        auto power = 0.0;

        for (const auto& block : fingerprint)
            power += std::pow (10.0, block.bands[band] / 10.0);

        sound.bands[band] = 10.0 * std::log10 (power / (double) std::max<size_t> (1, fingerprint.size()) + 1.0e-30);
    }

    return sound;
}

Audio readWav (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    sampleRate = reader->sampleRate;
    Audio audio (2, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio;
}

juce::var toVar (const SongSound& sound)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("loudness", sound.loudness);
    juce::Array<juce::var> bands;

    for (const auto band : sound.bands)
        bands.add (std::round (band * 100.0) / 100.0);

    object->setProperty ("bands", bands);
    return juce::var (object);
}
} // namespace

TEST_CASE ("The example songs that play the old Synth's presets, measured as their golden render measures them"
           * doctest::test_suite ("synth-conversion") * doctest::skip())
{
    const auto folder = getOutputFolder();
    const auto label = juce::SystemStats::getEnvironmentVariable ("PHAZITE_SONGS_LABEL", "now");
    auto* all = new juce::DynamicObject();

    for (const auto* id : { "exodus", "houseGroove", "loFiBeat" })
    {
        CAPTURE (std::string (id));
        phazite::test::TemporaryFolder temp;
        juce::String error;
        auto project = phazite::Project::create (
            temp.folder, id, error,
            phazite::getExampleSongContent (phazite::test::getEngineContext(), id, phazite::test::getFactoryPresetsFolder()));
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        phazite::setSong (context, &project->getSong());

        const auto exportAndWait = [&project] (const juce::File& file, const phazite::ExportSettings& settings)
        {
            std::optional<phazite::ExportReport> report;
            const auto running =
                phazite::startExport (project->getSong(), file, settings, [&report] (const phazite::ExportReport& r) { report = r; });
            REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 3600000));
            return *report;
        };

        phazite::ExportSettings settings;
        settings.bitDepth = 32;
        const auto songFolder = folder.getChildFile (label).getChildFile (id);
        songFolder.createDirectory();
        const auto mix = exportAndWait (songFolder.getChildFile ("mix.wav"), settings);
        REQUIRE_MESSAGE (mix.result.wasOk(), mix.result.getErrorMessage().toStdString());
        settings.stems = true;
        const auto stems = exportAndWait (songFolder.getChildFile ("stems"), settings);
        REQUIRE_MESSAGE (stems.result.wasOk(), stems.result.getErrorMessage().toStdString());

        auto* song = new juce::DynamicObject();
        double sampleRate = 44100.0;
        song->setProperty ("mix", toVar (measureSong (readWav (mix.file, sampleRate), sampleRate)));

        for (const auto& stem : stems.stems)
            song->setProperty (stem.getFileNameWithoutExtension(), toVar (measureSong (readWav (stem, sampleRate), sampleRate)));

        all->setProperty (id, juce::var (song));
        phazite::setSong (context, nullptr);
    }

    const auto file = folder.getChildFile ("songs-" + label + ".json");
    REQUIRE (file.replaceWithText (juce::JSON::toString (juce::var (all))));
    MESSAGE ("The songs: " << file.getFullPathName());

    // With "before" there too, how far each moved: its loudness in LU and its bands' largest move in dB.
    const auto before = juce::JSON::parse (folder.getChildFile ("songs-before.json"));
    const auto after = juce::JSON::parse (folder.getChildFile ("songs-after.json"));

    if (! before.isObject() || ! after.isObject())
        return;

    juce::String moved = "| Song | Part | Loudness before | after | LU | Largest band move dB |\n|---|---|---|---|---|---|\n";

    for (const auto& [songId, parts] : before.getDynamicObject()->getProperties())
    {
        for (const auto& [part, sound] : parts.getDynamicObject()->getProperties())
        {
            const auto now = after[songId][part];

            if (! now.isObject())
            {
                moved << "| " << songId.toString() << " | " << part.toString() << " | gone |\n";
                continue;
            }

            auto largest = 0.0;
            auto largestBand = 0;

            for (int band = 0; band < phazite::test::numBands; ++band)
            {
                // Only bands within 40 dB of the part's loudest, as the presets' spectra are compared.
                const auto loudestBefore = [&sound]
                {
                    auto top = -300.0;

                    for (int b = 0; b < phazite::test::numBands; ++b)
                        top = std::max (top, (double) sound["bands"][b]);

                    return top;
                }();

                if ((double) sound["bands"][band] < loudestBefore - 40.0)
                    continue;

                if (const auto difference = std::abs ((double) now["bands"][band] - (double) sound["bands"][band]); difference > largest)
                {
                    largest = difference;
                    largestBand = band;
                }
            }

            moved << "| " << songId.toString() << " | " << part.toString() << " | " << juce::String ((double) sound["loudness"], 2) << " | "
                  << juce::String ((double) now["loudness"], 2) << " | "
                  << juce::String ((double) now["loudness"] - (double) sound["loudness"], 2) << " | " << juce::String (largest, 2)
                  << " (band " << largestBand << ") |\n";
        }
    }

    const auto movedFile = folder.getChildFile ("songs-moved.md");
    REQUIRE (movedFile.replaceWithText (moved));
    MESSAGE ("How far they moved: " << movedFile.getFullPathName());
}

//==============================================================================
// The Wavetable's own presets that play Basic Shapes (roadmap step S6.7: its frames turned to the old Synth's waves), each
// held at C2, C3 and C4 through the engine: its RMS and third-octave bands, into wavetable-<PHAZITE_SONGS_LABEL>.json in the
// same folder; with a "before" and an "after" there, how far each moved (the same seeds both times, so that only the tables'
// change moves them). Run with
//   phazite_tests.exe --test-suite=synth-conversion --no-skip --test-case="The Wavetable's own*"
TEST_CASE ("The Wavetable's own presets that play Basic Shapes, measured before and after a change to its tables"
           * doctest::test_suite ("synth-conversion") * doctest::skip())
{
    const auto presets = phazite::test::getFactoryPresetsFolder();
    const auto folder = getOutputFolder();
    const auto label = juce::SystemStats::getEnvironmentVariable ("PHAZITE_SONGS_LABEL", "now");
    auto files =
        presets.getChildFile ("Wavetable").findChildFiles (juce::File::findFiles, true, "*" + juce::String (phazite::presetExtension));
    files.sort();
    auto* all = new juce::DynamicObject();

    for (const auto& file : files)
    {
        const auto path =
            file.getRelativePathFrom (presets.getChildFile ("Wavetable")).replace ("\\", "/").upToLastOccurrenceOf (".", false, false);

        // Its own: not one converted from the old Synth's.
        if (presets.getChildFile ("Synth").getChildFile (path + phazite::presetExtension).existsAsFile())
            continue;

        const auto device = phazite::readPreset (file)[keys::device];
        const auto values = readValues (New::info, device);
        auto playsBasicShapes = false;

        for (int osc = 0; osc < New::numOscillators; ++osc)
            playsBasicShapes = playsBasicShapes
                               || (values[(size_t) New::getParameter (osc, New::oscOn)] > 0.5f
                                   && device[keys::sounds][phazite::schema::getOscillatorWavetable (osc)].toString() == "Basic Shapes");

        if (! playsBasicShapes)
            continue;

        CAPTURE (path.toStdString());
        const auto phraseSeconds = std::ceil ((held + values[New::env1Release] * 0.001 + 0.5) / 0.5) * 0.5;
        std::vector<Phrase> phrases { { "C2", { { 36, 0.0, held, 100 } } },
                                      { "C3", { { 48, 0.0, held, 100 } } },
                                      { "C4", { { 60, 0.0, held, 100 } } } };

        for (size_t i = 0; i < phrases.size(); ++i)
            phrases[i].start = 0.5 + (double) i * phraseSeconds;

        const auto played = play (New::typeID, file, phrases, phraseSeconds);
        const auto length = (int) std::lround (phraseSeconds * rate);
        auto* preset = new juce::DynamicObject();

        for (const auto& phrase : phrases)
        {
            const auto start = (int) std::lround (phrase.start * rate);
            auto* measured = new juce::DynamicObject();
            measured->setProperty ("rms", 10.0 * std::log10 (getPower (played, start, length) + 1.0e-30));
            juce::Array<juce::var> bands;

            for (const auto band : getThirdOctaves (slice (played, start, length)))
                bands.add (10.0 * std::log10 (band + 1.0e-30));

            measured->setProperty ("bands", bands);
            preset->setProperty (phrase.name, juce::var (measured));
        }

        all->setProperty (path, juce::var (preset));
    }

    const auto file = folder.getChildFile ("wavetable-" + label + ".json");
    REQUIRE (file.replaceWithText (juce::JSON::toString (juce::var (all))));
    MESSAGE ("The Wavetable's presets: " << file.getFullPathName());

    // With "before" there too: each note's RMS move and its largest band move, among the bands within 40 dB of its loudest.
    const auto before = juce::JSON::parse (folder.getChildFile ("wavetable-before.json"));
    const auto after = juce::JSON::parse (folder.getChildFile ("wavetable-after.json"));

    if (! before.isObject() || ! after.isObject())
        return;

    juce::String moved = "| Preset | Note | RMS dB | Largest band move dB |\n|---|---|---|---|\n";

    for (const auto& [path, notes] : before.getDynamicObject()->getProperties())
    {
        for (const auto& [note, was] : notes.getDynamicObject()->getProperties())
        {
            const auto now = after[path][note];

            if (! now.isObject())
                continue;

            auto loudest = -300.0;

            for (const auto& band : *was["bands"].getArray())
                loudest = std::max (loudest, (double) band);

            auto largest = 0.0, largestAt = 0.0;

            for (int band = 0; band < was["bands"].size(); ++band)
            {
                if ((double) was["bands"][band] < loudest - 40.0)
                    continue;

                if (const auto difference = std::abs ((double) now["bands"][band] - (double) was["bands"][band]); difference > largest)
                {
                    largest = difference;
                    largestAt = 1000.0 * std::exp2 ((band - 15) / 3.0);
                }
            }

            moved << "| " << path.toString() << " | " << note.toString() << " | "
                  << juce::String ((double) now["rms"] - (double) was["rms"], 2) << " | " << juce::String (largest, 2) << " at "
                  << juce::roundToInt (largestAt) << " Hz |\n";
        }
    }

    const auto movedFile = folder.getChildFile ("wavetable-moved.md");
    REQUIRE (movedFile.replaceWithText (moved));
    MESSAGE ("How far they moved: " << movedFile.getFullPathName());
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's vocoder (roadmap step 2.17): put on a voice, it makes another sound, the carrier, speak with it, as the
// vocoders of Ableton Live, Bitwig, Cubase and Studio One do on the voice's track. The voice (both sides together) is split
// into bands and each band's level followed (Attack, Release); the carrier is split into the same bands, each turned up and
// down by the voice's level in its band, and the bands summed. Carriers: Buzz, a bright saw at a pitch of its own (C3 at
// first; automate Pitch to play a tune), so that it speaks the moment it is put on a voice; Noise, a whisper; Voice, the
// voice itself, smeared; and Track, the sound of the track picked as its sidechain (a synth's chords, for the classic robot
// choir), silent until one is picked. Bands: 8 to 40 (16 at first) from a low to a high frequency (100 Hz to 10 kHz), as
// wide as Band Width says. Formant moves the carrier's bands up or down, for a smaller or larger voice; Unvoiced adds noise
// where the voice has consonants ("s", "t"), so that they stay clear; Brightness tilts the bands; Stereo sends the bands left
// and right in turn; Output is a gain. The carrier is evened out band by band, by the louder of each band and its
// neighbours (so that a lone harmonic isn't raised in the bands around it), and never by more than 12 dB over what the band
// would hold of pink noise as loud as the carrier has been lately (so that a band it hardly reaches, or its tail, isn't
// brought up to the voice): the vocoder is about as loud as the voice, whatever the carrier, and silent with it. Every other
// band plays upside down, so that neighbours add up where they overlap rather than cancel. No latency.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "devices/core/LiveValue.h"
#include "devices/core/Processor.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Random.h"

#include <array>
#include <atomic>
#include <iterator>
#include <span>

namespace phazite::devices
{
class Vocoder final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteVocoder";
    static constexpr int maxBands = 40;

    enum Parameter
    {
        carrier, // Buzz, Noise, Voice or Track
        pitch,   // the buzz's, in semitones from C3
        bands,   // a choice: 8, 12, 16, 24, 32 or 40
        lowFrequency,
        highFrequency,
        bandWidth,
        formant, // semitones
        attack,  // milliseconds
        release,
        unvoiced,
        brightness,
        stereo, // the bands sent left and right in turn
        output, // decibels
        mix     // applied by its thin plug-in (DeviceInfo.h, mixParameterID)
    };

    enum class Carrier
    {
        buzz,
        noise,
        voice,
        track
    };

    static const DeviceInfo info;

    // Its bands for the parameters' values: how many, and each one's centre, where it listens to the voice and where it
    // plays the carrier (moved by Formant), in hertz: for its editor and tests.
    static int getNumBands (std::span<const float> parameters);
    static double getBandFrequency (std::span<const float> parameters, int band);
    static double getCarrierBandFrequency (std::span<const float> parameters, int band, double sampleRate);

    // What it shows as it plays, once a block (devices/core/LiveValue.h): the voice's level in each band (a gain, 0 for a
    // band it doesn't have), how loud the carrier is (a gain, before its bands), and how much of a consonant it hears now
    // (0 to 1).
    const LiveValue& getBandLevel (int band) const noexcept { return bandLevels[(size_t) band]; }
    const LiveValue& getCarrierLevel() const noexcept { return shownCarrierLevel; }
    const LiveValue& getConsonant() const noexcept { return shownConsonant; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override { return tailSeconds.load (std::memory_order_relaxed); }

private:
    struct Band
    {
        dsp::FilterCoefficients analysis {}, synthesis {};
        std::array<dsp::Filter, 2> voiceFilters {}, carrierFilters {};
        dsp::LevelFollower follower, carrierFollower; // the voice's level in the band, and the carrier's
        float gain = 1.0f;                            // Brightness's tilt
        float pan = 0.0f;                             // -1 left to 1 right, at full width
        float polarity = 1.0f;
        float played = 0.0f, carried = 0.0f; // the carrier in the band now, and its level (RMS)
    };

    double sampleRate = 44100.0;
    std::array<Band, maxBands> bandsInUse;
    int numBands = 0;

    // The parameters the bands are made from, and their values when they were made, to make them again when one changes.
    static constexpr int shapingParameters[] = { bands, lowFrequency, highFrequency, bandWidth, formant, attack, release, brightness };
    std::array<float, std::size (shapingParameters)> madeFrom {};
    bool made = false;

    // When the number of bands changes: the sound fades out, the bands change, and it fades in again.
    float fade = 1.0f, fadeStep = 0.01f;
    int pendingBands = 0;

    dsp::Oscillator buzz;
    dsp::Random noise;
    dsp::Filter consonantHighPass {};
    dsp::FilterCoefficients consonantCoefficients {};
    dsp::LevelFollower highLevel, wholeLevel, carrierLevel;
    float bandFloor = 0.0f; // the least level a band is evened out from, of the carrier's

    std::atomic<double> tailSeconds { 0.05 };
    std::array<LiveValue, maxBands> bandLevels;
    LiveValue shownCarrierLevel, shownConsonant;

    static_assert (std::atomic<double>::is_always_lock_free);

    static Carrier getCarrier (std::span<const float> parameters);
    void makeBands (std::span<const float> parameters, int count);
    float getCarrierSample (Carrier, float voice, const ProcessContext&, int sample);
};
} // namespace phazite::devices

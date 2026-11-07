# DSP audit

*2026-11-06, the owner's request: "do an audit of all of the DSP tools and packages we use (or are in JUCE) and find out if
they are the best version of said tool or if there are better open source alternatives". Built the same day, on the
owner's word ("implement all of the fixes (you can defer the mp3 fix)"): everything in section 6 but the MP3 delay, which
stays open. Section 8 says what was built, where it differs from the plan, and how it measures.*

It covers all the signal processing Phazite plays or renders: the libraries (Rubber Band, and JUCE's DSP and audio-file
code), and Phazite's own instruments, effects and engine. Each part was read in the code. Each library's version and licence
were checked against its upstream repository on 2026-11-06. Three things were measured by running copies of Phazite's or
JUCE's formulas outside the app (section 5). There were no listening tests: where a verdict depends on taste (a reverb's
sound, a stretcher's), it says so.

## 1. In short

- **Everything is up to date.** JUCE 9.0.5 is JUCE's newest release (2026-11-02), and Rubber Band 4.0.0 is Rubber Band's
  (2024-10-25). The codecs JUCE bundles are recent releases: FLAC 1.5.0, libvorbis 1.3.7, libogg 1.3.6 and libopus 1.6.1.
- **Rubber Band's R3 engine is still the best open-source choice** for what Phazite does with it: stretching whole clips
  offline. Keep it.
- **Phazite uses little of JUCE's DSP, and that is right.** Its own devices are built on newer methods than JUCE's classes
  (section 4): zero-delay-feedback filters, a feedback-delay-network reverb, a look-ahead limiter, velvet-noise widening,
  band-limited wavetables, and anti-aliased drive in the compressor.
- **Seven places fall short of the best open-source practice.** They are listed here roughly by how easily each would be
  heard:
  1. **Audio files whose sample rate differs from the device's** play through JUCE's 5-point Lagrange interpolation. A
     44.1 kHz sample in a 48 kHz project loses 1.2 dB at 15 kHz and 4.4 dB at 20 kHz. Its false tones ("aliases") are only
     17.5 dB under a 15 kHz tone. A 96 kHz file in a 48 kHz project folds everything above 24 kHz back down at full level.
     **Fix:** r8brain-free-src (MIT).
  2. **The synth's saw and pulse waves alias.** Their 2-point PolyBLEP leaves aliases 37 dB under the note at A5 and 25 dB
     under it at A7. **Fix:** read them from band-limited tables, as the wavetable synth already does (Phazite's own code).
  3. **True peak is measured two different ways, and neither follows ITU-R BS.1770.** The limiter's 8-tap interpolator
     reads content above about 15 kHz too low: by 3.8 dB at 18 kHz, halfway between samples. The loudness meter uses JUCE's
     IIR oversampler, whose phase isn't linear. **Fix:** one detector for both, with BS.1770-5's 48-tap filter.
  4. **The saturator still aliases at high drive.** Its comment says that running at twice the sample rate prevents this.
     **Fix:** antiderivative anti-aliasing, which the compressor's drive already uses.
  5. **MP3 files** decode with JUCE's own decoder, which doesn't drop the encoder delay and padding that the LAME tag
     records. An MP3 loop probably starts about 25 ms late and doesn't loop seamlessly; a test should confirm this.
     **Fix:** read the LAME tag, or switch to libmpg123.
  6. **The EQ's bells and shelves get narrower and lopsided above about 10 kHz.** This is the "cramping" that the bilinear
     transform causes. **Fix:** matched coefficients (Vicanek, 2016).
  7. **Smaller ones:** the delay's linear interpolation, the reverb's damping, the drum sampler's pitch-ups, and the
     unprotected drives in the multiband, the filter and the wavetable synth.

The fixes need one new library (r8brain-free-src), plus libmpg123 only if a small fix to JUCE's MP3 reader isn't enough.
Section 6 orders them.

## 2. The libraries

| Library | What Phazite uses | Upstream on 2026-11-06 | Best open-source alternative | Verdict |
|---|---|---|---|---|
| Rubber Band Library (clip stretching and pitching, browser previews) | 4.0.0: the single-file build, R3 engine, offline, with its built-in FFT and resampler | 4.0.0 is the newest | Signalsmith Stretch 1.4.0 (MIT) and Bungee 2.4.39 (MPL-2.0) are built for real-time stretching. Neither claims better offline quality than R3, and no published listening test ranks either above it | **Keep.** If Phazite ever needs real-time stretching (say, a clip following a tempo drag before its render is ready), try Signalsmith Stretch: MIT, cheap, with no block-size limits |
| Rubber Band's FFT | Built-in | (Rubber Band's own choice) | FFTW 3.3.11 (GPL-2.0-or-later; Rubber Band's docs call it "a bit faster than built-in"). SLEEF 3.9 (Boost licence, not on Phazite's list; "usually very fast") | **Keep.** The FFT changes only the speed, never the sound, and the gain is small |
| JUCE's resampling (`juce::LagrangeInterpolator`) | 5-point Lagrange interpolation, in `engine::AudioRegion`, for every audio clip whose file rate differs from the engine's | Unchanged in JUCE 9 | r8brain-free-src 7.7 (MIT, header-only, actively maintained). Also libsamplerate 0.2.2 (BSD-2-Clause, last released 2021) and soxr 0.1.3 (LGPL-2.1, last released 2018) | **Replace** with r8brain-free-src (3.1) |
| JUCE's FFT (`juce::dsp::FFT`) | Its fallback engine on Windows and Linux: plain scalar code, behind a spin lock, that computes a real transform as a full-size complex one | JUCE uses Apple's vDSP on macOS, and FFTW or Intel's libraries only if switched on | PFFFT 1.2.0 (BSD-style FFTPACK licence; r8brain-free-src bundles it), pocketfft (BSD-3-Clause), KissFFT 131.2.0 (BSD-3-Clause) | **Fine where it is used:** the analyzer, on the message thread, and wavetables, built off the audio thread. Anything that needs an FFT on the audio thread later (a convolution reverb, a linear-phase EQ or crossover, spectral effects) should use PFFFT |
| JUCE's oversampler (`juce::dsp::Oversampling`, polyphase IIR half-band filters) | The saturator (2x) and the loudness meter's true peak (4x) | | HIIR is faster (WTFPL, not on Phazite's list). A linear-phase FIR is what true peak needs | **Keep for the saturator** (with 3.4). **Replace in the meter** (3.3) |
| `juce::IIRFilter`, `juce::Random` | Drum synthesis (offline), the envelope follower's input filters, the random modulator | | Phazite's own filter (`dsp/Filters.h`) and generator (`dsp/Random.h`) | **Fine.** Switching would only unify the code |
| Audio files (`registerBasicFormats()`) | WAV and AIFF (JUCE's own readers, hardened in JUCE 9), FLAC 1.5.0, Ogg Vorbis 1.3.7, Opus 1.6.1, MP3 (JUCE's own decoder), and on Windows AAC, M4A and WMA through Media Foundation | The bundled codecs are recent releases | libmpg123 for MP3 (LGPL-2.1, gapless by default). minimp3 (CC0, not on Phazite's list) | **Keep,** but fix the MP3 delay (3.5). AAC and M4A open on Windows only. FAAD2 (GPL-2.0-or-later) would open them on Linux; FDK-AAC's licence isn't compatible with the GPL |
| libebur128's method (loudness) | Its K-weighting and gating, followed in Phazite's own `LoudnessMeter` | v1.2.6 (2021) | None needed | **Keep.** It is the reference implementation |
| `juce::AudioThumbnail`, `juce::FloatVectorOperations` | Waveform overviews; bulk adds, copies and peak finding | | | **Fine** |

## 3. Where Phazite falls short, and the fix for each

### 3.1 Audio files at another sample rate

`engine::AudioRegion` plays a file whose rate differs from the engine's through JUCE's `LagrangeInterpolator`. This covers
every such clip, including stretched clips: `ClipRenders` writes their renders at the file's own rate. The owner's device
runs at 48 kHz, and many sample packs are 44.1 kHz. Lagrange interpolation has no low-pass filter that follows the ratio,
so it dulls the top octave and lets aliases through. Going down a rate, nothing removes what lies above the new Nyquist
frequency. Section 5.1 has the numbers. The worst case is an exact 2:1 ratio (96 to 48 kHz): there it just drops every
other sample, so ultrasonic content folds into the audible range unattenuated.

The best practice is windowed-sinc conversion with a low-pass filter set by the ratio. **r8brain-free-src** does this:
- MIT licence, header-only, actively maintained (version 7.7).
- The caller sets the transition band (0.5 % to 45 %) and the stopband (49 to 218 dB).
- It runs in real time. Its README gives 860 audio streams per core for 44.1 to 96 kHz on a Ryzen 3700X.
- It bundles PFFFT, which then serves any later FFT need too.

**Fix:**
- Each region's state, which the model makes off the audio thread, holds one r8brain resampler per channel in place of the
  two interpolators.
- After a jump, the resampler is primed from the file ahead of the playhead; the region already reads ahead.
- Offline renders use the same code, so an export sounds like playback.
- Golden renders of files at other rates change, on purpose.
- Check the cost with the benchmarks: a song of many 44.1 kHz clips on a 48 kHz engine.

The alternative of converting each file once into the cache, as stretched clips are, would cost nothing during playback. But
a clip would be silent until its conversion is ready, and again after every device change. Real-time conversion plays at
once.

### 3.2 The synth's oscillators

The synth's saw and pulse waves use a 2-point PolyBLEP (`dsp/Oscillators.h`), a cheap correction that rounds each jump in
the wave. The triangle has no correction. Section 5.3 has the numbers:
- **Saw:** compared with a naive saw, PolyBLEP lowers the loudest alias by 8 dB and all the aliasing together by 15 to
  16 dB. Its loudest alias is still 37 dB under the note at A5 (880 Hz) and 25 dB under it at A7. Aliases are inharmonic, and they move against a bend or vibrato, so they are audible on bright leads.
- **Triangle:** clean up to A5. At A6 and A7, aliases are 47 dB and 34 dB under the note.

Band-limited synths avoid this with band-limited tables, long BLEP tables (minBLEP), or oversampling. Phazite already has
band-limited tables: the wavetable synth's (`wavetable/Wavetable.h`). Each half octave keeps only the harmonics whose
aliases stay above hearing, and the oscillator reads them with cubic interpolation.

**Fix:**
- The synth reads saw, triangle and square from factory tables of those shapes.
- Its pulse becomes the difference of two saws a width apart, so pulse width can still move.
- The sound stays the same and the aliases go.
- The cost is about the same as now; check it with the benchmarks. Golden renders change.

### 3.3 True peak

A true peak is the highest point of the waveform between samples, as a converter rebuilds it. Streaming services' loudness
rules ask for it. Phazite has two detectors, and they can disagree:
- **The limiter** (`dynamics/Limiter.cpp`) interpolates three points between samples with an 8-tap Hann-windowed sinc. It
  reads high frequencies low (section 5.2): 0.7 dB at 15 kHz, 3.8 dB at 18 kHz, 8.3 dB at 19.8 kHz (halfway between
  samples, at 44.1 kHz). A bright master can therefore pass the ceiling between samples. How far depends on the material.
- **The loudness meter** (`engine/nodes/LoudnessMeter.cpp`) upsamples 4x with JUCE's polyphase IIR filters. They delay each
  frequency by a different amount, so the waveform they draw between samples isn't the one a converter makes. ITU-R
  BS.1770 specifies a linear-phase FIR instead: 48 taps, 12 for each of the 4 phases at 48 kHz.

**Fix:**
- One true-peak detector in `src/devices/dynamics/`, engine-neutral, using BS.1770-5 Annex 2's filter, with less
  oversampling at higher rates.
- The limiter and the loudness meter both use it, so the ceiling and the meter's dBTP agree.
- Test it with the EBU's true-peak test signals (EBU Tech 3341), which give the expected readings.
- 12 taps for 3 points a sample costs almost nothing on the master. The limiter's latency grows by about 2 samples.
- Golden renders change.

### 3.4 The saturator, and the other drives

The saturator (`saturation/Saturator.cpp`) shapes the sound at twice the sample rate, through JUCE's IIR half-band filters,
with drive up to +36 dB and a hard clip and a fold among its curves. Its header says the doubled rate stops the harmonics
from folding back as aliasing. At high drive, that isn't true. A hard-driven shaper's harmonics fall only about 6 dB per
octave, and against such a spectrum 2x oversampling lowers aliasing by only about 6 dB (Vicanek's note on anti-aliased
distortion, section 7).

The modern method is **antiderivative anti-aliasing (ADAA)**: the curve applied to the average of the input between two
samples, worked out from the curve's antiderivative. It works best together with oversampling: ADAA makes the harmonics fall
steeply, and the oversampling filter then removes them. With 4x oversampling as well, even a hard clipper's aliasing is
negligible. Phazite's compressor already does this for its drive (`dynamics/Compressor.cpp`).

**Fix:** first-order ADAA for each saturator curve, keeping the 2x oversampling.
- Soft, tape, hard and fold each have a closed-form antiderivative.
- Tube can use a table of its antiderivative.
- The same goes for the multiband's band saturation, which isn't oversampled at all (Maximus's oversampling is listed as
  not built), and for the drive in the Filter device and the wavetable synth.

### 3.5 MP3 files

JUCE 9 turns its own MP3 decoder on by default (`JUCE_USE_MP3AUDIOFORMAT`). It is registered before Media Foundation, so
it decodes MP3s on Windows too. It reads the Xing header for the file's length, but nothing in it trims the encoder delay
and padding that LAME records in its tag: usually about 1,100 samples at the start, 25 ms at 44.1 kHz.

**Fix:**
1. First, a test: decode an MP3 of a click at a known sample and see where the click lands.
2. If the delay is there, read the LAME tag (a few bytes after the Xing header) and trim the delay and padding in Phazite's
   reader.
3. If JUCE's decoder needs more than that, decode MP3s with libmpg123 instead (LGPL-2.1, gapless by default).

### 3.6 The EQ near the top of the spectrum

The EQ's filter is the bilinear transform of an analog filter, which squeezes the top of the spectrum toward the Nyquist
frequency. Bells and shelves set above about 10 kHz therefore come out narrower and lopsided compared with the analog
response they are meant to have. The EQ draws its filters' exact response, so the curve shows this honestly.

**Fix:**
- Use matched coefficients (Vicanek, *Matched Second Order Digital Filters*, 2016) for the bells, shelves and cuts.
- The filter keeps its zero-delay structure, because any stable second-order filter maps onto its coefficients.
- The drawn curve follows the new coefficients.

### 3.7 Smaller ones

- **Delay:** `dsp::DelayLine::read (float)` interpolates linearly. At a fractional delay, each pass through the feedback
  loses a little treble, by an amount that depends on the fraction. **Fix:** use the 4-point Lagrange interpolation that the
  comb filter already uses.
- **Reverb:** the design is the modern standard: Dattorro's diffusers into an 8-line Householder feedback delay network,
  modulated. Signalsmith's and zita-rev1's reverbs are of the same family. But all eight lines share one damping
  coefficient, so the short lines, which go round more often, lose their treble faster than the long ones, and the tail's
  colour shifts as it fades. **Fix:** scale each line's damping to its length (Jot and Chaigne, 1991), as its loss already
  is, and read the modulated lines with cubic rather than linear interpolation. If the reverb's sound is ever in question,
  audition CloudSeed (MIT).
- **Drum sampler:** a pad that is tuned, or whose file has another rate, plays through 4-point Hermite interpolation. That
  dulls the top and aliases when the pad is tuned up. A windowed-sinc read whose cutoff follows the step would fix it. This
  is low priority for drums.

## 4. JUCE's DSP that Phazite doesn't use

| JUCE class | What it is | What Phazite uses instead |
|---|---|---|
| `juce::Reverb`, `dsp::Reverb` | Freeverb (1999): combs and all-passes, with a metallic tail | A modulated feedback-delay-network reverb, a newer design |
| `dsp::Compressor`, `dsp::Limiter`, `dsp::NoiseGate` | A hard knee and no look-ahead. The limiter is two compressors and a clipper, with no true-peak detection | Phazite's compressor (characters, drawn curves, anti-aliased drive) and look-ahead limiter |
| `dsp::Oscillator`, `dsp::WaveShaper` | No band-limiting and no anti-aliasing | PolyBLEP oscillators, band-limited wavetables, ADAA drive |
| `dsp::StateVariableTPTFilter`, `dsp::LadderFilter` | Topology-preserving filters, the same method as Phazite's | Phazite's own; switching would gain nothing |
| `dsp::Phaser`, `dsp::Chorus` | Simple designs | Phazite's phaser, which solves its feedback within each sample |
| `juce::WindowedSincInterpolator` | 200 taps a sample, with the cutoff fixed at the source's Nyquist frequency, so it still aliases going down a rate | r8brain-free-src (3.1) |
| `dsp::Convolution` | Partitioned convolution | Nothing yet. It is the one to use for a convolution reverb, but on Windows and Linux it runs on JUCE's fallback FFT: turn on JUCE's FFTW option (FFTW is GPL-2.0-or-later) or build on PFFFT |

## 5. Measurements

Each measurement ran a C# copy of the code's formula, outside the app, on single sines or single notes. The scripts aren't
in the repository.

### 5.1 JUCE's 5-point Lagrange resampling

The interpolator is copied from `juce_LagrangeInterpolator.cpp`: points at -2 to 2, read between 0 and 1. Each run uses a
full-scale sine and 65,536 output samples. "Gain" is the tone's level after resampling. "Aliases and images" is everything
else in the output, measured against the tone.

| From, to | Tone | Gain | Aliases and images under the tone |
|---|---|---|---|
| 44.1, 48 kHz | 1 kHz | 0.00 dB | 133.5 dB |
| | 5 kHz | 0.00 dB | 63.8 dB |
| | 10 kHz | -0.17 dB | 34.4 dB |
| | 15 kHz | -1.24 dB | 17.5 dB |
| | 18 kHz | -2.80 dB | 9.6 dB |
| | 20 kHz | -4.36 dB | 4.7 dB |
| 48, 44.1 kHz | 10 kHz | -0.11 dB | 37.9 dB |
| | 15 kHz | -0.83 dB | 21.0 dB |
| | 20 kHz | -3.06 dB | 8.6 dB |
| | 23 kHz (should vanish) | | Aliased to 21.1 kHz at -3.4 dB |
| 96, 48 kHz | 10 or 20 kHz | 0.00 dB | (exact) |
| | 30 or 40 kHz (should vanish) | | Aliased to 18 or 8 kHz at 0.0 dB |

### 5.2 The limiter's true-peak interpolator

The interpolator's gain is computed from the taps in `Limiter.cpp` (8 taps, Hann-windowed sinc, normalized), at three
points between samples. Frequencies are shown at 44.1 kHz.

| Frequency | A quarter of the way | Halfway |
|---|---|---|
| 4.4 kHz | -0.03 dB | -0.06 dB |
| 13.2 kHz | -0.05 dB | -0.09 dB |
| 15.0 kHz | -0.33 dB | -0.69 dB |
| 16.8 kHz | -0.90 dB | -2.03 dB |
| 18.1 kHz | -1.51 dB | -3.83 dB |
| 19.8 kHz | -2.42 dB | -8.28 dB |

### 5.3 The synth's oscillator

The oscillator is copied from `dsp/Oscillators.h`, at 48 kHz, with each note's frequency set to a whole number of FFT bins.
Every true harmonic then lands on a multiple of the note's bin, and everything else is aliasing.

| Wave | A3 (220 Hz) | A5 (880 Hz) | A6 (1,760 Hz) | A7 (3,520 Hz) |
|---|---|---|---|---|
| Naive saw, loudest alias under the note | 41 dB | 29 dB | 23 dB | 17 dB |
| Phazite's saw (PolyBLEP) | 49 dB | 37 dB | 31 dB | 25 dB |
| Phazite's triangle (no correction) | 82 dB | 59 dB | 47 dB | 34 dB |

## 6. The plan

In order, each its own change, with its tests:

1. **Resampling with r8brain-free-src** (3.1). Add it to `external/` and `docs/third-party.md`. The golden renders of
   files at other rates change. Measure the CPU cost with the benchmarks.
2. **One true-peak detector** (3.3), for the limiter and the loudness meter, tested with the EBU signals. This adds no
   dependency.
3. **The synth's oscillators from band-limited tables** (3.2). This adds no dependency.
4. **ADAA in the saturator** (3.4), then in the multiband and the other drives. This adds no dependency.
5. **MP3 delay** (3.5): the test first, then the LAME-tag trim. Use libmpg123 only if JUCE's decoder needs more.
6. **Matched EQ coefficients** (3.6).
7. **The smaller ones** (3.7).

What stays: Rubber Band and its build, JUCE's FFT for the analyzer and wavetables, JUCE's oversampler for the saturator, the
codecs, the loudness meter's method, and the rest of Phazite's own DSP.

**For the owner:**
- r8brain-free-src (MIT) is on CLAUDE.md's list of licences.
- libmpg123 is LGPL-2.1 only, while the list names LGPL-2.1-or-later. LGPL-2.1 lets a library be used under the GPL, so
  it is compatible, but whether to use it is your call.
- Of the alternatives above, Bungee (MPL-2.0), SLEEF (Boost), HIIR (WTFPL) and minimp3 (CC0) are not on the list. The plan
  needs none of them.

## 7. Sources

- Upstream versions and licences, from each repository on GitHub on 2026-11-06:
  [JUCE](https://github.com/juce-framework/JUCE/releases),
  [Rubber Band](https://breakfastquay.com/rubberband/) (and `external/rubberband/COMPILING.md`, section 7, for its FFTs),
  [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch),
  [Bungee](https://github.com/bungee-audio-stretch/bungee),
  [r8brain-free-src](https://github.com/avaneev/r8brain-free-src),
  [libsamplerate](https://github.com/libsndfile/libsamplerate), [soxr](https://github.com/chirlu/soxr),
  [PFFFT](https://github.com/marton78/pffft), [pocketfft](https://github.com/mreineck/pocketfft),
  [KissFFT](https://github.com/mborgerding/kissfft), [FFTW](https://github.com/FFTW/fftw3),
  [SLEEF](https://github.com/shibatch/sleef), [libebur128](https://github.com/jiixyj/libebur128),
  [minimp3](https://github.com/lieff/minimp3), [CloudSeed](https://github.com/ValdemarOrn/CloudSeed).
  JUCE's bundled codec versions are from `external/JUCE/modules/juce_audio_formats/codecs/`.
- ITU-R BS.1770-5 (2023), Annex 2:
  [itu.int](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf); EBU Tech 3341.
- M. Vicanek, a note on anti-aliased distortion: [vicanek.de](https://www.vicanek.de/articles/AADistortion.pdf); and
  *Matched Second Order Digital Filters* (2016): [vicanek.de](https://www.vicanek.de/articles/BiquadFits.pdf).
- S. Bilbao, F. Esqueda, J. Parker, V. Välimäki, "Antiderivative antialiasing for memoryless nonlinearities", IEEE Signal
  Processing Letters, 2017; J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the aliasing of nonlinear waveshaping using
  continuous-time convolution", DAFx-16.
- J.-M. Jot, A. Chaigne, "Digital delay networks for designing artificial reverberators", AES 90th Convention, 1991.
- mpg123's gapless decoding and licence: [mpg123 (Wikipedia)](https://en.wikipedia.org/wiki/Mpg123).
- BS.1770-5's true-peak filter coefficients, as two open implementations give them:
  [libtruepeak](https://github.com/audionuma/libtruepeak) (`src/truepeak/truepeak.c`) and
  [Sointu](https://github.com/vsariola/sointu) (`tracker/detector.go`).

## 8. What was built (2026-11-06)

The owner: "implement all of the fixes (you can defer the mp3 fix). Also note the startup sound must stay the same (same
as the reference sound)". Everything in section 6 is built but the MP3 delay (3.5), which stays open as written there. New
tests measure each fix the way section 5 measured the faults (`tests/AntialiasingTests.cpp`, `tests/TruePeakTests.cpp`,
and the audio clips' cases in `tests/EngineTests.cpp`). The numbers below are theirs, at 48 kHz unless said; "before" is
the same test run on the code as it was. A synth wave's figure is its loudest alias in the audible band (under 20 kHz),
which is why the "before" figures differ a little from section 5.3's, which counted every alias.

### 8.1 How it measures

| What | Before | After |
|---|---|---|
| A 44.1 kHz file at 48 kHz (3.1): its level from 1 to 20 kHz | -1.2 dB at 15 kHz, -4.4 dB at 20 kHz | Within 0.01 dB |
| ... aliases and images | 17.5 dB under a 15 kHz tone | More than 100 dB under, at 1, 10, 15 and 20 kHz |
| A 96 kHz file at 48 kHz: tones of 30 and 40 kHz | Folded down to 18 and 8 kHz, at full level | More than 100 dB down |
| The synth's saw at A3, A5, A6 and A7 (3.2): its loudest alias under the note | 53, 41, 35 and 29 dB | 105, 93, 87 and 82 dB |
| ... the square (50 % and 25 % wide) | 51 to 54, 41 to 42, 37 to 38 and 34 dB | 102 to 105, 90 to 94, 85 to 87 and 82 dB |
| ... the triangle | 84, 61, 49 and 38 dB | 144, 120, 108 and 96 dB |
| Sines like EBU Tech 3341's true-peak signals (3.3), as the loudness meter reads them: -6.02 dBTP four times, then +3.01 | | -6.02, -5.98, -6.32, -6.03 and +3.05 dBTP: within Tech 3341's +0.2 / -0.4 dB |
| The limiter's output for full-scale sines of 15, 17, 18 and 19 kHz at 44.1 kHz, its ceiling at -1 dB | 18 kHz 3.8 dB over the ceiling (5.2) | -1.03, -1.15, -1.07 and -1.00 dBTP |
| The saturator, 24 dB into hard, fold and tube, a loud 2.5 kHz tone, at 44.1 kHz (3.4): its loudest alias under the tone | 44, 67 and 39 dB | 59, 73 and 50 dB |
| ... at 48 kHz | 44, 74 and 49 dB | 56, 87 and 62 dB |
| ... a quiet tone, which the curve leaves straight, at 20 kHz | Level | -0.29 dB at 44.1 kHz, -0.17 dB at 48 kHz |
| The Filter device's drive, 24 dB, a loud 3 kHz tone | 26 dB | 71 dB |
| The multiband, its three bands saturating, flattening a loud 3 kHz tone on a -12 dB ceiling | 63 dB | 105 dB |
| The EQ against its analog bands, from 20 Hz to 20 kHz (3.6): a 15 kHz bell, Q 1, +12 dB | 5.7 dB off | 0.8 dB |
| ... a 12 kHz high shelf, +6 dB; an 8 kHz low shelf, -6 dB; a 16 kHz low-pass | 0.9, 0.4 and 6.3 dB off | 0.2, 0.05 and 0.6 dB |
| The delay, 44 ms at 44.1 kHz (1,940.4 samples), 80 % feedback (3.7): the eighth echo at 10 kHz against 1 kHz | -15.8 dB (2.3 dB a pass) | -4.6 dB (0.7 dB a pass) |
| The drum sampler: a 15 kHz pad sound at 44.1 kHz, on a 48 kHz engine | -1.65 dB | Within 0.001 dB |
| ... the same pad tuned up an octave, 30 kHz, above the engine's 24 kHz | Folded back to 18 kHz, at full level | 79 dB down |

### 8.2 Where the build differs from the plan

- **r8brain-free-src 7.6**, not 7.7: 7.7 came out the morning of the audit, while 7.6 had five weeks behind it. It is
  header-only, as section 3.1 said, with its built-in Ooura FFT; PFFFT is left out, as it would need compiling and nothing
  needs it. The filter: a 3 % transition band (flat to 0.95 of the lower rate's Nyquist frequency, 20.9 kHz at 44.1 kHz),
  136 dB down (`src/engine/audio/Resampler.cpp`).
- **A region's state is made for its file's rate and the engine's** (`RegionState`), with a resampler for each of the
  file's channels, two at most. A state made for other rates plays silence rather than play at the wrong speed. After a
  jump, the file starts at the nearest sample: up to half a sample off, 11 microseconds at 44.1 kHz.
- **True peak at four times the rate at every rate**, not less at higher rates as section 3.3 proposed: 48
  multiplications a sample a channel cost nothing measurable. The 48 coefficients are BS.1770-5's, checked against two
  open implementations (section 7), as the ITU's PDF couldn't be read here. A sine whose cycle is a whole number of
  samples or two (a third of the rate) can read up to 0.3 dB low, as any four-times meter does. The limiter's latency is
  2 samples more, 101 at 48 kHz, as section 3.3 said.
- **Antiderivative anti-aliasing needed oversampling, and a correction.** Its average of two samples dulls the top
  octave: by 2.4 dB at 20 kHz at twice 44.1 kHz, and by 11.7 dB at 44.1 kHz itself. So a short linear-phase filter after
  it makes what the curve leaves straight level again (within 0.3 dB to 20 kHz; 4 samples at twice the rate in all, a
  whole 2 at the sound's, `dsp/Saturation.h`), and the drives that ran at the sound's rate now run at twice it as well:
  - the Filter device, through `juce::dsp::Oversampling` as the saturator, while its drive is on; undriven, its sound
    takes a plain delay as long, so that its latency (now 6 samples at 48 kHz) stays the same, faded from one to the
    other over 64 samples as the drive comes on or goes off (always through the oversampling, an undriven filter cost
    five times what it had);
  - the wavetable synth, through a per-sample oversampler for each voice (`dsp/Oversampling.h`, its half-band filters
    those of `juce::dsp::Oversampling`), only while its drive is on, faded in and out over 32 samples;
  - the multiband: each band's saturation, and the dry sound, through the per-sample oversampler while any of the three
    bands saturates, so that the bands stay in step; while none does, a plain delay as long, which leaves a limited peak
    as it was (run through the half-band filters, the master band's limited peaks rose by 3 dB). The master's saturation
    alike. Its latency is 2 saturations' more, 5 samples each: 490 samples at 48 kHz, where it was 480.

  The compressor's drive still averages at the sound's rate, as it did before the audit.
- **The tube curve's antiderivative** comes from a table worked out once, as section 3.4 proposed.
- **The synth's tables keep 32 samples for each cycle of their highest harmonic**, not the wavetable's 4, and the
  oscillator reads them by straight lines between samples rather than cubics: at 4 samples, cubic interpolation left
  aliases only 55 dB under at A6; at 16, 91 dB at worst, but the synth cost half as much again as it had; at 32, read linearly,
  82 dB at worst (A7), and it costs about what it did (section 8.4). The two tables take 1 MB, made once. A band-limited wave overshoots
  its jumps by 9 % of them (Gibbs): a saw peaks at 1.13, a square at 1.17. The wavetable synth's sub and the vocoder's
  buzz, which use the same oscillator, are band-limited too.
- **Matched coefficients for the EQ alone.** The other devices keep their bilinear filters: their cuts and tone controls
  aren't meant as analog shapes, and the delay's cuts are in the startup sound. A notch keeps its zeros exactly at its
  frequency; a pole that would lie above the Nyquist frequency (a high shelf boosted near the top) stays just below it.
- **The delay's 4-point Lagrange interpolation** loses 0.7 dB at 10 kHz a pass at 44.1 kHz at the worst fraction, where a
  straight line lost 2.4. Longer interpolation, or an all-pass, would keep more; an all-pass rings when the delay glides.
- **The reverb:** each line's damping matched to its length at the Nyquist frequency, and its modulated lines read by the
  delay line's Lagrange interpolation.
- **The drum sampler:** a windowed sinc of 16 zero crossings a side, Kaiser-windowed (about 70 dB down), stretched as far
  as the pad is pitched up (up to 8 times); a pad at its own rate and untuned plays its samples as they are.
- **The MP3 delay** (3.5) isn't built.

### 8.3 The startup sound

The startup sound uses the delay, whose reading changed. At 48 and 96 kHz it is the same as before, sample for sample: its
44 ms echo is a whole number of samples there, which the new interpolation reads exactly. At 44.1 and 88.2 kHz the echo
fell between samples, and the old straight-line reading had dulled it a little more on every pass, so that those rates'
tails were darker than the 48 kHz sound, which was tuned to the reference. It now rounds its echo to a whole number of
samples at every rate, and the tails' brightness follows the 48 kHz sound's (the echoes' energy from 5 to 12 kHz against
0.5 to 3 kHz, in the mono sum):

| Rate | 0.13 to 0.4 s | 0.4 to 0.8 s | 0.8 to 1.2 s |
|---|---|---|---|
| 48 kHz, before and after | -7.4 dB | -9.7 dB | -9.1 dB |
| 44.1 kHz before | -11.3 dB | -20.9 dB | -28.3 dB |
| 44.1 kHz after | -7.3 dB | -8.7 dB | -8.6 dB |
| 96 kHz, before and after | -7.4 dB | -10.1 dB | -10.9 dB |
| 88.2 kHz before | -8.2 dB | -12.4 dB | -14.7 dB |
| 88.2 kHz after | -7.4 dB | -10.2 dB | -10.4 dB |

### 8.4 What it costs

From `phazite_tests.exe --test-suite=benchmark --no-skip` in the `windows-profile` build, before and after, on the
owner's PC; the full table is in [performance-audit.md](performance-audit.md), section 15.

| % of a core | Before | After |
|---|---|---|
| Synth, 8 notes | 0.71 | 0.78 |
| Drum Sampler, 4 pads every 16th (an untuned pad at its own rate now reads its samples as they are) | 0.87 | 0.22 |
| Saturator | 0.37 | 0.78 |
| Limiter | 0.23 | 0.33 |
| Reverb | 0.62 | 0.91 |
| A playing clip of a 44.1 kHz file on a 48 kHz engine | 0.08 | 0.24 |
| 20 tracks, each a synth, an EQ, a compressor and a reverb, on one thread | 27.6 | 34.1 |

The Filter device and the multiband cost what they did while nothing drives or saturates. Resampling costs three times
what Lagrange interpolation did; a 110 dB filter in place of 136 dB saved only 9 %, so the filter stays. Converting each
file once into the song's cache (3.1) would make a clip free as it plays, if many clips at another rate ever weigh on a
song.

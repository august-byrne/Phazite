// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Sampler.h"
#include "FactorySamples.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/NoteValues.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view modes[] = { "Classic", "One-Shot", "Slice" };
    constexpr std::string_view sliceMethods[] = { "Transients", "Beats", "Equal", "Manual" };
    constexpr std::string_view playbacks[] = { "Trigger", "Gate" };
    constexpr std::string_view lfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random" };
    constexpr std::string_view lfoTargets[] = { "Pitch", "Cutoff", "Level" };
    constexpr std::string_view voiceModes[] = { "Poly", "Mono" };

    // The note the default sound (an 808, FactorySamples.h) plays at its own pitch: C2.
    constexpr float defaultRoot = 36.0f;

    // A time, from minimum to most, with midpoint halfway along its knob.
    constexpr ParameterInfo makeTime (std::string_view id, std::string_view name, float minimum, float defaultValue, float most = 10000.0f,
                                      float midpoint = 300.0f)
    {
        return { .id = id,
                 .name = name,
                 .minimum = minimum,
                 .maximum = most,
                 .defaultValue = defaultValue,
                 .unit = Unit::milliseconds,
                 .midpoint = midpoint };
    }

    constexpr ParameterInfo makeChoice (std::string_view id, std::string_view name, std::span<const std::string_view> choices,
                                        float defaultValue)
    {
        return { .id = id,
                 .name = name,
                 .minimum = 0.0f,
                 .maximum = (float) choices.size() - 1.0f,
                 .defaultValue = defaultValue,
                 .unit = Unit::choice,
                 .choices = choices };
    }

    constexpr ParameterInfo makePercent (std::string_view id, std::string_view name, float defaultValue, float maximum = 1.0f)
    {
        return { .id = id, .name = name, .maximum = maximum, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    constexpr ParameterInfo makeToggle (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::toggle };
    }

    // How an envelope's stage bends (dsp::Adsr::setCurves()): from -100 % (fast at first) through 0 (straight) to 100 % (slow at
    // first), an analog envelope's curve at first.
    constexpr ParameterInfo makeCurve (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .minimum = -1.0f, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    // In the order of Sampler::Parameter. The IDs are saved in projects. The filter, its envelope and the LFO take the
    // synth's values (devices/synth/Synth.h), so each is heard at once as it is switched on.
    constexpr ParameterInfo parameters[] = {
        makeChoice ("mode", "Mode", modes, 0.0f),
        { .id = "root", .name = "Root", .minimum = 0.0f, .maximum = 127.0f, .defaultValue = defaultRoot, .unit = Unit::note, .step = 1.0f },
        { .id = "transpose",
          .name = "Transpose",
          .minimum = -48.0f,
          .maximum = 48.0f,
          .defaultValue = 0.0f,
          .unit = Unit::semitones,
          .step = 1.0f },
        { .id = "fine", .name = "Fine", .minimum = -50.0f, .maximum = 50.0f, .defaultValue = 0.0f, .unit = Unit::cents },
        makePercent ("start", "Start", 0.0f),
        makePercent ("end", "End", 1.0f),
        makeToggle ("reverse", "Reverse", 0.0f),
        makeToggle ("loopOn", "Loop On", 0.0f),
        makePercent ("loopStart", "Loop Start", 2.0f / 3.0f), // the last third, a held note's sustain, until a file says
        makePercent ("loopEnd", "Loop End", 1.0f),
        makePercent ("loopCrossfade", "Crossfade", 0.1f, 0.5f),
        makeChoice ("playback", "Playback", playbacks, 0.0f),
        makeTime ("fadeIn", "Fade In", 0.0f, 0.0f, 1000.0f, 50.0f),
        makeTime ("fadeOut", "Fade Out", 0.0f, 10.0f, 1000.0f, 50.0f),
        makeChoice ("sliceBy", "Slice By", sliceMethods, 0.0f),
        makePercent ("sensitivity", "Sensitivity", 0.5f),
        makeChoice ("sliceDivision", "Division", dsp::noteValueNames, 2.0f), // a sixteenth note
        { .id = "sliceCount",
          .name = "Slices",
          .minimum = 2.0f,
          .maximum = (float) Sampler::maxSlices,
          .defaultValue = 8.0f,
          .unit = Unit::none,
          .midpoint = 16.0f,
          .step = 1.0f },
        makeToggle ("playThrough", "Play Through", 0.0f),
        makeToggle ("choke", "Choke", 1.0f),
        makeTime ("attack", "Attack", 0.5f, 1.0f), // never instant, so that a note doesn't click
        makeTime ("decay", "Decay", 0.0f, 300.0f),
        makePercent ("sustain", "Sustain", 1.0f), // the sound as it is, for as long as the note is held
        makeTime ("release", "Release", 0.5f, 50.0f),
        makeCurve ("attackCurve", "Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("decayCurve", "Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("releaseCurve", "Release Curve", dsp::Adsr::analogSettleCurve),
        makeToggle ("filterOn", "Filter On", 0.0f), // the sound as it is, until switched on
        makeChoice ("filterType", "Filter Type", dsp::filterModeNames, 0.0f),
        { .id = "cutoff",
          .name = "Cutoff",
          .minimum = 20.0f,
          .maximum = 20000.0f,
          .defaultValue = 3000.0f,
          .unit = Unit::hertz,
          .midpoint = 632.456f }, // the middle of the range, on a log scale
        makePercent ("resonance", "Resonance", 0.15f),
        makeChoice ("filterSlope", "Filter Slope", dsp::filterSlopeNames, 0.0f),
        makePercent ("keyTracking", "Key Tracking", 0.5f),
        { .id = "filterEnvelope",
          .name = "Filter Envelope",
          .minimum = -48.0f,
          .maximum = 48.0f,
          .defaultValue = 24.0f,
          .unit = Unit::semitones },
        makeToggle ("filterEnvelopeOn", "Filter Envelope On", 0.0f),
        makeTime ("filterAttack", "Filter Attack", 0.0f, 1.0f),
        makeTime ("filterDecay", "Filter Decay", 0.0f, 400.0f),
        makePercent ("filterSustain", "Filter Sustain", 0.2f),
        makeTime ("filterRelease", "Filter Release", 0.0f, 300.0f),
        makeCurve ("filterAttackCurve", "Filter Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("filterDecayCurve", "Filter Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("filterReleaseCurve", "Filter Release Curve", dsp::Adsr::analogSettleCurve),
        makeToggle ("lfoOn", "LFO On", 0.0f),
        makeChoice ("lfoShape", "LFO Shape", lfoShapes, 0.0f),
        makeChoice ("lfoTarget", "LFO Target", lfoTargets, 0.0f),
        { .id = "lfoRate",
          .name = "LFO Rate",
          .minimum = 0.05f,
          .maximum = 20.0f,
          .defaultValue = 5.0f,
          .unit = Unit::hertz,
          .midpoint = 1.0f },
        makePercent ("lfoDepth", "LFO Depth", 0.25f), // heard at once, as the LFO is switched on
        makeChoice ("voiceMode", "Voices", voiceModes, 0.0f),
        { .id = "glide",
          .name = "Glide",
          .minimum = 0.0f,
          .maximum = 2000.0f,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds,
          .midpoint = 200.0f },
        { .id = "bendRange", .name = "Bend Range", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 2.0f, .unit = Unit::semitones },
        makePercent ("velocity", "Velocity", 0.6f),
        { .id = "level", .name = "Level", .minimum = -36.0f, .maximum = 6.0f, .defaultValue = -6.0f, .unit = Unit::decibels },
    };

    static_assert (std::size (parameters) == (size_t) Sampler::level + 1);

    constexpr int sustainPedal = 64; // the MIDI controller
    constexpr double lowestCutoff = 20.0;
    constexpr int mostCandidates = 4 * Sampler::maxSlices; // slices found, before those put by hand join them

    template <typename Enum>
    Enum choiceOf (float value)
    {
        return static_cast<Enum> (std::lround (value));
    }

    // What the markers mark out, in a sound's samples: from the start to the end (the other way round when they cross), at
    // least samplesApart long where the sound is.
    std::pair<int, int> getRegion (int frames, float startValue, float endValue) noexcept
    {
        auto from = (int) std::lround (std::clamp (std::min (startValue, endValue), 0.0f, 1.0f) * (float) frames);
        auto to = (int) std::lround (std::clamp (std::max (startValue, endValue), 0.0f, 1.0f) * (float) frames);
        from = std::clamp (from, 0, std::max (0, frames - 1));
        to = std::clamp (std::max (to, from + Sampler::samplesApart), from + 1, frames);
        return { from, std::max (to, from + 1) };
    }
} // namespace

const DeviceInfo Sampler::info { .typeID = typeID, .name = "Sampler", .kind = DeviceKind::instrument, .parameters = parameters };

Sampler::Slices Sampler::getSlices (const Sound& sound, const ManualSlices* manual, std::span<const float> values, double tempo) noexcept
{
    Slices slices;
    const auto frames = sound.audio.getNumSamples();

    if (frames <= 0)
        return slices;

    const auto region = getRegion (frames, values[start], values[end]);
    const auto from = region.first, to = region.second;

    // The slices it finds, in order, less those taken away by hand.
    std::array<int, mostCandidates> found {};
    auto numFound = 0;

    const auto takenAway = [manual, frames] (int at)
    {
        if (manual != nullptr)
            for (const auto position : manual->removed)
                if (std::abs ((int) std::lround (position * (float) frames) - at) < samplesApart)
                    return true;

        return false;
    };

    const auto find = [&] (int at)
    {
        if (numFound < mostCandidates && at > from && at < to && ! takenAway (at))
            found[(size_t) numFound++] = at;
    };

    switch (choiceOf<SliceBy> (values[sliceBy]))
    {
        case SliceBy::transients:
            for (const auto& onset : sound.onsets)
                if (onset.slicesFrom <= values[sensitivity])
                    find (onset.sample);

            break;

        case SliceBy::beats:
        {
            // At the sound's own tempo, where it says how many beats it lasts; else at the song's.
            const auto perBeat = sound.beats > 0.0 ? (double) frames / sound.beats : sound.sampleRate * 60.0 / std::max (1.0, tempo);
            const auto length = dsp::getNoteValueBeats (values[sliceDivision]) * perBeat;

            if (length >= samplesApart)
                for (auto at = (double) from + length; at < (double) to && numFound < mostCandidates; at += length)
                    find ((int) std::lround (at));

            break;
        }

        case SliceBy::equal:
        {
            const auto parts = std::clamp ((int) std::lround (values[sliceCount]), 1, maxSlices);

            for (int part = 1; part < parts; ++part)
                find (from + (int) ((juce::int64) (to - from) * part / parts));

            break;
        }

        case SliceBy::manual:
            break;
    }

    // With those put in by hand, in order: the first at the start, each at least samplesApart after the one before and
    // before the end.
    auto count = 0;

    const auto add = [&slices, &count, to] (int at, bool byHand)
    {
        if (count >= maxSlices || at > to - samplesApart)
            return;

        if (count > 0 && at < slices.bounds[(size_t) count - 1] + samplesApart)
        {
            slices.byHand[(size_t) count - 1] = slices.byHand[(size_t) count - 1] || byHand;
            return;
        }

        slices.bounds[(size_t) count] = at;
        slices.byHand[(size_t) count] = byHand;
        ++count;
    };

    add (from, false);
    auto next = 0;
    const auto numAdded = manual != nullptr ? manual->added.size() : 0;
    size_t nextAdded = 0;

    while (next < numFound || nextAdded < numAdded)
    {
        const auto added =
            nextAdded < numAdded ? (int) std::lround (std::clamp (manual->added[nextAdded], 0.0f, 1.0f) * (float) frames) : to;

        if (next < numFound && found[(size_t) next] <= added)
        {
            add (found[(size_t) next++], false);
            continue;
        }

        ++nextAdded;

        if (added > from)
            add (added, true);
    }

    slices.count = count;
    slices.bounds[(size_t) count] = to;
    return slices;
}

Sampler::Sampler()
    : playingSound (getFactorySample (0))
{
    sound.store (playingSound.get(), std::memory_order_release);
}

Sampler::~Sampler() = default;

void Sampler::freeRetired()
{
    const auto done = blocksDone.load (std::memory_order_acquire);
    retired.erase (std::remove_if (retired.begin(), retired.end(), [done] (const Retired& old) { return done >= old.safeAfter; }),
                   retired.end());
}

void Sampler::setSound (std::shared_ptr<const Sound> newSound)
{
    freeRetired();

    if (newSound == nullptr)
        newSound = getFactorySample (0);

    if (newSound == playingSound)
        return;

    sound.store (newSound.get(), std::memory_order_release);

    // The audio thread may play the one before until the end of its next block, when its voices notice the change.
    retired.push_back ({ std::move (playingSound), blocksDone.load (std::memory_order_acquire) + 2 });
    playingSound = std::move (newSound);
}

void Sampler::setManualSlices (std::shared_ptr<const ManualSlices> slices)
{
    freeRetired();

    if (slices == playingSlices)
        return;

    manualSlices.store (slices.get(), std::memory_order_release);
    retired.push_back ({ std::move (playingSlices), blocksDone.load (std::memory_order_acquire) + 2 });
    playingSlices = std::move (slices);
}

void Sampler::audition (int note, float strength)
{
    const auto scope = auditionFifo.write (1);

    if (scope.blockSize1 > 0)
        auditions[(size_t) scope.startIndex1] = { note, std::max (strength, 0.001f), juce::Time::getMillisecondCounter() };
}

void Sampler::stopAudition (int note)
{
    const auto scope = auditionFifo.write (1);

    if (scope.blockSize1 > 0)
        auditions[(size_t) scope.startIndex1] = { note, 0.0f, juce::Time::getMillisecondCounter() };
}

bool Sampler::isWaitingToPlay() const noexcept
{
    return auditionFifo.getNumReady() > 0;
}

void Sampler::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    for (auto& voice : voices)
    {
        voice.amp.prepare (sampleRate);
        voice.filterEnvelope.prepare (sampleRate);

        for (auto& filter : voice.filters)
            filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));
    }

    lfo.prepare (sampleRate);
    reset();
}

void Sampler::reset()
{
    for (auto& voice : voices)
    {
        voice.amp.reset();
        voice.filterEnvelope.reset();
        voice.note = -1;
        voice.held = voice.sustained = voice.oneShot = voice.fades = false;
        voice.sound = nullptr;
        voice.slice = -1;
        voice.filterEnvelopeLevel = 0.0f;
        voice.fade = 1.0f;
        voice.fadeStep = 0.0f;

        for (auto& filter : voice.filters)
            filter.reset();

        voice.filterIsSet = false;
    }

    lfo.reset();
    numHeld = 0;
    notesPlayed = 0;
    lastNote = -1.0f;
    bend = 0.0f;
    pedalDown = false;
    shownMode = -1;
}

double Sampler::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

void Sampler::releaseVoice (Voice& voice)
{
    voice.held = voice.sustained = false;

    if (voice.oneShot)
        return; // it plays to its end

    voice.amp.noteOff();
    voice.filterEnvelope.noteOff();
}

Sampler::Voice& Sampler::findVoiceFor (int note)
{
    // The voice playing this note already, then a free one, then the one ringing out longest, then the oldest: among those
    // that play notes, not the last, which fades out a voice taken (stealVoice()).
    const std::span<Voice> playing (voices.data(), (size_t) maxVoices);

    for (auto& voice : playing)
        if (voice.note == note && voice.amp.isActive())
            return voice;

    for (auto& voice : playing)
        if (! voice.amp.isActive())
            return voice;

    Voice* oldest = nullptr;

    for (auto& voice : playing)
        if (! voice.held && ! voice.sustained && (oldest == nullptr || voice.started < oldest->started))
            oldest = &voice;

    if (oldest != nullptr)
        return *oldest;

    for (auto& voice : playing)
        if (oldest == nullptr || voice.started < oldest->started)
            oldest = &voice;

    return *oldest;
}

void Sampler::stealVoice (Voice& voice)
{
    // What it played fades out from a copy of it, over a few milliseconds, while it starts the new note from silence. The
    // copy reuses the last voice's memory (every voice is prepared alike), so this doesn't allocate.
    auto& fading = voices[(size_t) maxVoices];
    fading = voice;
    fading.note = -1;
    fading.held = fading.sustained = false;
    fading.fadeStep = std::max (fading.fadeStep, (float) (1.0 / (stealFadeSeconds * sampleRate)));
    voice.amp.reset();
    voice.filterEnvelope.reset();
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
}

void Sampler::placeVoice (Voice& voice, const Sound& playing, int from, int to, std::span<const float> values, bool loops) const noexcept
{
    // Counted the way it plays: reversed, from the end back, so that what it plays always runs from regionStart up.
    const auto frames = playing.audio.getNumSamples();
    voice.sound = &playing;
    voice.reversed = values[reverse] > 0.5f;
    voice.regionStart = voice.reversed ? (double) (frames - to) : (double) from;
    voice.regionEnd = voice.reversed ? (double) (frames - from) : (double) to;
    voice.position = voice.regionStart;
    voice.fadeIn = (voice.reversed ? to < frames : from > 0) ? declickSamples : 0;
    voice.loops = false;
    voice.fadeLength = 0.0;

    if (! loops)
        return;

    auto loopFrom = std::clamp ((int) std::lround (values[loopStart] * (float) frames), from, to);
    auto loopTo = std::clamp ((int) std::lround (values[loopEnd] * (float) frames), from, to);

    if (loopTo < loopFrom)
        std::swap (loopFrom, loopTo);

    if (loopTo - loopFrom < samplesApart)
        return;

    voice.loops = true;
    voice.loopStartAt = voice.reversed ? (double) (frames - loopTo) : (double) loopFrom;
    voice.loopEndAt = voice.reversed ? (double) (frames - loopFrom) : (double) loopTo;

    // Its seam crossfades with what comes before the loop's start, as long as there is some.
    const auto length = voice.loopEndAt - voice.loopStartAt;
    voice.fadeLength = std::min ({ (double) values[loopCrossfade] * length, 0.5 * length, voice.loopStartAt });
}

void Sampler::startVoice (Voice& voice, float velocityValue, std::span<const float> values)
{
    for (auto& filter : voice.filters)
        filter.reset();

    const auto sensitivityValue = values[velocity];
    voice.filterIsSet = false;
    voice.filterEnvelopeLevel = 0.0f;
    voice.velocityGain = 1.0f - sensitivityValue + sensitivityValue * velocityValue * velocityValue;
    voice.age = 0;
    voice.started = ++notesPlayed;

    // A voice its fades shape lets go as a gated note does, over the fade out; the amp envelope shapes the others.
    if (voice.fades)
        voice.amp.setParameters (0.0f, 0.0f, 1.0f, std::max (0.0005f, values[fadeOut] * 0.001f));
    else
        voice.amp.setParameters (values[attack] * 0.001f, values[decay] * 0.001f, values[sustain], values[release] * 0.001f);

    voice.amp.noteOn();
    voice.filterEnvelope.noteOn();
}

void Sampler::startNote (int note, float velocityValue, std::span<const float> values, bool portamento)
{
    const auto* playing = sound.load (std::memory_order_acquire);

    if (playing == nullptr || playing->audio.getNumSamples() == 0)
        return;

    const auto playMode = choiceOf<Mode> (values[mode]);

    if (playMode == Mode::slice)
    {
        startSlice (note, velocityValue, values);
        return;
    }

    const auto glidesIn = portamento && lastNote >= 0.0f;
    const auto from = glidesIn || (values[glide] > 0.0f && lastNote >= 0.0f) ? lastNote : (float) note;
    lastNote = (float) note;

    // A portamento note slides in evenly over Glide's time, or a short one while Glide is off.
    const auto seconds = values[glide] > 0.0f ? (double) values[glide] * 0.001 : portamentoSeconds;
    const auto slideFrom = [&] (float pitch)
    { return glidesIn ? std::abs ((float) note - pitch) / (float) std::max (1.0, seconds * sampleRate) : 0.0f; };

    const auto region = getRegion (playing->audio.getNumSamples(), values[start], values[end]);
    const auto loops = playMode == Mode::classic && values[loopOn] > 0.5f;
    const auto oneShot = playMode == Mode::oneShot;

    const auto begin = [&] (Voice& voice, float pitchFrom)
    {
        placeVoice (voice, *playing, region.first, region.second, values, loops);
        voice.fades = oneShot;
        voice.oneShot = oneShot && choiceOf<int> (values[playback]) == 0;
        voice.slice = -1;
        voice.pitch = pitchFrom;
        startVoice (voice, velocityValue, values);
    };

    if (choiceOf<int> (values[voiceMode]) == 1)
    {
        // Mono: the key goes on top of those held; with one held already, the voice glides to it, playing on, as an 808
        // played as a bass line glides.
        auto kept = 0;

        for (int i = 0; i < numHeld; ++i)
            if (heldKeys[(size_t) i] != note)
                heldKeys[(size_t) kept++] = heldKeys[(size_t) i];

        numHeld = std::min (kept, maxHeld - 1);
        heldKeys[(size_t) numHeld++] = note;

        auto& voice = voices[0];
        const auto legato = voice.amp.isActive() && voice.held && voice.fadeStep <= 0.0f && ! oneShot && voice.sound == playing;

        if (! legato)
        {
            if (voice.amp.isActive())
                stealVoice (voice);

            begin (voice, from);
        }

        voice.note = note;
        voice.target = (float) note;
        voice.held = true;
        voice.sustained = false;
        voice.slidePerSample = slideFrom (voice.pitch);
        return;
    }

    auto& voice = findVoiceFor (note);

    if (voice.amp.isActive())
        stealVoice (voice);

    begin (voice, from);
    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.target = (float) note;
    voice.slidePerSample = slideFrom (from);
}

void Sampler::startSlice (int note, float velocityValue, std::span<const float> values)
{
    const auto* playing = sound.load (std::memory_order_acquire);
    const auto slices = getSlices (*playing, manualSlices.load (std::memory_order_acquire), values, songTempo);
    const auto index = note - firstSliceNote;

    if (! juce::isPositiveAndBelow (index, slices.count))
        return;

    // With choke, a slice cuts off the one playing, as a chopped break's slices do.
    if (values[choke] > 0.5f)
        for (size_t i = 0; i < (size_t) maxVoices; ++i)
            if (auto& other = voices[i]; other.amp.isActive() && other.slice >= 0)
                stealVoice (other);

    auto& voice = findVoiceFor (note);

    if (voice.amp.isActive())
        stealVoice (voice);

    const auto through = values[playThrough] > 0.5f;
    placeVoice (voice, *playing, slices.bounds[(size_t) index],
                through ? slices.bounds[(size_t) slices.count] : slices.bounds[(size_t) index + 1], values, false);
    voice.fades = true;
    voice.oneShot = choiceOf<int> (values[playback]) == 0;
    voice.slice = index;
    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.pitch = voice.target = (float) note;
    voice.slidePerSample = 0.0f;
    startVoice (voice, velocityValue, values);
}

void Sampler::stopNote (int note, std::span<const float> values)
{
    const auto legatoMode = choiceOf<int> (values[voiceMode]) == 1 && choiceOf<Mode> (values[mode]) == Mode::classic;

    if (legatoMode)
    {
        auto kept = 0;

        for (int i = 0; i < numHeld; ++i)
            if (heldKeys[(size_t) i] != note)
                heldKeys[(size_t) kept++] = heldKeys[(size_t) i];

        numHeld = kept;
        auto& voice = voices[0];

        if (voice.note != note || ! voice.held)
            return;

        // Back to the key held before, if one still is.
        if (numHeld > 0)
        {
            voice.note = heldKeys[(size_t) numHeld - 1];
            voice.target = (float) voice.note;
            voice.slidePerSample = 0.0f;
            lastNote = voice.target;
            return;
        }
    }

    for (auto& voice : voices)
    {
        if (voice.note == note && voice.held)
        {
            if (pedalDown && ! voice.oneShot)
            {
                voice.held = false;
                voice.sustained = true;
            }
            else
            {
                releaseVoice (voice);
            }
        }
    }
}

void Sampler::slideTo (int note, float seconds)
{
    // Every voice that sounds, to the slide note's pitch, evenly over its length; a note after it glides from there.
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
    {
        if (auto& voice = voices[i]; voice.amp.isActive() && (voice.held || voice.sustained) && voice.fadeStep <= 0.0f && voice.slice < 0)
        {
            voice.target = (float) note;
            voice.slidePerSample = std::abs (voice.target - voice.pitch) / (float) std::max (1.0, (double) seconds * sampleRate);
        }
    }

    lastNote = (float) note;
}

void Sampler::handle (const NoteEvent& event, std::span<const float> values)
{
    switch (event.type)
    {
        case NoteEvent::Type::noteOn:
        case NoteEvent::Type::portamentoOn:
            startNote (event.number, event.value, values, event.type == NoteEvent::Type::portamentoOn);
            break;

        case NoteEvent::Type::slide:
            slideTo (event.number, event.value);
            break;

        case NoteEvent::Type::noteOff:
            stopNote (event.number, values);
            break;

        case NoteEvent::Type::pitchBend:
            bend = event.value;
            break;

        case NoteEvent::Type::controller:
            if (event.number == sustainPedal)
            {
                pedalDown = event.value >= 0.5f;

                if (! pedalDown)
                    for (auto& voice : voices)
                        if (voice.sustained)
                            releaseVoice (voice);
            }

            break;

        case NoteEvent::Type::allNotesOff:
        {
            numHeld = 0;
            pedalDown = false;

            // Playback stopped or jumped: every note lets go. Those that had only just begun when it stopped fade out at
            // once too, one-shots with them, so that nothing the song began just before Stop sounds after it.
            const auto stopStep = (float) (1.0 / (stopFadeSeconds * sampleRate));

            for (auto& voice : voices)
            {
                if (! voice.amp.isActive())
                    continue;

                releaseVoice (voice);

                if (! songPlaying && voice.age < justBegunSeconds * sampleRate)
                    voice.fadeStep = std::max (voice.fadeStep, stopStep);
            }

            break;
        }
    }
}

double Sampler::getFilterMagnitude (std::span<const float> values, double cutoffHertz, double frequency, double rate)
{
    // As render() sets its voices' filters up.
    const dsp::FilterSettings settings { .mode = dsp::toFilterMode (values[filterType]),
                                         .frequency = cutoffHertz,
                                         .resonance = values[resonance],
                                         .steep = dsp::isSteep (values[filterSlope]),
                                         .q = dsp::getResonanceQ (values[resonance]) };
    return dsp::getMagnitude (dsp::makeMultimode (settings, rate), frequency, rate);
}

void Sampler::makeOctaves (Sound& sound)
{
    const auto* above = &sound.audio;

    for (auto& octave : sound.octaves)
    {
        const auto length = above->getNumSamples();
        octave.setSize (above->getNumChannels(), (length + 1) / 2);

        for (int channel = 0; channel < above->getNumChannels(); ++channel)
            dsp::SincReader::halve (above->getReadPointer (channel), length, octave.getWritePointer (channel));

        above = &octave;
    }
}

void Sampler::readAt (const Voice& voice, double position, float& left, float& right) const noexcept
{
    const auto frames = voice.sound->audio.getNumSamples();
    auto at = voice.reversed ? (double) (frames - 1) - position : position;

    // Pitched up an octave or more, from the octave of the sound under its pitch, read with less than twice its step: the
    // same sound, with as little as ever folded back, for at most twice the samples of a sound read at its own rate.
    const auto* audio = &voice.sound->audio;
    auto step = voice.step;

    for (const auto& octave : voice.sound->octaves)
    {
        if (step < 2.0 || octave.getNumSamples() == 0)
            break;

        audio = &octave;
        step *= 0.5;
        at *= 0.5;
    }

    const auto length = audio->getNumSamples();
    const auto* first = audio->getReadPointer (0);
    const auto stereo = audio->getNumChannels() > 1;
    const auto* second = audio->getReadPointer (stereo ? 1 : 0);

    // At its own rate and untuned, its samples as they are; otherwise read between them by a windowed sinc.
    if (juce::exactlyEqual (step, 1.0) && juce::exactlyEqual (at, std::floor (at)))
    {
        const auto index = (int) at;

        if (index < 0 || index >= length)
        {
            left = right = 0.0f;
            return;
        }

        left = first[index];
        right = second[index];
        return;
    }

    if (! stereo)
    {
        left = right = reader.read (first, length, at, step);
        return;
    }

    reader.read (first, second, length, at, step, left, right);
}

void Sampler::render (float* left, float* right, int numSamples, std::span<const float> values)
{
    const auto filterMode = dsp::toFilterMode (values[filterType]);
    const auto steep = dsp::isSteep (values[filterSlope]);
    const auto q = dsp::getResonanceQ (values[resonance]);
    const auto outputGain = dsp::decibelsToGain (values[level]);
    const auto lfoAim = choiceOf<LfoTarget> (values[lfoTarget]);
    const auto depth = values[lfoOn] > 0.5f ? values[lfoDepth] : 0.0f;
    const auto envelopeAmount = values[filterEnvelopeOn] > 0.5f ? values[filterEnvelope] : 0.0f;
    const auto filtered = values[filterOn] > 0.5f;
    const auto lfoWave = choiceOf<dsp::LfoShape> (values[lfoShape]);
    const auto glideSamples = (double) values[glide] * 0.001 * sampleRate;
    const auto bent = bend * values[bendRange];
    const auto offset = values[transpose] + values[fine] * 0.01f;
    const auto rootNote = values[root];
    const auto nyquistLimit = 0.45 * sampleRate;
    const auto fadeInSamples = (double) values[fadeIn] * 0.001 * sampleRate;
    const auto fadeOutSamples = (double) values[fadeOut] * 0.001 * sampleRate;

    for (int begin = 0; begin < numSamples; begin += controlStep)
    {
        const auto length = std::min (controlStep, numSamples - begin);
        std::array<float, controlStep> moved {};

        // The glide moves as far as this stretch's length takes it, so it keeps its time however the samples come.
        const auto glideStep = glideSamples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-length / glideSamples));

        for (int i = 0; i < length; ++i)
            moved[(size_t) i] = lfo.next (lfoWave);

        const auto pitchMove = lfoAim == LfoTarget::pitch ? depth * moved[0] : 0.0f;
        const auto cutoffMove = lfoAim == LfoTarget::cutoff ? depth * 24.0f * moved[0] : 0.0f;

        for (auto& voice : voices)
        {
            if (! voice.amp.isActive() || voice.sound == nullptr)
                continue;

            voice.age = std::min (voice.age + length, 1 << 30);

            // Pitch: a slide moves evenly until it gets there, a glide part of the way each step. A slice plays at its own
            // pitch; a note against the root key.
            if (voice.slidePerSample > 0.0f)
            {
                const auto stepSize = voice.slidePerSample * (float) length;
                const auto gap = voice.target - voice.pitch;
                voice.pitch = std::abs (gap) <= stepSize ? voice.target : voice.pitch + std::copysign (stepSize, gap);
                voice.slidePerSample = std::abs (gap) <= stepSize ? 0.0f : voice.slidePerSample;
            }
            else
            {
                voice.pitch += (voice.target - voice.pitch) * glideStep;
            }

            const auto keyed = voice.slice < 0 ? voice.pitch - rootNote : 0.0f;
            const auto semitones = (double) (keyed + offset + bent + pitchMove);
            voice.step = voice.sound->sampleRate / sampleRate * std::exp2 (semitones / 12.0);

            if (filtered)
            {
                const auto moves = envelopeAmount * voice.filterEnvelopeLevel + values[keyTracking] * keyed + cutoffMove;
                const auto frequency = std::clamp ((double) values[cutoff] * std::pow (2.0, moves / 12.0), lowestCutoff, nyquistLimit);
                const dsp::FilterSettings settings { .mode = filterMode,
                                                     .frequency = frequency,
                                                     .resonance = values[resonance],
                                                     .steep = steep,
                                                     .q = q };

                if (! voice.filterIsSet || ! dsp::areSame (settings, voice.filterSettings))
                {
                    const auto coefficients = dsp::makeMultimode (settings, sampleRate);

                    for (auto& filter : voice.filters)
                        filter.set (coefficients, length);

                    voice.filterSettings = settings;
                    voice.filterIsSet = true;
                }
            }

            const auto stereo = voice.sound->audio.getNumChannels() > 1;
            const auto loopLength = voice.loopEndAt - voice.loopStartAt;

            for (int i = 0; i < length; ++i)
            {
                if (voice.loops)
                {
                    while (voice.position >= voice.loopEndAt)
                        voice.position -= loopLength;
                }
                else if (voice.position >= voice.regionEnd)
                {
                    // Its end: it is done.
                    voice.amp.reset();
                    voice.filterEnvelope.reset();
                    voice.fade = 1.0f;
                    voice.fadeStep = 0.0f;
                    break;
                }

                auto soundLeft = 0.0f, soundRight = 0.0f;
                readAt (voice, voice.position, soundLeft, soundRight);

                // Near the loop's seam, what comes before its start fades in as its end fades out, at equal power.
                if (voice.loops && voice.fadeLength > 0.0 && voice.position >= voice.loopEndAt - voice.fadeLength)
                {
                    const auto t = (float) ((voice.position - (voice.loopEndAt - voice.fadeLength)) / voice.fadeLength);
                    auto earlierLeft = 0.0f, earlierRight = 0.0f;
                    readAt (voice, voice.position - loopLength, earlierLeft, earlierRight);
                    const auto out = std::cos (t * juce::MathConstants<float>::halfPi),
                               in = std::sin (t * juce::MathConstants<float>::halfPi);
                    soundLeft = soundLeft * out + earlierLeft * in;
                    soundRight = soundRight * out + earlierRight * in;
                }

                voice.filterEnvelopeLevel = voice.filterEnvelope.next();
                auto gain = voice.amp.next() * voice.velocityGain * outputGain * voice.fade;

                // Its fades (One-Shot, a slice), or at a start or an end in the middle of the sound a few samples' fade, so
                // that it doesn't click: each in the samples it plays.
                const auto played = (voice.position - voice.regionStart) / voice.step;
                const auto remaining = (voice.regionEnd - voice.position) / voice.step;
                const auto fadingIn = voice.fades ? std::max (fadeInSamples, (double) voice.fadeIn) : (double) voice.fadeIn;
                const auto fadingOut = voice.fades ? std::max (fadeOutSamples, (double) declickSamples) : (double) declickSamples;

                if (fadingIn > 0.0 && played < fadingIn)
                    gain *= (float) (played / fadingIn);

                if (! voice.loops && remaining < fadingOut)
                    gain *= (float) std::max (0.0, remaining / fadingOut);

                // A voice taken for another note, its sound swapped, or cut as playback stopped, fades out; faded, it is free.
                if (voice.fadeStep > 0.0f && (voice.fade -= voice.fadeStep) <= 0.0f)
                {
                    voice.fade = 1.0f;
                    voice.fadeStep = 0.0f;
                    voice.amp.reset();
                    voice.filterEnvelope.reset();
                }

                if (lfoAim == LfoTarget::level)
                    gain *= 1.0f - depth * 0.5f * (1.0f + moved[(size_t) i]);

                if (filtered)
                {
                    soundLeft = voice.filters[0].process (soundLeft);
                    soundRight = stereo ? voice.filters[1].process (soundRight) : soundLeft;
                }

                left[begin + i] += soundLeft * gain;

                if (right != nullptr)
                    right[begin + i] += soundRight * gain;

                voice.position += voice.step;
            }

            if (! voice.amp.isActive())
            {
                voice.note = -1;
                voice.sound = nullptr;
            }
        }
    }
}

void Sampler::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;

    if (block.numChannels == 0)
        return;

    songPlaying = block.isPlaying;
    songTempo = block.tempo > 0.0 ? block.tempo : 120.0;
    const auto* playing = sound.load (std::memory_order_acquire);

    // A voice of a sound swapped out fades out within this block, which the sound outlives.
    const auto swapStep = 1.0f / (float) std::max (1, std::min (block.numSamples, (int) (stealFadeSeconds * sampleRate)));

    for (auto& voice : voices)
        if (voice.amp.isActive() && voice.sound != playing)
            voice.fadeStep = std::max (voice.fadeStep, swapStep);

    // A switch of mode, or between poly and mono, lets go of every note, so that none is left hanging.
    const auto modeKey = choiceOf<int> (values[mode]) * 2 + choiceOf<int> (values[voiceMode]);

    if (modeKey != shownMode)
    {
        if (shownMode >= 0)
        {
            for (auto& voice : voices)
            {
                if (voice.amp.isActive())
                {
                    voice.oneShot = false;
                    releaseVoice (voice);
                }
            }
        }

        numHeld = 0;
        shownMode = modeKey;
    }

    // Each voice's envelopes: Classic's from the amp's knobs, a voice its fades shape letting go over the fade out.
    for (auto& voice : voices)
    {
        if (voice.fades)
            voice.amp.setParameters (0.0f, 0.0f, 1.0f, std::max (0.0005f, values[fadeOut] * 0.001f));
        else
            voice.amp.setParameters (values[attack] * 0.001f, values[decay] * 0.001f, values[sustain], values[release] * 0.001f);

        voice.amp.setCurves (values[attackCurve], values[decayCurve], values[releaseCurve]);
        voice.filterEnvelope.setParameters (values[filterAttack] * 0.001f, values[filterDecay] * 0.001f, values[filterSustain],
                                            values[filterRelease] * 0.001f);
        voice.filterEnvelope.setCurves (values[filterAttackCurve], values[filterDecayCurve], values[filterReleaseCurve]);
    }

    lfo.setRate (values[lfoRate]);

    // How long it rings on after its notes end: its release, or a sound played to its end, at most an octave down.
    const auto playMode = choiceOf<Mode> (values[mode]);
    const auto playsToEnd = playMode != Mode::classic && choiceOf<int> (values[playback]) == 0;
    const auto soundSeconds = playing != nullptr ? playing->audio.getNumSamples() / std::max (1.0, playing->sampleRate) : 0.0;
    const auto letGo = playMode == Mode::classic ? values[release] : values[fadeOut];
    tailSeconds.store ((float) std::max ((double) letGo * 0.001, playsToEnd ? std::min (60.0, 2.0 * soundSeconds) : 0.0),
                       std::memory_order_relaxed);

    // Notes the editor played and let go of, unless one waited too long for a block, as while the sampler was off.
    const auto now = juce::Time::getApproximateMillisecondCounter();
    const auto playAudition = [this, now, &values] (const Audition& waiting)
    {
        if ((int) (now - waiting.playedAt) > longestAuditionWait && waiting.velocity > 0.0f)
            return;

        handle ({ .type = waiting.velocity > 0.0f ? NoteEvent::Type::noteOn : NoteEvent::Type::noteOff,
                  .number = waiting.note,
                  .value = waiting.velocity },
                values);
    };
    const auto scope = auditionFifo.read (auditionFifo.getNumReady());

    for (int i = 0; i < scope.blockSize1; ++i)
        playAudition (auditions[(size_t) (scope.startIndex1 + i)]);

    for (int i = 0; i < scope.blockSize2; ++i)
        playAudition (auditions[(size_t) (scope.startIndex2 + i)]);

    auto* left = block.channels[0];
    auto* right = block.numChannels > 1 ? block.channels[1] : nullptr;
    size_t next = 0;

    // A stretch at a time, between the events.
    for (int done = 0; done < block.numSamples;)
    {
        while (next < block.events.size() && block.events[next].sample <= done)
            handle (block.events[next++], values);

        const auto until = next < block.events.size() ? std::min (block.numSamples, block.events[next].sample) : block.numSamples;
        render (left + done, right != nullptr ? right + done : nullptr, until - done, values);
        done = until;
    }

    // Events at the block's end, for the next one.
    while (next < block.events.size())
        handle (block.events[next++], values);

    // A voice still on a sound swapped out stops: the sound may be freed after the next block.
    for (auto& voice : voices)
    {
        if (voice.sound != nullptr && voice.sound != playing)
        {
            voice.amp.reset();
            voice.filterEnvelope.reset();
            voice.note = -1;
            voice.sound = nullptr;
            voice.fade = 1.0f;
            voice.fadeStep = 0.0f;
        }
    }

    activeVoices.store (
        (int) std::count_if (voices.begin(), voices.begin() + maxVoices, [] (const Voice& each) { return each.amp.isActive(); }),
        std::memory_order_relaxed);

    // For the editor: where each voice is, the newest voice, the LFO and the tempo.
    const Voice* newest = nullptr;

    for (size_t i = 0; i < (size_t) maxVoices; ++i)
    {
        const auto& voice = voices[i];

        if (! voice.amp.isActive() || voice.sound == nullptr)
        {
            shown.positions[i].set (-1.0f);
            continue;
        }

        const auto frames = std::max (1, voice.sound->audio.getNumSamples());
        const auto real = voice.reversed ? (double) frames - voice.position : voice.position;
        shown.positions[i].set ((float) std::clamp (real / frames, 0.0, 1.0));

        if (newest == nullptr || voice.started > newest->started)
            newest = &voice;
    }

    shown.amp.set (newest != nullptr ? newest->amp.getLevel() : 0.0f);
    shown.filterEnvelope.set (newest != nullptr ? newest->filterEnvelopeLevel : 0.0f);
    shown.cutoff.set (newest != nullptr && newest->filterIsSet && values[filterOn] > 0.5f ? (float) newest->filterSettings.frequency
                                                                                          : 0.0f);
    shown.slice.set (newest != nullptr ? (float) newest->slice : -1.0f);
    shown.lfoPhase.set (lfo.getPhase());
    shown.tempo.set ((float) songTempo);
    blocksDone.fetch_add (1, std::memory_order_release);
}
} // namespace phazite::devices

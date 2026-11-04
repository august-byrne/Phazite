// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExampleSongs.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/DrumSynthesis.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/filter/FilterEffect.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "internal/ExampleSongs.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/TrackColours.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/chords/ChordModel.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    BeatPosition at (double beat)
    {
        return BeatPosition::fromBeats (beat);
    }

    // Bars of 4/4 from the song's start, in beats: bar 0 is the first.
    double bar (double bars)
    {
        return bars * 4.0;
    }

    // General MIDI's drums, which are the drum sampler's pads.
    constexpr int kick = 36, rim = 37, snare = 38, clap = 39, closedHat = 42, openHat = 46;

    //==============================================================================
    // Sounds the songs play as samples, made as they are made, the same every time.
    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        auto output = std::make_unique<juce::FileOutputStream> (file);

        if (! output->openedOk())
            return false;

        std::unique_ptr<juce::OutputStream> stream = std::move (output);
        const auto options =
            juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (audio.getNumChannels()).withBitsPerSample (24);

        if (auto writer = juce::WavAudioFormat().createWriterFor (stream, options))
            return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());

        return false;
    }

    // One of the drum sampler's factory sounds (devices/drums/FactoryDrums.h), in stereo.
    devices::DrumSampler::Sound getStereoFactoryDrum (std::string_view name)
    {
        auto sound = *devices::getFactoryDrum (devices::findFactoryDrum (name));

        if (sound.audio.getNumChannels() == 1)
            sound.audio = devices::drums::makeStereo (sound.audio, sound.audio);

        return sound;
    }

    // A bar of sixteenth notes on a hat, accented on the beats and the eighths, each a little left or right in turn as a
    // shaker moves, at a tempo: a loop as long as the bar.
    devices::DrumSampler::Sound makeTopLoop (double bpm)
    {
        const auto hat = getStereoFactoryDrum ("House Closed Hat");
        const auto stepFrames = 60.0 / bpm / 4.0 * hat.sampleRate;
        devices::DrumSampler::Sound loop { juce::AudioBuffer<float> (2, (int) std::lround (16.0 * stepFrames)), hat.sampleRate };
        loop.audio.clear();
        constexpr float accents[] = { 0.9f, 0.35f, 0.6f, 0.4f };

        for (int step = 0; step < 16; ++step)
        {
            const auto start = (int) std::lround (step * stepFrames);
            const auto length = std::min (hat.audio.getNumSamples(), loop.audio.getNumSamples() - start);
            const auto side = step % 2 == 0 ? 0.15f : -0.15f;

            for (int channel = 0; channel < 2; ++channel)
                loop.audio.addFrom (channel, start, hat.audio, channel, 0, length,
                                    accents[step % 4] * (1.0f + (channel == 0 ? side : -side)));
        }

        return loop;
    }

    // A record's crackle and hiss, quiet, for bars at a tempo: clicks about eight a second, the louder ones rarer.
    devices::DrumSampler::Sound makeVinylCrackle (double bpm, int bars)
    {
        constexpr double sampleRate = 48000.0;
        const auto seconds = 60.0 / bpm * 4.0 * bars;
        const auto frames = (int) std::lround (seconds * sampleRate);
        devices::DrumSampler::Sound crackle { juce::AudioBuffer<float> (2, frames), sampleRate };
        juce::Random random (82);

        for (int channel = 0; channel < 2; ++channel)
        {
            auto* out = crackle.audio.getWritePointer (channel);
            auto hiss = 0.0f;

            for (int i = 0; i < frames; ++i)
            {
                hiss += 0.15f * (devices::drums::noise (random) - hiss); // a gentle low-pass
                out[i] = hiss * 0.02f;
            }

            for (int click = 0; click < (int) (seconds * 8.0); ++click)
            {
                const auto start = random.nextInt (frames);
                const auto level = 0.05f + 0.25f * std::pow (random.nextFloat(), 3.0f);
                const auto length = (int) (sampleRate * (0.0005 + 0.0015 * random.nextDouble()));

                for (int k = 0; k < length && start + k < frames; ++k)
                    out[start + k] += level * devices::drums::noise (random) * std::exp (-5.0f * (float) k / (float) length);
            }
        }

        return crackle;
    }

    //==============================================================================
    // Makes a song through the model's functions by ID, as the UI makes one, and keeps going past what isn't there (a preset
    // missing from an install), so that a song always opens.
    class Builder
    {
    public:
        Builder (ProjectContext& c, juce::File factoryPresets)
            : context (c),
              presets (std::move (factoryPresets))
        {
            sounds.createDirectory();
        }

        ~Builder() { sounds.deleteRecursively(); }

        ProjectContext& context;

        void setTempo (double bpm)
        {
            if (auto tempo = getTempoHandle (context); tempo.isValid())
                tempo.setValue (bpm);
        }

        TrackID addTrack (const juce::String& name, const char* colour)
        {
            const auto track = addTrackAtEnd (context, false);
            nameTrack (track, name, colour);
            return track;
        }

        // A bus: a folder whose tracks, these moved into it in their order, mix through its own effects and fader.
        TrackID addBus (const juce::String& name, const char* colour, const std::vector<TrackID>& tracks)
        {
            const auto bus = addTrackAtEnd (context, true);
            nameTrack (bus, name, colour);

            for (const auto track : tracks)
                setOutput (context, track, bus);

            setMixedAsBus (context, bus, true);
            return bus;
        }

        // A device's type (devices' typeID), and a preset that comes with Phazite by its path in their folder ("Synth/Pluck/
        // House Stab"); a device stays as it is when its preset isn't there.
        DeviceID setInstrument (TrackID track, const char* typeID, const char* preset = nullptr)
        {
            if (const auto type = findType (typeID))
                chooseInstrument (context, track, *type);

            const auto instrument = getInstrument (context, track);

            if (! instrument.has_value())
                return {};

            loadPreset (instrument->id, preset);
            return instrument->id;
        }

        // An effect after a track's effects, or the master's for an empty ID (whose loudness meter stays last).
        DeviceID addEffect (TrackID track, const char* typeID, const char* preset = nullptr)
        {
            const auto before = getEffects (context, track);

            if (const auto type = findType (typeID))
                phazite::addEffect (context, track, *type);

            for (const auto& effect : getEffects (context, track))
            {
                if (std::none_of (before.begin(), before.end(), [&effect] (const DeviceSlot& old) { return old.id == effect.id; }))
                {
                    loadPreset (effect.id, preset);
                    return effect.id;
                }
            }

            return {};
        }

        void loadPreset (DeviceID device, const char* preset)
        {
            if (const auto file = presets.getChildFile (juce::String (preset) + presetExtension); preset != nullptr && file.existsAsFile())
                loadPresetInto (context, device, file);
        }

        void set (DeviceID device, const char* parameterID, float value)
        {
            if (auto parameter = getParameter (context, { device, parameterID }); parameter.exists())
                parameter.setValue (value);
        }

        void setVolume (TrackID track, float decibels)
        {
            if (auto volume = getVolume (context, track); volume.exists())
                volume.setValue (decibelsToFaderPosition (decibels));
        }

        void send (TrackID from, TrackID returnTrack, float decibels)
        {
            if (auto level = getSendLevelTo (context, from, returnTrack); level.isValid())
                level.setValue (decibelsToFaderPosition (decibels));
        }

        // A return named after its effect, which plays only the effect, and a send to it from a track.
        TrackID addReturn (TrackID sender, const char* typeID, const char* preset, float sendDecibels)
        {
            const auto type = findType (typeID);

            if (! type.has_value())
                return {};

            const auto added = addReturnWith (context, sender, *type);

            if (const auto effects = getEffects (context, added); ! effects.empty())
            {
                loadPreset (effects.front().id, preset);
                set (effects.front().id, "mix", 1.0f);
            }

            send (sender, added, sendDecibels);
            return added;
        }

        //==============================================================================
        // A MIDI clip in song beats, and its notes, at song beats.
        ClipID addClip (TrackID track, double start, double length)
        {
            const auto clip = addMidiClipAt (context, track, at (start));

            if (clip.isValid())
                trimClipEnd (context, clip, at (start + length));

            return clip;
        }

        void note (ClipID clip, int pitch, double start, double length, int velocity)
        {
            addNote (context, clip, pitch, at (start), BeatDuration::fromBeats (length), velocity);
        }

        // Aliases of a clip, which play what it plays, starting at these song beats on its track.
        void repeat (ClipID clip, const std::vector<double>& starts)
        {
            const auto info = getClip (context, clip);

            if (! info.has_value())
                return;

            for (const auto start : starts)
                if (const auto copies = copyClips (context, { clip }, true); ! copies.empty())
                    moveClip (context, copies.front(), at (start), info->track);
        }

        // A sound made here, as a sample in the project's Audio folder (Import.h copies it there), on a track at a beat.
        ClipID addSample (TrackID track, const juce::String& name, const devices::DrumSampler::Sound& sound, double start)
        {
            const auto file = sounds.getChildFile (name + ".wav");

            if (! writeWav (file, sound.audio, sound.sampleRate))
                return {};

            const auto added = importFiles (context, juce::StringArray (file.getFullPathName()), track, at (start));
            return added.empty() ? ClipID() : added.front();
        }

        void automate (const ParameterRef& parameter, std::initializer_list<AutomationPoint> points)
        {
            for (const auto& point : points)
                addAutomationPoint (context, parameter, point);
        }

        ModulatorID addLfo (TrackID track, float rateHertz)
        {
            const auto lfo = addModulator (context, track, ModulatorType::lfo);

            if (auto rate = getModulatorParameter (context, lfo, "rate"); rate.exists())
                rate.setValue (rateHertz);

            return lfo;
        }

        //==============================================================================
        // The chord lane in a key: the numerals ("ii", "V"), a bar each, from a bar on, over and over until another bar.
        void layChords (const Key& key, const std::vector<const char*>& numerals, int fromBar, int toBar)
        {
            if (fromBar == 0)
                setLaneKey (context, BeatPosition(), key);

            for (int each = fromBar; each < toBar; ++each)
                if (const auto numeral = parseNumeral (numerals[(size_t) (each - fromBar) % numerals.size()]))
                    addLaneChord (context, { at (bar (each)), at (bar (each + 1)) }, *numeral);
        }

        // A part made from the chord lane over bars, on a new track at the end with Phazite's synth (ChordModel.h), named and
        // played with a preset.
        TrackID addPart (PartStyle style, int fromBar, int toBar, const juce::String& name, const char* colour, const char* preset)
        {
            const auto clip = makeChordPart (context, style, { at (bar (fromBar)), at (bar (toBar)) });
            const auto info = clip.has_value() ? getClip (context, *clip) : std::nullopt;

            if (! info.has_value())
                return {};

            nameTrack (info->track, name, colour);

            if (const auto instrument = getInstrument (context, info->track))
                loadPreset (instrument->id, preset);

            return info->track;
        }

        // The song's sections, from its start.
        void layOutSections (std::vector<Outline::Section> sections)
        {
            Outline outline;
            outline.id = "example";
            outline.name = "Example";
            outline.sections = std::move (sections);

            if (auto* song = findSong (context))
                applyOutline (*song, outline);
        }

    private:
        const juce::File presets;
        const juce::File sounds = juce::File::createTempFile ("phazite-example");

        std::optional<DeviceType> findType (const char* typeID) const
        {
            for (const auto& type : getDeviceTypes (context))
                if (type.description.fileOrIdentifier == juce::String (typeID))
                    return type;

            return {};
        }

        void nameTrack (TrackID track, const juce::String& trackName, const char* colour)
        {
            renameTrack (context, track, trackName);

            if (const auto found = findTrackColour (colour))
                setTrackColours (context, { track }, *found);
        }
    };

    //==============================================================================
    // The simplest: a drum track and a synth playing four bars, looped (what a first project played until 2026-11-01).
    void makeFirstBeat (Builder& song)
    {
        song.setTempo (120.0);

        // A bar of the beat, a sixteenth at a time: the kick on 1, 3 and the "and" after it, the snare on 2 and 4, closed
        // hats on the eighths, and an open hat at the bar's end, which the next closed hat chokes.
        struct Hit
        {
            int step, note, velocity;
        };

        constexpr Hit beat[] = { { 0, kick, 112 },     { 0, closedHat, 90 },  { 2, closedHat, 70 }, { 4, snare, 105 },
                                 { 4, closedHat, 90 }, { 6, closedHat, 70 },  { 8, kick, 104 },     { 8, closedHat, 90 },
                                 { 10, kick, 92 },     { 10, closedHat, 70 }, { 12, snare, 105 },   { 12, closedHat, 90 },
                                 { 14, openHat, 80 } };

        const auto drums = song.addTrack ("Drums", "Vermilion");
        song.setInstrument (drums, devices::DrumSampler::typeID);
        const auto beatClip = song.addClip (drums, 0.0, 16.0);

        for (int each = 0; each < 4; ++each)
            for (const auto& hit : beat)
                song.note (beatClip, hit.note, bar (each) + hit.step * 0.25, 0.25, hit.velocity);

        // The synth plucks an arpeggio up and down C, Am, F and G, an eighth note at a time, under the drums: two detuned saws
        // through a filter its envelope opens.
        constexpr int chords[4][4] = { { 60, 64, 67, 72 }, { 57, 60, 64, 69 }, { 53, 57, 60, 65 }, { 55, 59, 62, 67 } };
        constexpr int order[8] = { 0, 1, 2, 3, 2, 1, 0, 1 };

        const auto synth = song.addTrack ("Synth", "Orange");
        const auto device = song.setInstrument (synth, devices::Synth::typeID);

        for (const auto& [id, value] :
             { std::pair { "cutoff", 700.0f }, std::pair { "resonance", 0.25f }, std::pair { "filterEnvelope", 30.0f },
               std::pair { "filterDecay", 250.0f }, std::pair { "filterSustain", 0.1f }, std::pair { "decay", 400.0f },
               std::pair { "sustain", 0.4f }, std::pair { "release", 250.0f }, std::pair { "osc2Detune", 10.0f },
               std::pair { "level", -12.0f } })
            song.set (device, id, value);

        const auto arpeggio = song.addClip (synth, 0.0, 16.0);

        for (int each = 0; each < 4; ++each)
            for (int step = 0; step < 8; ++step)
                song.note (arpeggio, chords[each][order[step]], bar (each) + step * 0.5, 0.45, 100);

        loopSong (song.context);
    }

    //==============================================================================
    // House at 124 BPM in A minor, 40 bars: drums in a bus, samples in the playlist (a top loop that follows the tempo, a
    // crash and a reversed one), a bass made from the chord lane that ducks under the kick through a sidechain, chord stabs
    // whose filter an automation lane opens and an LFO moves, a lead, a noise riser whose volume rises, a reverb and a delay
    // on returns, and the master's multiband.
    void makeHouseGroove (Builder& song)
    {
        constexpr double bpm = 124.0;
        song.setTempo (bpm);
        const Key key { 9, Scale::minor };

        song.layOutSections (
            { { .name = "Intro", .role = "intro", .tip = "Just the kick and the hats, for a DJ to mix in over", .bars = 8, .energy = 0.3f },
              { .name = "Build-up",
                .role = "buildUp",
                .tip = "A snare roll and a noise riser raise the energy, and the chords' filter opens (its lane)",
                .bars = 8,
                .energy = 0.65f },
              { .name = "Drop", .role = "drop", .tip = "Everything plays; the bass ducks under the kick", .bars = 16, .energy = 1.0f },
              { .name = "Outro", .role = "outro", .tip = "Back to the kick and the hats", .bars = 8, .energy = 0.3f } });
        song.layChords (key, { "i", "VI", "III", "VII" }, 0, 40);

        // The drums: a beat on the drum sampler's house kit, and samples placed in the playlist, as FL Studio users place them.
        const auto beat = song.addTrack ("Beat", "Vermilion");
        song.setInstrument (beat, devices::DrumSampler::typeID, "Drum Sampler/Kits/House Kit");

        const auto introBeat = song.addClip (beat, bar (0), 4.0);

        for (int each = 0; each < 4; ++each)
        {
            song.note (introBeat, kick, each, 0.25, each % 2 == 0 ? 112 : 100);
            song.note (introBeat, closedHat, each + 0.5, 0.25, 80);
        }

        song.repeat (introBeat, { bar (1), bar (2), bar (3), bar (4), bar (5), bar (6), bar (7), bar (32), bar (33), bar (34), bar (35),
                                  bar (36), bar (37), bar (38), bar (39) });

        // The build-up: the beat with claps, then a snare roll in eighths and sixteenths, louder as it goes, without the kick
        // for its last two bars.
        const auto build = song.addClip (beat, bar (8), bar (8));

        for (int each = 0; each < 6; ++each)
            for (int beatInBar = 0; beatInBar < 4; ++beatInBar)
            {
                const auto start = bar (8 + each) + beatInBar;
                song.note (build, kick, start, 0.25, 108);

                if (each < 4)
                {
                    song.note (build, closedHat, start + 0.5, 0.25, 78);

                    if (beatInBar % 2 == 1)
                        song.note (build, clap, start, 0.25, 100);
                }
            }

        for (int step = 0; step < 16; ++step)
            song.note (build, snare, bar (12) + step * 0.5, 0.25, 60 + step * 2);

        for (int step = 0; step < 32; ++step)
            song.note (build, snare, bar (14) + step * 0.25, 0.25, 80 + step * 3 / 2);

        // The drop's groove: the kick on every beat, claps on 2 and 4, hats between and quiet sixteenths, an open hat at the end.
        const auto groove = song.addClip (beat, bar (16), 4.0);

        for (int each = 0; each < 4; ++each)
        {
            song.note (groove, kick, each, 0.25, each % 2 == 0 ? 115 : 105);
            song.note (groove, each == 3 ? openHat : closedHat, each + 0.5, 0.25, each == 3 ? 85 : 95);
            song.note (groove, closedHat, each + 0.25, 0.25, 45);
            song.note (groove, closedHat, each + 0.75, 0.25, 45);

            if (each % 2 == 1)
                song.note (groove, clap, each, 0.25, 105);
        }

        std::vector<double> dropBars;

        for (int each = 17; each < 32; ++each)
            dropBars.push_back (bar (each));

        song.repeat (groove, dropBars);

        // A top loop of hats, a bar long, through the drop: it knows its tempo by its name and follows the song's.
        const auto loops = song.addTrack ("Top Loop", "Yellow");
        const auto topLoop = song.addSample (loops, "Top Loop 124 BPM", makeTopLoop (bpm), bar (16));
        song.setVolume (loops, -9.0f);
        song.repeat (topLoop, dropBars);

        // A crash where the drop and the outro start, and the same crash reversed, rising into the drop, faded in.
        const auto fx = song.addTrack ("Crash", "Reddish purple");
        const auto crash = getStereoFactoryDrum ("House Crash");
        const auto crashBeats = crash.audio.getNumSamples() / crash.sampleRate * bpm / 60.0;
        const auto reversed = song.addSample (fx, "Crash", crash, bar (16) - crashBeats);
        setClipReversed (song.context, reversed, true);
        fadeClipTo (song.context, reversed, true, at (bar (16) - crashBeats / 2.0));

        if (const auto copies = copyClips (song.context, { reversed }, false); ! copies.empty())
        {
            setClipReversed (song.context, copies.front(), false);
            moveClip (song.context, copies.front(), at (bar (16)), fx);
            fadeClipTo (song.context, copies.front(), true, at (bar (16)));
            song.repeat (copies.front(), { bar (32) });
        }

        song.setVolume (fx, -6.0f);
        const auto drums = song.addBus ("Drums", "Vermilion", { beat, loops, fx });
        song.addEffect (drums, devices::Compressor::typeID, "Compressor/Drums/Drum Bus Glue");

        // The bass, an octave pattern made from the chord lane through the drop, ducking under the kick.
        const auto bass = song.addPart (PartStyle::bassOctaves, 16, 32, "Bass", "Blue", "Synth/Bass/Pluck Bass");
        duckUnder (song.context, bass, beat);

        // Chord stabs from the build-up on: their filter opens through the build-up (an automation lane), and an LFO moves it.
        const auto chords = song.addPart (PartStyle::offbeatStabs, 8, 32, "Chords", "Bluish green", "Synth/Pluck/House Stab");

        if (const auto synth = getInstrument (song.context, chords))
        {
            const ParameterRef cutoff { synth->id, "cutoff" };
            song.automate (cutoff, { { at (bar (8)), 400.0f, 0.3f }, { at (bar (16)), 2500.0f } });
            modulate (song.context, song.addLfo (chords, 0.25f), cutoff, 0.15f);
        }

        // The lead's riff, four bars in A minor, through the drop.
        const auto lead = song.addTrack ("Lead", "Sky blue");
        song.setInstrument (lead, devices::Synth::typeID, "Synth/Pluck/Bright Pluck");
        const auto riff = song.addClip (lead, bar (16), bar (4));
        constexpr int bars[4][5] = { { 69, 72, 76, 74, 72 }, { 69, 72, 77, 76, 72 }, { 67, 72, 76, 74, 72 }, { 71, 74, 79, 76, 74 } };

        for (int each = 0; each < 4; ++each)
        {
            const auto start = bar (16 + each);
            song.note (riff, bars[each][0], start, 0.5, 100);
            song.note (riff, bars[each][1], start + 0.75, 0.25, 85);
            song.note (riff, bars[each][2], start + 1.5, 0.5, 100);
            song.note (riff, bars[each][3], start + 2.5, 0.5, 95);
            song.note (riff, bars[each][4], start + 3.0, 0.75, 95);
        }

        song.repeat (riff, { bar (20), bar (24), bar (28) });
        song.setVolume (lead, -4.0f);

        // A noise riser held through the build-up, its volume rising (an automation lane).
        const auto riser = song.addTrack ("Riser", "Gray");
        song.setInstrument (riser, devices::Synth::typeID, "Synth/FX/Noise Riser");
        song.note (song.addClip (riser, bar (8), bar (8)), 57, bar (8), bar (8), 100);
        song.automate (getVolume (song.context, riser).getRef(),
                       { { at (bar (8)), decibelsToFaderPosition (-24.0f), -0.3f }, { at (bar (16)), decibelsToFaderPosition (0.0f) } });

        // A reverb and a delay that several tracks send to, and the master's multiband, which makes the mix loud.
        const auto reverb = song.addReturn (chords, devices::Reverb::typeID, "Reverb/Halls/Large Hall", -10.0f);
        song.send (lead, reverb, -16.0f);
        song.send (riser, reverb, -8.0f);
        song.addReturn (lead, devices::Delay::typeID, "Delay/Rhythmic/Dotted Eighth", -12.0f);
        song.addEffect ({}, devices::Multiband::typeID, "Multiband/Mastering/Gentle Master");

        useKeyOnEveryTrack (song.context, key);
    }

    //==============================================================================
    // A lo-fi beat at 82 BPM in F major, in the lo-fi outline's 48 bars: a dusty kit with swung hats, a shaker loop and a
    // record's crackle as samples, seventh chords from the chord lane through a container with macros (Muffle and Drive), an
    // LFO that pans them, a sub bass, a melody with a dub delay, the keys' filter opening in the intro and closing in the
    // break (an automation lane), and the drums fading out at the end.
    void makeLoFiBeat (Builder& song)
    {
        constexpr double bpm = 82.0;
        song.setTempo (bpm);
        const Key key { 5, Scale::major };

        // Intro 4, Loop A 8, Loop B 8, Loop A 8, Break 4, Loop B 8, Outro 8.
        if (auto* played = findSong (song.context))
            applyOutline (*played, juce::String ("lofi"));

        song.layChords (key, { "ii", "V", "I", "vi" }, 0, 12);
        song.layChords (key, { "IV", "iii", "vi", "V" }, 12, 20);
        song.layChords (key, { "ii", "V", "I", "vi" }, 20, 28);
        song.layChords (key, { "IV", "IV", "V", "V" }, 28, 32);
        song.layChords (key, { "IV", "iii", "vi", "V" }, 32, 40);
        song.layChords (key, { "ii", "V", "I", "vi" }, 40, 48);

        // A dusty boom-bap beat: two bars, the hats swung, a ghost snare before the second bar's end.
        const auto beat = song.addTrack ("Beat", "Vermilion");
        song.setInstrument (beat, devices::DrumSampler::typeID, "Drum Sampler/Kits/Lo-Fi Kit");
        constexpr double swing = 0.08; // how late the offbeat hats are, in beats

        const auto hatsOnly = song.addClip (beat, bar (0), 4.0);

        for (int each = 0; each < 4; ++each)
        {
            song.note (hatsOnly, closedHat, each, 0.25, 70);
            song.note (hatsOnly, closedHat, each + 0.5 + swing, 0.25, 48);
        }

        song.note (hatsOnly, rim, 2.75, 0.25, 70);
        song.repeat (hatsOnly, { bar (1), bar (2), bar (3), bar (44), bar (45), bar (46), bar (47) });

        const auto boomBap = song.addClip (beat, bar (4), bar (2));

        for (int each = 0; each < 2; ++each)
        {
            const auto start = bar (4 + each);

            for (int beatInBar = 0; beatInBar < 4; ++beatInBar)
            {
                song.note (boomBap, closedHat, start + beatInBar, 0.25, 70);
                song.note (boomBap, closedHat, start + beatInBar + 0.5 + swing, 0.25, 48);
            }

            song.note (boomBap, kick, start, 0.25, 110);
            song.note (boomBap, kick, start + (each == 0 ? 1.75 : 2.25) + (each == 0 ? swing : 0.0), 0.25, 88);
            song.note (boomBap, kick, start + 2.5 + swing, 0.25, 100);
            song.note (boomBap, snare, start + 1.0, 0.25, 100);
            song.note (boomBap, snare, start + 3.0, 0.25, 100);
        }

        song.note (boomBap, snare, bar (5) + 3.75, 0.25, 45);
        std::vector<double> loopBars;

        for (int each = 6; each < 44; each += 2)
            if (each < 28 || each >= 32)
                loopBars.push_back (bar (each));

        song.repeat (boomBap, loopBars);

        // A shaker loop in the B parts, a sample that follows the tempo.
        const auto shakers = song.addTrack ("Shaker", "Yellow");
        auto shakerLoop = makeTopLoop (bpm);
        devices::drums::makeDusty (shakerLoop.audio, shakerLoop.sampleRate, {});
        const auto shaker = song.addSample (shakers, "Shaker 82 BPM", shakerLoop, bar (12));
        std::vector<double> shakerBars;

        for (int each = 13; each < 40; ++each)
            if (each < 20 || each >= 32)
                shakerBars.push_back (bar (each));

        song.repeat (shaker, shakerBars);
        song.setVolume (shakers, -14.0f);

        const auto drums = song.addBus ("Drums", "Vermilion", { beat, shakers });
        song.addEffect (drums, devices::Multiband::typeID, "Multiband/Mixing/Drum Punch");

        // The drums fade out over the outro's last four bars.
        song.automate (getVolume (song.context, drums).getRef(),
                       { { at (bar (44)), decibelsToFaderPosition (0.0f), 0.3f }, { at (bar (48)), decibelsToFaderPosition (-40.0f) } });

        // A record's crackle under it all, four bars at a time.
        const auto vinyl = song.addTrack ("Vinyl", "Gray");
        const auto crackle = song.addSample (vinyl, "Vinyl 82 BPM", makeVinylCrackle (bpm, 4), bar (0));
        song.repeat (crackle,
                     { bar (4), bar (8), bar (12), bar (16), bar (20), bar (24), bar (28), bar (32), bar (36), bar (40), bar (44) });
        song.setVolume (vinyl, -10.0f);

        // Seventh chords from the chord lane on a soft e-piano, through a container of tape-like effects with two macros, and
        // an LFO that pans them slowly from side to side.
        const auto keys = song.addPart (PartStyle::sustainedSevenths, 0, 48, "Keys", "Orange", "Synth/Keys/Soft E-Piano");
        const auto saturator = song.addEffect (keys, devices::Saturator::typeID, "Saturator/Warmth/Warm Tape");
        const auto filter = song.addEffect (keys, devices::FilterEffect::typeID);
        song.set (filter, "cutoff", 3000.0f);
        song.set (filter, "resonance", 0.15f);

        if (const auto container = groupIntoContainer (song.context, { saturator, filter }); container.isValid())
        {
            renameContainer (song.context, container, "Tape");
            const auto macros = getContainerMacros (song.context, container);

            if (macros.size() >= 2)
            {
                renameMacro (song.context, macros[0].id, "Muffle");
                modulate (song.context, macros[0].id, { filter, "cutoff" }, -0.3f);
                renameMacro (song.context, macros[1].id, "Drive");
                modulate (song.context, macros[1].id, { saturator, "drive" }, 0.25f);

                for (const auto& macro : { macros[0].id, macros[1].id })
                    if (const auto parameters = getModulatorParameters (song.context, macro); ! parameters.empty())
                        if (auto knob = getModulatorParameter (song.context, macro, parameters.front().id); knob.exists())
                            knob.setValue (0.3f);
            }
        }

        // The keys' filter opens through the intro, closes for the break and opens again after it.
        song.automate ({ filter, "cutoff" }, { { at (bar (0)), 500.0f, 0.3f },
                                               { at (bar (4)), 3000.0f },
                                               { at (bar (28)), 3000.0f, -0.3f },
                                               { at (bar (30)), 700.0f },
                                               { at (bar (31)), 700.0f, 0.3f },
                                               { at (bar (32)), 3000.0f } });
        modulate (song.context, song.addLfo (keys, 0.15f), getPan (song.context, keys).getRef(), 0.3f);

        // A sub bass from the chord lane, after the intro.
        const auto bass = song.addPart (PartStyle::bassRoots, 4, 48, "Bass", "Blue", "Synth/Bass/Sub Bass");
        song.setVolume (bass, -3.0f);

        // A melody in the B parts, through a dub delay.
        const auto melody = song.addTrack ("Melody", "Sky blue");
        song.setInstrument (melody, devices::Synth::typeID, "Synth/Pluck/Lo-Fi Pluck");
        const auto motif = song.addClip (melody, bar (12), bar (4));

        struct Note
        {
            int pitch;
            double start, length;
        };

        constexpr Note notes[] = { { 74, 0.0, 1.0 },  { 72, 1.5, 0.5 },  { 69, 2.0, 1.5 },  { 72, 4.5, 0.5 },
                                   { 69, 5.0, 0.5 },  { 67, 5.5, 0.5 },  { 69, 6.0, 1.5 },  { 65, 8.0, 0.5 },
                                   { 69, 8.5, 0.5 },  { 74, 9.0, 1.0 },  { 72, 10.5, 0.5 }, { 69, 11.0, 1.0 },
                                   { 67, 12.0, 1.0 }, { 69, 13.0, 0.5 }, { 72, 13.5, 0.5 }, { 67, 14.0, 2.0 } };

        for (const auto& each : notes)
            song.note (motif, each.pitch, bar (12) + each.start, each.length, 92);

        song.repeat (motif, { bar (16), bar (32), bar (36) });
        song.setVolume (melody, -6.0f);

        const auto reverb = song.addReturn (keys, devices::Reverb::typeID, "Reverb/Rooms/Small Room", -12.0f);
        song.send (melody, reverb, -14.0f);
        song.addReturn (melody, devices::Delay::typeID, "Delay/Vintage/Dub Echo", -10.0f);
        song.addEffect ({}, devices::Limiter::typeID, "Limiter/Mastering/Transparent Master");

        useKeyOnEveryTrack (song.context, key);
    }
} // namespace

const std::vector<ExampleSong>& getExampleSongs()
{
    static const std::vector<ExampleSong> songs {
        { "firstBeat", "First Beat", "Four bars of drums and a synth arpeggio, looped: the simplest song, to play along with and build on.",
          "Synth" },
        { "houseGroove", "House Groove",
          "House at 124 BPM: drums in a bus, samples in the playlist, a bass that ducks under the kick, chords from the chord lane, "
          "automation, an LFO, a riser and sends to a reverb and a delay.",
          "Lead" },
        { "loFiBeat", "Lo-fi Beat",
          "A lo-fi beat at 82 BPM: a dusty kit, shaker and vinyl samples, seventh chords through a container with macros, an LFO "
          "that pans them, automation and a dub delay.",
          "Melody" },
    };

    return songs;
}

const ExampleSong* findExampleSong (const juce::String& id)
{
    for (const auto& song : getExampleSongs())
        if (id == song.id)
            return &song;

    return nullptr;
}

bool addExampleSong (ProjectContext& context, const juce::String& id, const juce::File& factoryPresets)
{
    Builder song (context, factoryPresets);

    if (id == "firstBeat")
        makeFirstBeat (song);
    else if (id == "houseGroove")
        makeHouseGroove (song);
    else if (id == "loFiBeat")
        makeLoFiBeat (song);
    else
        return false;

    context.getSongSelection().clear();
    return true;
}

TrackID findExampleTrackToPlay (const ProjectContext& context, const ExampleSong& song)
{
    for (const auto& row : getPlaylistRows (context))
        if (row.name == song.playedTrack)
            return row.id;

    return {};
}

std::function<void (Song&)> getExampleSongContent (EngineContext& engine, const juce::String& id, const juce::File& factoryPresets)
{
    return [&engine, id, factoryPresets] (Song& song)
    {
        // Not undo steps that matter: the project starts from its first save, without them.
        deleteTracks (song, getTrackIDs (song), nullptr);
        ProjectContext building { engine };
        setSong (building, &song);
        addExampleSong (building, id, factoryPresets);
        setSong (building, nullptr);
    };
}
} // namespace phazite

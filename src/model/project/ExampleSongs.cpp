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
#include "devices/wavetable/WavetableSynth.h"
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
#include "model/notes/internal/Notes.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

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

    // Rain on a window, quiet, for bars at a tempo: a band of noise (no rumble under it, no hiss over it) and drops, each a
    // short high ring that dies away, about a dozen a second, the louder ones rarer.
    devices::DrumSampler::Sound makeRain (double bpm, int bars)
    {
        constexpr double sampleRate = 48000.0;
        const auto seconds = 60.0 / bpm * 4.0 * bars;
        const auto frames = (int) std::lround (seconds * sampleRate);
        devices::DrumSampler::Sound rain { juce::AudioBuffer<float> (2, frames), sampleRate };
        juce::Random random (217);

        for (int channel = 0; channel < 2; ++channel)
        {
            auto* out = rain.audio.getWritePointer (channel);
            auto low = 0.0f, lower = 0.0f;

            for (int i = 0; i < frames; ++i)
            {
                low += 0.5f * (devices::drums::noise (random) - low); // under about 4 kHz
                lower += 0.05f * (low - lower);                       // under about 400 Hz, taken away
                out[i] = (low - lower) * 0.05f;
            }

            for (int drop = 0; drop < (int) (seconds * 12.0); ++drop)
            {
                const auto start = random.nextInt (frames);
                const auto level = 0.01f + 0.04f * std::pow (random.nextFloat(), 4.0f);
                const auto frequency = 2000.0 + 3000.0 * random.nextDouble();
                const auto length = (int) (sampleRate * 0.012);

                for (int k = 0; k < length && start + k < frames; ++k)
                    out[start + k] += level * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * k / sampleRate)
                                      * std::exp (-6.0f * (float) k / (float) length);
            }
        }

        return rain;
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

        // A slide note (NoteValues.h): it plays nothing itself, but glides what sounds as it starts to its pitch over its
        // length, as an 808 glides.
        void slide (ClipID clip, int pitch, double start, double length)
        {
            const auto added = addNote (context, clip, pitch, at (start), BeatDuration::fromBeats (length), 100);

            if (auto* played = findSong (context); played != nullptr && added.isValid())
                toggleNotesSlide (*played, { added });
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

        // A part that plays the chord lane's chords over bars in a style (ChordValues.h: makePartNotes()), on a new track at the
        // end with Phazite's synth, named and played with a preset.
        TrackID addPart (PartStyle style, int fromBar, int toBar, const juce::String& name, const char* colour, const char* preset)
        {
            const auto track = addTrack (name, colour);
            setInstrument (track, devices::Synth::typeID, preset);
            const auto clip = addClip (track, bar (fromBar), bar (toBar) - bar (fromBar));

            if (! clip.isValid())
                return track;

            std::vector<PlacedChord> chords;

            for (const auto& chord : getLaneChords (context))
                chords.push_back ({ chord.range, chord.numeral, chord.key });

            for (const auto& made : makePartNotes (chords, style, { at (bar (fromBar)), at (bar (toBar)) }, clip))
                note (clip, made.pitch, made.start.inBeats(), made.length.inBeats(), made.velocity);

            return track;
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
    // Trap at 140 BPM in C minor, 56 bars: an intro on iv and V that the hook's i resolves; drums in half time on the trap
    // kit, the snare and the clap on each bar's third beat, and hi-hat rolls in 32nds and triplets; an 808 whose slide notes
    // glide it up an octave and on to the next chord's root; FM bells, a dark pad and, in the verse, a pluck that answers
    // them; a noise riser into each hook and a crash on its first beat; and a plate and a ping-pong delay on returns.
    void makeTrapBeat (Builder& song)
    {
        constexpr double bpm = 140.0;
        song.setTempo (bpm);
        const Key key { 0, Scale::minor };

        song.layOutSections (
            { { .name = "Intro",
                .role = "intro",
                .tip = "The bells and the pad on iv and V, which the hook resolves",
                .bars = 4,
                .energy = 0.3f },
              { .name = "Hook",
                .role = "hook",
                .tip = "Everything plays: the 808 glides between its notes (slide notes) and the hats roll",
                .bars = 16,
                .energy = 1.0f },
              { .name = "Verse",
                .role = "verse",
                .tip = "Room for a voice: the bells rest, a pluck answers and the 808 plays less",
                .bars = 16,
                .energy = 0.65f },
              { .name = "Hook 2", .role = "hook", .tip = "The hook again", .bars = 16, .energy = 1.0f },
              { .name = "Outro", .role = "outro", .tip = "The bells and the pad on i, and a last 808", .bars = 4, .energy = 0.25f } });
        song.layChords (key, { "iv", "iv", "V", "V" }, 0, 4);
        song.layChords (key, { "i", "i", "VI", "VI", "iv", "iv", "V", "V" }, 4, 52);
        song.layChords (key, { "i" }, 52, 56);

        // Each of the hook's and the verse's chords, two bars each: the 808's note under it (C, Ab, F and G, low), and its
        // three notes for the bells.
        struct Chord
        {
            int bass;
            int tones[3];
        };

        constexpr Chord cycle[4] = { { 36, { 72, 75, 79 } }, { 32, { 68, 72, 75 } }, { 29, { 65, 68, 72 } }, { 31, { 67, 71, 74 } } };

        // The kick's hits in two bars, in sixteenths from their start, which the 808 plays with.
        constexpr int hookKicks[] = { 0, 3, 10, 16, 22, 27, 30 };
        constexpr int verseKicks[] = { 0, 7, 10, 16, 25 };
        const auto kicksOf = [&] (bool hook) { return hook ? std::span<const int> (hookKicks) : std::span<const int> (verseKicks); };

        // The drums: the kick; the snare on each bar's third beat (half time), with a clap in the hook; hats in eighths, an
        // open one in the hook; a roll of 32nds before the first bar's snare is done, and one at the end of the second bar,
        // in triplets, or for the last two bars of a part in 32nds without the kick, into the next.
        const auto drums = song.addTrack ("Drums", "Vermilion");
        song.setInstrument (drums, devices::DrumSampler::typeID, "Drum Sampler/Kits/Trap Kit");

        const auto roll = [&song] (ClipID clip, double start, double length, int count, int from, int to)
        {
            for (int each = 0; each < count; ++each)
                song.note (clip, closedHat, start + length * each / count, length / count, from + (to - from) * each / (count - 1));
        };

        const auto addDrums = [&] (double start, bool hook, bool ending)
        {
            const auto clip = song.addClip (drums, start, bar (2));

            for (const auto step : kicksOf (hook))
                if (! ending || step < 28)
                    song.note (clip, kick, start + step * 0.25, 0.25, step % 16 == 0 ? 118 : 104);

            for (int each = 0; each < 2; ++each)
            {
                song.note (clip, snare, start + bar (each) + 2.0, 0.25, 112);

                if (hook)
                    song.note (clip, clap, start + bar (each) + 2.0, 0.25, 100);
            }

            // Eighths but where the rolls are: the first bar's last half beat and the second bar's last beat.
            for (int step = 0; step < 16; ++step)
                if (step != 7 && step < 14)
                    song.note (clip, hook && step == 3 ? openHat : closedHat, start + step * 0.5, hook && step == 3 ? 0.5 : 0.25,
                               step % 2 == 0 ? 92 : 70);

            roll (clip, start + 3.5, 0.5, 4, 60, 92);

            if (ending)
                roll (clip, start + 7.0, 1.0, 8, 50, 110);
            else
                roll (clip, start + 7.0, 1.0, 6, 55, 100);

            return clip;
        };

        const auto hookDrums = addDrums (bar (4), true, false);
        song.repeat (hookDrums, { bar (6), bar (8), bar (10), bar (12), bar (14), bar (16), bar (36), bar (38), bar (40), bar (42),
                                  bar (44), bar (46), bar (48) });
        song.repeat (addDrums (bar (18), true, true), { bar (50) });
        song.repeat (addDrums (bar (20), false, false), { bar (22), bar (24), bar (26), bar (28), bar (30), bar (32) });
        addDrums (bar (34), false, true);

        // A crash on the first beat of each part after the intro, and a riser into each hook.
        const auto crash = song.addTrack ("Crash", "Yellow");
        song.repeat (song.addSample (crash, "Crash", getStereoFactoryDrum ("Trap Crash"), bar (4)), { bar (20), bar (36) });
        song.setVolume (crash, -8.0f);

        const auto riser = song.addTrack ("Riser", "Gray");
        song.setInstrument (riser, devices::Synth::typeID, "Synth/FX/Noise Riser");
        song.note (song.addClip (riser, bar (2), bar (2)), 57, bar (2), bar (2), 90);
        song.note (song.addClip (riser, bar (34), bar (2)), 57, bar (34), bar (2), 90);
        song.setVolume (riser, -13.0f);

        // The 808: a note with each kick, held until the next, at the root of the chord it is under. In the hook a slide note
        // glides it up an octave and back in each chord's first bar, and another on to the next chord's root at its end.
        const auto bass = song.addTrack ("808", "Blue");
        song.setInstrument (bass, devices::Synth::typeID, "Synth/Bass/Sine 808");
        song.addEffect (bass, devices::Saturator::typeID, "Saturator/Warmth/Subtle Drive");

        const auto add808 = [&] (double start, bool hook)
        {
            const auto clip = song.addClip (bass, start, bar (8));
            const auto kicks = kicksOf (hook);

            for (int each = 0; each < 4; ++each)
            {
                const auto chordStart = start + bar (2 * each);
                const auto root = cycle[each].bass;

                for (size_t hit = 0; hit < kicks.size(); ++hit)
                {
                    const auto from = kicks[hit] * 0.25, to = hit + 1 < kicks.size() ? kicks[hit + 1] * 0.25 : 8.0;
                    song.note (clip, root, chordStart + from, to - from - 0.05, kicks[hit] % 16 == 0 ? 112 : 100);
                }

                if (hook)
                {
                    song.slide (clip, root + 12, chordStart + 3.0, 0.25);
                    song.slide (clip, root, chordStart + 3.5, 0.25);
                }

                if (hook || each == 3)
                    song.slide (clip, cycle[(each + 1) % 4].bass, chordStart + 7.75, 0.25);
            }

            return clip;
        };

        song.repeat (add808 (bar (4), true), { bar (12), bar (36), bar (44) });
        song.repeat (add808 (bar (20), false), { bar (28) });
        song.note (song.addClip (bass, bar (52), bar (2)), 36, bar (52), bar (2), 110);

        // The bells: each chord's notes in a figure over its two bars, an eighth note at a time with rests: in the hooks, in
        // the intro (on iv and V) and in the outro (on i).
        const auto bells = song.addTrack ("Bells", "Sky blue");
        song.setInstrument (bells, devices::WavetableSynth::typeID, "Wavetable/Keys/Glass Bells");
        constexpr int figure[16] = { 0, 1, 2, 1, 3, 2, 1, 2, 0, 1, 2, -1, 3, 2, 1, -1 }; // 3 is the lowest an octave up, -1 a rest

        const auto addFigure = [&song, &figure] (ClipID clip, double start, const Chord& chord)
        {
            for (int step = 0; step < 16; ++step)
                if (const auto place = figure[step]; place >= 0)
                    song.note (clip, place == 3 ? chord.tones[0] + 12 : chord.tones[place], start + step * 0.5, 0.45,
                               step % 4 == 0 ? 100 : 82);
        };

        const auto hookBells = song.addClip (bells, bar (4), bar (8));

        for (int each = 0; each < 4; ++each)
            addFigure (hookBells, bar (4 + 2 * each), cycle[each]);

        song.repeat (hookBells, { bar (12), bar (36), bar (44) });
        const auto intro = song.addClip (bells, bar (0), bar (4));
        addFigure (intro, bar (0), cycle[2]);
        addFigure (intro, bar (2), cycle[3]);
        const auto outro = song.addClip (bells, bar (52), bar (4));
        addFigure (outro, bar (52), cycle[0]);
        addFigure (outro, bar (54), cycle[0]);
        song.setVolume (bells, -6.0f);

        // A dark pad from the chord lane, under it all; and in the verse a pluck that answers, down the chord from its top.
        const auto pad = song.addPart (PartStyle::sustainedTriads, 0, 56, "Pad", "Reddish purple", "Synth/Pad/Dark Pad");
        song.setVolume (pad, -10.0f);

        const auto pluck = song.addTrack ("Pluck", "Bluish green");
        song.setInstrument (pluck, devices::WavetableSynth::typeID, "Wavetable/Pluck/Future Pluck");
        const auto answer = song.addClip (pluck, bar (20), bar (8));

        for (int each = 0; each < 4; ++each)
        {
            const auto start = bar (20 + 2 * each);
            const auto& tones = cycle[each].tones;
            song.note (answer, tones[2] + 12, start + 2.5, 0.25, 92);
            song.note (answer, tones[1] + 12, start + 2.75, 0.25, 84);
            song.note (answer, tones[0] + 12, start + 3.0, 0.5, 88);
            song.note (answer, tones[2], start + 6.5, 0.5, 80);
        }

        song.repeat (answer, { bar (28) });
        song.setVolume (pluck, -8.0f);

        // A plate and a ping-pong delay that the bells send to, the pad and the pluck to the plate; and the master's
        // limiter, which makes the mix loud.
        const auto plate = song.addReturn (bells, devices::Reverb::typeID, "Reverb/Plates/Bright Plate", -10.0f);
        song.send (pad, plate, -12.0f);
        song.send (pluck, plate, -8.0f);
        song.addReturn (bells, devices::Delay::typeID, "Delay/Rhythmic/Ping-Pong Eighths", -14.0f);
        song.addEffect ({}, devices::Limiter::typeID, "Limiter/Mastering/Loud Master");

        useKeyOnEveryTrack (song.context, key);
    }

    //==============================================================================
    // House at 124 BPM in A minor, 64 bars: an intro for a DJ to mix in, a build-up, a drop, a breakdown and a second drop.
    // The drums in a bus, with open hats on the offbeats, and a shaker loop and crashes as samples in the playlist (some
    // reversed, rising into the drops); house's offbeat bass and chord stabs from the chord lane, both ducking under the kick
    // through sidechains (the pump); the chords' filter in an automation lane, closing in the breakdown, and an LFO on it; a
    // pad in the breakdown; a lead, an octave up in the second drop; noise risers whose volume rises; a reverb and a delay
    // on returns; and the master's multiband.
    void makeHouseGroove (Builder& song)
    {
        constexpr double bpm = 124.0;
        song.setTempo (bpm);
        const Key key { 9, Scale::minor };

        song.layOutSections (
            { { .name = "Intro",
                .role = "intro",
                .tip = "The kick, hats and a shaker loop, for a DJ to mix in over",
                .bars = 8,
                .energy = 0.3f },
              { .name = "Build-up",
                .role = "buildUp",
                .tip = "Claps join, a snare roll and a noise riser lift it, and the chords' filter opens (its lane)",
                .bars = 8,
                .energy = 0.65f },
              { .name = "Drop",
                .role = "drop",
                .tip = "Everything plays; the bass and the chords duck under the kick",
                .bars = 16,
                .energy = 1.0f },
              { .name = "Breakdown",
                .role = "breakdown",
                .tip = "No kick: a pad under the chords, whose filter closes and opens again, and a riser into the second drop",
                .bars = 8,
                .energy = 0.45f },
              { .name = "Drop 2",
                .role = "drop",
                .tip = "The drop again, with a ride, and the lead an octave up",
                .bars = 16,
                .energy = 1.0f },
              { .name = "Outro", .role = "outro", .tip = "Back to the kick, the hats and the shaker", .bars = 8, .energy = 0.3f } });
        song.layChords (key, { "i", "VI", "III", "VII" }, 0, 64);

        // The drums: a beat on the drum sampler's house kit, and samples placed in the playlist, as FL Studio users place them.
        const auto beat = song.addTrack ("Beat", "Vermilion");
        song.setInstrument (beat, devices::DrumSampler::typeID, "Drum Sampler/Kits/House Kit");
        constexpr int ride = 51;

        // The intro's and the outro's bar: the kick on every beat, a closed hat between.
        const auto introBeat = song.addClip (beat, bar (0), 4.0);

        for (int each = 0; each < 4; ++each)
        {
            song.note (introBeat, kick, each, 0.25, each % 2 == 0 ? 112 : 102);
            song.note (introBeat, closedHat, each + 0.5, 0.25, 82);
        }

        std::vector<double> introBars;

        for (int each = 1; each < 64; ++each)
            if (each < 8 || each >= 56)
                introBars.push_back (bar (each));

        song.repeat (introBeat, introBars);

        // The build-up: claps on 2 and 4 and open hats between the kicks, then a snare roll in eighths and sixteenths, louder
        // as it goes, without the kick for its last two bars.
        const auto build = song.addClip (beat, bar (8), bar (8));

        for (int each = 0; each < 6; ++each)
            for (int beatInBar = 0; beatInBar < 4; ++beatInBar)
            {
                const auto start = bar (8 + each) + beatInBar;
                song.note (build, kick, start, 0.25, 108);

                if (each < 4)
                {
                    song.note (build, openHat, start + 0.5, 0.25, 70 + each * 4);

                    if (beatInBar % 2 == 1)
                        song.note (build, clap, start, 0.25, 100);
                }
            }

        for (int step = 0; step < 16; ++step)
            song.note (build, snare, bar (12) + step * 0.5, 0.25, 60 + step * 2);

        for (int step = 0; step < 32; ++step)
            song.note (build, snare, bar (14) + step * 0.25, 0.25, 80 + step * 3 / 2);

        // The drops' bar: the kick on every beat, claps on 2 and 4, an open hat on every offbeat (house's "tss"), quiet
        // closed sixteenths between and a rim that pushes; in the second drop a ride on the beats too.
        const auto addGroove = [&song, beat] (double start, bool withRide)
        {
            const auto groove = song.addClip (beat, start, 4.0);

            for (int each = 0; each < 4; ++each)
            {
                const auto onBeat = start + each;
                song.note (groove, kick, onBeat, 0.25, each % 2 == 0 ? 115 : 106);
                song.note (groove, openHat, onBeat + 0.5, 0.25, 84);
                song.note (groove, closedHat, onBeat + 0.25, 0.25, 44);
                song.note (groove, closedHat, onBeat + 0.75, 0.25, 50);

                if (each % 2 == 1)
                    song.note (groove, clap, onBeat, 0.25, 106);

                if (withRide)
                    song.note (groove, ride, onBeat, 0.25, 70);
            }

            song.note (groove, rim, start + 1.75, 0.25, 70);
            song.note (groove, rim, start + 3.25, 0.25, 58);
            return groove;
        };

        std::vector<double> dropBars, drop2Bars;

        for (int each = 17; each < 32; ++each)
            dropBars.push_back (bar (each));

        for (int each = 41; each < 56; ++each)
            drop2Bars.push_back (bar (each));

        song.repeat (addGroove (bar (16), false), dropBars);
        song.repeat (addGroove (bar (40), true), drop2Bars);

        // The breakdown's last four bars: a snare roll rising into the second drop.
        const auto rise = song.addClip (beat, bar (36), bar (4));

        for (int step = 0; step < 16; ++step)
            song.note (rise, snare, bar (36) + step * 0.5, 0.25, 55 + step * 2);

        for (int step = 0; step < 32; ++step)
            song.note (rise, snare, bar (38) + step * 0.25, 0.25, 75 + step * 3 / 2);

        // A shaker loop, a bar long, from the intro's second half to the outro's first, but in the build-up and the breakdown:
        // it knows its tempo by its name and follows the song's.
        const auto loops = song.addTrack ("Top Loop", "Yellow");
        const auto topLoop = song.addSample (loops, "Top Loop 124 BPM", makeTopLoop (bpm), bar (4));
        std::vector<double> loopBars;

        for (int each = 5; each < 60; ++each)
            if (each < 8 || (each >= 16 && each < 32) || each >= 40)
                loopBars.push_back (bar (each));

        song.repeat (topLoop, loopBars);
        song.setVolume (loops, -9.0f);

        // A crash where each drop and the outro start, and the same crash reversed, faded in, rising into each drop.
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
            song.repeat (copies.front(), { bar (40), bar (56) });
        }

        song.repeat (reversed, { bar (40) - crashBeats });
        song.setVolume (fx, -6.0f);
        const auto drums = song.addBus ("Drums", "Vermilion", { beat, loops, fx });
        song.addEffect (drums, devices::Compressor::typeID, "Compressor/Drums/Drum Bus Glue");

        // House's bass between the kicks: the chord's root on each offbeat, its octave before the third beat, and a fifth
        // to lead into the next four bars, through both drops, ducking under the kick.
        const auto bass = song.addTrack ("Bass", "Blue");
        song.setInstrument (bass, devices::Synth::typeID, "Synth/Bass/Pluck Bass");
        song.setVolume (bass, -4.0f);
        const auto bassline = song.addClip (bass, bar (16), bar (4));
        constexpr int roots[4] = { 33, 29, 36, 31 }; // A, F, C and G: the chords i, VI, III and VII

        for (int each = 0; each < 4; ++each)
        {
            const auto start = bar (16 + each);
            song.note (bassline, roots[each], start + 0.5, 0.3, 108);
            song.note (bassline, roots[each], start + 1.5, 0.3, 100);
            song.note (bassline, roots[each] + 12, start + 2.5, 0.3, 104);
            song.note (bassline, roots[each], start + 3.5, each == 3 ? 0.2 : 0.3, 100);
        }

        song.note (bassline, roots[3] + 7, bar (19) + 3.75, 0.2, 92);
        song.repeat (bassline, { bar (20), bar (24), bar (28), bar (40), bar (44), bar (48), bar (52) });
        duckUnder (song.context, bass, beat);

        // Chord stabs from the build-up to the outro: their filter opens through the build-up, closes in the breakdown and
        // opens again into the second drop (an automation lane), an LFO moves it, and they pump under the kick too.
        const auto chords = song.addPart (PartStyle::offbeatStabs, 8, 56, "Chords", "Bluish green", "Synth/Pluck/House Stab");

        if (const auto synth = getInstrument (song.context, chords))
        {
            const ParameterRef cutoff { synth->id, "cutoff" };
            song.automate (cutoff, { { at (bar (8)), 400.0f, 0.3f },
                                     { at (bar (16)), 2500.0f },
                                     { at (bar (32)), 2500.0f, -0.3f },
                                     { at (bar (34)), 900.0f },
                                     { at (bar (36)), 900.0f, 0.3f },
                                     { at (bar (40)), 2500.0f } });
            modulate (song.context, song.addLfo (chords, 0.25f), cutoff, 0.15f);
        }

        duckUnder (song.context, chords, beat);

        // A pad under the breakdown, its chords spread wide.
        const auto pad = song.addPart (PartStyle::spreadChords, 32, 40, "Pad", "Orange", "Synth/Pad/Warm Pad");
        song.setVolume (pad, -5.5f);

        // The lead's riff, four bars in A minor, through the first drop, and an octave up through the second.
        const auto lead = song.addTrack ("Lead", "Sky blue");
        song.setInstrument (lead, devices::Synth::typeID, "Synth/Pluck/Bright Pluck");
        constexpr int riffNotes[4][5] = { { 69, 72, 76, 74, 72 }, { 69, 72, 77, 76, 72 }, { 67, 72, 76, 74, 72 }, { 71, 74, 79, 76, 74 } };

        const auto addRiff = [&song, lead, &riffNotes] (double from, int transpose)
        {
            const auto riff = song.addClip (lead, from, bar (4));

            for (int each = 0; each < 4; ++each)
            {
                const auto start = from + bar (each);
                const auto* notes = riffNotes[each];
                song.note (riff, notes[0] + transpose, start, 0.5, 100);
                song.note (riff, notes[1] + transpose, start + 0.75, 0.25, 85);
                song.note (riff, notes[2] + transpose, start + 1.5, 0.5, 100);
                song.note (riff, notes[3] + transpose, start + 2.5, 0.5, 95);
                song.note (riff, notes[4] + transpose, start + 3.0, 0.75, 95);
            }

            return riff;
        };

        song.repeat (addRiff (bar (16), 0), { bar (20), bar (24), bar (28) });
        song.repeat (addRiff (bar (40), 12), { bar (44), bar (48), bar (52) });
        song.setVolume (lead, -2.0f);

        // A noise riser held through the build-up and the breakdown's end, its volume rising each time (an automation lane).
        const auto riser = song.addTrack ("Riser", "Gray");
        song.setInstrument (riser, devices::Synth::typeID, "Synth/FX/Noise Riser");
        song.note (song.addClip (riser, bar (8), bar (8)), 57, bar (8), bar (8), 100);
        song.note (song.addClip (riser, bar (36), bar (4)), 57, bar (36), bar (4), 100);
        const auto quiet = decibelsToFaderPosition (-27.0f), full = decibelsToFaderPosition (-3.0f);
        song.automate (
            getVolume (song.context, riser).getRef(),
            { { at (bar (8)), quiet, -0.3f }, { at (bar (16)), full }, { at (bar (36)), quiet, -0.3f }, { at (bar (40)), full } });

        // A reverb and a delay that several tracks send to, and the master's multiband, which makes the mix loud.
        const auto reverb = song.addReturn (chords, devices::Reverb::typeID, "Reverb/Halls/Large Hall", -10.0f);
        song.send (lead, reverb, -16.0f);
        song.send (riser, reverb, -8.0f);
        song.send (pad, reverb, -6.0f);
        song.addReturn (lead, devices::Delay::typeID, "Delay/Rhythmic/Dotted Eighth", -12.0f);
        song.addEffect ({}, devices::Multiband::typeID, "Multiband/Mastering/Gentle Master");

        useKeyOnEveryTrack (song.context, key);
    }

    //==============================================================================
    // A lo-fi beat at 82 BPM in F major, in the lo-fi outline's 48 bars: ninth chords from the chord lane on a soft e-piano
    // that wobbles as a tape does (its own LFO on its pitch), through a container with macros (Muffle and Drive), and an LFO
    // that pans it; a lazy boom-bap beat with ghost notes, swung, its velocities a little uneven, and a fill at each part's
    // end; shaker, vinyl and rain samples; a walking bass; a melody with a dub delay and an answer to it; and the whole song
    // coming in through a filter on the master, which opens over the intro and closes as the drums fade out (lanes).
    void makeLoFiBeat (Builder& song)
    {
        constexpr double bpm = 82.0;
        song.setTempo (bpm);
        const Key key { 5, Scale::major };
        juce::Random random (19); // the drums' velocities: uneven, but the same in every song made

        // Intro 4, Loop A 8, Loop B 8, Loop A 8, Break 4, Loop B 8, Outro 8, each part's chords a bar each, with each chord's
        // root for the bass (F major's G, C, F, D, Bb and A, low).
        if (auto* played = findSong (song.context))
            applyOutline (*played, juce::String ("lofi"));

        struct Chord
        {
            const char* numeral;
            int bassRoot;
        };

        struct Part
        {
            std::array<Chord, 4> chords;
            int fromBar, toBar;
        };

        constexpr std::array<Chord, 4> loopA { { { "ii9", 31 }, { "V9", 36 }, { "Imaj9", 29 }, { "vi9", 38 } } };
        constexpr std::array<Chord, 4> loopB { { { "IVmaj7", 34 }, { "iii7", 33 }, { "vi9", 38 }, { "V7", 36 } } };
        constexpr std::array<Chord, 4> breakChords { { { "IVmaj7", 34 }, { "IVmaj7", 34 }, { "V9", 36 }, { "V9", 36 } } };
        const Part parts[] = { { loopA, 0, 12 },        { loopB, 12, 20 }, { loopA, 20, 28 },
                               { breakChords, 28, 32 }, { loopB, 32, 40 }, { loopA, 40, 48 } };
        std::vector<int> roots;

        for (const auto& part : parts)
        {
            std::vector<const char*> numerals;

            for (const auto& chord : part.chords)
                numerals.push_back (chord.numeral);

            song.layChords (key, numerals, part.fromBar, part.toBar);

            for (int each = part.fromBar; each < part.toBar; ++each)
                roots.push_back (part.chords[(size_t) (each - part.fromBar) % 4].bassRoot);
        }

        // The drums: swung hats, their offbeats quieter, and two bars of boom bap, the kick on 1 and pushed before 3 in the A
        // parts, later in the B parts, the snare a little late on 2 and 4 with ghost notes before it; for a part's last two
        // bars, the same ending in a fill of snare and toms.
        const auto beat = song.addTrack ("Beat", "Vermilion");
        song.setInstrument (beat, devices::DrumSampler::typeID, "Drum Sampler/Kits/Lo-Fi Kit");
        constexpr double swing = 0.08; // how late the offbeats are, in beats
        constexpr double lazy = 0.03;  // how late the snare is
        constexpr int lowTom = 45, midTom = 47, highTom = 50;
        const auto vary = [&random] (int velocity) { return juce::jlimit (1, 127, velocity + random.nextInt (11) - 5); };

        const auto addHats = [&song, &vary] (ClipID clip, double start, int beats)
        {
            for (int each = 0; each < beats; ++each)
            {
                song.note (clip, closedHat, start + each, 0.25, vary (72));
                song.note (clip, closedHat, start + each + 0.5 + swing, 0.25, vary (46));
            }
        };

        const auto hatsOnly = song.addClip (beat, bar (0), 4.0);
        addHats (hatsOnly, 0.0, 4);
        song.note (hatsOnly, rim, 2.75 + swing, 0.25, 66);
        song.repeat (hatsOnly, { bar (1), bar (2), bar (3), bar (44), bar (45), bar (46), bar (47) });

        const auto addBoomBap = [&] (double start, bool partB, bool fill)
        {
            const auto clip = song.addClip (beat, start, bar (2));

            for (int each = 0; each < 2; ++each)
            {
                const auto from = start + bar (each);
                const auto fillsHere = fill && each == 1;
                addHats (clip, from, fillsHere ? 3 : 4);
                song.note (clip, kick, from, 0.25, vary (110));

                if (partB)
                {
                    song.note (clip, kick, from + 1.5 + swing, 0.25, vary (86));
                    song.note (clip, kick, from + 2.75, 0.25, vary (96));
                }
                else
                {
                    song.note (clip, kick, from + (each == 0 ? 1.75 + swing : 2.25), 0.25, vary (88));
                    song.note (clip, kick, from + 2.5 + swing, 0.25, vary (100));
                }

                song.note (clip, snare, from + 0.75 + swing / 2.0, 0.25, vary (30));
                song.note (clip, snare, from + 1.0 + lazy, 0.25, vary (100));
                song.note (clip, snare, from + 3.0 + lazy, 0.25, vary (fillsHere ? 96 : 100));

                if (fillsHere)
                {
                    song.note (clip, highTom, from + 3.25, 0.25, vary (90));
                    song.note (clip, midTom, from + 3.5 + swing, 0.25, vary (92));
                    song.note (clip, lowTom, from + 3.75, 0.25, vary (100));
                }
                else
                {
                    song.note (clip, snare, from + 2.75 + swing / 2.0, 0.25, vary (30));
                }
            }

            return clip;
        };

        const auto loopABeat = addBoomBap (bar (4), false, false);
        song.repeat (loopABeat, { bar (6), bar (8), bar (20), bar (22), bar (24), bar (40), bar (42) });
        song.repeat (addBoomBap (bar (10), false, true), { bar (26) });
        song.repeat (addBoomBap (bar (12), true, false), { bar (14), bar (16), bar (32), bar (34), bar (36) });
        song.repeat (addBoomBap (bar (18), true, true), { bar (38) });

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

        // The drums through a bus that crunches them a little and makes them punch; they fade out over the outro's last four
        // bars.
        const auto drums = song.addBus ("Drums", "Vermilion", { beat, shakers });
        song.addEffect (drums, devices::FilterEffect::typeID, "Filter/Creative/Lo-Fi Downsample");
        song.addEffect (drums, devices::Multiband::typeID, "Multiband/Mixing/Drum Punch");
        song.automate (getVolume (song.context, drums).getRef(),
                       { { at (bar (44)), decibelsToFaderPosition (0.0f), 0.3f }, { at (bar (48)), decibelsToFaderPosition (-40.0f) } });

        // A record's crackle under it all, four bars at a time, and rain in the intro, the break and the outro.
        const auto vinyl = song.addTrack ("Vinyl", "Gray");
        const auto crackle = song.addSample (vinyl, "Vinyl 82 BPM", makeVinylCrackle (bpm, 4), bar (0));
        song.repeat (crackle,
                     { bar (4), bar (8), bar (12), bar (16), bar (20), bar (24), bar (28), bar (32), bar (36), bar (40), bar (44) });
        song.setVolume (vinyl, -10.0f);

        const auto weather = song.addTrack ("Rain", "Sky blue");
        song.repeat (song.addSample (weather, "Rain 82 BPM", makeRain (bpm, 4), bar (0)), { bar (28), bar (40), bar (44) });
        song.setVolume (weather, -8.0f);

        // The chord lane's ninths spread out on a soft e-piano, which wobbles as a tape does (its own LFO, slowly and
        // slightly, on its pitch), through a container of tape-like effects with two macros, and an LFO that pans it slowly
        // from side to side.
        const auto keys = song.addPart (PartStyle::spreadChords, 0, 48, "Keys", "Orange", "Synth/Keys/Soft E-Piano");

        if (const auto piano = getInstrument (song.context, keys))
        {
            song.set (piano->id, "lfoTarget", 0.0f); // its pitch
            song.set (piano->id, "lfoRate", 0.6f);
            song.set (piano->id, "lfoDepth", 0.07f); // a semitone at all of it
        }

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

        // The keys' filter closes for the break and opens again after it.
        song.automate (
            { filter, "cutoff" },
            { { at (bar (28)), 3000.0f, -0.3f }, { at (bar (30)), 700.0f }, { at (bar (31)), 700.0f, 0.3f }, { at (bar (32)), 3000.0f } });
        modulate (song.context, song.addLfo (keys, 0.15f), getPan (song.context, keys).getRef(), 0.3f);

        // A walking bass after the intro: each chord's root, its fifth and its octave, then a step up to the next bar's root
        // from a semitone under it.
        const auto bass = song.addTrack ("Bass", "Blue");
        song.setInstrument (bass, devices::Synth::typeID, "Synth/Bass/Sub Bass");
        const auto walk = song.addClip (bass, bar (4), bar (44));

        for (int each = 4; each < 48; ++each)
        {
            const auto start = bar (each);
            const auto root = roots[(size_t) each];
            const auto next = each + 1 < 48 ? roots[(size_t) each + 1] : root;
            song.note (walk, root, start, 1.4, 104);
            song.note (walk, root + 7, start + 1.5 + swing, 0.9, 86);
            song.note (walk, root + 12, start + 2.5 + swing, 0.9, 82);
            song.note (walk, next - 1, start + 3.5 + swing, 0.4, 76);
        }

        song.setVolume (bass, 1.5f);

        // A melody in the B parts, through a dub delay, and its answer in the second A part.
        const auto melody = song.addTrack ("Melody", "Sky blue");
        song.setInstrument (melody, devices::Synth::typeID, "Synth/Pluck/Lo-Fi Pluck");

        struct Note
        {
            int pitch;
            double start, length;
        };

        const auto addPhrase = [&song, melody] (double from, std::initializer_list<Note> notes)
        {
            const auto phrase = song.addClip (melody, from, bar (4));

            for (const auto& each : notes)
                song.note (phrase, each.pitch, from + each.start, each.length, 92);

            return phrase;
        };

        song.repeat (addPhrase (bar (12), { { 74, 0.0, 1.0 },
                                            { 72, 1.5, 0.5 },
                                            { 69, 2.0, 1.5 },
                                            { 72, 4.5, 0.5 },
                                            { 69, 5.0, 0.5 },
                                            { 67, 5.5, 0.5 },
                                            { 69, 6.0, 1.5 },
                                            { 65, 8.0, 0.5 },
                                            { 69, 8.5, 0.5 },
                                            { 74, 9.0, 1.0 },
                                            { 72, 10.5, 0.5 },
                                            { 69, 11.0, 1.0 },
                                            { 67, 12.0, 1.0 },
                                            { 69, 13.0, 0.5 },
                                            { 72, 13.5, 0.5 },
                                            { 67, 14.0, 2.0 } }),
                     { bar (16), bar (32), bar (36) });
        song.repeat (addPhrase (bar (20), { { 74, 0.0, 0.75 },
                                            { 77, 1.0, 0.5 },
                                            { 81, 1.5, 1.0 },
                                            { 79, 3.0, 0.75 },
                                            { 76, 4.5, 0.5 },
                                            { 74, 5.0, 1.5 },
                                            { 70, 7.0, 0.5 },
                                            { 72, 7.5, 0.5 },
                                            { 69, 8.0, 1.0 },
                                            { 72, 9.5, 0.5 },
                                            { 76, 10.0, 1.5 },
                                            { 77, 12.5, 0.5 },
                                            { 76, 13.0, 0.5 },
                                            { 74, 13.5, 2.0 } }),
                     { bar (24) });
        song.setVolume (melody, -6.0f);

        const auto reverb = song.addReturn (keys, devices::Reverb::typeID, "Reverb/Rooms/Small Room", -12.0f);
        song.send (melody, reverb, -14.0f);
        song.addReturn (melody, devices::Delay::typeID, "Delay/Vintage/Dub Echo", -10.0f);

        // The whole song comes in through a filter on the master, which opens over the intro, and goes out through it.
        const auto master = song.addEffect ({}, devices::FilterEffect::typeID);
        song.automate (
            { master, "cutoff" },
            { { at (bar (0)), 500.0f, 0.4f }, { at (bar (4)), 20000.0f }, { at (bar (44)), 20000.0f, -0.4f }, { at (bar (48)), 600.0f } });
        song.addEffect ({}, devices::Limiter::typeID, "Limiter/Mastering/Transparent Master");

        useKeyOnEveryTrack (song.context, key);
    }
} // namespace

const std::vector<ExampleSong>& getExampleSongs()
{
    static const std::vector<ExampleSong> songs {
        { "trapBeat", "Trap Beat",
          "Trap at 140 BPM: half-time drums with hi-hat rolls in triplets and 32nds, an 808 that glides on slide notes, FM bells, "
          "a dark pad from the chord lane, a pluck in the verse, a riser and a crash, and sends to a plate and a delay.",
          "Bells" },
        { "loFiBeat", "Lo-fi Beat",
          "A lo-fi beat at 82 BPM: a lazy, swung kit with fills, shaker, vinyl and rain samples, ninth chords on a wobbling "
          "e-piano through a container with macros, an LFO that pans them, a walking bass, automation and a dub delay.",
          "Melody" },
        { "houseGroove", "House Groove",
          "House at 124 BPM with a breakdown and two drops: drums in a bus, samples in the playlist, a bass and chords that pump "
          "under the kick through sidechains, a pad, automation, an LFO, risers and sends to a reverb and a delay.",
          "Lead" },
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

    if (id == "loFiBeat")
        makeLoFiBeat (song);
    else if (id == "trapBeat")
        makeTrapBeat (song);
    else if (id == "houseGroove")
        makeHouseGroove (song);
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

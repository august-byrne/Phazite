// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExampleSongs.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/DrumSynthesis.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/fm/FmSynth.h"
#include "devices/motion/Chorus.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
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
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContents.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Tempo.h"

#include <PhaziteData.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

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
    // A song by someone else that ships as its notes (resources/songs/*.mid, among Phazite's data), each of its parts by its
    // track's name: its notes in the file's ticks, so that two clips' notes compare exactly.
    struct SongNotes
    {
        struct Note
        {
            int start = 0, length = 0; // in ticks
            int pitch = 0, velocity = 0;

            bool operator== (const Note&) const = default;
        };

        int ticksPerBeat = 1;
        std::map<juce::String, std::vector<Note>> parts; // each in time order

        double toBeats (int ticks) const { return ticks / (double) ticksPerBeat; }

        const std::vector<Note>& getPart (const juce::String& name) const
        {
            static const std::vector<Note> none;
            const auto found = parts.find (name);
            return found != parts.end() ? found->second : none;
        }
    };

    // A song's notes from its MIDI file among Phazite's data by the data's name ("Exodus_mid" for resources/songs/Exodus.mid);
    // none if it isn't there.
    SongNotes readSongNotes (const char* resourceName)
    {
        SongNotes song;
        int size = 0;
        const auto* data = PhaziteData::getNamedResource (resourceName, size);
        juce::MidiFile file;

        if (data == nullptr)
            return song;

        if (juce::MemoryInputStream stream (data, (size_t) size, false); ! file.readFrom (stream) || file.getTimeFormat() <= 0)
            return song;

        song.ticksPerBeat = file.getTimeFormat();

        for (int index = 0; index < file.getNumTracks(); ++index)
        {
            juce::MidiMessageSequence sequence (*file.getTrack (index));
            sequence.updateMatchedPairs();
            juce::String name;
            std::vector<SongNotes::Note> notes;

            for (const auto* event : sequence)
            {
                const auto& message = event->message;

                if (message.isTrackNameEvent() && name.isEmpty())
                    name = message.getTextFromTextMetaEvent();
                else if (message.isNoteOn() && event->noteOffObject != nullptr)
                {
                    const auto start = juce::roundToInt (message.getTimeStamp());
                    notes.push_back ({ start, juce::roundToInt (event->noteOffObject->message.getTimeStamp()) - start,
                                       message.getNoteNumber(), message.getVelocity() });
                }
            }

            if (name.isNotEmpty())
                song.parts[name] = std::move (notes);
        }

        return song;
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

        // Tempos along the song, each from its beat, the first at its start.
        void setTempoChanges (const std::vector<TempoChange>& tempos)
        {
            if (auto* song = findSong (context))
                takeTempoChanges (*song, tempos, BeatPosition());
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

        // A part of a song that ships as its notes (SongNotes) on a track, in clips of so many bars: a clip ends where a bar
        // ends with no note held over it and holds only the bars with notes, and one that plays what an earlier one plays is
        // an alias of it (repeat()), as a pattern is played again. The notes come in any order.
        void addNotes (TrackID track, const SongNotes& song, std::vector<SongNotes::Note> notes, int barsPerClip)
        {
            // In time order, so that a clip starts with the bar of its earliest note, whichever was given first.
            std::stable_sort (notes.begin(), notes.end(), [] (const SongNotes::Note& a, const SongNotes::Note& b)
                              { return std::tie (a.start, a.pitch) < std::tie (b.start, b.pitch); });

            const auto ticksPerBar = song.ticksPerBeat * 4;
            auto end = 0;

            for (const auto& each : notes)
                end = std::max (end, each.start + each.length);

            std::vector<int> ends;

            for (auto at = ticksPerBar * barsPerClip; at < end; at += ticksPerBar * barsPerClip)
                if (std::none_of (notes.begin(), notes.end(),
                                  [at] (const SongNotes::Note& held) { return held.start < at && held.start + held.length > at; }))
                    ends.push_back (at);

            ends.push_back (end);

            struct Made
            {
                int length;
                std::vector<SongNotes::Note> notes; // from the clip's start
                ClipID clip;
            };

            std::vector<Made> made;
            auto from = 0;

            for (const auto to : ends)
            {
                std::vector<SongNotes::Note> inside;

                for (const auto& each : notes)
                    if (each.start >= from && each.start < to)
                        inside.push_back (each);

                from = to;

                if (inside.empty())
                    continue;

                const auto first = inside.front().start / ticksPerBar * ticksPerBar;
                auto last = 0;

                for (auto& each : inside)
                {
                    last = std::max (last, each.start + each.length);
                    each.start -= first;
                }

                const auto length = (last + ticksPerBar - 1) / ticksPerBar * ticksPerBar - first;
                const auto same = std::find_if (made.begin(), made.end(), [length, &inside] (const Made& earlier)
                                                { return earlier.length == length && earlier.notes == inside; });

                if (same != made.end())
                {
                    repeat (same->clip, { song.toBeats (first) });
                    continue;
                }

                const auto clip = addClip (track, song.toBeats (first), song.toBeats (length));

                for (const auto& each : inside)
                    note (clip, each.pitch, song.toBeats (first + each.start), song.toBeats (each.length), each.velocity);

                made.push_back ({ length, std::move (inside), clip });
            }
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
    // Exodus, a synthwave song by XCALIBUR (https://soundcloud.com/xc4libur/exodus), which its author shared with its LMMS
    // project under CC BY 4.0 (resources/licenses/notices.json): its notes (resources/songs/Exodus.mid, written from the
    // project by tools/lmms-to-midi.ps1), played on Phazite's instruments as its recording plays them: in F major, 112 bars,
    // with its tempo (the tempo lane): a tape speeding up from 50 BPM over the intro, 114 BPM through the first two themes,
    // spinning up to 260 BPM over each breakdown before it drops to 50 and speeds up again over the build-up, and 111 BPM in
    // the last theme.
    //
    // Its parts are the original's, each on the preset made after its sound, as it comes (docs/presets.md): five drums,
    // each on a track of its own with its own hits, in a folder; a bass in two parts, sixteenth notes rolling over a sub
    // that sinks from bright to dark through each held chord; and an arpeggio of seventh chords in sixteenths in four
    // layers, a square, a wide saw beside it, a stack of saws an octave under them and a sparkle two octaves over them. Its
    // mix is the original's too: two buses, the bass's with a dark plate and the arpeggio's through a warped record, each
    // ducked deep under the kick (the pump), its compressor listening to the Kick track; every layer's own plate before its
    // bus, so that the tails pump with it; and the master's warm, dark EQ, its high-pass and low-pass in automation lanes,
    // each sweep a few points with bent lines between them (the intro opens from a narrow band, the breakdowns and the
    // build-ups thin and sweep into the themes, the low-pass closes over the outro) and its limiter. The chord lane follows
    // the sub. Its credit is in the project's notes, so that it stays with what is made from it.
    void makeExodus (Builder& song)
    {
        const Key key { 5, Scale::major };
        const auto notes = readSongNotes ("Exodus_mid");
        const auto ticksPerBeat = notes.ticksPerBeat, ticksPerBar = 4 * ticksPerBeat, sixteenth = ticksPerBeat / 4;

        // Its recording differs from the project XCALIBUR shared, and is what the song follows (measured against the
        // project as LMMS renders it): it leaves the project's bars 24 to 32 out, plays every pitched part a fourth higher
        // (the drums and every effect stay where they are), holds 114 BPM through its first two themes, and has the drums
        // and the pump in its first intro. Bars here are the recording's: the project's from 32 on come 8 earlier.
        constexpr int cutFrom = 24, cutTo = 32;
        constexpr int up = 5; // the recording's fourth

        // The mix: each track's fader, in decibels, set so that the song's parts sit as the original's do (each of its
        // parts as LMMS renders it alone, and its recording).
        constexpr float kickLevel = 0.1f, snareLevel = -0.2f, clapLevel = -3.7f, percLevel = -2.8f, openHatLevel = 2.3f;
        constexpr float rollingBassLevel = -12.7f, subLevel = 0.6f, sparkleLevel = -11.0f, bassBusLevel = 0.8f;
        constexpr float squareLevel = -2.3f, wideSawLevel = 2.8f, supersawLevel = -17.0f, arpBusLevel = 0.8f;
        constexpr float masterLevel = 0.0f;

        const auto part = [&notes, ticksPerBar] (const juce::String& name, int transpose)
        {
            std::vector<SongNotes::Note> kept;

            for (auto note : notes.getPart (name))
            {
                if (note.start >= cutFrom * ticksPerBar && note.start < cutTo * ticksPerBar)
                    continue;

                if (note.start >= cutTo * ticksPerBar)
                    note.start -= (cutTo - cutFrom) * ticksPerBar;

                note.pitch += transpose;
                kept.push_back (note);
            }

            return kept;
        };

        // The recording's tempo, straight from point to point as LMMS's automation ramps it: up from 50 BPM to 98 by bar 4
        // and to 114 by bar 8, which the first two themes hold; each breakdown from 111 up to 260 BPM three sixty-fourths
        // of a bar before its end, where it drops to 51; and each build-up from 50 back up to 98 and to its theme's tempo,
        // 114 and then 111. Every kick then falls within 15 ms of the recording's.
        std::vector<TempoChange> tempos { { at (bar (0)), 50.0, tempoRamp }, { at (bar (4)), 98.0, tempoRamp }, { at (bar (8)), 114.0 } };

        for (const auto& [breakdown, theme] : { std::pair (32.0, 114.0), std::pair (72.0, 111.0) })
            for (const auto& [after, bpm, curve] : { std::tuple (0.0, 111.0, tempoRamp), std::tuple (7.0 + 141.0 / 192.0, 260.0, tempoRamp),
                                                     std::tuple (7.75, 51.0, tempoJump), std::tuple (8.0, 50.0, tempoRamp),
                                                     std::tuple (12.0, 98.0, tempoRamp), std::tuple (16.0, theme, tempoJump) })
                tempos.push_back ({ at (bar (breakdown + after)), bpm, curve });

        song.setTempoChanges (tempos);

        song.layOutSections (
            { { .name = "Intro",
                .role = "intro",
                .tip = "Everything plays from the start, through the master's low-pass and high-pass, which open bar by bar",
                .bars = 8,
                .energy = 0.3f },
              { .name = "Theme",
                .role = "drop",
                .tip = "The arpeggio's four layers over the rolling bass and the sub, all pumping under the kick",
                .bars = 24,
                .energy = 0.85f },
              { .name = "Breakdown",
                .role = "breakdown",
                .tip = "No drums, so no pump: the tempo spins up while the master's filters close in",
                .bars = 8,
                .energy = 0.4f },
              { .name = "Build-up",
                .role = "buildUp",
                .tip = "The drums come back, and the pump with the kick, as the tempo climbs; the high-pass sweeps into the theme",
                .bars = 8,
                .energy = 0.6f },
              { .name = "Theme 2",
                .role = "drop",
                .tip = "The theme again; the master's low-pass shuts and opens over its last eight bars",
                .bars = 24,
                .energy = 0.9f },
              { .name = "Breakdown 2", .role = "breakdown", .tip = "The breakdown again", .bars = 8, .energy = 0.4f },
              { .name = "Build-up 2", .role = "buildUp", .tip = "The build-up again, into a slower theme", .bars = 8, .energy = 0.6f },
              { .name = "Theme 3", .role = "drop", .tip = "The chords change every two bars now", .bars = 16, .energy = 1.0f },
              { .name = "Outro", .role = "outro", .tip = "The master's low-pass closes over the last bars", .bars = 8, .energy = 0.5f } });

        setProjectNotes (song.context, "Exodus, by XCALIBUR: https://soundcloud.com/xc4libur/exodus\n"
                                       "From its LMMS project, which XCALIBUR shared on the LMMS Sharing Platform "
                                       "(https://lmms.io/lsp/?action=show&file=12900) under the Creative Commons Attribution 4.0 license "
                                       "(CC BY 4.0: https://creativecommons.org/licenses/by/4.0/).\n"
                                       "Phazite plays its notes as its recording plays them: with its tempo, its first theme 24 bars long, "
                                       "every pitched part a fourth higher than the project's, and the drums in the intro. The sounds are "
                                       "Phazite's own presets and effects, made after the original's and mixed as it is mixed; its drum "
                                       "samples are synthesized drums here. If you share something made from it, credit XCALIBUR.");

        // Every track plays one of Phazite's presets as it comes, and the song mixes them: their levels, their buses, the
        // pump and the master's chain.
        //
        // The drums: a kick on every beat and a snare on two and four, a fill of struck percs and claps with an open hat
        // closing every eighth bar, on the Dark Synthwave Kit; in the intro too, as the recording has them. Each drum is a
        // track of its own with its own hits, as the original has its five samples: the kit as it comes on each, playing
        // that drum alone (the perc is the kit's pad 37, where General MIDI has its rim shot), and its fader balancing it
        // as the original does. They are in a folder, Drums, which mixes them as they are.
        auto drumNotes = part ("Drums", 0);
        const auto hit = [&drumNotes, sixteenth] (int tick, int pitch)
        { drumNotes.push_back ({ .start = tick, .length = sixteenth, .pitch = pitch, .velocity = 100 }); };

        for (int introBar = 0; introBar < 8; ++introBar)
        {
            const auto fill = introBar == 7; // the file has the fill's last beat: its snare, percs and claps

            for (int each = 0; each < (fill ? 3 : 4); ++each)
                hit (introBar * ticksPerBar + each * ticksPerBeat, kick);

            hit (introBar * ticksPerBar + ticksPerBeat, snare);

            if (fill)
                hit (introBar * ticksPerBar + 10 * sixteenth, openHat);
            else
                hit (introBar * ticksPerBar + 3 * ticksPerBeat, snare);
        }

        const auto addDrum = [&song, &notes, &drumNotes] (const juce::String& name, int pad, float decibels)
        {
            const auto track = song.addTrack (name, "Vermilion");
            song.setInstrument (track, devices::DrumSampler::typeID, "Drum Sampler/Kits/Dark Synthwave Kit");
            std::vector<SongNotes::Note> hits;

            for (const auto& each : drumNotes)
                if (each.pitch == pad)
                    hits.push_back (each);

            song.addNotes (track, notes, hits, 8);
            song.setVolume (track, decibels);
            return track;
        };

        const auto kickTrack = addDrum ("Kick", kick, kickLevel);
        song.addBus ("Drums", "Vermilion",
                     { kickTrack, addDrum ("Snare", snare, snareLevel), addDrum ("Clap", clap, clapLevel),
                       addDrum ("Open Hat", openHat, openHatLevel), addDrum ("Perc", rim, percLevel) });

        // The bass, in two parts. The rolling bass: sixteenth notes on each chord's root (its sound's carrier rings an
        // octave and a fifth over the note played, where the file has its notes), each dying before the next, with its low
        // end lifted. The sub under it: one note held through each chord, bright as it starts and sinking as its filter
        // closes, through a slow, wide chorus and a lift of its deepest octave.
        const auto rollingBass = song.addTrack ("Rolling Bass", "Blue");
        song.setInstrument (rollingBass, devices::FmSynth::typeID, "FM/Bass/Darksynth Bass");
        song.addEffect (rollingBass, devices::Equalizer::typeID, "EQ/Mixing/Low-End Lift");
        song.addNotes (rollingBass, notes, part ("Bass", up - 19), 4);
        song.setVolume (rollingBass, rollingBassLevel);

        const auto sub = song.addTrack ("Sub", "Blue");
        song.setInstrument (sub, devices::WavetableSynth::typeID, "Wavetable/Bass/Slow Sweep Bass");
        song.addEffect (sub, devices::Chorus::typeID, "Chorus/Ensemble/Deep Swirl");
        song.addEffect (sub, devices::Equalizer::typeID, "EQ/Mixing/Deep Sub Lift");
        song.addNotes (sub, notes, part ("Sub", up), 4);
        song.setVolume (sub, subLevel);

        // The arpeggio's top layer, two octaves over the others: a glassy pluck through an amplifier's scooped tone and a
        // bright plate, an LFO panning it slowly. It plays with the bass, so it pumps as deep as the bass does and stays
        // steady over the layers that warble.
        const auto sparkle = song.addTrack ("Sparkle", "Sky blue");
        song.setInstrument (sparkle, devices::FmSynth::typeID, "FM/Pluck/Sparkle Pluck");
        song.addEffect (sparkle, devices::Equalizer::typeID, "EQ/Creative/Amp Mid Scoop");
        song.addEffect (sparkle, devices::Reverb::typeID, "Reverb/Plates/Airy Plate");
        song.addNotes (sparkle, notes, part ("Arp", up + 24), 4);
        song.setVolume (sparkle, sparkleLevel);

        {
            const auto lfo = song.addLfo (sparkle, 0.2f);

            for (const auto& [id, value] : { std::pair ("bipolar", 1.0f), std::pair ("phase", 0.75f) }) // from the left
                if (auto parameter = getModulatorParameter (song.context, lfo, id); parameter.exists())
                    parameter.setValue (value);

            modulate (song.context, lfo, getPan (song.context, sparkle).getRef(), 0.07f);
        }

        // The bass's bus: a dark plate over all three, then the pump, its compressor listening to the kick, so that the
        // plate's tail pumps too.
        const auto bassBus = song.addBus ("Bass + Sparkle", "Blue", { rollingBass, sub, sparkle });
        song.addEffect (bassBus, devices::Reverb::typeID, "Reverb/Plates/Dark Plate");

        if (const auto ducker = duckUnder (song.context, bassBus, kickTrack); ducker.isValid())
            song.loadPreset (ducker, "Compressor/Sidechain/Sidechain Full Pump");

        song.setVolume (bassBus, bassBusLevel);

        // The arpeggio's three other layers, the same line on each: a bare square pluck into a long, dark plate, which
        // fills the gaps between its notes; a wide saw beside it, each of its harmonics a band of its own, which gives the
        // arpeggio its width; and an octave under them a stack of drifting saws, squeezed, excited and squeezed again.
        const auto square = song.addTrack ("Square", "Bluish green");
        song.setInstrument (square, devices::Synth::typeID, "Synth/Pluck/Square Pluck");
        song.addEffect (square, devices::Reverb::typeID, "Reverb/Plates/Long Dark Plate");
        song.addNotes (square, notes, part ("Arp", up), 4);
        song.setVolume (square, squareLevel);

        const auto wideSaw = song.addTrack ("Wide Saw", "Bluish green");
        song.setInstrument (wideSaw, devices::Sampler::typeID, "Sampler/Pluck/Wide Saw Pluck");
        song.addEffect (wideSaw, devices::Reverb::typeID, "Reverb/Plates/Dark Plate");
        song.addNotes (wideSaw, notes, part ("Arp", up), 4);
        song.setVolume (wideSaw, wideSawLevel);

        const auto supersaw = song.addTrack ("Supersaw", "Bluish green");
        song.setInstrument (supersaw, devices::WavetableSynth::typeID, "Wavetable/Pluck/Octave Supersaw Pluck");
        song.addEffect (supersaw, devices::Limiter::typeID, "Limiter/Mixing/Dense Squeeze");
        song.addEffect (supersaw, devices::Exciter::typeID, "Exciter/Creative/Saturated Highs");
        song.addEffect (supersaw, devices::Limiter::typeID, "Limiter/Mixing/Drum Bus Shaver");
        song.addEffect (supersaw, devices::Reverb::typeID, "Reverb/Plates/Dark Plate");
        song.addNotes (supersaw, notes, part ("Arp", up - 12), 4);
        song.setVolume (supersaw, supersawLevel);

        // The arpeggio's bus: the pump, a little less deep than the bass's, then a warped record over the layers and their
        // plates: its quick, shallow wow, its dark top, its narrower sides and its hiss.
        const auto arpBus = song.addBus ("Arp", "Bluish green", { square, wideSaw, supersaw });

        if (const auto ducker = duckUnder (song.context, arpBus, kickTrack); ducker.isValid())
            song.loadPreset (ducker, "Compressor/Sidechain/Sidechain Deep Pump");

        song.addEffect (arpBus, devices::Vinyl::typeID, "Vinyl/Records/Warped Record");
        song.setVolume (arpBus, arpBusLevel);

        // The chord lane: each of the sub's notes is its chord's root, in F major.
        setLaneKey (song.context, BeatPosition(), key);
        constexpr std::array<const char*, 12> numerals { "I", "", "ii", "", "iii", "IV", "", "V", "", "vi", "bVII", "" };

        for (const auto& root : part ("Sub", up))
            if (const auto* name = numerals[(size_t) ((root.pitch - key.root + 12) % 12)]; *name != 0)
                if (const auto numeral = parseNumeral (name))
                    addLaneChord (song.context, { at (notes.toBeats (root.start)), at (notes.toBeats (root.start + root.length)) },
                                  *numeral);

        // The master, each device a preset as it comes: the original's warm, dark EQ (its lows up, its mids and its top
        // down); its high-pass, with a little peak over its cutoff, and its low-pass, both at 12 dB an octave; and a
        // limiter that takes most of each kick's peak away, which the faders feed. Its fader at 0 dB, as the original's
        // master: what the limiter lets through is what the song plays, its ceiling the song's peak.
        song.addEffect ({}, devices::Equalizer::typeID, "EQ/Mastering/Warm Dark Master");
        const auto highPass = song.addEffect ({}, devices::FilterEffect::typeID, "Filter/Classic/Peaked High-Pass");
        const auto lowPass = song.addEffect ({}, devices::FilterEffect::typeID, "Filter/Classic/Gentle Low-Pass");
        song.addEffect ({}, devices::Limiter::typeID, "Limiter/Mastering/Loud Master");

        if (auto volume = getMasterVolume (song.context); volume.exists())
            volume.setValue (decibelsToFaderPosition (masterLevel));

        // The filters' sweeps, in hertz, as the original's automation moves them. LMMS draws each with three to six points
        // and a smooth curve through them in its knob's own scale; here each is a few points from its bar with the line
        // between two bent (AutomationPoint::curve), fitted to follow the original's within an eighth of an octave where a
        // filter can be heard (a high-pass over 30 Hz, a low-pass under 18 kHz). A last point marked held stays until the
        // next sweep's first.
        struct SweepPoint
        {
            double bars;
            float hertz;
            float bend = 0.0f; // of the line to the next point
        };

        const auto sweep = [&song] (DeviceID filter, double fromBar, std::initializer_list<SweepPoint> points, bool thenJumps = true)
        {
            for (const auto* point = points.begin(); point != points.end(); ++point)
                addAutomationPoint (
                    song.context, { filter, "cutoff" },
                    { at (bar (fromBar + point->bars)), point->hertz, thenJumps && point + 1 == points.end() ? 1.0f : point->bend });
        };

        // The high-pass thins the intro, each breakdown and each build-up, dipping at first so that the lows come in, then
        // sweeping up to 1 kHz near the end; at the end of the intro and of each build-up it drops away into the theme.
        // Through the themes it is switched out by its mix: its cutoff can't go under 20 Hz, where its peak would lift the
        // deepest lows.
        sweep (highPass, 0.0,
               { { 0.0, 342.0f, -0.005f },
                 { 4.25, 97.0f, -0.5f },
                 { 5.96875, 85.0f, 0.345f },
                 { 6.875, 443.0f, -0.2f },
                 { 7.625, 998.0f, -0.005f },
                 { 7.90625, 104.0f, -0.47f },
                 { 8.0, 20.0f } });

        for (const auto breakdown : { 32.0, 72.0 })
        {
            sweep (highPass, breakdown,
                   { { 0.0, 342.0f, -0.005f },
                     { 4.25, 97.0f, -0.5f },
                     { 5.96875, 85.0f, 0.35f },
                     { 6.84375, 440.0f, -0.11f },
                     { 7.5625, 1056.0f } });
            sweep (highPass, breakdown + 8.0,
                   { { 0.0, 342.0f, -0.01f },
                     { 5.09375, 101.0f, -0.5f },
                     { 6.375, 94.0f, 0.385f },
                     { 7.3125, 353.0f, -0.035f },
                     { 7.5625, 1056.0f, 0.045f },
                     { 7.90625, 104.0f, -0.47f },
                     { 8.0, 20.0f } });
        }

        for (const auto& [atBar, mix] : { std::pair (0.0, 1.0f), std::pair (8.0, 0.0f), std::pair (32.0, 1.0f), std::pair (48.0, 0.0f),
                                          std::pair (72.0, 1.0f), std::pair (88.0, 0.0f) })
            addAutomationPoint (song.context, { highPass, "mix" }, { at (bar (atBar)), mix, 1.0f });

        // The low-pass opens the intro from a narrow band, darkens each breakdown, shuts for four bars and opens again
        // before the second breakdown, as the recording does, and closes over the outro.
        constexpr double lastTick = 8.0 - 1.0 / 192.0;
        sweep (lowPass, 0.0, { { 0.0, 495.0f, 0.5f }, { 7.3125, 5387.0f, 0.015f }, { 8.0, 20000.0f } });

        for (const auto breakdown : { 32.0, 72.0 })
            sweep (lowPass, breakdown, { { 0.0, 5423.0f, 0.5f }, { 7.46875, 8550.0f, -0.015f }, { 8.0, 20000.0f } });

        sweep (lowPass, 64.0, { { 0.0, 607.0f }, { 4.0, 607.0f, 0.245f }, { lastTick, 20000.0f } });
        sweep (lowPass, 104.0, { { 0.0, 20000.0f, -0.255f }, { 6.0, 512.0f }, { 8.0, 499.0f } }, false);

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
        song.setVolume (beat, 2.2f); // the kick where it was, before the kit's pads were leveled as a kit (docs/presets.md)
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
        song.setVolume (beat, 2.5f);   // the kick where it was, before the kit's pads were leveled as a kit (docs/presets.md)
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
        { "exodus", "Exodus",
          "Dark synthwave at 114 BPM: Exodus by XCALIBUR (CC BY 4.0), its notes on presets made after its sounds, its tempo "
          "speeding up and slowing down in the tempo lane: an arpeggio in four layers over a rolling bass and a sub, two buses "
          "that pump deep under the kick, each drum on a track of its own, plates, and the master's filters "
          "sweeping in automation lanes.",
          "Square" },
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
    else if (id == "exodus")
        makeExodus (song);
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

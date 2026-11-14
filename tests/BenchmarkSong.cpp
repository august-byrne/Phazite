// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BenchmarkSong.h"
#include "TestClips.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/fm/FmSynth.h"
#include "devices/motion/Chorus.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>

namespace phazite::test
{
namespace
{
    namespace dev = phazite::devices;

    enum class Part
    {
        chords,
        bass,
        beat
    };

    // A clip as long as the song, with its part's notes.
    ClipID addPart (Song& song, TrackID track, Part part, double lengthBeats, int transpose)
    {
        const auto clip = addMidiClip (song, track, { at (0.0), at (lengthBeats) }, "Part");
        REQUIRE (clip.isValid());

        switch (part)
        {
            case Part::chords:
                for (int beat = 0; beat < (int) lengthBeats; ++beat)
                    for (const auto pitch : { 48, 52, 55, 59 })
                        addMidiNote (song, clip, pitch + transpose, at (beat), lasting (0.9), 90);

                break;

            case Part::bass:
                for (int eighth = 0; eighth < (int) (lengthBeats * 2.0); ++eighth)
                    addMidiNote (song, clip, 36 + transpose + (eighth % 8 == 6 ? 7 : 0), at (eighth * 0.5), lasting (0.4), 100);

                break;

            case Part::beat:
                for (int sixteenth = 0; sixteenth < (int) (lengthBeats * 4.0); ++sixteenth)
                {
                    addMidiNote (song, clip, sixteenth % 8 == 6 ? 46 : 42, at (sixteenth * 0.25), lasting (0.2), 80);

                    if (sixteenth % 4 == 0)
                        addMidiNote (song, clip, sixteenth % 8 == 0 ? 36 : 38, at (sixteenth * 0.25), lasting (0.2), 110);
                }

                break;
        }

        return clip;
    }
} // namespace

BenchmarkSong buildBenchmarkSong (TestSong& s, int numTracks, double lengthBeats)
{
    REQUIRE (numTracks > 0);
    auto* open = findSong (s.context);
    REQUIRE (open != nullptr);
    auto& song = *open;
    setTempo (song, 120.0);

    BenchmarkSong built;
    built.lengthBeats = lengthBeats;

    struct Instrument
    {
        const char* typeID;
        Part part;
    };

    const std::array<Instrument, 5> instruments { { { dev::Synth::typeID, Part::chords },
                                                    { dev::WavetableSynth::typeID, Part::chords },
                                                    { dev::FmSynth::typeID, Part::bass },
                                                    { dev::DrumSampler::typeID, Part::beat },
                                                    { dev::Sampler::typeID, Part::bass } } };

    for (int i = 0; i < numTracks; ++i)
    {
        const auto track = i == 0 ? s.firstTrack : s.addTrack ("Track " + juce::String (i + 1));
        const auto& instrument = instruments[(size_t) i % instruments.size()];
        const auto device = s.setInstrument (track, instrument.typeID);
        s.addEffect (track, dev::Equalizer::typeID);
        s.addEffect (track, dev::Compressor::typeID);

        if (i % 3 == 2)
            s.addEffect (track, dev::Saturator::typeID);

        if (i % 5 == 4)
            s.addEffect (track, dev::Chorus::typeID);

        built.tracks.push_back (track);
        built.instruments.push_back (device);
        built.clips.push_back (addPart (song, track, instrument.part, lengthBeats, (i / 5) % 12));
    }

    // The returns, fed by every track (the first's sends made with them), and the automation.
    built.reverbReturn = s.addReturn (built.tracks.front(), dev::Reverb::typeID, -18.0f);
    built.delayReturn = s.addReturn (built.tracks.front(), dev::Delay::typeID, -20.0f);

    for (size_t i = 0; i < built.tracks.size(); ++i)
    {
        const auto track = built.tracks[i];

        if (i > 0)
            s.addSend (track, built.reverbReturn, -18.0f);

        if (i > 0 && i % 2 == 0)
            s.addSend (track, built.delayReturn, -20.0f);

        if (i % 4 == 3)
        {
            const auto volume = s.getVolumeParameter (track);

            for (double start = 0.0; start < lengthBeats; start += 16.0)
                s.addAutomation (volume, { { at (start), decibelsToFaderPosition (0.0f) },
                                           { at (start + 8.0), decibelsToFaderPosition (-12.0f) },
                                           { at (start + 16.0 - 0.25), decibelsToFaderPosition (0.0f) } });
        }

        if (i % instruments.size() == 0) // a Synth
            s.addAutomation ({ built.instruments[i], "cutoff" }, { { at (0.0), 800.0f }, { at (lengthBeats), 6000.0f } });
    }

    // The buses, of ten tracks each.
    for (size_t first = 0; first < built.tracks.size(); first += 10)
    {
        const std::vector<TrackID> members (built.tracks.begin() + (std::ptrdiff_t) first,
                                            built.tracks.begin() + (std::ptrdiff_t) std::min (first + 10, built.tracks.size()));
        const auto bus = s.addBus ("Bus " + juce::String ((int) first / 10 + 1), members);
        s.addEffect (bus, dev::Compressor::typeID);
        built.buses.push_back (bus);
    }

    song.getUndoManager().clearUndoHistory();
    dispatchPendingMessages();
    return built;
}

std::vector<double> timeEngineBlocks (int count, int blockSize)
{
    dispatchPendingMessages();
    auto& engine = getPlaybackHost (getEngineContext()).getEngine();
    juce::AudioBuffer<float> output (2, blockSize), input (2, blockSize);
    input.clear();
    const float* inputs[] = { input.getReadPointer (0), input.getReadPointer (1) };
    float* outputs[] = { output.getWritePointer (0), output.getWritePointer (1) };
    std::vector<double> times;
    times.reserve ((size_t) count);

    for (int block = 0; block < count; ++block)
    {
        const auto start = std::chrono::steady_clock::now();
        engine.process (inputs, 2, outputs, 2, blockSize);
        times.push_back (std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
    }

    return times;
}

double percentileOf (std::vector<double> times, double fraction)
{
    REQUIRE (! times.empty());
    std::sort (times.begin(), times.end());
    return times[std::min (times.size() - 1, (size_t) (fraction * (double) times.size()))];
}

double meanOf (const std::vector<double>& times)
{
    REQUIRE (! times.empty());
    return std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
}
} // namespace phazite::test

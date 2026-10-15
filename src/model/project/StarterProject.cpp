// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StarterProject.h"
#include "devices/drums/DrumSampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Tracks.h"
#include "model/devices/DevicePlugin.h"
#include "model/engine/AppInfo.h"
#include "model/mixer/Mixer.h"
#include "model/transport/Playback.h"

using namespace tracktion::literals; // 0_tp (time position), 16_bp (beat position), ...

namespace phazite
{
namespace
{
    // C - Am - F - G, one chord per bar.
    constexpr int chordNotes[4][4] = { { 60, 64, 67, 72 },   // C
                                       { 57, 60, 64, 69 },   // Am
                                       { 53, 57, 60, 65 },   // F
                                       { 55, 59, 62, 67 } }; // G

    // Eighth notes going up and down the chord: 8 steps per 4/4 bar.
    constexpr int arpOrder[8] = { 0, 1, 2, 3, 2, 1, 0, 1 };

    // The arpeggio's sound on Phazite's synth: two detuned saws, plucked by a filter its envelope opens, under the drums.
    struct Setting
    {
        const char* id;
        float value;
    };

    constexpr Setting pluck[] = { { "cutoff", 700.0f },      { "resonance", 0.25f }, { "filterEnvelope", 30.0f }, { "filterDecay", 250.0f },
                                  { "filterSustain", 0.1f }, { "decay", 400.0f },    { "sustain", 0.4f },         { "release", 250.0f },
                                  { "osc2Detune", 10.0f },   { "level", -12.0f } };

    // A bar of the beat, a sixteenth at a time (General MIDI's drums, which are the drum sampler's pads): the kick on
    // 1, 3 and the "and" after it, the snare on 2 and 4, closed hats on the eighths, and an open hat at the bar's end,
    // which the next closed hat chokes.
    constexpr int kick = 36, snare = 38, closedHat = 42, openHat = 46;

    struct Hit
    {
        int step, note, velocity;
    };

    constexpr Hit beat[] = { { 0, kick, 112 },     { 0, closedHat, 90 },  { 2, closedHat, 70 }, { 4, snare, 105 }, { 4, closedHat, 90 },
                             { 6, closedHat, 70 }, { 8, kick, 104 },      { 8, closedHat, 90 }, { 10, kick, 92 },  { 10, closedHat, 70 },
                             { 12, snare, 105 },   { 12, closedHat, 90 }, { 14, openHat, 80 } };

    te::Plugin::Ptr createOwnDevice (te::Edit& edit, const char* type)
    {
        return edit.getPluginCache().createNewPlugin (type, {});
    }

    void addDrums (te::AudioTrack& track)
    {
        auto& edit = track.edit;
        track.setName ("Drums");
        track.setColour (getColourForName (track.getName()).value_or (getTrackColours()[1].colour));
        setInstrument (track, createOwnDevice (edit, devices::DrumSampler::typeID));

        if (auto clip = track.insertMIDIClip ("Beat", edit.tempoSequence.toTime (te::BeatRange (0_bp, 16_bp)), nullptr))
            for (int bar = 0; bar < 4; ++bar)
                for (const auto& hit : beat)
                    clip->getSequence().addNote (hit.note, te::BeatPosition::fromBeats (bar * 4.0 + hit.step * 0.25),
                                                 te::BeatDuration::fromBeats (0.25), hit.velocity, 0, nullptr);
    }

    void addSynth (te::AudioTrack& track)
    {
        auto& edit = track.edit;
        track.setName ("Synth");
        track.setColour (getTrackColours()[0].colour);
        const auto synth = createOwnDevice (edit, devices::Synth::typeID);

        if (auto* device = dynamic_cast<DevicePlugin*> (synth.get()))
            for (const auto& setting : pluck)
                if (auto* parameter = device->findParameter (setting.id))
                    parameter->setParameter (setting.value, juce::sendNotification);

        setInstrument (track, synth);

        // A 4-bar MIDI clip at bar 1: 16 beats, 8 seconds at 120 BPM.
        if (auto clip = track.insertMIDIClip ("Arpeggio", edit.tempoSequence.toTime (te::BeatRange (0_bp, 16_bp)), nullptr))
            for (int bar = 0; bar < 4; ++bar)
                for (int step = 0; step < 8; ++step)
                    clip->getSequence().addNote (chordNotes[bar][arpOrder[step]], te::BeatPosition::fromBeats (bar * 4.0 + step * 0.5),
                                                 te::BeatDuration::fromBeats (0.45), 100, 0, nullptr);
    }
} // namespace

void addStarterContent (te::Edit& edit)
{
    edit.ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (edit);

    if (tracks.size() < 2)
        return;

    addDrums (*tracks[0]);
    addSynth (*tracks[1]);

    // Playback loops the song, which grows as clips go later.
    loopSong (edit);
    edit.getTransport().setPosition (0_tp);
}

juce::File getDefaultProjectsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Projects");
}
} // namespace phazite

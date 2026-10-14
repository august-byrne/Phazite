// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StarterProject.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/AppInfo.h"
#include "model/transport/Playback.h"

using namespace tracktion::literals; // 0_tp (time position), 16_bp (beat position), ...

namespace phazite
{
namespace
{
    // A saw wave (waveShape1 = 3) through a low-pass filter (filterType = 1), as in Tracktion's demos.
    constexpr const char* leadPatchXml = R"(<PLUGIN type="4osc" filterType="1" waveShape1="3" filterFreq="100"/>)";

    // C - Am - F - G, one chord per bar.
    constexpr int chordNotes[4][4] = { { 60, 64, 67, 72 },   // C
                                       { 57, 60, 64, 69 },   // Am
                                       { 53, 57, 60, 65 },   // F
                                       { 55, 59, 62, 67 } }; // G

    // Eighth notes going up and down the chord: 8 steps per 4/4 bar.
    constexpr int arpOrder[8] = { 0, 1, 2, 3, 2, 1, 0, 1 };
} // namespace

void addStarterContent (te::Edit& edit)
{
    edit.ensureNumberOfAudioTracks (1);
    auto* track = te::getAudioTracks (edit)[0];

    if (track == nullptr)
        return;

    track->setName ("Synth");
    track->setColour (getTrackColours()[0].colour);

    // Tracktion's 4OSC, before the track's volume and meter. (It locks and allocates on the audio
    // thread, see docs/realtime-audit.md; Phazite's own synth replaces it in roadmap step 1.6.)
    auto plugin = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});

    if (auto* synth = dynamic_cast<te::FourOscPlugin*> (plugin.get()))
    {
        if (auto patch = juce::ValueTree::fromXml (leadPatchXml); patch.isValid())
            synth->restorePluginStateFromValueTree (patch);

        track->pluginList.insertPlugin (plugin, 0, nullptr);
    }

    // A 4-bar MIDI clip at bar 1: 16 beats, 8 seconds at 120 BPM.
    const auto clipTime = edit.tempoSequence.toTime (te::BeatRange (0_bp, 16_bp));
    auto clip = track->insertMIDIClip ("Arpeggio", clipTime, nullptr);

    if (clip == nullptr)
        return;

    auto& sequence = clip->getSequence(); // positions in beats from the clip's start

    for (int bar = 0; bar < 4; ++bar)
        for (int step = 0; step < 8; ++step)
            sequence.addNote (chordNotes[bar][arpOrder[step]], te::BeatPosition::fromBeats (bar * 4.0 + step * 0.5),
                              te::BeatDuration::fromBeats (0.45), 100, 0, nullptr);

    // Playback loops the song, which grows as clips go later.
    loopSong (edit);
    edit.getTransport().setPosition (0_tp);
}

juce::File getDefaultProjectsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Projects");
}
} // namespace phazite

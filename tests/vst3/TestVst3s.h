// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' real VST3s: a small effect and a small instrument, each built by JUCE's own plug-in client into a .vst3 of
// its own (juce_add_plugin in CMakeLists.txt), which the tests scan, load and play in the tests' helper as a user's
// plug-in loads (tests/Vst3PluginTests.cpp). Test code alone: built with the tests, never installed, never next to the
// app. What both the plug-ins and the tests know of them is here; each is as simple as the stand-ins of host/TestPlugins.h
// and plays the same every time.
#pragma once

namespace phazite::test::vst3
{
inline constexpr const char* manufacturer = "Phazite Tests";

// The effect: in mono alone (one channel in, one out, and no other layout), it plays what comes in latencySamples late,
// times its Gain, its program's trim and, while Invert is on, -1; it reports that latency. Its parameters' own IDs
// ("gain", "invert") are JUCE's, which its VST3 turns into numbers of its own (VST3ClientExtensions::convertJuceParameterId).
// Its programs, Unity and Half, set its trim, 1 and 0.5: a setting of its own that is no parameter, as a synth's patch is,
// which its state keeps with its parameters (three floats: gain, invert, trim), but not which program it is on.
namespace effect
{
    inline constexpr const char* name = "Test VST3 Effect";
    inline constexpr const char* gainID = "gain";
    inline constexpr const char* invertID = "invert";
    inline constexpr int latencySamples = 100;
    inline constexpr const char* programs[] = { "Unity", "Half" };
    inline constexpr float trims[] = { 1.0f, 0.5f };
} // namespace effect

// The instrument: a stereo input bus that it doesn't listen to, as an instrument that takes sound in (a vocoder) has, and a
// stereo output; for each note held on each MIDI channel, a sine at the note's pitch (A4, note 69, at 440 Hz), as loud as a
// quarter of full scale times the note's velocity (0 to 1) over its channel's number (channel 1 full, channel 4 a
// quarter), from the sample the note starts on to the one it ends on. Each channel's pitch bend bends that channel's notes,
// by up to bendSemitones either way. Its Level (a parameter, "level", from 0 to 1) scales all it plays. Its two programs:
// the first plays the notes at their pitch at a Level of 1, the second an octave higher at a Level of 0.5, which each
// sets, as a synth's programs set its parameters; its program is set only as its host sets it (JUCE's
// setCurrentProgram(), a VST3's program parameter), not by a MIDI program change, as many VST3s don't read those. It
// takes no controller, all notes off included, as many VST3s don't: only note-offs end its notes. It maps MIDI's
// controllers, the bend and the channel's pressure to parameters of its own (JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS,
// JUCE's default for a plug-in that takes MIDI), as VST3 instruments that take the bend do: JUCE's VST3 host hands a
// VST3 those only as changes of the parameters it maps them to, which JUCE's plug-in client turns back into MIDI.
namespace synth
{
    inline constexpr const char* name = "Test VST3 Synth";
    inline constexpr float fullLevel = 0.25f;
    inline constexpr double bendSemitones = 2.0;
    inline constexpr const char* programs[] = { "Unison", "Octave Up" };
    inline constexpr const char* levelID = "level";
    inline constexpr float programLevels[] = { 1.0f, 0.5f };
} // namespace synth
} // namespace phazite::test::vst3

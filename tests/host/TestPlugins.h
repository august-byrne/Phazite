// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' stand-ins for plug-ins (VST3s or the like), as the tests' helper hosts them: made in the helper's process by
// a format of their own, "Test", so that tests reach what Phazite does with plug-ins through its host without one
// installed. Their parameters have IDs of the plug-in's own, as a VST3's have.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace phazite::test
{
struct TestPlugins
{
    static constexpr const char* formatName = "Test";

    enum class Kind
    {
        effect,         // scales its sound by its Gain ("1"), and has a Tone ("0") that does nothing but make its editor larger
                        // (300 by 200, 360 by 240 above 0.9); keeps both as its state; its programs, Full and Half, set its Gain
        synth,          // a sine for each note held, at its Level ("10"), until the note ends or the synth is reset; note 127
                        // turns its Level to 0.75 itself, in a gesture held while the note is (as a control of its window would).
                        // It takes no controller, all notes off included, as a VST3 that maps none gets none from JUCE's host
        crash,          // an effect that crashes as it plays a block while its Crash ("20") is on, and plays what isn't a number
                        // (NaN) in place of its sound while its NaN ("21") is on
        hang,           // an effect that plays nothing back while its Hang ("30") is on: it loops for ever; and its Stall ("31")
                        // stops the thread that turns it on for ever (the helper's message thread, set by hand)
        latency,        // delays its sound by 64 samples, or 128 while its Latency ("40") is on, and says so
        sidechain,      // plays what comes into its sidechain bus in place of its sound
        crashLoading,   // crashes as it is made: in the helper, never in the tests' process
        playhead,       // passes its sound, and keeps as its state where its playhead said the song was as the last piece of a
                        // block it played started (TestPlugins::Playhead); its Watch ("50") does nothing, but moved by a
                        // modulator it cuts the helper's blocks into pieces
        synthWithInput, // the synth with an input bus besides, as an instrument that takes sound in has (a vocoder)
        mono,           // an effect in mono alone, in and out, that plays its sound at half
        slow,           // an effect that passes its sound at its Level ("60"), and waits while a slow gate is shut
        slowToo,        // (getSlowGate()): as it is made and as its state is taken, or as it goes; the second in a file of its own,
        slowSibling,    // the third in the first's file (one helper for both, as a file of several plug-ins has). Each crashes as
                        // it is made from a state whose Level is below 0
    };

    static constexpr Kind kinds[] = { Kind::effect,    Kind::synth,        Kind::crash,      Kind::hang,           Kind::latency,
                                      Kind::sidechain, Kind::crashLoading, Kind::playhead,   Kind::synthWithInput, Kind::mono,
                                      Kind::slow,      Kind::slowToo,      Kind::slowSibling };

    // While this file is there, the slow stand-ins wait, in their helpers: as one is made and as its state is taken, or (the
    // closing gate's) as it goes. In the temporary folder, which the helpers share with the tests' process. And the file each
    // slow one leaves as it begins to wait as it is made, so that a test sees it has begun.
    static juce::File getSlowGate (bool closing = false);
    static juce::File getSlowWaiting (Kind);

    // The playhead stand-in's state: its place in quarter notes (ppqPosition), its time in samples, its tempo, its time
    // signature, whether the song played, and where the bar it is in starts, in quarter notes, and which bar that is; each
    // -1 (0 for the time signature) where the playhead said nothing. And whether it had been told it plays offline
    // (isNonRealtime()) as it played that piece, and whether it is now, as its state is taken.
    struct Playhead
    {
        double ppq = -1.0;
        juce::int64 sample = -1;
        double bpm = -1.0;
        int numerator = 0, denominator = 0;
        bool playing = false;
        double barStart = -1.0;
        juce::int64 barCount = -1;
        bool playedOffline = false, offline = false;

        static Playhead read (const juce::MemoryBlock& state);
    };

    static juce::PluginDescription getDescription (Kind);

    // Files of the format that hang for ever and crash as they are scanned, which hold no plug-in.
    static constexpr const char* hangsScanned = "PhaziteTestHangScanned";
    static constexpr const char* crashesScanned = "PhaziteTestCrashScanned";

    // One made in the tests' own process, as the format makes it (for tests that play it here to compare), in a version of
    // it (the effect's: 1, Gain and Tone; 2, Tone, Gain and Drive; 3, Tone alone), which the format takes from the
    // description's version ("2.0"), as an update of the plug-in would change it.
    static std::unique_ptr<juce::AudioPluginInstance> create (Kind, int version = 1);
    static int getVersion (const juce::PluginDescription&);

    // The format, for a format manager.
    static std::unique_ptr<juce::AudioPluginFormat> createFormat();
};
} // namespace phazite::test

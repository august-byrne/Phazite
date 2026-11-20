// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' stand-ins for plug-ins (VST3s or the like), as the tests' helper hosts them: made in the helper's process by
// a format of their own, "Test", so that tests reach what Phazite does with plug-ins through its host without one
// installed. Their parameters have IDs of the plug-in's own, as a VST3's have.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

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
                        // It takes no controller, all notes off included, as a VST3 that maps none gets none from JUCE's host;
                        // the keys' pressure turns its notes down, the channel's all of them and a note's (aftertouch) its own: by
                        // half at full pressure, by nothing at none. Given a state whose Level a value of 1 follows, it waits
                        // while the state gate is shut (getStateGate()), as a sampler that reads its library; of 2, it takes
                        // half a millisecond, a tenth of a block of 256 at 48 kHz, as a quick preset does
        crash,          // an effect that crashes as it plays a block while its Crash ("20") is on, and plays what isn't a number
                        // (NaN) in place of its sound while its NaN ("21") is on; its Wild ("23") has NaN for its default, a name
                        // and a label thousands of characters long, a text of 100,000 for any value, and reads any text as
                        // NaN; its Nonsense ("22"), set, tells the host the Wild is NaN (from 0.5 up) or an infinity (below),
                        // then that its Told ("24") is the value it was set to; and two parameters whose IDs, 201 characters
                        // long, differ only in their last
        hang,           // an effect that plays nothing back while its Hang ("30") is on: it loops for ever; and its Stall ("31")
                        // stops the thread that turns it on for ever (the helper's message thread, set by hand)
        latency,        // delays its sound by 64 samples, or 128 while its Latency ("40") is on, and says so; its Meter ("41"),
                        // which can't be automated, it sets to the peak of each block it plays, telling the host, as a plug-in
                        // tells a gain-reduction meter it has as a parameter
        sidechain,      // plays what comes into its sidechain bus in place of its sound
        crashLoading,   // crashes as it is made: in the helper, never in the tests' process
        playhead,       // passes its sound, and keeps as its state where its playhead said the song was as the last piece of a
                        // block it played started (TestPlugins::Playhead); its Watch ("50") does nothing, but moved by a
                        // modulator it cuts the helper's blocks into pieces
        synthWithInput, // the synth with an input bus besides, as an instrument that takes sound in has (a vocoder)
        mono,           // an effect in mono alone, in and out, that plays its sound at half
        midiLog,        // an instrument that plays nothing and keeps, as its state, the MIDI messages it was given since it was last
                        // reset (as playback starts and jumps), up to 4096 of them (TestPlugins::readMidiLog())
        slow,           // an effect that passes its sound at its Level ("60"), and waits while a slow gate is shut
        slowToo,        // (getSlowGate()): as it is made, as its state is taken, as it makes its editor and as its Level's text
                        // is written or read, or as it goes; the second in a file of its own,
        slowSibling,    // the third in the first's file (one helper for both, as a file of several plug-ins has). Each crashes as
                        // it is made from a state whose Level is below 0, and takes its Level as its own window would, telling the
                        // host, from a state with a second value of 1 after it
    };

    static constexpr Kind kinds[] = { Kind::effect,    Kind::synth,        Kind::crash,       Kind::hang,           Kind::latency,
                                      Kind::sidechain, Kind::crashLoading, Kind::playhead,    Kind::synthWithInput, Kind::mono,
                                      Kind::slow,      Kind::slowToo,      Kind::slowSibling, Kind::midiLog };

    // While this file is there, the slow stand-ins wait, in their helpers: as one is made and as its state is taken, or (the
    // closing gate's) as it goes. In the temporary folder, which the helpers share with the tests' process. And the file each
    // slow one leaves as it begins to wait as it is made, so that a test sees it has begun.
    static juce::File getSlowGate (bool closing = false);
    static juce::File getSlowBulk(); // while it is there, a slow stand-in's state is 20 MB larger, as one with samples in it
    static juce::File getSlowWaiting (Kind);

    // The file the hanging stand-in leaves while it keeps a block (its Hang on), for a test to see it has begun to keep one.
    static juce::File getHangKeeping();

    // While the state gate is there, the synth given a state that asks it to wait waits (on the helper's message thread), and
    // leaves its setting file meanwhile, for a test to see it has begun.
    static juce::File getStateGate();
    static juce::File getStateSetting();

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

    // The MIDI log stand-in's state: each message it was given since it was last reset, in order.
    static std::vector<juce::MidiMessage> readMidiLog (const juce::MemoryBlock& state);

    static juce::PluginDescription getDescription (Kind);

    // Files of the format that hang for ever and crash as they are scanned, which hold no plug-in.
    static constexpr const char* hangsScanned = "PhaziteTestHangScanned";
    static constexpr const char* crashesScanned = "PhaziteTestCrashScanned";

    // Shell files, as Waves' WaveShell is one: each scanned as holding so many plug-ins ("Test Shell 1" to "Test Shell n",
    // which can't be made).
    static constexpr const char* shellScanned = "PhaziteTestShellScanned";
    static constexpr const char* hugeShellScanned = "PhaziteTestHugeShellScanned";
    static constexpr int shellSize = 1000, hugeShellSize = 9000;

    // One made in the tests' own process, as the format makes it (for tests that play it here to compare), in a version of
    // it (the effect's: 1, Gain and Tone; 2, Tone, Gain and Drive; 3, Tone alone), which the format takes from the
    // description's version ("2.0"), as an update of the plug-in would change it.
    static std::unique_ptr<juce::AudioPluginInstance> create (Kind, int version = 1);
    static int getVersion (const juce::PluginDescription&);

    // The format, for a format manager.
    static std::unique_ptr<juce::AudioPluginFormat> createFormat();
};
} // namespace phazite::test

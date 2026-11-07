# Real-time audit

*Phazite's own engine, with JUCE 9.0.5; measured on 2026-11-05.* This is [roadmap](roadmap.md) step 0.3. It records:
- how Phazite checks that the audio thread follows the real-time rules in `CLAUDE.md`;
- what is known to break them, in JUCE's code, a plug-in's, or Phazite's own until it was fixed;
- what Phazite does about each finding.

A problem in Phazite's own code is fixed; one in JUCE's is worked around, or accepted where it is harmless. The findings keep the numbers that code and other documents cite them by; the numbers of findings that no longer apply aren't used again.

## How it is measured

- **The scenarios:** [tests/RealtimeAuditTests.cpp](../tests/RealtimeAuditTests.cpp) builds small projects through the model (`tests/TestSong.h`) and plays them in real time through Phazite's engine, as the app plays them. Playback goes through the tests' own audio device (`tests/TestAudioDevice.h`), opened on Phazite's device manager as the app opens a device, on a separate thread that acts as the audio thread.
  - The settings are 48 kHz with 256-sample blocks.
  - The engine plays on that one thread, its workers off, so that the monitor counts all it does (the workers play the same strips' code).
  - Like the app, the device opens no audio inputs unless a scenario asks for them; a tone comes into its two inputs.
- **Allocations (Windows Debug):** [tests/AudioThreadMonitor.cpp](../tests/AudioThreadMonitor.cpp) hooks the debug C runtime and counts every `malloc`, `realloc` and `free` on the audio thread. That includes `operator new` and JUCE's `HeapBlock`. It also records each one's call stack, which DbgHelp turns into function names.
- **Warm-up, then steady playback:** each scenario plays at least 0.25 s, and on until the audio thread has gone 0.25 s without allocating or freeing, 2 s at most; then it measures one second of steady playback. Something that keeps allocating still shows in the steady second.
- **Memory errors and data races (Linux CI):** the same playback runs under AddressSanitizer and ThreadSanitizer (the `linux-asan` and `linux-tsan` presets). Their suppression files (`tools/lsan-suppressions.txt`, `tools/tsan-suppressions.txt`) are for leaks and races in code Phazite can't change, each explained there and here; they suppress nothing now.
- **Locks, and posting to the message thread, are not measured yet.** Those findings come from reading the code. Clang's RealtimeSanitizer could measure them on Linux later.
- **To run it:**

  ```
  build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
  ```

- **Guards:** 34 of the 38 scenarios must allocate and free nothing on the audio thread once playback is running, or the tests fail: the browser's preview of a loop, which plays from memory (step 2.18), one empty track, the metronome and a recording of the computer keyboard's notes (step 1.7), an audio input recorded onto an armed track that hears it, with notes played live from another thread (step 3.4), audio clips read from their files as they play, and audio clips of a file at another rate than the engine's, resampled as they play (r8brain-free-src, [dsp-audit.md](dsp-audit.md)), automation with an LFO, lanes sweeping the EQ a cycle a beat and fading a volume (step 2.2), every kind of modulator and a macro moving the EQ (step 2.3; the engine moves what they modulate every 32 samples), a container of the EQ and the compressor at half mix with its macro and the track's LFO turning them (step 2.4), the mix of step 1.4 (a tone through a bus, a send to a reverb return, and the master's loudness meter, built with the functions the mixer uses), a frozen track playing its file (step 2.11), a version's bounce heard in place of the song, with both measured (versions, step V3), and Phazite's own devices (step 1.6: the EQ with every band on and its analyzer listening, the compressor ducked through its sidechain by a muted track, the limiter on the master, the reverb on a return, the synth playing more notes than it has voices, and a beat on the drum sampler; then the devices of step 2.10 in a row, the synth's unison, the wavetable synth, and the filter types of steps 2.14 and 2.15; then, in step 2.17, every effect with its editor's displays listening, and the vocoder at 40 bands with another track as its carrier through the sidechain), and plug-ins in their helpers (step 3.3): an effect after Phazite's synth, and three synths, each handed its block as the engine's block starts. The other scenarios (an audio clip, a loop stretched to the song's tempo, a clip pitched and reversed, and two tracks in a folder bus) only report.

## Results (Windows Debug)

Every scenario plays as the app plays: its song opened as a project, the tests' device calling Phazite's callback on the one audio thread, and Phazite's engine on that thread, with a tone coming into the device's two inputs; the preview plays beside an empty song. The first column covers the warm-up after playback starts (about 0.5 s); the second covers the next second. Measured on 2026-11-05.

| Scenario | Warm-up | Next 1 s |
|---|---|---|
| One empty track | 1 allocation | 0 |
| The metronome, clicking louder on each bar's first beat | 0 | 0 |
| Phazite's engine: its transport and its metronome | 0 | 0 |
| Phazite's engine: chords on Phazite's synth through a track's EQ and fader into the master | 0 | 0 |
| Phazite's engine: audio clips on two tracks, one faded in, read from their files as they play | 0 | 0 |
| Audio clips of a 44.1 kHz file on two tracks, resampled to the 48 kHz engine's rate as they play (r8brain-free-src, 2026-11-06) | 0 | 0 |
| Phazite's engine: chords through a filter whose cutoff is automated, with the volume and the pan automated too | 0 | 0 |
| Phazite's engine: chords through a filter that an LFO, a note follower and a breakpoint envelope move, the pan and the volume by an envelope follower, steps, a random source and a macro | 0 | 0 |
| Phazite's engine: chords in a bus with a container, sends before and after the fader to returns, a sidechain and the loudness meter | 0 | 0 |
| Phazite's engine: chords through a plug-in in its helper (the tests' stand-in) after Phazite's synth | 0 | 0 |
| Phazite's engine: chords on three plug-in synths in their helpers, each handed its block as the engine's block starts | 0 | 0 |
| Phazite's engine recording a take: an armed track's input, heard and recorded, and notes played live from another thread | 0 | 0 |
| Volume automation and an LFO on pan | 0 | 0 |
| A tone through a bus, with a send to Phazite's reverb on a return, into the loudness meter | 0 | 0 |
| Phazite's EQ, every band on and one automated, with its analyzer listening | 0 | 0 |
| Lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume | 0 | 0 |
| Every kind of modulator, and a macro, moving Phazite's EQ | 0 | 0 |
| A container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them | 0 | 0 |
| Phazite's compressor, ducked by another track at 70 % mix, with its meters read | 0 | 0 |
| Phazite's vocoder at 40 bands, its carrier another track through the sidechain | 0 | 0 |
| Phazite's limiter on the master, pushed 12 dB, with its meters read | 0 | 0 |
| Phazite's reverb on a return, with an 8-second decay | 0 | 0 |
| MIDI into Phazite's synth, notes overlapping, with noise and its LFO | 0 | 0 |
| Phazite's synth with seven unison voices, spread wide, playing chords a sixteenth apart | 0 | 0 |
| Phazite's wavetable synth with eight unison voices and two oscillators warped, an envelope sweeping each note's table | 0 | 0 |
| Phazite's filter, saturator, multiband, delay, phaser, flanger, stereo expander and limiter in a row on a tone, the filter swept by a lane | 0 | 0 |
| Phazite's EQ, filter, saturator, compressor, multiband, delay, reverb, phaser and limiter in a row on a tone, every one's editor's displays listening | 0 | 0 |
| Phazite's synth's Comb +, swept by its filter envelope, chords a sixteenth apart, and its filter's Comb - on a tone | 0 | 0 |
| Phazite's wavetable synth's Comb +, swept by an envelope at each note, chords a sixteenth apart | 0 | 0 |
| Phazite's synth's Vowel, swept by its filter envelope, chords a sixteenth apart, and its filter's Phaser on a tone | 0 | 0 |
| Phazite's wavetable synth's Sample & Hold, swept by an envelope at each note, chords a sixteenth apart | 0 | 0 |
| A beat on Phazite's drum sampler, its hats choking each other, a pad played from its editor | 0 | 0 |
| A frozen track: its synth's notes played from its frozen file | 0 | 0 |
| A version's bounce heard in place of the song, both measured as they play (Hear It) | 0 | 0 |
| An audio clip | 0 | 0 |
| Two tracks in a folder bus with Phazite's compressor | 0 | 0 |
| A loop at 120 BPM stretched to 100 BPM, played from Phazite's render | 0 | 0 |
| An audio clip turned up 6 dB, pitched up 3 semitones and reversed, played from Phazite's render | 0 | 0 |
| A loop at 120 BPM previewed at 100 BPM, stretched by Phazite into memory as it starts (the browser's preview) | 0 | 0 |

Steady playback is clean everywhere, and the warm-up nearly so: what is left is JUCE's device callback sizing its buffer on a device's first callback (finding 3).

## Findings and actions

"Measured" means the audit saw it. "Source" means it was found by reading the code.

### Allocations

**3. JUCE's device callback** (measured): `juce::AudioDeviceManager` sizes a temporary buffer on its first callback, once per device. Accept it.

### Locks and messages

**9. Hosted plug-ins** (source).
- **What:** JUCE's plug-in hosting holds each plug-in's callback lock while it processes a block, which the message thread also takes when it prepares, resets or bypasses the plug-in.
- **Action:** every plug-in plays in a helper process of Phazite's (`model/plugins/internal/SandboxedProcessor.h`, since 2026-11-02, step 3.3): Phazite's audio thread takes no lock of a plug-in's; it hands the block to the helper through shared memory and waits for the answer, asleep, until a deadline of the block's own length, and the helper's audio thread holds the plug-in's lock as it plays, as any host's does (`src/host/core/Instance.h`). A plug-in that misses the deadline makes its own block late, never the song's. The guarded scenarios with plug-ins play the tests' stand-ins through their helpers, an effect, and three synths each handed its block as the engine's block starts (`devices::Processor::startsEarly()`), and allocate nothing in steady playback. Renders (exports, stems, freezing, bouncing) hand each block to the plug-in's helper too, and wait for it as long as it takes, off the audio thread.

**11. Stretched, pitched and reversed clips** (measured, steps 1.5, 2.6 and 2.18).
- **What:** stretching or pitching audio as it plays would grow the stretcher's buffers on the audio thread, and cost far more than reading a file.
- **Action:** since step 2.18 (2026-10-28) Phazite renders those clips itself, with Rubber Band's R3 engine, on two background threads of its own (`model/arrangement/internal/ClipRenders.h`), and the engine plays each render as a plain clip: only a file read, ahead of where it plays, by the engine's reading thread (`engine/audio/FileReader.h`). The clip is silent until its render is ready, usually well under a second for a loop. The audit's two scenarios play from those renders.

**13. Recording** (measured, step 3.4; through Phazite's engine since 2026-11-02).
- **What:** a take must not lock, allocate or write files on the audio thread, and must not lose its first blocks while the message thread is slow to hear that recording started.
- **Action:** Phazite's engine takes the audio device's inputs in its own callback, plays them through the tracks that hear them, and copies each armed track's input, from the first sample the song plays after the take is given to the engine (after the count-in), into a buffer of its own that one reading thread empties into a file (`engine/core/Recording.h`, `model/transport/internal/Take.h`). The take is made and freed on the message thread and handed to the engine whole; the engine knows the song's sample it started on, so nothing depends on when the message thread hears that recording started. The notes played live come out of the engine with the clock they came in at. The guarded scenario records a tone onto an armed track that hears it, with notes played live from another thread, and allocates nothing once recording runs.

**17. Previews** (measured, step 1.5; fixed 2026-10-28): stretching a previewed loop as it played grew the stretcher's buffers in its first blocks, so a preview started while the song played could make it glitch.
- **Action (step 2.18):** Phazite stretches a previewed loop itself, as its clip would be, with Rubber Band's R3 engine (`model/engine/internal/PreviewLoop.h`): on a thread of its own, into memory, at the output's rate, starting at once (R3 offline needs no study pass, which only counts frames), and the preview plays as soon as a quarter of a second is ready with the render running at least twice as fast as it plays: about 55 ms after it is chosen for four bars (`windows-profile`).
- **Since 2026-11-02:** previews play on Phazite's engine for previews, beside the song's (`model/playback/internal/PlaybackHost.h`). The stretched loop is an audio region whose file the previews' own reading thread reads from memory, waiting there for the render, and a loop starts once the first 32768 frames are stretched (the part the engine reads of a file at once). The guarded scenario plays it beside an empty song and allocates nothing, in steady playback or (since JUCE 9) its warm-up.

**22. Live MIDI** (source; found by the performance audit, 2026-10-21, finding R8).
- **What:** a note played live comes from another thread (a MIDI input's, or the message thread for the computer keyboard) while the audio thread plays; a lock between them would make the audio thread wait whenever a note arrives as a block is processed.
- **Action:** live notes reach the engine through a queue of a fixed size that any thread pushes into and the audio thread takes from, without locks or allocation (`engine/core/EventQueue.h`), and play in the next block. Watch it in the benchmark of a live note (`tests/Benchmarks.cpp`); `tests/LiveInputTests.cpp` checks that a live note sounds at once, and that every live note sounds however long the message thread was busy.

**24. Yielding while another thread's task finishes** (measured, 2026-11-02, after the owner heard crackling "in different scenarios": the Fold Lead preset played live, House Groove's start).
- **What:** Phazite's engine plays a block's strips as tasks on the audio thread and its workers (`engine/core/Workers.h`). A task waits for the inputs other tasks are playing, and the audio thread for the tasks the workers took, and both waited with `std::this_thread::yield()`. On Windows that is `SwitchToThread()`, which hands the core to any thread ready on it, a busy UI thread at the normal priority included, for the rest of its time slice. Measured on Windows 11 with a normal thread busy on the core of a thread in MMCSS's Pro Audio task, idle 2 ms between blocks as an audio thread is: a third of 30,000 yields took over 1 ms, and the worst 16.6 ms, several blocks. `Sleep (0)` was no better (a fifth over 1 ms, worst 15.8 ms). A spin with the processor's pause instruction never took a measurable time. A busy UI while the song plays (the playlist repainting, keys being played) therefore cost blocks whenever it shared a core with a waiting audio thread.
- **Action:** the engine's threads spin for each other (`devices::spinUntil()` in `devices/core/AudioThreads.h`), never giving the core away: the tasks they wait for are real-time safe and end by the block's deadline (a plug-in's round trip has one), so a wait costs at most that long a spin.

## Next

- **CI:** check the AddressSanitizer and ThreadSanitizer runs after every push that touches playback.
- **Locks:** a RealtimeSanitizer job (Clang 20 or newer, Linux) would measure locks and system calls, not just allocations.
- **Re-run:** run this audit whenever a step touches playback, and after every JUCE update. The guards in the test suite catch regressions in the basic paths.

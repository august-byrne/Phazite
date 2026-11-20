# Real-time audit

*Phazite's own engine, with JUCE 9.0.5; measured on 2026-11-05.* This is [roadmap](roadmap.md) step 0.3. It records:
- how Phazite checks that the audio thread follows the real-time rules in `CLAUDE.md`;
- what is known to break them, in JUCE's code, a plug-in's, or Phazite's own until it was fixed;
- what Phazite does about each finding.

A problem in Phazite's own code is fixed; one in JUCE's is worked around, or accepted where it is harmless. The findings keep the numbers that code and other documents cite them by; the numbers of findings that no longer apply aren't used again.

## How it is measured

- **The scenarios:** [tests/RealtimeAuditTests.cpp](../tests/RealtimeAuditTests.cpp) builds small projects through the model (`tests/TestSong.h`) and plays them in real time through Phazite's engine, as the app plays them. Playback goes through the tests' own audio device (`tests/TestAudioDevice.h`), opened on Phazite's device manager as the app opens a device, on a separate thread that acts as the audio thread.
  - The settings are 48 kHz with 256-sample blocks.
  - The engine plays on four threads, that one and three workers, and the monitor counts what the workers do too; one scenario plays on that thread alone, as a machine with one core does.
  - Like the app, the device opens no audio inputs unless a scenario asks for them; a tone comes into its two inputs.
- **Allocations (Windows Debug, and Linux):** [tests/AudioThreadMonitor.cpp](../tests/AudioThreadMonitor.cpp) counts every `malloc`, `realloc` and `free` on the audio thread and on the engine's workers (Phazite's own audio threads, `src/devices/core/AudioThreads.h`). That includes `operator new` and JUCE's `HeapBlock`. In Windows Debug builds it hooks the debug C runtime and records each one's call stack, which DbgHelp turns into function names. In Linux builds with glibc the test program's own `malloc` and `free` stand in for glibc's, hand each call on to it and record its stack; under AddressSanitizer and ThreadSanitizer their allocator's hooks count, without stacks. Windows Release builds and `windows-asan` count nothing.
- **Every block counts:** each scenario plays a set number of blocks, and every one must allocate and free nothing, from the first, unless the scenario names warm-up blocks with the reason (none does now). The harness plays one block with the song stopped before it measures, for JUCE's one allocation in a device's first callback (finding 3).
- **Edited while it plays:** a scenario can list edits, each made on the message thread at a set block while the audio thread waits outside what is measured: a parameter and a fader turned, a note added, a clip moved, undo and redo; effects added, removed and the instrument replaced; the loop moved and the playhead moved; a track muted and soloed out in the middle of its notes and heard again (its devices play on while it is muted), and a device with latency added and removed, across which the engine keeps the plan before playing while what goes out fades, and a delay grown past its ring, which reads the past from the ring before rather than copy it; and the everyday edits: a send added, switched before the fader and taken away, a track moved into a bus and a track deleted as it plays, whose paths alone fade out and in (a block's work bounded by the block's length, whatever the rings hold), and a track deleted with a latency changed at once, the mix held dipped until the paths that grew read past the leaving paths' fades (2026-11-15, known bugs 78 and 167). None allocates on the audio thread.
- **Memory errors and data races (Linux CI):** the same playback runs under AddressSanitizer and ThreadSanitizer (the `linux-asan` and `linux-tsan` presets). Their suppression files (`tools/lsan-suppressions.txt`, `tools/tsan-suppressions.txt`) are for leaks and races in code Phazite can't change, each explained there and here; they suppress nothing now.
- **A JUCE assertion on the audio thread (Debug builds):** the tests' assertion net (`tests/TestMain.cpp`) notes it there in a slot made before, without a lock or an allocation of its own, and the tests' thread fails the test with its place. JUCE makes the assertion's message, a `juce::String`, before the net hears of it (`juce_core/logging/juce_Logger.cpp`, `logAssertion()`; only replacing JUCE's undocumented `JUCE_LOG_CURRENT_ASSERTION` macro for every unit of `phazite_core`, the app's Debug build too, would avoid them), and on Linux `jassert` then asks whether a debugger is there to stop in (`juce_isRunningUnderDebugger()`, which reads `/proc/self/status` into strings each time: a file read and some 130 allocations): those allocations and frees, in a Debug test build only and on a path that fails the test anyway, are left out of a scenario's counts and call sites (`AudioThreadMonitor::ofAssertions()`, by their stacks), and its report says that an assertion fired instead. Under the sanitizers, whose counts have no stacks, they can't be told apart, and the test fails by the assertion all the same. The last scenario fires one on purpose (known bug 153).
- **Locks, and posting to the message thread, are not measured yet.** Those findings come from reading the code. Clang's RealtimeSanitizer could measure them on Linux later.
- **To run it:**

  ```
  build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
  ```

- **Guards:** 58 of the 62 scenarios (recounted from the file on 2026-11-18, after the EQ's linear phase) must allocate and free nothing on the audio thread once playback is running, or the tests fail: the browser's preview of a loop, which plays from memory (step 2.18), one empty track, the metronome and a recording of the computer keyboard's notes (step 1.7), an audio input recorded onto an armed track that hears it, with notes played live from another thread (step 3.4), and a take's input whose reading thread is a whole buffer behind, noting what it loses (2026-11-10, known bug 12), audio clips read from their files as they play, and audio clips of a file at another rate than the engine's, resampled as they play (r8brain-free-src, [dsp-audit.md](archive/dsp-audit.md)), automation with an LFO, lanes sweeping the EQ a cycle a beat and fading a volume (step 2.2), every kind of modulator and a macro moving the EQ (step 2.3; the engine moves what they modulate every 32 samples), a container of the EQ and the compressor at half mix with its macro and the track's LFO turning them (step 2.4), containers' chains: three bands with an effect in each, a crossover moved by an LFO and a chain's volume by a macro, two layers, one muted and the other a delay, and an instrument container of two synths split by keys (step 4.11), the mix of step 1.4 (a tone through a bus, a send to a reverb return, and the master's loudness meter, built with the functions the mixer uses), a frozen track playing its file (step 2.11), a version's bounce heard in place of the song, with both measured (versions, step V3), and Phazite's own devices (step 1.6: the EQ with every band on and its analyzer listening, the compressor ducked through its sidechain by a muted track (since 2026-11-11 in its Duck mode, and also under a kick track, held and let go along a curve, with Listen on), the limiter on the master, the reverb on a return, the synth playing more notes than it has voices, and a beat on the drum sampler, since step 4.12 with its snare in two layers in turn and its kick on a track of its own; then the devices of step 2.10 in a row, the synth's unison and the filter types of steps 2.14 and 2.15; then, in step 2.17, every effect with its editor's displays listening, and the vocoder at 40 bands with another track as its carrier through the sidechain; then the sampler of step 4.6, a crash chopped into reversed slices that choke each other, an 808 gliding in mono through its filter and a loop crossfading under more notes than voices, with a slice played from its editor; and the FM synth of step 4.7, a stack at full feedback with its LFO on Timbre and a pitch envelope, and four operators side by side, one fixed, gliding in mono), and plug-ins in their helpers (step 3.3): an effect after Phazite's synth, and three synths, each handed its block as the engine's block starts; and, from 2026-11-10 (roadmap step 4.13), the chorus of many voices with the phaser's and the flanger's Safe Bass on (since 2026-11-18 Motion's three modes, its Mode automated), the synth's unison stacked and drifting, the FM synth's filter envelope and its operators' start phases, the sampler's sound made from its spectrum, the exciter (since 2026-11-18 the saturator's Band, its Quality automated), the vinyl (since 2026-11-18 Age, its medium and each part's switch automated) and the reverb as a plate; and the tempo changing along the song (step 4.8). The other scenarios (an audio clip, a loop stretched to the song's tempo, a clip pitched and reversed, and two tracks in a folder bus) only report.

## Results (Windows Debug)

Every scenario plays as the app plays: its song opened as a project, the tests' device calling Phazite's callback on the one audio thread, and Phazite's engine on that thread, with a tone coming into the device's two inputs; the preview plays beside an empty song. The first column covers the warm-up after playback starts (about 0.5 s); the second covers the next second. Measured on 2026-11-10.

| Scenario | Warm-up | Next 1 s |
|---|---|---|
| One empty track | 1 allocation | 0 |
| The metronome, clicking louder on each bar's first beat | 0 | 0 |
| Phazite's engine: its transport and its metronome | 0 | 0 |
| Phazite's engine: chords on Phazite's synth through a track's EQ and fader into the master | 0 | 0 |
| Phazite's engine: audio clips on two tracks, one faded in, read from their files as they play | 0 | 0 |
| Audio clips of a 44.1 kHz file on two tracks, resampled to the 48 kHz engine's rate as they play (r8brain-free-src, 2026-11-06) | 0 | 0 |
| Phazite's engine: chords through a filter whose cutoff is automated, with the volume and the pan automated too | 0 | 0 |
| Phazite's engine: chords and the metronome through tempo changes that ramp, bend, hold and jump (step 4.8) | 0 | 0 |
| Phazite's engine: chords through a filter that an LFO, a note follower and a breakpoint envelope move, the pan and the volume by an envelope follower, steps, a random source and a macro | 0 | 0 |
| Phazite's engine: chords in a bus with a container, sends before and after the fader to returns, a sidechain and the loudness meter | 0 | 0 |
| Phazite's engine: chords through a plug-in in its helper (the tests' stand-in) after Phazite's synth | 0 | 0 |
| Phazite's engine: chords on three plug-in synths in their helpers, each handed its block as the engine's block starts | 0 | 0 |
| Phazite's engine recording a take: an armed track's input, heard and recorded, and notes played live from another thread | 0 | 0 |
| A take's input that falls a whole buffer behind, noting what it loses | 0 | 0 |
| Volume automation and an LFO on pan | 0 | 0 |
| A send's level following its lane and an LFO, the lane edited, redrawn, deleted and undone while it plays (known bug 192, 2026-11-17) | 0 | 0 |
| A tone through a bus, with a send to Phazite's reverb on a return, into the loudness meter | 0 | 0 |
| Phazite's EQ, every band on and one automated, with its analyzer listening | 0 | 0 |
| Phazite's EQ with dynamic bands on the middle, the side and the left, a band's placement and another's Dynamic automated (step S6.4, 2026-11-18) | 0 | 0 |
| Phazite's EQ in Linear phase, a band's Frequency and Gain automated, a band dynamic and one on Mid (step S6.4, 2026-11-18) | 0 | 0 |
| Lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume | 0 | 0 |
| Every kind of modulator, and a macro, moving Phazite's EQ | 0 | 0 |
| A container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them | 0 | 0 |
| Containers' chains: three bands, their crossover modulated, layers muted, and two synths split by keys (2026-11-09) | 0 | 0 |
| Phazite's compressor, ducked by another track at 70 % mix, with its meters and its trail read | 0 | 0 |
| Phazite's compressor ducking under a kick track, held and let go along a curve, Listen on (2026-11-11) | 0 | 0 |
| Phazite's compressor keyed by another track, its mode, look-ahead and hold moved by lanes (2026-11-18, step S6.3) | 0 | 0 |
| A bass ducking under a kick through a linear-phase EQ, its sound waiting for its key (`Plan::Device::inputDelay`, 2026-11-19) | 0 | 0 |
| Edited while it plays: the EQ of a kick that keys a bass's compressor switched to Linear phase, back to Minimum and to Linear again, the bass's wait following (2026-11-19) | 0 | 0 |
| Phazite's multiband, its low band and master keyed by another track through the sidechain, its key cut, Listen on (2026-11-10) | 0 | 0 |
| Phazite's vocoder at 40 bands, its carrier another track through the sidechain | 0 | 0 |
| Phazite's limiter on the master, pushed 12 dB, with its meters read | 0 | 0 |
| Phazite's reverb on a return, with an 8-second decay | 0 | 0 |
| Phazite's reverb as a plate on a return, large and fully diffused, with an 8-second decay (2026-11-10) | 0 | 0 |
| Phazite's reverb on a return as a large hall, its mode moved by a lane through the room, the plate and the hall (2026-11-18) | 0 | 0 |
| MIDI into Phazite's synth, notes overlapping, with noise and its LFO | 0 | 0 |
| Phazite's synth with seven unison voices, spread wide, playing chords a sixteenth apart | 0 | 0 |
| Phazite's synth with eight unison voices, stacked two octaves and drifting (2026-11-10), and two oscillators warped, an envelope sweeping each note's table, LFOs fading in, legato glides, Drift and a slot moving another's amount, the fade and that amount automated (S6.7), and its Quality switched by a lane between 4x, 2x and 1x while chords sound, its filter Driven near singing and its Character switched by a lane every two beats (S6.7) | 0 | 0 |
| Phazite's filter, saturator, multiband, delay, Motion as a phaser, as a flanger and as a chorus (eight voices a side, each in its part of the sweep, with Shimmer, feedback, High Cut and Safe Bass; 2026-11-10; its Mode moved by a lane from mode to mode a beat each, so that its modes fade into each other all the time; 2026-11-18), Stereo (Spread, Bass Mono, Balance, Swap Sides and the DC filter on; 2026-11-18) and limiter in a row on a tone, the phaser's and the flanger's Safe Bass on at a quarter of the mix (half theirs before they were Motion's modes), the filter swept by a lane | 0 | 0 |
| Phazite's saturator on the synth's chords, its Band on (To at 12 kHz), driven 30 dB into a lopsided Tube and lifted 6 dB at half its mix, From swept by a lane from 1 to 8 kHz each beat and its Quality moved by a lane from 8x to 4x and 2x and back a beat each, its editor's scope listening and Listen on for the first half second (2026-11-18, in place of the exciter's of 2026-11-10) | 0 | 0 |
| Phazite's filter on the synth's chords, a low-pass at a Resonance of 97 % driven 18 dB at half its mix, its cutoff swept by a lane from 200 Hz to 4 kHz each beat, its Character switched by a lane every two beats and its Quality moved from 8x to 4x and 2x and back a beat each, its editor's display listening (2026-11-18, roadmap step S6.6) | 0 | 0 |
| Phazite's Age on the synth's chords: a deep wow and flutter, its drive, a head bump, the lows in mono, hiss, dense crackle and dropouts, its medium and each part's switch moved by lanes, its high cut swept by a lane, its editor's pitch, clicks and dropouts read (2026-11-10; Age since 2026-11-18) | 0 | 0 |
| Phazite's delay on the synth's chords: each side its own time, ping-pong, a feedback of 105 %, the modulation and the duck at work, both times swept by lanes, its Sync, Freeze, Character and Transition switched by lanes, its editor's display listening (2026-11-18, step S6.6) | 0 | 0 |
| Phazite's EQ, filter, saturator, compressor, multiband, delay, reverb, Motion and limiter in a row on a tone, every one's editor's displays listening | 0 | 0 |
| Phazite's synth's Comb +, swept by Envelope 2, chords a sixteenth apart, and its filter's Comb - on a tone | 0 | 0 |
| Phazite's synth's Comb +, Envelope 2 opening the cutoff by four octaves at each note, chords a sixteenth apart | 0 | 0 |
| Phazite's synth's Vowel, swept by Envelope 2, chords a sixteenth apart, and its filter's Phaser on a tone | 0 | 0 |
| Phazite's synth's Sample & Hold, swept by an envelope at each note, chords a sixteenth apart | 0 | 0 |
| A beat on Phazite's drum sampler, its hats choking each other, its snare in two layers in turn, its kick on a track of its own, a pad played from its editor | 0 | 0 |
| Phazite's sampler: a crash chopped into reversed slices that choke each other, an 808 gliding in mono through its filter, a loop crossfading under more notes than voices, a seamless sound plucked from random starts with times and a filter that follow the key and the velocity (2026-11-10), a slice played from its editor | 0 | 0 |
| Phazite's FM synth: a stack at full feedback, chords a sixteenth apart, its LFO on Timbre, a pitch envelope, and four operators side by side, one fixed, one fed back toward a square, spread, gliding in mono, its filter opened by its envelope and by velocity on every note and two operators starting from their own phases (2026-11-10) | 0 | 0 |
| A frozen track: its synth's notes played from its frozen file | 0 | 0 |
| A version's bounce heard in place of the song, both measured as they play (Hear It) | 0 | 0 |
| An old version opened live in the song's place while it plays, back to Now, and A/B twice (2026-11-19) | 0 | 0 |
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
- **Action:** every plug-in plays in a helper process of Phazite's (`model/plugins/internal/SandboxedProcessor.h`, since 2026-11-02, step 3.3): Phazite's audio thread takes no lock of a plug-in's; it hands the block to the helper through shared memory and waits for the answer, asleep, until a deadline of the block's own length (a millisecond at least), and the helper's audio thread holds the plug-in's lock as it plays, as any host's does (`src/host/core/Instance.h`). A plug-in that misses the deadline makes its own block late, never the song's. The guarded scenarios with plug-ins play the tests' stand-ins through their helpers, an effect, and three synths each handed its block as the engine's block starts (`devices::Processor::startsEarly()`), and allocate nothing in steady playback. Renders (exports, stems, freezing, bouncing) hand each block to the plug-in's helper too, and wait for it off the audio thread: as long as it takes (a plug-in may only be slow: known bug 42), a little at a time, until its helper has gone, which fails the render, naming the plug-in, or the render is cancelled; once a render has waited as long as a hang takes live, its window names the plug-in, so that the user can cancel it. A plug-in prepared for another rate or a longer block is asked for its channel without waiting (known bug 166), and skipped, as a late one is, until it has come.
- **Waiting for the offline switch (renders only):** a render's first block waits on the helper's thread that plays the plug-in, in sleeps of a millisecond, until the helper's message thread has told the plug-in that it plays offline (JUCE's VST3 host prepares a plug-in only on the message thread; `Instance::run()`, `src/host/core/Instance.cpp`). Only a block that says it is a render's waits so, and a render waits for its plug-ins anyway; a live block never does: the plug-in is told it plays live again as the render gives it back, on the message thread, and failing that a live block plays as the plug-in is while the message thread switches it.

**11. Stretched, pitched and reversed clips** (measured, steps 1.5, 2.6 and 2.18).
- **What:** stretching or pitching audio as it plays would grow the stretcher's buffers on the audio thread, and cost far more than reading a file.
- **Action:** since step 2.18 (2026-10-28) Phazite renders those clips itself, with Rubber Band's R3 engine, on two background threads of its own (`model/arrangement/internal/ClipRenders.h`), and the engine plays each render as a plain clip: only a file read, ahead of where it plays, by the engine's reading thread (`engine/audio/FileReader.h`). The clip is silent until its render is ready, usually well under a second for a loop. The audit's two scenarios play from those renders.

**13. Recording** (measured, step 3.4; through Phazite's engine since 2026-11-02).
- **What:** a take must not lock, allocate or write files on the audio thread, and must not lose its first blocks while the message thread is slow to hear that recording started.
- **Action:** Phazite's engine takes the audio device's inputs in its own callback, plays them through the tracks that hear them, and copies each armed track's input, from the first sample the song plays after the take is given to the engine (after the count-in), into a buffer of its own that one reading thread empties into a file (`engine/core/Recording.h`, `model/transport/internal/Take.h`). The take is made and freed on the message thread and handed to the engine whole; the engine knows the song's sample it started on, so nothing depends on when the message thread hears that recording started. The notes played live come out of the engine with the clock they came in at. The guarded scenario records a tone onto an armed track that hears it, with notes played live from another thread, and allocates nothing once recording runs. Since 2026-11-10 (known bug 12), what an input loses while its reading thread is a whole buffer behind (a drive that stalls) is noted, where and how much, in a queue of a fixed size beside the buffer, which the reader fills with silence so that the take stays in time; a second guarded scenario checks that noting it allocates nothing. Since 2026-11-15 (known bug 38), the take that goes on after a jump is made whole and handed to the engine before the jump is asked for, and waits for the jump by the engine's count of locates taken, an atomic; a block that still plays the take before hands what it records on to it through an atomic pointer, so it starts where the playhead lands, to the sample, with neither thread waiting for the other. A write of a take's file that fails is noted in an atomic flag by the writing thread, never the audio thread, which the message thread's timer reads.

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

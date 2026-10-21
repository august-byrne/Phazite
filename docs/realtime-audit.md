# Real-time audit

*October 2026: Tracktion Engine `bb386171` (3.5.0) with JUCE 8.0.13.* This is [roadmap](roadmap.md) step 0.3. It records:
- how Phazite checks that the audio thread follows the real-time rules in `CLAUDE.md`;
- what Tracktion Engine's code does on the audio thread today;
- what Phazite does about each finding.

The policy is decision 4 in the [design](design.md#25-open-questions):
- try simple workarounds first;
- keep a fork of Tracktion Engine if the workarounds get awkward, or if upstream issues block us;
- report every problem upstream either way.

Replacing a part of the engine outright is a separate decision, made one capability at a time ([design section 23](design.md#23-tracktion-engine-what-it-gives-and-what-we-build)).

## How it is measured

- **The scenarios:** [tests/RealtimeAuditTests.cpp](../tests/RealtimeAuditTests.cpp) builds small projects and plays them in real time. Playback goes through Tracktion's hosted audio device (the way the engine runs inside a plug-in), on a separate thread that acts as the audio thread.
  - The settings are 48 kHz with 256-sample blocks.
  - The engine's worker threads are off in the tests, so all audio processing happens on that one thread.
  - Like the app, the device opens no audio inputs unless a scenario asks for them.
- **Allocations (Windows Debug):** [tests/AudioThreadMonitor.cpp](../tests/AudioThreadMonitor.cpp) hooks the debug C runtime and counts every `malloc`, `realloc` and `free` on the audio thread. That includes `operator new` and JUCE's `HeapBlock`. It also records each one's call stack, which DbgHelp turns into function names.
- **Warm-up, then steady playback:** each scenario plays at least 0.25 s, and on until the audio thread has gone 0.25 s without allocating or freeing, 2 s at most; then it measures one second of steady playback. Tracktion swaps MIDI buffers between its graph's nodes and pools them, so a buffer can be used for the first time, and allocate, some way into playback, depending on when notes arrive: with a fixed 0.25 s warm-up the recording scenario failed now and then (2026-10-16). Something that keeps allocating still shows in the steady second. The recording scenario's warm-up starts once recording has begun and its first note was played, and it starts with a 12-note chord: notes played from another thread can bunch into one block on a busy machine, and the chord makes the arrays bigger than that first (also 2026-10-16).
- **Memory errors and data races (Linux CI):** the same playback runs under AddressSanitizer and ThreadSanitizer (the `linux-asan` and `linux-tsan` presets).
- **Locks, and posting to the message thread, are not measured yet.** Those findings come from reading the engine's source. Clang's RealtimeSanitizer could measure them on Linux later.
- **To run it:**

  ```
  build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
  ```

- **Guards:** 24 scenarios must allocate and free nothing on the audio thread once playback is running, or the tests fail. They read no files, but for the frozen track's, which Phazite made: one empty track, the metronome and a recording of the computer keyboard's notes (step 1.7), automation with an LFO, lanes sweeping the EQ a cycle a beat and fading a volume (step 2.2, automation clips until 2026-10-17), every kind of modulator and a macro moving the EQ (step 2.3), a container of the EQ and the compressor at half mix with its macro and the track's LFO turning them (step 2.4), the mix of step 1.4 (a tone through a bus, a send to a reverb return, and the master's loudness meter, built with the functions the mixer uses), a frozen track playing its file (step 2.11), and Phazite's own devices (step 1.6: the EQ with every band on and its analyzer listening, the compressor ducked through its sidechain by a muted track, the limiter on the master, the reverb on a return, the synth playing more notes than it has voices, and a beat on the drum sampler; then the devices of step 2.10 in a row, the synth's unison, the wavetable synth, and the filter types of steps 2.14 and 2.15; then, in step 2.17, every effect with its editor's displays listening, and the vocoder at 40 bands with another track as its carrier through the sidechain). The other scenarios only report.

## Results (Windows Debug)

The first column covers the warm-up after playback starts (about 0.5 s); the second covers the next second. Measured on 2026-10-16, each scenario added since when it was added, and the two that moved to Phazite's devices on 2026-10-18; the counts vary a little from run to run, with timing and the file cache.

| Scenario | Warm-up | Next 1 s |
|---|---|---|
| One empty track | 3 allocations | 0 |
| The metronome, clicking louder on each bar's first beat | 2 allocations | 0 |
| Volume automation and an LFO on pan | 2 allocations | 0 |
| A tone through a bus, with a send to Phazite's reverb on a return, into the loudness meter | 7 allocations | 0 |
| Phazite's EQ, every band on and one automated, with its analyzer listening | 2 allocations | 0 |
| Lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume | 3 allocations | 0 |
| Every kind of modulator, and a macro, moving Phazite's EQ | 3 allocations | 0 |
| A container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them | 3 allocations | 0 |
| Phazite's compressor, ducked by another track at 70 % mix, with its meters read | 4 allocations | 0 |
| Phazite's vocoder at 40 bands, its carrier another track through the sidechain (step 2.17) | 5 allocations | 0 |
| Phazite's limiter on the master, pushed 12 dB, with its meters read | 2 allocations | 0 |
| Phazite's reverb on a return, with an 8-second decay | 4 allocations | 0 |
| MIDI into Phazite's synth, notes overlapping, with noise and its LFO | 24 allocations (Tracktion's MIDI message arrays, used for the first time as Tracktion passes them between its nodes) | 0 |
| Phazite's synth with seven unison voices, spread wide, playing chords a sixteenth apart (step 2.10) | about 22 allocations (the same) | 0 |
| Phazite's wavetable synth with eight unison voices and two oscillators warped (sync and FM), an envelope sweeping each note's table and an LFO the other's, playing chords a sixteenth apart (step 2.13) | about 25 allocations (the same) | 0 |
| Phazite's filter, saturator, multiband, delay, phaser, flanger, stereo expander and maximizer in a row on a tone, the filter swept by a lane (step 2.10; the phaser, the flanger and the stereo expander since step 2.17) | 3 allocations | 0 |
| Phazite's EQ, filter, saturator, compressor, multiband, delay, reverb, phaser and maximizer in a row on a tone, every one's editor's displays listening to what goes in and out of it (step 2.17) | 3 allocations | 0 |
| Phazite's synth's Comb +, swept by its filter envelope, playing chords a sixteenth apart, and its filter's Comb - on a tone, swept by a lane (step 2.14) | about 27 allocations (Tracktion's MIDI message arrays, as above) | 0 |
| Phazite's wavetable synth's Comb +, swept by an envelope at each note, playing chords a sixteenth apart (step 2.14) | about 24 allocations (the same) | 0 |
| Phazite's synth's Vowel, swept by its filter envelope, playing chords a sixteenth apart, and its filter's Phaser on a tone, swept by a lane (step 2.15) | about 27 allocations (the same) | 0 |
| Phazite's wavetable synth's Sample & Hold, swept by an envelope at each note, playing chords a sixteenth apart (step 2.15) | about 24 allocations (the same) | 0 |
| Notes recorded from the computer keyboard onto Phazite's synth | 20 allocations (the same) | 0 |
| A beat on Phazite's drum sampler, its hats choking each other, a pad played from its editor | 24 allocations (the same) | 0 |
| A frozen track: its synth's notes played from its frozen file (step 2.11) | 1 allocation | 0 |
| An audio clip | 6 allocations, 3 frees (fewer if the file cache hits) | 0 |
| Two tracks in a folder bus with Phazite's compressor | 13 allocations, 6 frees (a file cache miss logs in Debug builds) | 0 |
| One empty track with two audio inputs enabled | 8 allocations, 4 frees | 0 |
| A loop at 120 BPM stretched to 100 BPM, played from its rendered proxy (a loop dropped from the browser) | 12 allocations, 9 frees (from a file cache miss) | 0 |
| An audio clip turned up 6 dB, pitched up 3 semitones and reversed, played from its rendered proxy (as the audio clip editor sets them, step 2.6) | 10 allocations, 6 frees (from a file cache miss) | 0 |
| A loop at 120 BPM previewed at 100 BPM, stretched as it plays (the browser's preview) | 21 allocations, 14 frees | 0 |

Steady playback is clean everywhere. Every scenario allocates something in its first blocks, though.

## Findings and actions

"Measured" means the audit saw it. "Source" means it was found by reading the engine's code (October 2026).

### Allocations

**1. Audio-input buffers** (measured).
- **What:** when an audio input is enabled, its first blocks allocate on the audio thread:
  - the retrospective-record buffer: about 11 MB per stereo input at 48 kHz, for the default 30 seconds;
  - its per-project bookkeeping, a map insert under a spin lock;
  - the input copy buffers.
- **Action:** Phazite opens no inputs by default (step 0.2), so this can't happen yet. Fix it before audio recording (step 3.4):
  - turning the buffer off isn't an option, because MIDI Capture shares its length setting;
  - so the fix is a small engine change (fork) that sizes the buffer when the input is enabled.
- Report upstream.

**2. Level meters** (measured): every meter allocates a small buffer in its first block (`LevelMeasurer::Client::ensureNumChannels`). This happens once per meter, so accept it for now. Report upstream; fix it in the fork if one exists by then.

**3. JUCE's device callback** (measured): `juce::AudioDeviceManager` sizes a temporary buffer on its first callback, once per device. Accept it.

**4. MIDI message arrays** (measured).
- **What:** the arrays that carry MIDI through the playback graph grow on the audio thread when a block carries more events than any block before. This happens in `PluginNode`, `SummingNode`, `CombiningNode`, `TrackMutingNode` and others. `PluginNode` also swaps its array with its output's, and the graph pools those buffers, so an array can carry MIDI for the first time, and allocate, well into playback, depending on when notes arrive (found 2026-10-16, when it made the recording scenario fail now and then; the audit's warm-up now waits for a quiet stretch, and the recording scenario plays a chord first). An app playing live MIDI meets the same: a block with more notes than any before it allocates once. A plug-in with automation, which `PluginNode` plays in pieces of a block, swaps and merges arrays once a piece, so after an instrument it moves them around more: a Filter swept by a lane after a synth playing chords allocated once a second into playback (found 2026-10-18, step 2.14; its scenario plays the Filter on a track of its own).
- **Why it is mostly harmless:** it stops once every pooled array has been used and their capacity covers the busiest block.
- **Action:** re-measure with MIDI-heavy projects when MIDI recording lands (step 1.7); reserving capacity up front needs the fork. Report upstream.

**5. 4OSC** (measured and source): allocates for every note (a `juce::MidiBuffer` per block, the MPE note list) and locks on every block. Phazite doesn't use it: its own synth replaced it in the starter project and the device list (step 1.6, 2026-10-15), and since 2026-10-18 projects that held it aren't supported either (the owner's decision: pre-alpha keeps no backward compatibility), so its audit scenario went.

**6. Audio file reading** (measured and source).
- **What:**
  - every read takes a `juce::ReadWriteLock`, and releasing it can free memory;
  - in Debug builds, a cache miss also writes a log message from the audio thread.
- **Action:**
  - the log is Debug-only, so ignore it;
  - the lock is uncontended unless the cache's own thread is updating its file list: report it upstream and measure in the performance pass (step 3.5).

### Locks and messages

**7. Automation and modulation** (source; the audit saw one post).
- **What:** every change of an automated or modulated parameter calls `triggerAsyncUpdate()` on the audio thread (`AutomatableParameter::setParameterValue`). Posting a message takes a lock and can allocate on Windows. In the app it repeats at the UI's refresh rate for each moving parameter.
- **Action:** there is no simple workaround, because the call is unconditional. It is the first candidate for the fork: replace the post with an atomic flag that a timer polls. Measure its real cost in step 3.5 to decide. Report upstream.

**8. Volume and pan on every track** (source).
- **What:**
  - `VolumeAndPanPlugin` locks a `std::mutex` on every block;
  - tracks inside a folder also lock the folder's plug-in mutex.
- **Why it is mostly harmless:** the locks are uncontended in normal use, because the message thread only takes them when a folder's VCA changes.
- **Action:** report upstream, and fork if contention shows up.

**9. Hosted plug-ins** (source).
- **What:**
  - each plug-in's processing holds a lock that the message thread also takes when it initialises, resets or bypasses the plug-in;
  - every call also takes the crash tracer's global spin lock.
- **Action:** this matches how JUCE itself hosts plug-ins. Watch it during plug-in work (step 2.12) and sandboxing (after v1). Report upstream.

**10. Modulator timing** (source): the Edit takes a lock once per block to update its modulators, and the MIDI tracker modulator takes a spin lock. Report upstream; measure in step 3.5.

**11. Scratch buffers** (source): `AudioScratchBuffer` takes a shared mutex. It allocates when its 8 pooled buffers are busy or a buffer has more than 2 channels. It is used by time-stretching, multi-output plug-ins and sidechains; the guarded scenarios with a sidechain (the compressor ducked by a muted track, the vocoder with a track as its carrier) allocate nothing in steady playback.
  - **Time-stretching, revisited in step 1.5 (2026-10-13):** a loop dropped from the browser follows the song's tempo, and Tracktion plays it from a stretched copy that it renders in the background (a "proxy"), so the audio thread only reads a file: its steady playback allocates nothing (measured). The render starts 600 ms after the clip changes, and the clip is silent until it is ready, usually well under a second for a loop. Since step 2.6 (2026-10-17), the audio clip editor's pitch, Reverse and Follow Tempo play the same way: from a reversed copy of the file, and a pitched or stretched proxy rendered from it (measured: steady playback allocates nothing). The browser's preview stretches as it plays instead (finding 17).

**12. Tempo changes** (source): the tempo map swaps its rebuilt sequence under the audio callback's lock. Report upstream; test with tempo automation later.

**13. Recording** (source): the MIDI and audio recording paths lock, allocate and start timers on the audio thread. Revisit with MIDI recording (step 1.7) and audio recording (step 3.4).

**14. The click track** (source): reads message-thread values on the audio thread without synchronisation. That's a data race rather than a lock. ThreadSanitizer in CI should show it. Report upstream.

**15. The clip launcher** (source): uses spin locks, and its follow actions have unbounded loops. The launcher waits until after v1 (decision 2), so revisit then. Since 2026-10-21 Phazite turns clip slots off (`EngineBehaviour::areClipSlotsEnabled()`), so tracks have none of its nodes.

**22. Live MIDI** (source; found by the performance audit, 2026-10-21, finding R8).
- **What:** a note played live goes through two locks the audio thread takes too: the MIDI input device's (`MidiInputDevice`, taken by the thread that delivers the note) and the live input node's (`MidiInputDeviceNode::incomingMessagesMutex` and `liveMessagesMutex`, taken on the audio thread each block). They are held briefly and contended only while a note arrives as a block is processed.
- **Action:** report upstream, with a lock-free FIFO from the delivering thread to the node as the fix. Watch it in the benchmark of a live note (`tests/Benchmarks.cpp`).

### Built-in effects

**16. Tracktion's built-in effects** (measured and source).
- **Measured** (until 2026-10-18, when its scenario went): the EQ, compressor, reverb and a send allocated nothing in steady playback.
- **Source:** reading the code finds locks or reallocation on the audio thread in:
  - the 4-band EQ;
  - the sampler, which also allocates for every note;
  - chorus and delay;
  - the patch bays and the channel mapper;
  - the Airwindows wrapper.
- **Action:** Phazite ships its own devices (steps 1.6 and 2.10) and uses none of Tracktion's: its device list (`phazite::getDeviceTypes()`) offers Phazite's own and the plug-ins found by scanning. Tracktion's reverb, which reads its parameters atomically and allocates nothing (measured), was offered until Phazite's own reverb replaced it (2026-10-15), and projects that held it played it until 2026-10-18. The phaser, the filter and the compressor read `juce::CachedValue`s on the audio thread, so they stayed out from the start.

**17. Previews** (measured, step 1.5): the browser's preview plays a loop in an Edit of its own, mixed on the same audio thread as the song, and stretches it as it plays (Tracktion's preview Edit uses no proxy). In its first blocks, SoundTouch grows its buffers (`FIFOSampleBuffer::ensureCapacity`, 10 allocations), and a file cache miss logs in Debug builds; then it allocates nothing. A preview started while the song plays could therefore make it glitch.
- **Action:** accept it for now: previews are short, and are mostly started while the song is stopped. Later, prime the stretcher on the message thread before the preview starts, or render the preview ahead as the clips do. Report SoundTouch's growing buffers upstream with finding 11.
- **Signalsmith Stretch instead** (measured 2026-10-17, Release build, a voice-like stereo recording sped up from 120 to 140 BPM): it would take 1.5% of each 512-sample block on average, against SoundTouch's 0.13%, and, with its 150 ms delay, 45% of one block to catch up each time the preview loops or jumps, where SoundTouch takes 1%. Previews stay on SoundTouch even if clips move to Signalsmith ([design section 23](design.md#23-tracktion-engine-what-it-gives-and-what-we-build)).

### Data races

**18. The audio file cache** (measured by ThreadSanitizer in CI, 2026-10-14).
- **What:** each time Tracktion's cache maps an audio file again, its background thread (`CacheMapper`) replaces the file's whole `AudioFileInfo` under the file's lock (`AudioFileCache::CachedFile::createNewReader`). Preparing playback on the message thread reads the sample rate and channel count, and the audio thread reads the length, without that lock. The audit's scenario that plays a dropped loop from a file shows it.
- **Why it is mostly harmless:** for a file that hasn't changed, the values written are the ones already there.
- **Action:** `tools/tsan-suppressions.txt` suppresses it, so that the `linux-tsan` tests still catch new races. Report upstream.

**19. The hosted MIDI input** (measured by ThreadSanitizer in CI, 2026-10-16).
- **What:** Tracktion's hosted audio device, which the tests (and plug-ins built on Tracktion) play through, passes each block's MIDI to the input instances of the Edits playing (`HostedMidiInputDevice::processBlock`) without the lock under which an Edit's playback adds and removes its instances on the message thread (`MidiInputDevice::addInstance`, `removeInstance`). A test that starts or stops playback while the hosted device processes blocks on a thread of its own shows it.
- **Why it is harmless to Phazite:** the app plays through the computer's own audio and MIDI devices, not the hosted one; only the tests use it.
- **Action:** `tools/tsan-suppressions.txt` suppresses it. Report upstream.

**20. The transport's position while the song is stopped** (measured by ThreadSanitizer in CI, 2026-10-17).
- **What:** while the transport is stopped, a plug-in with automation sets its parameters from where the playhead is, in each block (`Plugin::applyToBufferWithAutomation`). It reads that position with `TransportControl::getPosition()`, a `juce::CachedValue` that the message thread writes whenever the playhead moves, without synchronisation. The touch test in `tests/AutomationModelTests.cpp` shows it, now and then: it stops the song while the volume it automated keeps playing. The app does the same when the playhead moves while the song is stopped.
- **Why it is mostly harmless:** the position is an aligned 8-byte value, which x86-64 and ARM64 read and write whole, so the audio thread sees the old position or the new one. At worst, one block applies the automation from where the playhead was.
- **Action:** `tools/tsan-suppressions.txt` suppresses races in `TransportControl::getPosition()`, which Phazite's own code calls on the message thread only. Naming the plug-in function instead would hide every race inside any device's processing. A fork would read the playhead's own position, which the audio thread may read. Report upstream.

**21. Render jobs deleted while their thread lets go of them** (measured by ThreadSanitizer in CI, 2026-10-17).
- **What:** Tracktion makes some files in the background with `RenderManager` jobs on a JUCE thread pool, such as the reversed copy of an audio clip's file that Reverse plays (step 2.6). A finished job posts a message, and on the message thread `RenderManager::Job::handleMessage` drops its last reference as soon as `isRunning()` says the pool is done with it, so `RenderManager` deletes it. But JUCE's `ThreadPool::runNextJob` clears a job's running flag before it writes the job's `shouldStop` flag and `pool` pointer, so the pool's thread can still write to a job that has just been deleted. ThreadSanitizer saw the deletion race with the pool's thread four times in the tests that reverse clips. The race's other stack didn't fit in GitHub's annotation, which keeps 4096 characters: `tools/ci-sanitizer-reports.sh` now shortens reports so that both stacks fit.
- **Why it is rare:** the message must be handled in the moment between those writes, on another thread; if the job still runs then, a timer tries again later. It happens once per render, not during playback.
- **Action:** `tools/tsan-suppressions.txt` suppresses races in `RenderManager::handleAsyncUpdate()`, which only deletes finished jobs; Phazite's code runs on neither side. The fix belongs in Tracktion (delete a job once the pool has let go of it) or JUCE (clear the running flag last). Report upstream.

## Reporting upstream

Tracktion takes bug reports in the [Tracktion Engine category of the JUCE forum](https://forum.juce.com/c/tracktion-engine), not as pull requests. These findings are worth reporting there: 1, 2, 4, 6, 7, 8, 9, 10, 11, 12, 14, 18, 19, 20 and 21. Posting is the owner's call.

## Next

- **CI:** the first ThreadSanitizer run with a clip playing from a file found finding 18 (2026-10-14); AddressSanitizer found nothing. Check both after every push that touches playback.
- **Locks:** a RealtimeSanitizer job (Clang 20 or newer, Linux) would measure locks and system calls, not just allocations.
- **Re-run:** run this audit whenever a step touches playback, and after every Tracktion Engine update. The guards in the test suite catch regressions in the basic paths.

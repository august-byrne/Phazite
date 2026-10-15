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
- **Memory errors and data races (Linux CI):** the same playback runs under AddressSanitizer and ThreadSanitizer (the `linux-asan` and `linux-tsan` presets).
- **Locks, and posting to the message thread, are not measured yet.** Those findings come from reading the engine's source. Clang's RealtimeSanitizer could measure them on Linux later.
- **To run it:**

  ```
  build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
  ```

- **Guards:** nine scenarios read no files and use none of the code Phazite is replacing: one empty track, built-in effects with a send, automation with an LFO, the mix of step 1.4 (a tone through a bus, a send to a reverb return, and the master's loudness meter, built with the functions the mixer uses), and one for each of Phazite's own devices so far (step 1.6: the EQ with every band on and its analyzer listening, the compressor ducked through its sidechain by a muted track, the limiter on the master, the reverb on a return, and the synth playing more notes than it has voices). Once playback is running, they must allocate and free nothing on the audio thread, or the tests fail. The other scenarios only report.

## Results (Windows Debug)

The first column covers the first 0.25 s after playback starts; the second covers the next second.

| Scenario | First 0.25 s | Next 1 s |
|---|---|---|
| One empty track | 3 allocations | 0 |
| Built-in EQ, compressor, and a send to a reverb return | 4 allocations | 0 |
| Volume automation and an LFO on pan | 2 allocations | 0 |
| A tone through a bus, with a send to a reverb return, into the loudness meter | 6 allocations | 0 |
| Phazite's EQ, every band on and one automated, with its analyzer listening | 2 allocations | 0 |
| Phazite's compressor, ducked by another track at 70 % mix, with its meters read | 4 allocations | 0 |
| Phazite's limiter on the master, pushed 12 dB, with its meters read | 2 allocations | 0 |
| Phazite's reverb on a return, with an 8-second decay | 5 allocations | 0 |
| MIDI into Phazite's synth, notes overlapping, with noise and its LFO | 25 allocations (Tracktion's MIDI message arrays growing in their first blocks) | 0 |
| An audio clip | 2 allocations (more if the file cache misses) | 0 |
| Two tracks in a folder bus with a compressor | 7 allocations, 1 free | 0 |
| MIDI into Tracktion's 4OSC synth | 29 allocations, 3 frees | 10 allocations, 8 frees |
| One empty track with two audio inputs enabled | 8 allocations, 4 frees | 0 |
| A loop at 120 BPM stretched to 100 BPM, played from its rendered proxy (a loop dropped from the browser) | 7 allocations, 3 frees (from a file cache miss) | 0 |
| A loop at 120 BPM previewed at 100 BPM, stretched as it plays (the browser's preview) | 22 allocations, 14 frees | 0 |

Steady playback is clean everywhere except in 4OSC. Every scenario allocates something in its first blocks, though.

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
- **What:** the arrays that carry MIDI through the playback graph grow on the audio thread when a block carries more events than any block before. This happens in `PluginNode`, `SummingNode`, `CombiningNode`, `TrackMutingNode` and others.
- **Why it is mostly harmless:** it stops once their capacity covers the busiest block.
- **Action:** re-measure with MIDI-heavy projects when MIDI recording lands (step 1.7); reserving capacity up front needs the fork. Report upstream.

**5. 4OSC** (measured and source): allocates for every note (a `juce::MidiBuffer` per block, the MPE note list) and locks on every block. Don't use it: Phazite's own synth replaces it in the starter project (step 1.6).

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
- **Action:** this matches how JUCE itself hosts plug-ins. Watch it during plug-in work (step 2.10) and sandboxing (after v1). Report upstream.

**10. Modulator timing** (source): the Edit takes a lock once per block to update its modulators, and the MIDI tracker modulator takes a spin lock. Report upstream; measure in step 3.5.

**11. Scratch buffers** (source): `AudioScratchBuffer` takes a shared mutex. It allocates when its 8 pooled buffers are busy or a buffer has more than 2 channels. It is used by time-stretching, multi-output plug-ins and sidechains. Revisit when sidechains (step 2.7) land.
  - **Time-stretching, revisited in step 1.5 (2026-10-13):** a loop dropped from the browser follows the song's tempo, and Tracktion plays it from a stretched copy that it renders in the background (a "proxy"), so the audio thread only reads a file: its steady playback allocates nothing (measured). The render starts 600 ms after the clip changes, and the clip is silent until it is ready, usually well under a second for a loop. The browser's preview stretches as it plays instead (finding 17).

**12. Tempo changes** (source): the tempo map swaps its rebuilt sequence under the audio callback's lock. Report upstream; test with tempo automation later.

**13. Recording** (source): the MIDI and audio recording paths lock, allocate and start timers on the audio thread. Revisit with MIDI recording (step 1.7) and audio recording (step 3.4).

**14. The click track** (source): reads message-thread values on the audio thread without synchronisation. That's a data race rather than a lock. ThreadSanitizer in CI should show it. Report upstream.

**15. The clip launcher** (source): uses spin locks, and its follow actions have unbounded loops. The launcher waits until after v1 (decision 2), so revisit then.

### Built-in effects

**16. Tracktion's built-in effects** (measured and source).
- **Measured:** the scenario with the EQ, compressor, reverb and send allocates nothing in steady playback.
- **Source:** reading the code finds locks or reallocation on the audio thread in:
  - the 4-band EQ;
  - the sampler, which also allocates for every note;
  - chorus and delay;
  - the patch bays and the channel mapper;
  - the Airwindows wrapper.
- **Action:** Phazite ships its own devices (steps 1.6 and 2.8), and Tracktion's stay out of Phazite's device list. The list (`phazite::getDeviceTypes()`) offers Phazite's own and, until Phazite's synth comes in step 1.6, the 4OSC synth, which the starter project uses. Tracktion's reverb, which reads its parameters atomically and allocates nothing (measured), was offered until Phazite's own reverb replaced it (2026-10-15); projects that hold it still play it. The phaser, the filter and the compressor read `juce::CachedValue`s on the audio thread, so they stay out too.

**17. Previews** (measured, step 1.5): the browser's preview plays a loop in an Edit of its own, mixed on the same audio thread as the song, and stretches it as it plays (Tracktion's preview Edit uses no proxy). In its first blocks, SoundTouch grows its buffers (`FIFOSampleBuffer::ensureCapacity`, 10 allocations), and a file cache miss logs in Debug builds; then it allocates nothing. A preview started while the song plays could therefore make it glitch.
- **Action:** accept it for now: previews are short, and are mostly started while the song is stopped. Later, prime the stretcher on the message thread before the preview starts, or render the preview ahead as the clips do. Report SoundTouch's growing buffers upstream with finding 11.

### Data races

**18. The audio file cache** (measured by ThreadSanitizer in CI, 2026-10-14).
- **What:** each time Tracktion's cache maps an audio file again, its background thread (`CacheMapper`) replaces the file's whole `AudioFileInfo` under the file's lock (`AudioFileCache::CachedFile::createNewReader`). Preparing playback on the message thread reads the sample rate and channel count, and the audio thread reads the length, without that lock. The audit's scenario that plays a dropped loop from a file shows it.
- **Why it is mostly harmless:** for a file that hasn't changed, the values written are the ones already there.
- **Action:** `tools/tsan-suppressions.txt` suppresses it, so that the `linux-tsan` tests still catch new races. Report upstream.

## Reporting upstream

Tracktion takes bug reports in the [Tracktion Engine category of the JUCE forum](https://forum.juce.com/c/tracktion-engine), not as pull requests. These findings are worth reporting there: 1, 2, 4, 6, 7, 8, 9, 10, 11, 12, 14 and 18. Posting is the owner's call.

## Next

- **CI:** the first ThreadSanitizer run with a clip playing from a file found finding 18 (2026-10-14); AddressSanitizer found nothing. Check both after every push that touches playback.
- **Locks:** a RealtimeSanitizer job (Clang 20 or newer, Linux) would measure locks and system calls, not just allocations.
- **Re-run:** run this audit whenever a step touches playback, and after every Tracktion Engine update. The guards in the test suite catch regressions in the basic paths.

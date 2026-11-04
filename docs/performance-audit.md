# Performance audit

*2026-10-21, the owner's request as the last part of phase 2: "a lag between when I press a note on the keyboard, and
when I hear the sound", and a thorough audit of Phazite's performance, its unoptimal practices and the chances for
faster and more robust implementations. The owner accepted its plan, which is roadmap step 2.16, and its three
recommendations (section 6) the same day.*

It covers the delay between playing a note and hearing it, the audio thread's work, the UI thread's work, the build,
and robustness. Four code reviews read the code (Phazite's, and the engine's and JUCE's where Phazite relies on them), and
temporary tests measured what the reviews estimated. The real-time rules and what breaks them are in
[realtime-audit.md](realtime-audit.md); this audit doesn't repeat them.

**Where the numbers come from:** the owner's PC (Intel Core i9-9900K, 8 cores and 16 threads, Windows 11), the Release
build, 48 kHz and 480-sample blocks (the owner's settings), one audio thread unless said otherwise. A mid-range laptop
would be about 1.5 to 2 times slower (an estimate). Paint times come from JUCE's software renderer in the tests; the
app draws with Direct2D, which fills pixels differently but walks the same notes and clips.

## 1. The delay between a key and its sound

**The cause is the Bluetooth headphones.** Phazite plays through the owner's Bluetooth headphones (its settings,
`%APPDATA%\Phazite\Phazite.settings`), which Windows lists as a Bluetooth A2DP device. Bluetooth audio adds a buffer of its
own, outside anything Phazite or Windows' audio API can see or change. The Phazite running on 2026-10-21 was the
Release build, so the lag isn't a Debug-build effect.

| Stage | Now (Bluetooth) | Wired, same settings | How we know |
|---|---|---|---|
| A key or a MIDI note reaches the engine | about 1 ms | same | The code: the computer keyboard sends the note as JUCE delivers the key; a busy UI thread delays it (section 3) |
| Waiting for the next audio block | 0 to 10 ms (5 on average) | same | The code: the engine puts live notes at the start of the next block |
| The engine and Phazite's instrument | 0.04 ms | same | Measured: the synth is heard 0.04 ms into the block (-60 dB), 0.12 ms (-30 dB) |
| Delay compensation | 0 in a new project | same | Measured: +2.06 ms for each Limiter or Maximizer anywhere in the song, plus any plug-in's own latency |
| Windows' shared-mode output (480 samples) | about 15 to 35 ms | same | JUCE keeps one device buffer queued, plus Windows' audio engine (an estimate) |
| The Bluetooth link | about 150 to 250 ms | none | Published measurements (RTINGS gives 173 ms for SBC, from search results: not checked on their page). Windows can't use the headphones' LDAC |
| **Total** | **about 170 to 300 ms** | **about 20 to 50 ms** | |

With a wired output and a smaller buffer ("Windows Audio (Low Latency Mode)", or the Realtek ASIO driver this PC has,
at 128 to 256 samples) it would be about 8 to 15 ms, which feels immediate (an estimate, to be measured).

**What the owner can do today:** plug the headphones' AUX cable into the PC's headphone jack and choose "Speakers /
Headphones (Realtek Audio)" in **Options > Audio Settings**. Then try "Windows Audio (Low Latency Mode)" there.

**What Phazite can do** (plan, part C): show each output's real latency, say when an output is Bluetooth, pick a
low-latency setup on first run, and keep look-ahead devices from delaying what you play.

## 2. The audio thread

### What the devices cost

Each device processed 20 s of stereo audio in 480-sample blocks, at its default settings unless said; instruments
held their notes. The percentage is of one core's time (100% means it can't keep up).

| Device | Release | Debug | Latency |
|---|---|---|---|
| Synth: 1 note / 8 notes / 16 notes with 7-voice unison / idle | 0.14 / 0.88 / 6.4 / 0.03 % | 0.51 / 3.2 / 21 / 0.11 % | 0 |
| Wavetable: 1 note / 8 notes / 8 notes with full unison / idle | 0.21 / 1.6 / 6.0 / 0.01 % | 0.95 / 7.3 / 27 / 0.06 % | 0 |
| Drum Sampler: 4 pads every 16th note / idle | 0.90 / 0.001 % | 4.2 / 0.006 % | 0 |
| EQ | 0.02 % | 0.16 % | 0 |
| Filter | 0.08 % | 0.47 % | 0 |
| Compressor | 0.24 % | 0.48 % | 0 |
| Multiband | 0.80 % | 3.1 % | 0 |
| Limiter | 0.24 % | 4.5 % | 99 samples (2.06 ms) |
| Maximizer | 0.74 % | 6.8 % | 99 samples (2.06 ms) |
| Saturator | 0.38 % | 2.2 % | 4 samples |
| Delay | 0.12 % | 0.48 % | 0 |
| Reverb | 0.66 % (0.75 % with silence in) | 4.2 % | 0 |
| Utility | 0.04 % | 0.04 % | 0 |

A whole project, through the engine's playback graph on one thread: 20 empty tracks take 0.5 % of a core; 20 tracks
each with a synth, an EQ, a compressor and a reverb take **17 % while stopped and silent** and 29 % while playing
chords. Notes are placed on their exact sample, every `process()` flushes denormals, and the guarded scenarios of the
real-time audit allocate nothing.

### Findings

- **A1. Devices work at full cost on silence (high).** The engine keeps the playback graph running while stopped, so
  that live notes play, and calls every enabled device every block; no device checks for silence. That is the 17 % of
  a stopped project above. A device could sleep once its input has been silent for longer than its latency and tail,
  and wake at the first sound, note or change.
- **A2. 15 worker threads at time-critical priority (medium).** Phazite asks for one audio thread per logical CPU:
  16 here, so 15 workers that the engine wakes every block, even
  while stopped, and that spin before they sleep. Physical cores minus one is the usual choice. The tests run one
  thread, so this path is unmeasured: compare idle CPU in Task Manager before and after.
- **A3. Graphs nobody hears (low).** The browser's preview keeps its playback graph after the first preview (a synth
  for MIDI files), processed every block for the rest of the session (`Preview.cpp`). The engine's clip launcher
  (on by default) adds nodes to every track, though Phazite has no launcher. The Microsoft GS
  Wavetable Synth is enabled as a MIDI output and opened, though nothing plays it.
- **A4. Control-rate maths in the instruments (medium).** Every 16 samples each voice computes 6 to 14 `pow`, `tan`
  and `cos` calls, many with unchanged inputs: the gain of filter shapes that don't use it, constant Butterworth Qs,
  unison detune ratios, pan trigonometry with nothing modulating it (`Synth.cpp`, `WavetableSynth.cpp`,
  `Filters.cpp`, `MultimodeFilter.cpp`). Estimated 30 to 50 % of a voice's cost.
- **A5. Per-sample maths in the effects (medium-low).** The compressor, multiband and maximizer compute `log10` and
  `pow` on every sample (3 of each in the last two), and the maximizer divides per band per sample; the reverb does 9
  divisions and 24 `floor` calls per sample, 8 of the divisions constant over the block; the limiter does integer
  modulo and a division per sample; the drum sampler interpolates mono sounds twice; the saturator runs its tone
  filters at Tone 0; the loudness meter on the master computes 4x true peak always. Each is small; together they are
  most of the effects' cost.
- **A6. The wavetable's inner loop (medium; heavy at full unison).** Per sample, per unison voice and per oscillator
  it checks the warp, reads the table and checks FM and ring modulation. Working oscillator by oscillator over each
  16-sample step, with the warp chosen once, and SIMD across unison voices would cut its worst case.
- **A7. Automation splits devices into 128-sample pieces (medium-low).** The engine processes any automated or
  modulated plug-in in 128-sample sub-blocks, each taking a mutex and posting a UI update; Phazite's synths read their
  parameters once per piece and don't smooth them, so automated cutoffs step every 2.7 ms (zipper noise with
  resonance). Smoothing in the devices would fix the sound, and would let Phazite's own devices skip the sub-blocks.
- **A8. Timing bugs.**
  - *The wavetable synth's modulation runs fast* (verified). Its envelopes 2 and 3 and its LFOs step once per
    16-sample control step even when a step is cut short by a block's end or a note, so they run 1.6 % fast at
    441-sample blocks, faster with many notes, and tempo-synced LFOs drift off the beat. The synth's glide has the
    same pattern.
  - *A switched-off look-ahead device keeps its latency* (verified). The engine counts a device's latency in delay
    compensation even while it is off, but passes the sound through undelayed: that track plays 2 ms early and every
    other track waits 2 ms for nothing.
  - *Voice stealing jumps* to the new note at full level, which can click; a 1 to 3 ms fade avoids it.
- **A9. The build.** Debug runs the devices 3 to 20 times slower (table). The README tells the owner to run Debug,
  F5 builds whichever preset is active, and there is no optimised build with symbols for profiling. Release uses
  `/O2` without link-time optimisation, for plain SSE2: link-time optimisation and an AVX2 baseline aren't worth it
  yet (AVX2 would need a check of the CPU at startup, and changes the golden renders).

## 3. The UI thread

The computer keyboard's notes go through the UI thread, so anything that keeps it busy delays them; the rest makes
the window less smooth. Measured on a 16-track song with 256 clips and 16,384 notes:

| What | Time on the UI thread |
|---|---|
| One parameter change (a knob or fader drag makes one per mouse move) | 5.5 ms |
| The check for unsaved changes, once a second after any change | 12.7 ms |
| Save | 114 ms (77 to 100 ms for a 40-track song; its autosave 56 ms) |
| Opening a 40-track song of Phazite's own devices | 61 ms (third-party plug-ins take longer to load) |
| The whole window painted | 13.7 ms |
| The strip a moving playhead repaints, 60 times a second | 1.4 ms (the piano roll's note grid 0.7 ms) |

### Findings

- **U1. Parameter changes rebuild the views (medium).** A device's parameter value is a project property, so every
  change counts as a "devices" change: the playlist re-reads every clip and its notes and lays them out again, the
  piano roll and the Devices view repaint, and the routing view rebuilds even when hidden (`ProjectContext.cpp`,
  `Playlist.cpp`). Every view that shows a value already reads it on a timer.
- **U2. Saving on the UI thread (medium).** Save and the 2-minute autosave ask every plug-in for its state, build the
  XML of the whole project and write it, all on the UI thread; with big sample-based plug-ins that can take hundreds
  of milliseconds, while the window and the computer keyboard wait. The plug-ins must be asked on the UI thread, but
  the XML and the file can be done on a background thread from a copy, and autosave can wait while playing or
  recording.
- **U3. The unsaved-changes check compares the whole project (medium-low).** The title's 1 Hz timer compares the
  project with its saved copy after any change notification (12.7 ms here, growing with the song).
- **U4. Edits rebuild the playback graph (medium, to measure).** The engine rebuilds the graph on the UI thread for
  note, clip, tempo and device edits; a note drag rebuilds at every grid step and a velocity drag at every mouse move.
  The engine can hold the rebuild until the drag ends.
- **U5. Painting and timers (low now, grows with songs).** The piano roll re-reads, colours and checks the selection
  of every shown note at every repaint, and its velocity lane draws every note; the playhead repaints at 60 Hz in
  editors that aren't showing; meters, mixer strips and device editors keep their timers running when hidden; the EQ
  and compressor graphs repaint 30 times a second whether or not anything changed; controls look their parameter up
  by name three or four times per tick.
- **U6. A quick key tap can be lost (medium, correctness; verified).** The computer keyboard reads the key's state
  when the message is handled, not when the key went down (`GetAsyncKeyState`): if the UI thread was busy for longer
  than the tap, the key is already up and no note plays. Windows' `GetKeyState` gives the state as of the message.
- **U7. Smaller.** The engine rescans the MIDI devices every 4 s on the UI thread (to measure); the browser searches up
  to 50,000 items on every keystroke on the UI thread; over a plug-in window, every key is first looked up through
  every command.

## 4. Robustness

- **R1. An untitled song's recordings can be deleted (high, data loss; verified).** At start-up Phazite deletes the
  folder of every untitled project without an autosave (`UnsavedProjects.cpp`), and the first autosave comes 2
  minutes after opening: record into a new song, crash within 2 minutes, and the recording is gone.
- **R2. A plug-in that crashes on load crashes every start (high).** Phazite reopens the last project on its own, with
  no record that the last start didn't finish.
- **R3. A failed audio device is silent (high).** If the output can't open, or disappears (unplugged, or Bluetooth
  disconnected), Play makes no sound and nothing says why.
- **R4. The window shows before the project loads (medium).** Opening a heavy project at start-up leaves a blank or
  "Not responding" window.
- **R5. A plug-in that hangs while scanning stalls the scan (medium).** The scan's child process answers pings while
  stuck, so it never times out; the next scan hangs on the same plug-in.
- **R6. Two Phazites can open the same project (low-medium).** Both autosave to the same file, and the first to close
  deletes it.
- **R7. Failures leave no trace (low).** There is no log file, and a failing autosave retries silently.
- **R8. Locks on the live MIDI path (low; the engine's).** The audio thread and the thread delivering a live note share
  two locks: a candidate for realtime-audit.md (finding 22).

Already fine: saves are atomic (a temporary file, flushed, then swapped in), memory is bounded (30 undo steps, a
capped library index), the background threads lock correctly, and plug-in scanning survives crashes (out of process).

## 5. The plan (roadmap step 2.16)

In order, each part a commit or a few, tested and measured before and after. Sizes: S a day or less, M a few days.

**A. Measuring (S).** The temporary tests become a benchmark suite (`--test-suite=benchmark`, not run by ctest or CI)
that prints the tables above, recorded here after each part. A `windows-profile` preset (optimised, with symbols) for
profiling and everyday playing, which the README and F5 recommend instead of Debug.
- *Done when:* one command prints every table in this document, and F5 can run the profile build.

**B. Correctness first (S each).** Untitled recordings survive a crash (R1); quick key taps always play (U6); a
switched-off look-ahead device drops its latency (A8); the wavetable's modulation and the glides keep time (A8); voice
stealing fades (A8).
- *Done when:* each has a test that failed before.

**C. Latency you can see and choose (M).** The audio settings show the buffer size and the output's latency in
milliseconds, without the "advanced" button; Bluetooth outputs are recognised, and the settings and the welcome page
say what they cost; on first run Phazite picks Low Latency Mode at 128 to 256 samples where the device offers it
(shared mode otherwise), or ASIO for an interface; the settings show the song's delay compensation.
- *Done when:* with a wired output on this PC, a key to its sound measures under 15 ms (a phone recording of the key's
  click and the sound), and every number above shows in the settings.

**D. Idle and audio CPU (M).** Devices sleep on silence (A1) and the loudness meter works only when needed; worker
threads are physical cores minus one (A2); the preview's graph, the clip launcher's nodes and the GS Wavetable output
go (A3); the instruments' control-rate maths, the effects' per-sample maths and the wavetable's inner loop are
reworked (A4 to A6); the synths smooth their parameters (A7).
- *Done when:* the stopped 20-track song costs under 2 % of a core (17 % now), the worst-case synth and wavetable
  cases cost at least 30 % less, the real-time audit stays clean, and the golden renders change only where expected
  (with the reasons in the commit).

**E. The UI thread (M).** Parameter changes stop rebuilding the views (U1); saving builds and writes the file on a
background thread, and autosave waits while playing or recording (U2); the unsaved-changes check stops comparing the
whole song (U3); drags rebuild the playback graph once, when they end (U4); hidden views stop their timers, the
playhead runs only while playing and showing, and the piano roll keeps its notes between repaints (U5); browser
searches and the MIDI rescans move off the hot path (U7).
- *Done when:* on the benchmark song a parameter change takes under 0.5 ms (5.5 now), saving blocks the UI thread for
  under 15 ms, and the unsaved-changes check under 1 ms.

**F. Robustness (M).** After a crash Phazite doesn't reopen the project on its own, and offers to open it with its
plug-ins off (R2); "No audio output" shows when the device fails, with a button to the audio settings (R3); the window
paints "Opening…" before the project loads (R4); plug-in scans time out per plug-in (R5); a second Phazite brings the
first forward instead of opening the project again (R6); a log file, and a message after repeated autosave failures
(R7); the live-MIDI locks go into realtime-audit.md (R8).
- *Done when:* each has a test, or a check by hand where it needs a real device or a crashing plug-in.

**Not now:** link-time optimisation and an AVX2 baseline (measure in the profile build first: done 2026-10-27, section 8,
and neither helps), `/fp:fast` (changes the golden renders and makes MSVC and GCC differ).

## 6. Decisions for the owner

1. **The first-run audio setup:** Low Latency Mode at 128 to 256 samples where the device offers it, falling back to
   today's shared mode (recommended), or keep today's shared mode with the device's own buffer.
2. **A "Low latency" switch** in the top bar, as Cubase and Studio One have: while on, the buffer shrinks and devices
   with look-ahead are switched off, so that playing live has no delay compensation. Recommended, off at first;
   or none.
3. **Bluetooth outputs:** say what they cost in the settings and on the welcome page (recommended), or also offer to
   switch to a wired output when there is one.

## 7. After the plan (2026-10-21)

Measured again with `phazite_tests.exe --test-suite=benchmark --no-skip` from the `windows-profile` build, on the same PC.

| What | Before | After | Done when |
|---|---|---|---|
| A stopped 20-track song (a synth, an EQ, a compressor and a reverb each) | 17 % of a core | 1.96 % | under 2 % |
| The same, playing chords | 29 % | 26 % | |
| Synth: 16 notes with 7-voice unison | 6.4 % | 4.55 % (-29 %) | 30 % less |
| Wavetable: 8 notes, full unison | 6.0 % | 3.50 % (-42 %) | 30 % less |
| Compressor | 0.24 % | 0.19 % | |
| A parameter's change, as the views handle it | 5.5 ms | 0.02 ms | under 0.5 ms |
| The unsaved-changes check after a notification | 12.7 ms | 0.78 ms | under 1 ms |
| Autosave, on the UI thread | 56 ms | 5.4 ms | under 15 ms |
| Save (16 tracks, 16,384 notes) | 114 ms | 115 ms | |
| The whole window painted | 13.7 ms | 11.3 ms | |
| A note moved a step, with the views and the playback graph's rebuild | not measured | 2.8 ms | |

What each part became (roadmap step 2.16 has the files):
- **C** as planned, except that a first run doesn't pick ASIO by itself (design.md, section 20, says why); the Audio
  settings suggest it when drivers are installed. The key-to-sound check with a wired output is left for the owner.
- **D:** devices sleep on silence (A1); the loudness meter rests after 3.5 s of silence; worker threads, the preview's
  graph, the clip launcher and MIDI outputs (A2, A3); the instruments' unison ratios and filter coefficients, and the
  wavetable's inner loop a step at a time without FM (A4, A6); every device glides its continuous parameters over each
  block (A7). Of A5, only the compressor's maths changed: the multiband's and maximizer's need approximations of `log10`
  and `pow` that would change their sound, for under 1 % of a core each that sleeping already saves on silence. The engine's
  128-sample automation pieces stay: with Phazite's devices reading values once a block they would lag a block.
- **E:** parameter changes (U1), autosave on a background thread that waits while the song plays (U2; an explicit Save
  still writes on the UI thread, which the user waits for anyway), the unsaved-changes check (U3), hidden editors, meters
  and strips (U5), controls' parameter lookups (U5) and slow browser searches (U7). A drag's rebuilds (U4) cost 2.8 ms a
  step, so they stay as they are. The EQ's and compressor's repaints and the piano roll's note cache go with the devices'
  new editors and the piano roll's next work; the 4-second MIDI rescan and keys over plug-in windows are left as they are.
- **F** as planned. A plug-in that crashes as it loads can't be tested without one: check by hand with a crashing VST3.
- It found known bug 3, taken then for two Limiter-type devices silencing live notes: in fact any live note could be
  lost after the UI thread was busy, whatever the devices (fixed 2026-10-28; since Phazite's own engine plays live notes,
  `tests/LiveInputTests.cpp` checks that every one sounds however long the message thread was busy).

## 8. An outside review (2026-10-27)

*The owner asked Claude on the web for "the most impactful changes I could make to Phazite in order to improve its audio
and video performance", then asked for its findings to be checked and fixed, together with what this audit had left. The
review read `main`, which didn't have parts A to F yet, so three of its points were done already.*

| The review said | Checked against `phase-2-polish` | What changed |
|---|---|---|
| Every clip is a component, and every scroll and zoom moves them all; rows are found by searching; rebuilding the clips is quadratic, and runs at every change of a clip | True, and worse than it says: rebuilding read every MIDI clip's notes and repainted every clip, at each step of a drag | Components only for the clips in view, made as they come into view (`Playlist::layOutClips()`); rows found by their tracks; the clips listed again in linear time, their notes read only for clips in view; hit-testing and selection by the list, not the components |
| Meters repaint 30 times a second in silence | True: a level equal to the peak counted as a new peak | A meter repaints only when what it draws moves (`LevelMeter::tick()`) |
| Nothing is opaque | True | 29 components that fill themselves in opaque colours say so |
| The playhead moves on a 60 Hz timer; each meter has a timer | True | While playing, the playhead moves at each frame of the display (`juce::VBlankAttachment` in `TimelineEditor`), a 5 Hz timer otherwise; meters tick with the device editors' displays, from one timer (`ui::DisplayTicker`, moved down to `ui/components/`) |
| Cache waveform images | Partly: JUCE's thumbnail keeps its levels per pixel for one area and draws only what is painted of it, but the playlist gave it the part being painted, so each scroll step and each strip under the playhead read the levels again | Waveforms are drawn over the part of the clip in view, which stays the same between paints (`src/app/ClipWaveforms.cpp`); the audio clip editor draws its trimmed and kept audio from one span, through clip regions, rather than three that took turns in the thumbnail's one cache. No images: one per clip would take a few hundred kilobytes to megabytes each, made again at each zoom |
| Plug-in sandboxing needs shared memory, one wake-up per block and MMCSS | Not built yet (roadmap step 3.3) | Written into step 3.3, for when it is |
| Audio threads default to every logical CPU | Not on this branch: physical cores minus one since part D (A2) | Nothing |
| Bypassed plug-ins stay in the graph | Not on this branch: they leave it since part B (A8) | Nothing |
| Link-time optimisation and AVX2 would speed up Phazite's devices; processing unison voices with SIMD is the usual large win | Measured (below): neither speeds them up | Neither turned on. SIMD across the wavetable's unison voices is left for later: each voice reads its own table at its own place, eight reads a sample that SSE2 can't gather, so only the arithmetic around them would go four at a time, for at most about a percent of a core in the worst case measured |
| No benchmarks of audio CPU or the UI's frame times | Not on this branch (part A), but none of scrolling or zooming | A benchmark of scrolling and zooming a 2,400-clip song, and tests that fail if the playlist, the meters or the device editors go back to their old ways (`tests/PlaylistTests.cpp`, `tests/ControlsTests.cpp`, `tests/DeviceEditorTests.cpp`); timings stay out of CI, which runs on machines of every speed |

Found while checking:
- **Switching the mouse pointer.** JUCE keeps a standard pointer only while something holds it, so the playlist moving
  between a clip's halves (one selects a range, the other moves the clip) made the hand again from its image at each
  switch: 150 to 200 ms in the tests' window, which an ordinary scroll set off whenever the pointer came to rest on a
  clip's lower half. Phazite's look and feel now keeps every standard pointer for the session.
- **The rest of U5.** Every device editor's picture repainted at each of the ticker's 30 ticks a second; the EQ switched
  its slope and gain controls on and off again at each tick; spectra analysed the same samples again at each tick in
  silence; and the multiband's, maximizer's, delay's, saturator's and vocoder's levels fell forever without reaching
  rest. Each picture now gathers the numbers it draws and repaints when they change (`ui::DrawnNumbers`), and a spectrum
  at rest skips its analysis (`devices::Spectrum::update()`).
- **The piano roll's note cache (U5).** It read every note, its colour and whether it was selected at each repaint,
  the playhead's strip included; it now keeps them until the song (`ProjectContext::getSongVersion()`, which changes at
  once with each change), the clips shown or the selection change, and keeps the other tracks' ghost notes the same way.

Measured as section 7 was, with `phazite_tests.exe --test-suite=benchmark --no-skip` from the `windows-profile` build,
before and after; the big song is the new benchmark's: 100 tracks, 2,400 clips and 38,400 notes, in the whole window.

| What | Before | After |
|---|---|---|
| A wheel notch scrolling the big song's tracks: the UI thread's work | 43 ms | 0.23 ms |
| ... scrolling it in time, or zooming | 0.7 to 1.1 ms | 0.13 ms |
| The mouse moving from one half of a clip to the other (another pointer) | 150 to 200 ms at each switch | 0.11 ms |
| A clip moved a beat, as each step of a drag moves it | 385 ms | 1.5 ms |
| Clip components in the playlist | 2,402 | 8, those in view |
| The big song's playlist painted | 2.4 ms | 2.0 ms |
| An 11-px column of the window, as the playhead repaints it (section 7's song) | 0.85 ms | 0.66 ms |
| ... of the piano roll | 0.50 ms | 0.13 ms |
| The whole window painted | 11.3 ms | 11.0 ms |
| A silent level meter, a device editor with nothing playing and nothing turned | 30 repaints a second | none |

The audio benchmarks didn't change, as nothing on the audio thread did. The playhead's smoothness at each frame of the
display, and the CPU of a window at rest, are for the owner to see in the app (Task Manager, with the mixer showing and
the song stopped).

**Link-time optimisation and AVX2**, each in a build of its own made from the `windows-profile` preset (with
`CMAKE_INTERPROCEDURAL_OPTIMIZATION`, and with `/arch:AVX2`), each benchmark run twice beside the plain build:
- *Link-time optimisation* sped up no device and slowed several: the vocoder from 3.2 to 4.5 % of a core, the maximizer
  from 0.71 to 0.79 %, the compressor from 0.20 to 0.25 %; the synths and the projects were within the runs' noise (about
  5 %). Its link of the tests took about 15 GB of memory and many minutes, at every change. Not worth it.
- *AVX2*: every device within the noise of plain SSE2 (the synth's worst case 4.77 against 4.96 % of a core, the
  wavetable's 3.71 against 3.73 %, the vocoder's 3.30 against 3.22 %): MSVC finds little to vectorise in loops that work
  a sample at a time, and the devices that do work on blocks are cheap already. It would cost the computers without AVX2
  (Intel's Pentium and Celeron chips of up to about 2020) a start that crashes, and on GCC fused multiply-adds would
  change the golden renders. Not worth it either; section 5's "measure first" is answered.

## 9. Phazite's own engine (2026-11-02)

Phazite's own engine ([roadmap](roadmap.md#architecture-track), A6, phase T2) plays every project since T2's step 10.
The benchmarks of a project and of a live note now play as the app plays: a project opened in a project context, and
Phazite's engine playing all of it. Measured with `phazite_tests.exe --test-suite=benchmark --no-skip` from the
`windows-profile` build, on the same PC, before step 10 (the engine Phazite played on until then) and after.

| What | The earlier engine | Phazite's engine, one thread | Phazite's engine, four threads |
|---|---|---|---|
| 20 empty tracks, stopped | 0.96 % of the audio thread | 0.33 % | 0.33 % |
| 20 empty tracks, playing | 0.50 % | 0.33 % | 0.33 % |
| 20 tracks with a synth, an EQ, a compressor and a reverb each, stopped | 2.53 % | 0.86 % | 0.50 % |
| The same, playing chords | 29.35 % | 27.67 % | 8.18 % |

A live note is heard as soon as it was with the earlier engine once it is in a block, but its place in the block is new:
that engine put every live note at the start of the next block (heard 2 samples in), so that a note's delay was anything from none to
a whole block, as it came; Phazite's engine plays it as far into the next block as it came after the last one started
(T2 step 2), so that every note waits exactly a block and keeps its timing against the others, as Ableton Live and REAPER
time what they record. The benchmark, which plays its note about 1.5 ms after a block, hears it 71 samples into the next
(1.48 ms at 48 kHz), and the delay compensation of a limiter on another track (+98 samples) and on the master too (+99)
as before. With 256-sample blocks at 48 kHz a live note waits a constant 5.3 ms, where it waited 0 to 5.3 ms before.

**The engine's workers between blocks (2026-11-02, found by T3's measurement).** Each of the engine's workers (T2 step 8)
spun through 4000 yields for its next job before it slept, and at 128 to 480 samples the next block came before it slept:
every worker was busy between blocks, taking CPU from plug-ins and their helpers, and a laptop's battery. The benchmark
of a project now also measures the whole process's CPU while blocks come one each block's length, as a device brings
them (480 samples at 48 kHz). A worker now spins for 50 microseconds at most, long enough to keep up with a render's
blocks, played back to back, then sleeps until the next job; the audio thread's share is as it was:

| 20 tracks, four threads | The audio thread, playing | The process's CPU, before | After |
|---|---|---|---|
| Empty | 0.31 % | 53.5 % of a core | 7.4 % |
| A synth, an EQ, a compressor and a reverb each, chords | 8.14 % | 91.4 % | 39.4 % (30.8 % on one thread) |

## 10. Plug-ins in helpers (2026-11-02)

T3 step 0's measurement ([roadmap](roadmap.md#architecture-track), A6, and step 3.3): a plug-in played in a
helper process, a block at a time at 128 samples (2.67 ms at 48 kHz), one each block's length as a device brings them,
against the same plug-in in Phazite's process, from `tests/SandboxBenchmarks.cpp` (`--test-suite=benchmark --no-skip`,
`windows-profile`, the same PC). Each block's time is from handing it over to having it back; the memory is the
helper's private working set (what of it is in memory and no other process's: its private bytes are about 56 MB more,
which OLE, started by JUCE on Windows, commits and never touches).

| One plug-in | In Phazite's process (median / 99th % / worst) | In a helper, asleep at once | The thread that waits | The helper |
|---|---|---|---|---|
| The tests' stand-in effect (a gain) | 1.4 / 3.0 / 156 us | 29.6 / 89.7 / 594 us | 1.6 % of a core | 1.6 MB empty |
| Surge XT Effects | 19.1 / 74 / 262 us | 47.8 / 133 / 573 us | 0.9 % | 16.6 MB with it |
| Surge XT, a chord held | 74.6 / 192 / 1013 us | 104.4 / 248 / 996 us | 1.0 % (3.4 % in the helper) | 38.9 MB with it |

A round trip costs about 28 microseconds a block (a wake-up each way), about 1 % of a 128-sample block for each plug-in
in a chain. Waiting asleep at once was both the quickest and the cheapest: Windows runs the helper's woken thread on the
core the waiting thread gives up, warm; spinning 30 microseconds first took 57.9 us for the stand-in, spinning until it
answered 44.6 us and twice the CPU.

| Eight Surge XTs, a chord each | Median | 99th % | Worst | CPU | Memory |
|---|---|---|---|---|---|
| Phazite's process, one after another | 464 us | 639 us | 3.3 ms | 17.9 % of a core | |
| Phazite's process, on the engine's workers | 237 us | 397 us | 3.5 ms | 68 % (the whole process) | |
| A helper each, each waited for in turn | 451 us | 717 us | 7.8 ms | 1.0 % here, 17.9 % in the helpers | 311 MB |
| A helper each, all handed over first, then waited for | 253 us | 360 us | 1.7 ms | 2.3 %, 54 % | 310 MB |
| One helper with all eight, handed over first | 236 us | 337 us | 1.3 ms | 1.9 %, 45 % | 232 MB |

Played side by side, as the engine plays tracks, sandboxed plug-ins cost no more than plug-ins in Phazite's process:
the same median, a lower 99th percentile and worst case, and less CPU. (Eight synths side by side take three to four
times the CPU of eight one after another, sandboxed or not: the cores' second threads, lower boost clocks and shared
caches.) A helper for each instance costs about 10 MB more for each of them, for Surge XT.

**Decided from these numbers (Claude, as the owner asked, 2026-11-02; [design section 19](design.md#19-plug-in-hosting-and-stability)):** sandboxing on,
and the only way Phazite hosts plug-ins (its cost is small: decision 7's condition); one helper for each plug-in, every
instance of it in that helper with an audio thread of its own, as Bitwig's default ("By plug-in"), so that a plug-in that
crashes takes only its own instances with it, at a fraction of a helper for each instance's memory; Phazite's audio
thread waits asleep at once; and the engine hands each plug-in instrument's block over as the block starts, so that the
instruments of every track play side by side.

**In a project (2026-11-02, T3 step 9):** the benchmark of a project's CPU (`tests/Benchmarks.cpp`, `windows-profile`,
480 samples, 10 ms at 48 kHz) plays 20 tracks, each the tests' stand-in synth in a helper (one helper, an audio thread
for each instance) playing chords. Each block's time on Phazite's engine, as a share of the block, and the process's CPU
while it plays (Phazite's process alone, not the helper's):

| 20 plug-in synths in a helper | Threads | Stopped | Playing | The process's CPU playing |
|---|---|---|---|---|
| Each handed its block as its track played | 1 | 3.47 % | 6.34 % | 5.1 % of a core |
| | 4 | 1.24 % | 1.80 % | 5.9 % |
| Each handed its block as the engine's block starts (two runs) | 1 | 0.80 / 0.77 % | 1.11 / 1.10 % | 5.1 / 2.7 % |
| | 4 | 0.79 / 0.81 % | 1.18 / 1.15 % | 13.3 / 10.2 % |

Handed over as its track played, each plug-in's round trip added to its thread's: about 32 microseconds a plug-in on
one thread. Handed over first, as the engine's block starts (a task for each before the tracks' tasks,
`engine/core/Engine.h`; `devices::Processor::startsEarly()`), the helpers play side by side while the engine plays the
rest, and a block takes a sixth as long on one thread and two thirds as long on four. On four threads the process takes
a few percent of a core more meanwhile: the master's task, taken sooner, waits for the last helpers by yielding, as the
engine's tasks wait for what they take in (T2 step 8); that column moves by as much from run to run (the 20 empty
tracks on four threads took 10.2 % and 6.2 % of a core with nothing changed between).

## 11. Parameters on the song (2026-11-03)

T6 step 4 ([roadmap](roadmap.md#architecture-track), A6) puts `ParameterHandle` on the song's parameters: a handle holds the song and
the parameter's reference, and reads its description and values from the song (`model/devices/internal/Parameters.h`).
Controls read their handles at each of the display ticker's ticks, a strip its volume, pan and sends, so a handle keeps
the element it found and finds it again only once that element has left the song or names another; a parameter that
nothing moves reads its value set by hand from the song, and a knob asks the song whether anything modulates its
parameter before it asks for the modulation's range. The UI's benchmark (`tests/Benchmarks.cpp`,
`windows-profile`), before (the build of 2026-11-02, before T6's steps of 2026-11-03) and after:

| The UI's work, 16 tracks, 256 clips, 16,384 notes | Before | After |
|---|---|---|
| A parameter's change, as the views handle it | 0.20 ms | 0.19 ms |
| The whole window painted | 18.7 ms | 12.8 ms |
| An 11-px column of the window, as a playhead repaints | 0.65 ms | 0.66 ms |
| Save | 132.1 ms | 135.5 ms |
| Autosave, on the UI thread | 10.7 ms | 15.8 ms |
| A note moved a step, with what follows | 2.54 ms | 2.07 ms |

A project's CPU, stopped and playing, moved within its noise (20 tracks of Phazite's devices on one thread: 0.84 % and
27.19 % of each block before, 0.80 % and 26.59 % after). The comparison spans every T6 change of that day, so the
autosave's 5 ms more may come from any of them (one run each); which is left to measure.

## 12. On JUCE alone (2026-11-04)

T6 ends A6 ([roadmap](roadmap.md#architecture-track)): Phazite builds on JUCE alone, its song, its engine and its
plug-in hosting its own. Measured after it with `phazite_tests.exe --test-suite=benchmark --no-skip` from the
`windows-profile` build, against section 11's numbers (2026-11-03):

| The UI's work, 16 tracks, 256 clips, 16,384 notes | 2026-11-03 | 2026-11-04 |
|---|---|---|
| A parameter's change, as the views handle it | 0.19 ms | 0.03 ms |
| The whole window painted | 12.8 ms | 12.1 ms |
| An 11-px column of the window, as a playhead repaints | 0.66 ms | 0.64 ms |
| Save | 135.5 ms | 102.6 ms |
| Autosave, on the UI thread | 15.8 ms | 6.6 ms |
| A note moved a step, with what follows | 2.07 ms | 3.5 to 3.8 ms |

The note's row moves by 2 ms from run to run of the same build (2.07, 2.78 and 3.74 ms on 2026-11-04 without the
changes below): it times a 30 ms wait for the messages that follow and takes the 30 ms away, so it shows when the wait
ends as much as the work. The work it times is less than before (one track's notes compiled again, below). A 40-track
song of 10,240 notes saves in 66 ms, autosaves in 49 ms and opens in 28 ms (77 to 100 ms, 56 ms and 61 ms in section 3).
A project's CPU is as it was: 20 tracks of Phazite's devices playing chords take 26.4 to 26.9 % of each block on one
thread in four runs (26.59 % in section 11; a fifth, just after a build, 29.4 %), the strips' CPU meters, which time each
device as it plays (`engine::DeviceNode::getLoad()`), included.

**A clip moved in a big song.** The playlist's benchmark (100 tracks, 2,400 clips, 38,400 notes) timed 25.1 ms of the UI
thread's work for a clip moved a beat, as each step of a drag moves it, where section 8 measured 1.5 ms on 2026-10-27,
before Phazite's engine played projects (T2). Split up, 14.2 ms were the new plan the playback session compiles for the
change and 6.4 ms the playlist's. Three causes, each fixed:
- Every plan compiled every track's notes again (6.1 ms). The song player keeps each track's
  (`SongPlayer::getNotes()`, `model/playback/internal/SongPlayer.h`), compiled again only once something in its clips
  changes, which it hears from the song itself, so that every player's plans follow it.
- Finding a track by its ID (`findTrackState()`) and listing the tracks looked through every track's children, its clips
  among them, so that the playlist's look at its 100 tracks walked the song's 2,400 clips 200 times (6.3 ms). Only a
  folder holds tracks, so only a folder's children are looked through now.
- Ordering the channels and taking in their sends looked through every other channel's children, clips included, for
  sends to each channel (4.9 and 2.5 ms): each channel's sends are now found once a plan.

| The playlist: 100 tracks, 2,400 clips, 38,400 notes | Before | After |
|---|---|---|
| A clip moved a beat: the UI thread's work | 25.1 ms | 3.3 ms |
| ... of which the new plan | 14.2 ms | 0.8 ms |
| ... and the playlist | 6.4 ms | 1.5 ms |
| A wheel notch, scrolling down | 0.70 ms | 0.27 ms |
| Ctrl and a wheel notch, zooming in | 0.99 ms | 0.53 ms |
| The mouse moved a pixel over the tracks | 0.53 ms | 0.13 ms |

A test checks that a new plan compiles again the notes of the tracks whose clips changed only, and plays every change of
them (`tests/EnginePlaybackTests.cpp`); with the player hearing nothing of the song's changes, it fails.

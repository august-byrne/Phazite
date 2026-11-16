# Performance

Phazite's performance targets, its benchmarks, and the latest number recorded for each; the gates that fail a change
that makes Phazite slower; and the long run.

**The numbers on this page are the latest that the performance audit records, but those of the song of 100 tracks and
of the long run, which were measured on 2026-11-14.**
The audit ([archive/performance-audit.md](archive/performance-audit.md)) is a history of sessions, each of which
measured what it changed; the "From" column of each table names the audit's section a number comes from, where the way
it was measured and the numbers before it are. A benchmark, or a row of one, for which the audit records no number is
listed as "not recorded". Devices changed after a number was recorded (the Compressor's drive runs at twice the rate
since its number) are not measured again either: run the benchmark before relying on a number.

## What the numbers were measured on

As the audit states it: a PC with an Intel Core i9-9900K (8 cores, 16 threads) on Windows 11; the `windows-profile`
build (optimised, with symbols) unless a row says otherwise; 48 kHz and blocks of 480 samples, on one audio thread
unless a row says otherwise. The audit estimates that a mid-range laptop is 1.5 to 2 times slower; no benchmark has run
on one. The paint times of "the UI's work on a big song" and "scrolling and zooming a big song" are those of JUCE's
software renderer; the two benchmarks of a song scrolling continuously paint through Direct2D, as the window does.
Times depend on the machine: compare runs on one machine only.

## Targets

The design sets four targets ([design/performance.md](design/performance.md)), and the audit's plan added five checks.
"Met?" is the audit's own verdict (its section 13 for the design's targets, section 7 for the plan's).

| Target | Met? |
|---|---|
| Audio: no dropouts at a buffer of 128 samples (about 3 ms) with a typical electronic project, 20 to 40 tracks and a dozen third-party synths, on a mid-range laptop | On the PC above, with room to spare. At 128 samples, 100 busy tracks of Phazite's own devices play on four threads with little room, and not on one (the song of 100 tracks below). Not measured on a laptop |
| UI: 60 frames a second scrolling and zooming the playlist and the piano roll with 100 tracks or more | Yes, when judged (section 13): under 4 ms of a frame's 16.7 ms. A clip's move has grown to 8.1 ms since |
| UI: near-zero CPU at rest, only what changed repainted | Nothing repaints at rest. The CPU is low, not zero: about 1 % of a core goes to the views' own timers |
| Startup: under 3 seconds to a project that plays, on an SSD, with the plug-in scan in the background | What Phazite does itself takes under 0.2 s. The whole start, with the process and the audio device, is not timed |
| Memory: long audio streams from disk rather than loading into memory | Yes |
| A key to its sound in under 15 ms with a wired output | Not measured |
| A stopped song of 20 tracks (a synth, an EQ, a compressor and a reverb each) costs under 2 % of a core | Yes |
| A parameter's change takes under 0.5 ms of the message thread | Yes |
| The check for unsaved changes takes under 1 ms | Yes, when measured (0.78 ms, section 7); not recorded since |
| An autosave keeps the message thread under 15 ms | Yes |

## Running the benchmarks

The benchmarks are test cases of the suite `benchmark` in `tests/Benchmarks.cpp` and `tests/SandboxBenchmarks.cpp`.
ctest and CI skip them. Build the `windows-profile` preset, then, from the repository's root:

```
cmake --preset windows-profile
cmake --build --preset windows-profile
build\windows-profile\phazite_tests_artefacts\RelWithDebInfo\phazite_tests.exe --test-suite=benchmark --no-skip
```

To run one, add `--test-case` with a part of its name between asterisks, for example `--test-case="*live note*"`.
doctest splits a filter at commas, so use a part of the name that has none. Each prints its table. The sandbox's
benchmarks play Surge XT where it is installed (`tools\setup-windows.cmd -WithTestPlugin` installs it), and the tests'
stand-in plug-ins otherwise. Run them on a machine that does nothing else, before and after a change, and record the
new numbers here.

The benchmarks only print their numbers. What fails a change is in [The gates](#the-gates) below.

## The benchmarks

### Benchmark: each device's CPU, latency and time to sound

Each device plays 20 seconds of stereo sound in blocks of 480 samples, at its default settings unless the row says;
instruments hold their notes. The figure is the share of one core's time.

| Device, and what it plays | % of a core | From |
|---|---|---|
| Synth: 1 note / 8 notes / 16 notes with 7-voice unison | 0.15 / 1.0 / 2.0 | 19 |
| Wavetable: 1 note / 8 notes / 8 notes with full unison | 0.19 / 1.4 / 4.1 | 19 |
| Wavetable: 8 notes, full unison, stacked and drifting | 4.2 | 19 |
| Drum Sampler: 4 pads every 16th / idle | 0.312 / 0.003 | 18 |
| Sampler: 1 note / 8 notes / 16 notes filtered, looping | 0.10 / 4.7 / 12.6 | 19 |
| Sampler, idle | 0.03 | 17 |
| FM: 1 note / 8 notes / 16 notes filtered, held | 0.68 / 5.6 / 12.4 | 19 |
| FM: 8 notes held, with Spread, against the same without it in that run | 12.5 against 6.0 | 17 |
| FM, idle | 0.03 | 17 |
| Filter | 0.09 | 19 |
| Compressor | 0.19 | 7 |
| Multiband | 1.51 | 19 |
| Limiter | 0.34 | 19 |
| Saturator | 0.80 | 19 |
| Exciter: its band from 5 kHz / from 1 kHz | 2.57 / 2.72 | 19 |
| Vinyl: stopped / playing, everything on / silence in, stopped | 1.32 / 1.48 / 0.005 | 19 |
| Delay | 0.15 | 19 |
| Reverb: room / plate | 0.91 / 0.43 | 19 |
| Chorus: three voices with Shimmer / eight voices spread | 0.36 / 0.72 | 19 |
| Vocoder, 40 bands | 3.2 | 8 |

Not recorded: the Stereo Expander, the Phaser, the Flanger, and the Reverb and the Multiband with silence in. The
Synth's and the Wavetable's idle cost is recorded for the Release build only (0.03 % and 0.01 %, section 2).

| Latency reported at 48 kHz | Samples | From |
|---|---|---|
| Limiter | 101 | 15 |
| Saturator | 6 | 15 |
| Filter | 6 | 15 |
| Multiband | 490 | 15 |

The Sampler and the FM synth sound 0.04 to 0.06 ms after their note (to -60 dB; section 17). The other devices' times
to sound are not recorded for this build.

### Benchmark: a sound made ready for the sampler as it loads, a minute of it in stereo

What the sampler does with a sound as it loads, on the message thread.

| For a minute of stereo sound at 48 kHz | Time | From |
|---|---|---|
| Its octaves made | 90 ms | 17 |
| Its hits found | 225 ms | 17 |

### Benchmark: an analyzer's work on a second of sound, as an EQ's editor shows it

The spectrum a device's editor shows, taken in at the display's 30 refreshes a second, on the message thread while an
editor shows it.

| Sample rate | % of a core | From |
|---|---|---|
| 48 kHz | 0.77 | 20 |
| 96 kHz | 1.11 | 20 |

The benchmark times one channel's spectrum. Since 2026-11-15 an analyzer's trace (`SpectrumTrace`) analyzes each
channel apart and shows the louder (known bug 120), so a trace costs twice this. Measured on 2026-11-15 on the same PC,
the one channel took 1.13 % of a core at 48 kHz and 1.58 % at 96 kHz (another day's load; compare within one run).

### Benchmark: a device's graph repainted, its curve kept or worked out again

Each graph painted 200 times into a software image, its values as they were ("Kept") and with a value changed before
each paint ("Worked out again", the analyzer refreshed with it). Measured on 2026-11-15 on the PC above, in the
`windows-profile` build, before and after known bug 130's fix (before: the editors' code as it was, in the same build).

| Graph, a repaint | Before | After |
|---|---|---|
| EQ, its values as they were | 0.92 ms | 0.55 ms |
| Reverb, its values as they were | 1.41 ms | 0.41 ms |
| Sampler's sound, its values as they were | 0.12 ms | 0.12 ms (unchanged: its peaks were already kept for each width; drawing its columns as filled paths took 1.05 ms, so it draws them as lines, as before) |

### Benchmark: a live note's way through Phazite's engine

How soon a note played live is heard.

| What | Value | From |
|---|---|---|
| A live note on a synth, played about 1.5 ms after a block started: heard after the next block starts | 71 samples, 1.48 ms | 16 |
| A live note's wait with blocks of 256 samples at 48 kHz (every note waits one block) | 5.3 ms | 9 |

The delay compensation that a limiter on another track or on the master adds to a live note is not recorded since the
limiter's latency became 101 samples.

### Benchmark: a project's CPU, stopped and playing

Songs of 20 tracks through Phazite's engine. The figure is the share of each 480-sample block that playing it takes,
on one thread and on four.

| Song | Stopped: 1 thread / 4 | Playing: 1 thread / 4 | From |
|---|---|---|---|
| 20 empty tracks | 0.33 % / 0.33 % | 0.33 % / 0.33 % | 9 |
| 20 tracks, each a synth, an EQ, a compressor and a reverb, playing chords | 1.39 % / 0.50 % | 41.9 % / 13.2 % | 18 and 9 (stopped), 19 (playing) |
| 20 drum samplers, each a beat of a kick, a snare and sixteenth hats | 0.68 % / 0.59 % | 1.81 % / 1.29 % | 18; playing on one thread, 19 |
| 20 plug-in synths in one helper (the tests' stand-in), playing chords | 0.80 and 0.77 % / 0.79 and 0.81 % (two runs) | 1.11 and 1.10 % / 1.18 and 1.15 % | 10 |

The drum samplers' figures of section 18 were measured while another build ran, so they are high if anything. Section
19 also records 0.38 % for the 20 empty tracks, without saying whether stopped or playing; and section 18's 1.39 % is
what it records for "20 synths' tracks stopped", taken here for this song.

| The whole process's CPU while it plays, blocks coming one each block's length | % of a core | From |
|---|---|---|
| 20 empty tracks, four threads | 7.4 | 9 |
| 20 tracks of a synth, an EQ, a compressor and a reverb, chords: four threads / one | 39.4 / 30.8 | 9 |
| 20 plug-in synths in a helper (Phazite's process alone): one thread / four, two runs each | 5.1 and 2.7 / 13.3 and 10.2 | 10 |

The process's figures of section 9 are older than the changes that made the devices' song cost more to play
(sections 15 and 19).

### Benchmark: a song of 100 tracks, its blocks at 128 and 512 samples, its plan and an edit

The benchmark song (`tests/BenchmarkSong.h`) of 100 tracks, every one playing all the way through: an instrument on
each, in turn a Synth, a Wavetable, an FM synth, a Drum Sampler and a Sampler, each with a part that suits it; an EQ
and a compressor after it, a saturator on every third track and a chorus on every fifth; a send to a reverb from every
track and to a delay from every other; buses of ten, each with a compressor; and automation on every fourth track's
volume and every Synth's cutoff. Ten seconds of blocks at 48 kHz, played one after another on the test's thread as the
audio device's thread plays them. The figure is each block's time as a share of the block's length, 2.67 ms at 128
samples and 10.67 ms at 512: over 100 % is a dropout.

Measured on 2026-11-14 on the PC above, from `windows-profile`, two runs, **while other builds loaded the machine**:
the numbers are high if anything, the most a block took above all.

| Blocks of | Threads | Stopped: mean / 99th percentile / most | Playing: mean / 99th percentile / most |
|---|---|---|---|
| 128 samples | 1 | 8.8 to 11.3 % / 17 to 23 % / 29 to 30 % | 200 to 205 % / 326 to 342 % / 431 to 445 % |
| 128 samples | 4 | 4.7 to 5.4 % / 27 to 30 % / 44 to 53 % | 48 to 52 % / 73 to 79 % / 92 to 106 % |
| 512 samples | 1 | 5.2 to 5.5 % / 7 to 9 % / 9 to 10 % | 181 to 195 % / 245 to 307 % / 295 to 396 % |
| 512 samples | 4 | 2.9 to 3.1 % / 6 % / 7 to 8 % | 51 to 52 % / 72 to 75 % / 81 to 91 % |

| The message thread, with 100 tracks | Median | Most |
|---|---|---|
| The plan's compile, given to the engine | 38 to 42 ms | 49 to 60 ms |
| A note added, with its new plan | 38 to 42 ms | |
| A clip moved, with its new plan | 39 to 43 ms | |
| Save, for a project file of 3,115 KB | 106 to 113 ms | |

What it says of scale:
- **One thread can't play 100 busy tracks:** it takes about twice each block's length, at either block size.
- **Four threads use about half of each block,** and the worst block of a run went over its length once at 128
  samples. So the limit is near 100 busy tracks on four threads at 128 samples, and near 50 on one.
- **Every edit costs a compile of the whole song,** about 0.4 ms a track: at 100 tracks a note added or each step of
  a drag takes about 40 ms of the message thread, more than two frames (known bug 145 in
  [known-bugs.md](known-bugs.md)). Roadmap step S5.7 compiles only what changed.

### Benchmark: saving and opening a big project

| A song of 40 tracks and 10,240 notes, of Phazite's own devices | Time | From |
|---|---|---|
| Save | 66 ms | 12 |
| Autosave | 49 ms | 12 |
| Open | 28 ms | 12 |

### Benchmark: rendering audio clips stretched and pitched

Not recorded.

### Benchmark: audio clips of files at another rate than the engine's

A clip of a file at another sample rate, resampled as it plays on a 48 kHz engine; the figure is for one stereo clip
of 64 playing at once.

| File | % of a core, a clip | From |
|---|---|---|
| 44.1 kHz | 0.244 | 15 |
| 96 kHz | 0.172 | 15 |

### Benchmark: a loop previewed in time, until it can start

Not recorded.

### Benchmark: the UI's work on a big song

The message thread's work on a song of 16 tracks, 256 clips and 16,384 notes, in the whole window.

| What | Time | From |
|---|---|---|
| A parameter's change, as the views handle it | 0.03 ms | 12 |
| The whole window painted | 18.7 ms at its first paint, 7.2 ms at its second | 16 |
| An 11-pixel column of the window, as a playhead repaints it | 0.77 ms | 16 |
| Save | 145 ms | 16 |
| Autosave, on the message thread | 6.6 ms | 12 |
| A note moved a step, with what follows | 3.5 to 3.8 ms | 12 |

The note's row moves by 2 ms from run to run of one build. Why Save grew from 103 to 145 ms is not found
([backlog.md](backlog.md)).

### Benchmark: frosted glass behind popups over a big song

What a popup's glass costs over the song above, at 100 %: taking what is behind it once as it opens or moves, and each
paint of the glass after that.

| A popup | Taken as it opens | The glass painted | From |
|---|---|---|---|
| A tooltip, 220 x 30 | 0.33 ms | 1.13 ms | 14 |
| A menu, 220 x 320 | 1.73 ms | 0.69 ms | 14 |
| The command search, 530 x 480 | 4.71 ms | 1.08 ms | 14 |

### Benchmark: scrolling and zooming a big song

The playlist with 100 tracks, 2,400 clips and 38,400 notes, in the whole window.

| What | The message thread's work | From |
|---|---|---|
| A wheel notch, scrolling down | 0.27 ms | 12 |
| Ctrl and a wheel notch, zooming in | 0.53 ms | 12 |
| The mouse moved a pixel over the tracks | 0.13 ms | 12 |
| A clip moved a beat, as each step of a drag moves it | 8.1 ms | 16 |
| The playlist painted | 1.34 to 1.51 ms | 16 |
| Clip components in the playlist | 8, those in view | 8 |

The clip's move took 3.2 ms before; why it grew is not found ([backlog.md](backlog.md)). Of its 3.3 ms in section 12,
0.8 ms was the new plan and 1.5 ms the playlist's work.

### Benchmark: a song scrolling continuously as it plays, each frame painted as the window paints it

600 frames of the example song House Groove at 22 pixels a beat and 1920 x 1080, painted through Direct2D.

| Each frame | Mean / 95th percentile, two runs | From |
|---|---|---|
| Its paint | 4.98 / 5.87 ms and 5.03 / 5.80 ms | 16 |
| ... of which the audio clips | 0.44 / 0.74 ms and 0.47 / 0.87 ms | 16 |
| The first frame, which makes the clips' pictures | 34.8 ms and 40.0 ms | 16 |

### Benchmark: the mouse moving over the playlist while the song scrolls continuously, at the display's pace

1,200 frames paced at 60 a second, the mouse moving over the track headers and the lanes, four moves a frame.

| What | Value | From |
|---|---|---|
| A move of the mouse over the playlist, mean / most | 0.01 / 1.0 ms | 16 |
| A frame's work with its moves, mean / 95th percentile / most | 0.56 / 1.60 / 2.2 ms | 16 |
| Frames missed | 0 | 16 |

### Benchmark: starting up, and the window at rest

| What | Value | From |
|---|---|---|
| The services, the commands and the window's content, at startup | 12.7 ms | 14 |
| The first start's project opened and the window painted | 26 to 29 ms | 13 |
| The example song House Groove (64 bars) opened from Help and painted | 136 to 141 ms | 13 |
| At rest for 5 seconds, an example song open and stopped: what was asked to repaint | nothing | 13, 14 |
| At rest: the process's CPU | 0.6 to 3.8 % of a core, in runs one after another | 14 |
| At rest: the message thread's CPU | 0.9 to 1.3 % of a core | 13 |

### Benchmark: a sandboxed plug-in's round trip at 128 samples

One plug-in played in a helper, a block of 128 samples (2.67 ms at 48 kHz) at a time, against the same plug-in in
Phazite's process. A block's time is from handing it over to having it back; the waiting thread sleeps at once.

| One plug-in | In Phazite's process: median / 99th percentile / worst | In a helper | The thread that waits | The helper's memory | From |
|---|---|---|---|---|---|
| The tests' stand-in effect (a gain) | 1.4 / 3.0 / 156 µs | 29.6 / 89.7 / 594 µs | 1.6 % of a core | 1.6 MB, empty | 10 |
| Surge XT Effects | 19.1 / 74 / 262 µs | 47.8 / 133 / 573 µs | 0.9 % | 16.6 MB | 10 |
| Surge XT, a chord held | 74.6 / 192 / 1013 µs | 104.4 / 248 / 996 µs | 1.0 % (3.4 % in the helper) | 38.9 MB | 10 |

A round trip costs about 28 microseconds a block.

### Benchmark: eight sandboxed synths in a block at 128 samples

Eight Surge XTs, a chord each, in a block of 128 samples.

| How they are played | Median | 99th percentile | Worst | CPU | Memory | From |
|---|---|---|---|---|---|---|
| In Phazite's process, one after another | 464 µs | 639 µs | 3.3 ms | 17.9 % of a core | | 10 |
| In Phazite's process, on the engine's workers | 237 µs | 397 µs | 3.5 ms | 68 % (the whole process) | | 10 |
| A helper each, each waited for in turn | 451 µs | 717 µs | 7.8 ms | 1.0 % here, 17.9 % in the helpers | 311 MB | 10 |
| A helper each, all handed over first, then waited for | 239 µs | 341 µs | 1.7 ms | 2.3 % here, 54 % in the helpers | 310 MB | 13 (times), 10 |
| One helper with all eight, handed over first | 236 µs | 337 µs | 1.3 ms | 1.9 % here, 45 % in the helpers | 232 MB | 10 |

## The gates

Timings can't be compared with stored numbers on a shared or another machine, so two kinds of gate fail a change that
makes Phazite slower without comparing a time with a number.

**The performance gates** (`tests/PerformanceGateTests.cpp`) run in every ctest run, on every preset. They count work,
which is the same on any machine (`tests/PerformanceCounts.h`):
- each edit gives the engine one plan, however many changes it makes in one turn of the message loop, and a value set
  by hand, a mute, a selection, the playhead moved or a save gives it none (`PlanCounter`);
- a new plan keeps what didn't change: a note added on one track compiles that track's notes again and no other's,
  and every device sounds on;
- the playlist of 200 tracks makes as many clip components as that of 20, and at most seven more components a track;
- what an edit repaints in the playlist: nothing for a volume or a pan set by hand, and only its clip for a note added
  (today it repaints every lane: a known exception, known bug 144, which fails once fixed);
- the allocations a note added and a save make, at 16 and 64 tracks, grow no faster than the song, and a save writes
  the song's text once (`countAllocations()`, counted in Windows Debug builds only; elsewhere the case checks nothing
  and says so).
- the changes since the last commit compare nothing while nothing changed, however often the versions tell, and
  only its track for a note added or a plug-in's own change: 270 and 172 elements of the 4,096 the whole song of 32
  tracks compares, measured 2026-11-15 (`countElementsCompared()`; known bug 126).

They don't see anything that does the same amount of work more slowly: a device three times as costly, a slower
paint.

**The benchmark gates** (`tests/BenchmarkGates.cpp`, suite `benchmark-gate`) measure costs as ratios within one run,
each the least of seven tries, taking turns with what it is compared with:
- each of Phazite's devices against a fixed reference workload written in the test (a row of biquad filters on
  noise), which no change to Phazite touches;
- the benchmark song of 100 tracks against the same song of 10, stopped and playing at 512 samples on one thread, the
  plan's compile, a note added and a save: about ten times the work is allowed (15 or 20 times), far short of the 100
  times of work that grows as the square of the tracks.

Their bounds are twice what a `windows-profile` build measured on the PC above, so they catch a device that has become
two or three times as costly, not one 30 % slower, and they hold for MSVC's optimised builds only; a Debug build
reports the ratios without checking them. They don't see a cost that grows in both of what is compared (the engine's
own work for each track, doubled, shows only where it is a device's). ctest skips them; the nightly workflow
(`.github/workflows/nightly.yml`) runs them, or run them from `windows-profile`:

```
build\windows-profile\phazite_tests_artefacts\RelWithDebInfo\phazite_tests.exe --test-suite=benchmark-gate --no-skip
```

After a change that makes a device costlier on purpose, measure again and set its bound to twice the new ratio, saying
why in the commit message.

## The long run

`tests/LongRun.cpp` (suite `long-run`) plays the benchmark song of 8 tracks, looped, through Phazite's engine, in blocks
of 256 samples at 48 kHz, one after another as the audio device plays them, for an hour of music. Meanwhile it edits the
song from a fixed seed every two seconds of music (notes, clips, values set by hand, effects added and removed, Undo
and Redo, the loop, the tempo and the playhead moved) and saves it every 60 edits. It checks that nothing allocates on
the audio thread (in Windows Debug builds); that the process's memory, once the run has warmed up, grows by no more
than 64 MB (512 MB under a sanitizer) and 16 KB an edit for its undo steps; that what plays stays finite and within
reason; that the song's IDs stay whole; and that the song saved and opened again at the end is the same song.

Measured on 2026-11-14 from `windows-profile`, on the PC above: an hour of music in 674 seconds, with 1,919 edits and
31 saves; the process's memory went from 85 to 103 MB. In a Debug build an hour of music takes about an hour.

ctest skips it; the nightly workflow runs it under `linux-asan` and `linux-tsan`. On GitHub's machines (2026-11-15)
the `linux-tsan` job took about 80 minutes with its build, and the `linux-asan` job about four hours: an hour of music
in about two and a half, then the file readers' fuzzer with 20,000 rounds.
Started by hand, the workflow runs one job alone, which doesn't wait behind a whole run
(`gh workflow run nightly.yml -f only=long-run`, or `benchmark-gates`, or `windows-asan`). Run the long run from any
build:

```
build\windows-profile\phazite_tests_artefacts\RelWithDebInfo\phazite_tests.exe --test-suite=long-run --no-skip
```

`PHAZITE_LONG_RUN_SECONDS` sets the seconds of music (3600 unless set; 60 tries it quickly), and
`PHAZITE_LONG_RUN_SEED` the seed of its edits (1 unless set).

## Measured outside the benchmarks

| What | Value | How the audit has it |
|---|---|---|
| A key to its sound through Bluetooth headphones | about 170 to 300 ms | The stages added up, section 1: the link itself is about 150 to 250 ms, from published measurements |
| A key to its sound through a wired output, Windows' shared mode, 480 samples | about 20 to 50 ms | An estimate, section 1 |
| The same with a low-latency output at 128 to 256 samples | about 8 to 15 ms | An estimate, section 1; to be measured by hand |

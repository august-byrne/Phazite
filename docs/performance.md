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

### Benchmark: a wavetable of 256 frames made

What the wavetable synth does with a table as it loads one, on the message thread: 256 frames of 2048 samples, each
a saw of all 1024 harmonics, read back as a table of 21 levels a frame, with what its warps leave off centre (each
level's area across its cycle at 128 places and its means bent at 49 bends, both exactly from the frame's harmonics,
`src/devices/wavetable/Wavetable.cpp`). Measured on 2026-11-15 with other builds running on the machine, so each figure
is the least of five runs, each the best of three; the same harness timed the table without the areas and the bent
means, in turns with it. The machine was busier for the second pair than for the first.

| A table of 256 frames | Time | From |
|---|---|---|
| Its levels alone, against its levels, the areas from its levels' samples and the bent means | 93 ms against 106 (13 ms more) | 2026-11-15 |
| Its levels alone, against its levels and the areas and bent means from its harmonics | 120 ms against 141 (21 ms more, a sixth) | 2026-11-15 |
| As first built for the bent means (each level's samples summed again, the means in doubles harmonic by harmonic) | 388 ms, against 83 ms for its levels alone (about 300 ms more) | 2026-11-15 |

The first table made in a session also works out the bends' weights once, 49 transforms of 8192 points: about 15 ms.

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

Muted tracks play on (their instruments and effects, for when they are heard again: known bug 78, 2026-11-15), so they
cost what heard ones do; Freeze frees them. Measured side by side on 2026-11-15, `windows-profile`, while other builds
ran on the machine (so the heard row is higher than above, and the two are to be compared with each other):

| Song | Stopped: 1 thread / 4 | Playing: 1 thread / 4 |
|---|---|---|
| 20 tracks, each a synth, an EQ, a compressor and a reverb, playing chords | 11.49 % / 4.56 % | 72.39 % / 24.76 % |
| The same, every track muted | 12.35 % / 4.08 % | 74.38 % / 22.34 % |

Every path into a sum keeps a ring of what came into it since 2026-11-15, a delay of none too (each block written into
it: two channels of the block's length). The same song heard, measured just before and after, `windows-profile`, while
other builds ran: 44.89 % / 18.79 % playing without the rings, 48.34 % / 14.85 % with them: within what varies from
one run to the next. The rings' memory: 128 KB a ring at 48 kHz (16,384 samples on two channels), 512 KB at 192 kHz.

The paths from one point of a strip share one ring since 2026-11-15 (its tap: a track's sends after its fader read its
sound before its mute, its way to the master its output), which the strip writes once a block; before, each path had
its own. `Benchmark: 200 tracks each sending to 8 returns` (each track a Synth holding a chord, 8 returns with an EQ
each, one track's 64 samples of latency lining the others up; blocks of 10 ms played back to back), two runs of each,
the ring per path switched back on for the "before" runs, `windows-profile`, 2026-11-15:

| | Rings | Their memory, 48 kHz / 192 kHz | Playing at 48 kHz, 1 thread / 4 (mean) | At 192 kHz, 1 thread / 4 (mean) |
|---|---|---|---|---|
| A ring per path | 1,808 | 226.9 MB / 904.9 MB | 139.2 and 140.1 % / 47.6 and 50.0 % | 561.0 and 551.1 % / 200.5 and 182.1 % |
| A ring per tap | 408 | 52.7 MB / 210.2 MB | 145.7 and 131.0 % / 50.3 and 45.5 % | 517.9 and 635.3 % / 165.1 and 206.6 % |

The block's cost is the synths' (within what varies from one run to the next either way); the memory falls by
three quarters. A render keeps no room in its rings, and none for a path with no delay or for what it leaves out
(`tests/PerformanceGateTests.cpp` counts its samples): the stems benchmark below peaked at 138.5 MB for the whole test
process with a ring per path and room in renders, and at 130.8 MB now.

### Benchmark: the benchmark song's stems exported

The benchmark song (`tests/BenchmarkSong.h`) of 20 tracks and 16 beats, its 4 stems at 48 kHz, each a render of one
channel as it reaches the master. A render leaves out what it can't hear (2026-11-15): what doesn't reach the channel,
and a muted track that feeds nothing heard. Measured on 2026-11-15, `windows-profile`, while other builds ran.

| Song | Each render playing the whole song | Leaving out what it can't hear |
|---|---|---|
| As it is | 28.32 s | 13.75 s |
| A quarter of its tracks muted | 28.44 s | 10.59 s |

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

### Benchmark: a loop's wrap and a jump on a song of 200 tracks

Known bug 85. Measured on 2026-11-16 on the PC above, from `windows-profile`, **while other agents' builds loaded the
machine**: each figure is a block's time as a share of a block of 128 samples at 48 kHz (2.67 ms).

The wrap's own work, alone (`Benchmark: a loop's wrap's own work`), the median of 50, two runs: the notes sounding at
the loop's start found on 200 tracks of 1,024 notes each (a chord of four on every beat for 256 beats), and a block
played from the loop's start by 40 clips of a stereo file at 44.1 kHz.

| A loop's wrap, its own work | Before | Now |
|---|---|---|
| The notes sounding at its start found on 200 tracks | 18 to 22 % (a look through every note before it) | 0.7 to 1.6 % (each track's index of them) |
| 40 resampled clips playing their first block from its start | 59 to 94 % (each resampler started from nothing) | 1.4 to 3.0 % (each one's spare, made ready a little a block) |

A resampled clip's state holds about 276 KB, half of it the spare resampling (two channels of r8brain's filters and
buffers each): 5.5 MB for the spares of the song's 40 clips. A resampler gives nothing out until it has taken 832 samples
of a 44.1 kHz file (18.9 ms of it) at 48 kHz: what a jump's 3 ms of fade can't play through. Made ready all at once in
the fade's first block, as the second round of the review had it, forty of them took that block 46 % more than a steady
one on four threads; so a jump that lands among clips not ready there is held, silent, for whole blocks while each makes
its resampling ready a share of each block (20 ms at most, never waiting for the disk), and then plays as it would have
at once. The jumps of a run (a tenth of a second apart or less, as a drag of the playhead makes them) are held at their
first at most, the rest starting from what is ready as before, so a drag sounds as it did before holds.

The whole song (`Benchmark: a loop's wrap and a jump on a song of 200 tracks`): the benchmark song of 200 tracks with
256 beats of notes, and 40 tracks of those clips, looping two beats near the song's end; and a jump back to the loop's
start in the middle of a round, 20 times. The song is far more than one thread plays in time, and under the load the
blocks around moved by a quarter from run to run, so the figures only show the order of what the wrap and the jump add.

| | Threads | Blocks around: median | The wrap's block: mean | A jump's block: mean |
|---|---|---|---|---|
| Before | 1 | 566 % | 744 % (+178) | 1036 % (+470) |
| Before | 4 | 167 % | 224 % (+57) | 281 % (+114) |
| Now, three runs | 1 | 629 to 714 % | 694 to 827 % (+66 to +161) | 731 to 929 % (+80 to +301) |
| Now, three runs | 4 | 163 to 208 % | 186 to 261 % (+23 to +53) | 255 to 295 % (+87 to +107) |
| Now, the rings reset in place, the landing prepared | 1 | 670 % | 718 % (+48) | 639 % (-31) |
| Now, the rings reset in place, the landing prepared | 4 | 182 % | 216 % (+34) | 252 % (+70) |
| Now, the landing held, a jump to half a beat into the loop (the clips not ready there), the most of the 8 blocks after it | 1 | 663 % | 796 % (+133) | 496 % (-167) |
| Now, the landing held, a jump to half a beat into the loop (the clips not ready there), the most of the 8 blocks after it | 4 | 131 % | 156 % (+25) | 152 % (+21) |

The blocks after such a jump, each by itself (a temporary timer in the engine, the means of 20 jumps), in milliseconds of
a 2.67 ms block, on four threads: before the hold, steady 7.53, the fade's first block 8.76 (its 40 clips' resamplers
started there: 2.25 ms of the strips' tasks' time) and the landing 7.89 (the resets of every device: 2.26 ms of the tasks'
time); now steady 6.30, the fade's first 6.74, a block held 3.19 (silent but for the fade's last 16 samples), the landing
5.93, then 4.4 to 4.9 while the spares are made ready again for the loop's start. On one thread, the same shape: steady
22.0, fade 20.2, held 5.6, landing 18.6.
Play and a jump reset every device and every path's ring, which starts the song from silence (`Benchmark: each device's
reset`): a ring's reset writes nothing now, its past silence: 400 rings of a third of a second at 48 kHz in 0.1 % of a
block, where clearing them, as a reset did before, took 32.5 %. A device's own reset takes 0.07 % of a block at most for
the instruments, 0.33 % for the delay (its seconds of buffer) and 0.21 % for the reverb, 0.03 % or less for the rest:
which would make about 15 % for the song's 600 devices; measured in the landing block, with what each runner resets
of its own, they take 1.7 ms of one thread (64 % of a block; 2.3 to 2.7 ms of the tasks' time on four, shared by them),
and the chase of every track 0.3 ms.

The message thread's wait as a new plan keeps each clip's start in its file (`AudioFile::keep()`), while a reading
thread reads a file whose reads take 20 ms each, as a cold read of a slow or busy drive can
(`Benchmark: what the message thread waits for`): before, a median of 25 ms and as long as 1.2 s, as the reading thread
held the file's lock across each read and took it again for the next; now a median of 0.002 ms and at most 0.03 ms,
keep() taking a lock of its own that no read holds.

The chase's index of the controllers (`Benchmark: a long take dense with pressure`): each controller's changes a list of
places among the events, 4 bytes a change. A take of a minute, ten keys' pressure at 200 values a second (120,000
values): a note added and the song compiled in 23.7 ms (the median), its sequence holding 3.2 MB; five minutes at the
bound a take keeps to (a thousand values a second for each key, 3 million): the sequence made in 353 ms, holding 80 MB,
of which 72 MB are the events themselves. The chase at a wrap on a track of sixty controllers with 2,000 changes each
and ten keys' pressure: 0.045 % of a block of 128 at 48 kHz.
### Benchmark: saving and opening a big project

| A song of 40 tracks and 10,240 notes, of Phazite's own devices | Time | From |
|---|---|---|
| Save | 66 ms | 12 |
| Autosave | 49 ms | 12 |
| Open | 28 ms | 12 |

### Benchmark: what a branch switch and a merge spend on the message thread, by part

The 32-track benchmark song of 64 beats, `windows-profile`, three runs each, measured 2026-11-15 (known bug 126). Before, both
commands read their songs, split the song left and worked the merge out on the message thread, in the call; now they read
and work out on the history's thread, and the message thread is kept by the call and by the message that takes what was
read in (the song made the other branch's, its plan compiled; the merge's list shown).

| | Before: the call | After: the call | After: the longest message after it | After: the whole, start to end |
|---|---|---|---|---|
| A branch switch | 280 to 310 ms | 1.6 to 2.7 ms | 89 to 114 ms | 314 to 352 ms |
| The start of a merge | 208 to 225 ms | 2.4 to 2.7 ms | 10 to 11 ms | 308 to 342 ms |
| Songs read, and split, on the message thread (switch, merge) | 2, 2 | 0, 0 | | |

A commit's song read, which now passes the loader's checks as it is joined (`joinSong()`), takes 80 to 97 ms on its
thread, 61 to 80 ms before; the song split into pieces 23 to 33 ms (33 to 37 ms before). Re-measured 2026-11-16 after the review's round (the song left split again on the history's
thread when it changes meanwhile; reasons; the import in steps). What still runs on the message thread of a DAWproject
import as a branch: the commit's song made a song of its own (`makeSong()`, which belongs there) with what came back
taken in, its audio copied out of the file (`takeDawProject()`): 35 to 51 ms for the song made, plus the copy, which
depends on the file.

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

### Benchmark: the mixer of 200 tracks

200 tracks, each with a synth and an EQ, in a mixer 1600 pixels wide (about a dozen strips in view). The song's plans
aren't compiled (its session is lent, as to a render), so that the times are the mixer's work alone; a strip shows a
change at its next tick, eight times a second. Measured on 2026-11-15 from `windows-profile` on the PC above, **while
other builds loaded the machine**, before and after the fix of known bugs 127 and 144 (only the strips in view, and
only the changed channel's brought up to date).

| What | Before | After |
|---|---|---|
| Opened: its strips made and laid out | 546 ms | 16.5 to 16.8 ms |
| Strips made / components | 200 / 6,025 | 13 / 415 |
| At rest: a tick of the strips, eight a second | 42.7 ms | 1.5 ms |
| A device added to a track in view: the change heard / shown at the next tick | 0.20 / 84.4 ms | 0.25 / 0.5 to 4.2 ms |
| A device added to a track out of view: the same | 0.24 / 171 ms | 0.29 to 0.68 / 0.9 to 2.0 ms |
| A fader's drag: a step / the next tick | 19.7 / 18.1 ms | 0.10 to 0.63 / 1.1 to 2.5 ms |
| Scrolled a strip sideways | 0.02 ms | 1.6 to 2.7 ms, a strip made as it comes into view |
| The scroll bar's thumb dragged end to end in 60 events: each event / the worst | | 6.9 / 11.1 ms; 9.2 / 33.7 ms under a heavier load |
| ... each event with what it left for the next turns of the message loop | | 9.7 ms; 18.4 ms under the heavier load |

What is left of a tick, about 0.1 ms a strip, is the model's lookups of each strip's values by ID (known bug 122).

The thumb's drag was measured after the review's round (2026-11-15), whose first version made every strip a jump brought
into view, and deleted every one it took out, at once: 10.5 ms an event and 24 to 34 ms at the worst, over a frame. A jump
now makes and deletes what 6 ms of a frame can bear (two strips at least), the rest at the next turns of the message
loop; a strip also reads its channel once as it is made (twice before). Under load the two strips it always makes can
take a frame by themselves.
### Benchmark: the playlist of 200 tracks and 4,800 clips

200 tracks, each with 24 clips of 4 bars and 16 notes (4,800 clips, 76,800 notes), in a playlist 1600 by 900 pixels,
its plans not compiled, as above. Measured the same day, the same way.

| What | Before | After |
|---|---|---|
| A note added to a clip in view: the work / asked to repaint | 306 ms / 1400 x 900 px | 2.9 to 3.0 ms / the clip, 384 x 47 px |
| A note added to a clip out of view | 302 ms / 1400 x 900 px | 3.5 to 3.7 ms / nothing |
| A clip moved a beat, as a drag's step moves it | 294 ms / 1400 x 900 px | 3.5 to 3.9 ms / its row's part and the overview, 1400 x 184 px |
| A wheel notch, scrolling down / up | 9.1 / 1.3 ms | 7.3 to 7.5 / 1.0 ms |
| A track renamed | 326 ms | 130 ms |
| A track added at the end | 384 ms | 162 ms |
| Headers / clip components / components | 200 / 72 / 1,506 | 19 / 72 / 239 |
| The rows' scroll bar's thumb dragged top to bottom in 60 events: each event / the worst | | 4.5 / 7.9 ms; 6.1 / 17.0 ms under a heavier load |
| ... each event with what it left for the next turns of the message loop | | 23.8 ms; 141 ms under the heavier load |
What is left of a track renamed or added is the playlist reading every clip again (`getClips()`, about 0.6 ms a track
of 24 clips, 122 ms for the 200), which is the model's lookups (known bug 122): a clip's information looks its track
up by ID, and an audio clip itself three times. A clip moved still reads its own track's clips again (0.6 ms).

A jump of the rows (the thumb dragged) makes the headers, automation lanes and clips that come into view as 6 ms of a
frame can bear (four at least), the rest at the next turns of the message loop: before, the first version made them all
at once, 16 ms an event and up to 230 ms. What it leaves is mostly the clips' notes read from the song
(`getNoteShapes()`, about 0.45 ms a clip of 16 notes, the model's lookups again).
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

### Benchmark: a clip of 5,000 notes edited in the piano roll, and what views read of a song of 4,800 clips

Known bug 122: the model found each track, clip and note by looking through the song at every call, several times for each
note an edit touched, and the piano roll read every note of its clips, and the other tracks' notes, at each move of the
mouse. Since 2026-11-15 the song keeps an index of where its tracks, clips and notes are by their IDs
(`src/model/base/internal/SongIndex.h`), and the piano roll finds what is under the mouse among the notes it paints.

The first table: the piano roll in a window (`PianoRollScreen`), a clip of 5,000 notes, four a beat over twelve rows,
with another track's 5,000 as ghosts; each through the mouse or the command, the song's changes delivered after it, the
engine's new plan included. The second: the model's reads a view makes in a song of 200 tracks of 24 clips of 16 notes.
Measured 2026-11-15, the `windows-profile` build, on a machine other builds shared.

| What | Before | After |
|---|---|---|
| The mouse moved over the notes (hover) | 7.96 ms | 5.5 ms (finding the note under the mouse 0.013 ms of it; the rest is the hint and the cursor, which change at every move here, the notes a fifth of a pixel apart: not measured further) |
| ... over empty rows, where ghosts are looked for | 5.36 ms | 0.56 ms |
| A step of a rectangle selecting across the clip | 122.5 ms | 2.9 ms |
| A step of a drag of 500 notes | 22.1 ms | 4.5 ms |
| ... its Undo | 18.0 ms | 7.1 ms |
| Select All | 340.8 ms | 3.2 ms |
| Transpose Up, every note / its Undo | 2,377 / 356 ms | 22 / 12 ms |
| Quantize, every note / its Undo | 2,500 / 530 ms | 58 / 20 ms |
| Duplicate, every note / its Undo | 6,286 / 805 ms | 37 / 9.5 ms |
| Delete, every note / its Undo | 1,944 / 408 ms | 27 / 15 ms |
| Every track's clips (`getClips()`, 200 calls) | 253 ms | 5.9 ms |
| A clip's info by its ID (`getClip()`) | 0.173 ms | 0.0014 ms |
| A clip found by its ID (`findClipState()`), alone | not measured | 350 to 440 ns, of which the check of the answer against the tree (it still has the ID, on a track in the track list) is about 250: 90 to 170 ns without it |
| A track's row (`getTrackRow()`) | 0.091 ms | 0.0026 ms |
| A clip's note shapes (`getNoteShapes()`) | 0.127 ms | 0.0030 ms |
| A clip moved, as a drag's step moves it (the model alone) | 0.134 ms | 0.0034 ms |

Where the time went, counted before the change (`SongIndex::countElementsVisited()`, which the walks counted then): a
step of a drag of 20 notes in a song of 10 tracks looked at 14,988 elements, and of 200 notes in a clip of 2,000 at
1,229,268, about 6,000 for each note moved: each note was looked for through its clip's notes three times (the drag's
`setNotes()`, `placeNote()`'s read of it and its change), and its clip through the tracks four times. Ten moves of the
mouse read 15,000 notes from the song (each move every note of the clip twice, and the ghosts), and ten steps of a
rectangle 9,181, which then compared each note chosen with every other (`Selection::selectNotes()`: the square of the
notes, 12.5 million comparisons for Select All). Every clip's info looked up its track's name, whose number ("Track 3")
was counted through every track. After: 623 elements for the step of 20 notes, 6,203 for 200 in a clip of 2,000, 623
in a song of ten times the tracks (counting the model's own walks of a track's clips too, which each note's move makes to
find how far its clip can grow: 24 elements); no notes read by the hover.

What the index costs: about 100 bytes a track and a clip, made in one walk as the song is first asked for one (as a
single lookup walked before), and about 60 bytes a note of each clip whose notes were asked for, made as the first is;
each change to the song costs it a lookup in a hash table at most, and each answer a check that the element is still where the index holds it (above), and a change it doesn't follow (an ID changed in
place, a track made a folder) makes it again at the next lookup.

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

### Benchmark: what the browser's search and the commands that read audio files keep the message thread for

`tests/MessageThreadWorkBenchmarks.cpp`, measured 2026-11-15 (known bugs 128 and 123; the first "after" while other builds
ran on the machine, the second round's alone). Before: the browser searched the whole index on the message thread at
each key, and an import, Normalize and Quantize Audio read their audio there.

The browser's search, typing "kick deep" key by key into a library of 50,000 sounds (empty files in sample packs'
folders, indexed by the library) and an index of 500,000 made in the test:

| What | Before | After |
|---|---|---|
| 50,000 sounds: the message thread, per key | 60 to 83 ms (the whole search) | 0.06 to 0.14 ms (the search started; the rows made once its results are in) |
| 50,000 sounds: until the results show | the same | 4 to 11 ms |
| 50,000 sounds: the search itself, wherever it runs | 60 to 83 ms | 3.6 to 7.5 ms (search keys made with the index) |
| 500,000 sounds: the search itself, on its own thread | (the index stopped at 50,000) | 37 to 53 ms |

A ten-minute stereo recording at 48 kHz, 24-bit (173 MB), with a file of the same name in the project already:

| Command | Before: the message thread | After: the call | After: the message thread in all | After: done after |
|---|---|---|---|---|
| Import (the file compared with the one there) | 512 ms | 0.1 ms | 16 ms | 109 ms |
| Normalize | 470 ms | 0.03 ms | 24 ms | 299 ms |
| Quantize Audio (its hits found first) | 4,247 ms | 0.03 ms | 167 ms | 2,781 ms |

"The message thread in all" is the call and everything the message thread ran until the change was made: the change
itself, the plan it compiles, the clip's render it starts, and the other timers. The reading is on a thread of its own;
the window takes no input meanwhile, and a job that takes longer than a second shows its progress window with Cancel.

Quantize Audio's change itself, on a ten-minute clip (`Quantize Audio's edit of 1,000 and 5,000 hits`): the edit, then
what the message thread runs for a second after it (the plan, the render's recipe). Most of what came after was the name
of the clip's render, a key of thousands of points built by appending to a string, which copied it whole at each point,
several times for each plan; written into a stream now (`getRenderFile()`, `src/model/arrangement/AudioClips.cpp`).

| Hits | The edit (ms) | After it, before (ms) | After it, now (ms) |
|---|---|---|---|
| 1,000 | 3.6 to 3.9 | 181 | 92 |
| 5,000 | 12 to 13 | 381 | 122 |

A sound tried on a drum pad by Swap (`a sound tried on a drum pad by Swap`, 20 tries): median 0.07 ms before and after
(a try copies nothing: the sound kept is copied in by its keep, in the background).

## The gates

Timings can't be compared with stored numbers on a shared or another machine, so two kinds of gate fail a change that
makes Phazite slower without comparing a time with a number.

**The performance gates** (`tests/PerformanceGateTests.cpp`) run in every ctest run, on every preset. They count work,
which is the same on any machine (`tests/PerformanceCounts.h`):
- each edit gives the engine one plan, however many changes it makes in one turn of the message loop, and a value set
  by hand, a mute, a selection, the playhead moved or a save gives it none (`PlanCounter`);
- a new plan keeps what didn't change: a note added on one track compiles that track's notes again and no other's,
  and every device sounds on;
- the playlist of 200 tracks makes as many clip components, track headers and components in all as that of 20 (and
  with every track's automation lane shown, as many components), and the mixer of 200 tracks as many strips and
  components as that of 20;
- what an edit repaints in the playlist: nothing for a volume or a pan set by hand, and only its clip for a note added;
  in a playlist of 4,800 clips, a note added repaints its clip alone, or nothing if the clip is out of view, a clip
  moved its old and new places and the overview, and a note added allocates less than twice what it does in one of
  480 clips;
- a device changed on one channel brings only that channel's strip up to date, a send switched before the fader the
  return's too, and a send added every strip (where sound can go changed, which the send knobs follow);
- the allocations a note added and a save make, at 16 and 64 tracks, grow no faster than the song, and a save writes
  the song's text once (`countAllocations()`, counted in Windows Debug builds only; elsewhere the case checks nothing
  and says so).
- the changes since the last commit compare nothing while nothing changed, however often the versions tell, and
  only its track for a note added or a plug-in's own change: 270 and 172 elements of the 4,096 the whole song of 32
  tracks compares, measured 2026-11-15 (`countElementsCompared()`; known bug 126).
- a step of a drag of notes looks at as many elements as it moves notes, whatever the song and the clip: ten times the
  notes in a clip of ten times as many, under twenty times the elements; ten times the tracks, under twice
  (`SongIndex::countElementsVisited()`; known bug 122: 623, 6,203 and 623 measured 2026-11-15, the model's walks of a
  track's and a clip's elements counted too (`countVisited()`); 14,988, 1,229,268 and 22,368 before, the walks by ID alone).
- the piano roll's hover reads no notes from the song, and ten steps of its selection rectangle fewer than its clip
  holds, choosing each note once (`countNotesRead()`, `Selection::getNoteComparisons()`; 0 and 362 of 500 measured,
  15,000 and 9,181 before).
- an import, Normalize and Quantize Audio read no byte of audio on the message thread (`AudioWork::getMessageThreadReads()`,
  known bug 123), and a key typed in the browser's search looks through none of the library there
  (`Library::getItemsSearchedOnMessageThread()`, in `tests/BrowserTests.cpp`; known bug 128).
- a branch switch, a checkout, going back to the branch, the start of a merge and Merge read, split and write no song on
  the message thread (`VersionStore::countSongsRead()`, `countSongsWritten()`, `countSongsSplit()`; known bug 126).

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

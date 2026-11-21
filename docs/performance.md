# Performance

Phazite's performance targets, its benchmarks, and the latest number recorded for each; the gates that fail a change
that makes Phazite slower; and the long run.

**The numbers on this page are those of one run of the whole benchmark suite on 2026-11-16, on the PC below with
nothing else running**, but where a row or a section gives another date: the long run (2026-11-14), the comparisons
that record what a change did (marked where they were measured while other builds loaded the machine), and the few rows
that the run doesn't measure, whose "From" column names the audit's section.
The audit ([archive/performance-audit.md](archive/performance-audit.md)) is a history of sessions, each of which
measured what it changed; the "From" column of each table names the audit's section a number comes from, where the way
it was measured and the numbers before it are, or the date of the run that measured it. A number that no run records is
listed as "not recorded". Run the benchmark before relying on a number.

## What the numbers were measured on

As the audit states it: a PC with an Intel Core i9-9900K (8 cores, 16 threads) on Windows 11; the `windows-profile`
build (optimised, with symbols) unless a row says otherwise; 48 kHz and blocks of 480 samples, on one audio thread
unless a row says otherwise. The audit estimates that a mid-range laptop is 1.5 to 2 times slower; no benchmark has run
on one. The paint times of "the UI's work on a big song" and "scrolling and zooming a big song" are those of JUCE's
software renderer; the two benchmarks of a song scrolling continuously paint through Direct2D, as the window does.
The run of 2026-11-16 had the machine to itself; a number measured while other builds loaded it says so where it
stands. Times depend on the machine and on what else runs: compare runs on one machine only.

## Targets

The design sets four targets ([design/performance.md](design/performance.md)), and the audit's plan added five checks.
"Met?" is the audit's own verdict (its section 13 for the design's targets, section 7 for the plan's), checked against
the run of 2026-11-16.

| Target | Met? |
|---|---|
| Audio: no dropouts at a buffer of 128 samples (about 3 ms) with a typical electronic project, 20 to 40 tracks and a dozen third-party synths, on a mid-range laptop | On the PC above, with room to spare. At 128 samples, 100 busy tracks of Phazite's own devices play on four threads (47 % of a block on average, 92 % at most), and not on one (the song of 100 tracks below). Not measured on a laptop |
| UI: 60 frames a second scrolling and zooming the playlist and the piano roll with 100 tracks or more | Yes: a wheel notch or a zoom takes under 0.7 ms of the message thread and the playlist's paint after it under 1.4 ms; a clip's move 2.4 ms, and its paint 3.2 ms |
| UI: near-zero CPU at rest, only what changed repainted | Nothing repaints at rest. The CPU is low, not zero: about 1 % of a core goes to the views' own timers |
| Startup: under 3 seconds to a project that plays, on an SSD, with the plug-in scan in the background | What Phazite does itself takes under 0.2 s. The whole start, with the process and the audio device, is not timed |
| Memory: long audio streams from disk rather than loading into memory | Yes |
| A key to its sound in under 15 ms with a wired output | Not measured |
| A stopped song of 20 tracks (a synth, an EQ, a compressor and a reverb each) costs under 2 % of a core | Yes, since 2026-11-16: 0.94 % on one thread, 0.52 % on four, on a quiet machine (7.5 % / 2.6 % before, its reverbs playing their tails from silence as it opened) |
| A parameter's change takes under 0.5 ms of the message thread | Yes |
| The check for unsaved changes takes under 1 ms | Yes, since 2026-11-16: 0.01 ms after a notification that changes nothing and 0.04 ms after a note added and taken back, in the song of "the UI's work on a big song" (22 ms before that day's fix); 8.3 ms after a change of the tracks' list, which compares every track |
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
| Synth: 1 note / 8 notes / 8 notes with full unison | 0.27 / 1.96 / 6.42 | 2026-11-19 |
| Synth: 8 notes, full unison, stacked and drifting | 6.61 | 2026-11-19 |
| Synth, idle | 0.031 | 2026-11-19 |
| Synth: 1 note at 2x / at 4x | 0.65 / 1.31 | 2026-11-19 |
| Synth: 16 notes at 1x / 2x / 4x | 3.89 / 7.89 / 15.9 | 2026-11-19 |
| Synth: 1 note Driven at 1x / 2x / 4x; 16 notes Driven | 0.63 / 1.38 / 2.83; 9.45 | 2026-11-19 |
| Drum Sampler: 4 pads every 16th / idle | 0.293 / 0.004 | 2026-11-19 |
| Sampler: 1 note / 8 notes / 16 notes filtered, looping | 0.095 / 4.54 / 11.9 | 2026-11-16 |
| Sampler, idle | 0.03 | 2026-11-16 |
| FM: 1 note / 8 notes / 16 notes filtered, held | 0.63 / 5.33 / 12.1 | 2026-11-16 |
| FM: 8 notes held, with Spread, against the same without it in that run | 10.9 against 5.3 | 2026-11-16 |
| FM, idle | 0.03 | 2026-11-16 |
| EQ | 0.04 | 2026-11-19 |
| EQ, linear phase, eight bands on | 0.89 | 2026-11-19 |
| Filter | 0.13 | 2026-11-19 |
| Compressor | 0.45 | 2026-11-19 |
| Multiband: sound in / silence in | 1.46 / 1.22 | 2026-11-16 |
| Limiter | 0.35 | 2026-11-16 |
| Saturator / its Band from 1 to 8 kHz, Tube with Colour | 1.17 / 1.96 | 2026-11-19 |
| Age, a record: stopped / playing, everything on / silence in, stopped; a tape, playing, everything on | 1.36 / 1.47 / 0.013; 1.76 | 2026-11-19 |
| Delay | 0.24 | 2026-11-19 |
| Reverb: room / plate / room with silence in | 0.89 / 0.40 / 0.93 | 2026-11-16 |
| Reverb: hall | 1.45 | 2026-11-19 |
| Stereo, spreading | 0.27 | 2026-11-16 |
| Motion, phaser, six notches (then the Phaser) | 0.49 | 2026-11-16 |
| Motion, flanger (then the Flanger) | 0.20 | 2026-11-16 |
| Motion, chorus (then the Chorus): three voices with Shimmer / eight voices spread | 0.36 / 0.71 | 2026-11-16 |
| Vocoder, 40 bands | 3.24 | 2026-11-16 |

Stage 6 made some of these dearer (the run of 2026-11-19, a quiet machine, once; not run beside `main`, so the older rows
are the comparison): the Synth by about 40 % a note and 60 % with full unison (its modulation, its filter and its sub glide
for every sample, and its tables keep 16 samples a cycle); the Saturator from 0.74 to 1.17 (its Quality's three rates are
kept ready, and the lower ones wait in a delay); the Compressor from 0.35 to 0.45 (its mode's hand-over ring and its
look-ahead line); the Delay from 0.14 to 0.24 (a line and a time for each side, its duck's follower). Each is a backlog
row's worth of looking at before 1.0, none is near a limit: a Synth voice at 4x costs 1.3 % of a core, sixteen 16 %.

Three of these were higher in the run of 2026-11-16 than the audit's numbers that stood here before (the Compressor
0.35 against 0.19 of section 7, the Drum Sampler's beat 0.384 against 0.312 of section 18, the Vinyl (now Age) with silence in
0.012 against 0.005 of section 19). None is a change of the known-bugs round: `main` and the branch, run side by side on
2026-11-16 **while other agents' builds and benchmarks loaded the machine** (each three times):

| Device | `main` | The branch |
|---|---|---|
| Compressor (Clean, Drive at 0: its drive doesn't run) | 0.338, 0.370, 0.403 | 0.348, 0.344, 0.369 |
| Drum Sampler, 4 pads every 16th | 0.363, 0.610, 0.435 | 0.300, 0.359, 0.305 |
| Vinyl (now Age), silence in, stopped | 0.013, 0.014, 0.021 | 0.011, 0.011, 0.011 |

The audit's numbers came from older builds of these devices. At its defaults the compressor's drive (at twice the rate)
doesn't run at all, so the drive's cost is none of this. In the engine the Vinyl (now Age) sleeps while the song is stopped
and silent (2026-11-16), so its stopped cost there is none.

| Latency reported at 48 kHz | Samples | From |
|---|---|---|
| Synth, at every Quality | 5 | 2026-11-19 |
| Limiter | 101 | 2026-11-16 |
| Saturator, at every Quality | 7 | 2026-11-18 |
| Filter, at every Quality | 7 | 2026-11-18 |
| Compressor | 6 | 2026-11-16 |
| Multiband | 492 | 2026-11-16 |
| Age (then the Vinyl) | 531 | 2026-11-16 |
| EQ, linear phase | 4096 (by design: 4096 r, r = 1 up to 50 kHz) | 2026-11-18 |

The instruments sound 0.04 to 0.06 ms after their note to -60 dB, and 0.15 to 0.25 ms after it to -30 dB (2026-11-16; the Synth 2026-11-19, later by its 5 samples of latency:
the Synth 0.17 / 0.25 ms, the Sampler 0.04 / 0.15, the FM synth 0.06 / 0.25). The Drum Sampler's and the effects' times to sound are not recorded.

### Benchmark: each device's reset, as Play and a jump reset every device in the block that starts or lands

Known bug 85's review: Play, and a jump while the song plays, reset every device on every track in the block that starts
or lands (a jump while it is stopped resets nothing), so a reset must cost little; a device is reset too as it is
switched on, and made ready anew by prepare(). Each device through its runner, as the engine resets it, after it has
played a while (8 notes held, or noise in), at 48 kHz in blocks of 128; the median of 50, as a share of a block's
2.67 ms. Run of 2026-11-16, on a quiet machine.

| Device | Its reset (% of a block of 128 at 48 kHz) | From |
|---|---|---|
| Synth / FM | 0.02 / 0.01 | 2026-11-16 |
| Drum Sampler / Sampler | 0.01 / 0.01 | 2026-11-16 |
| EQ / Filter / Compressor / Limiter / Stereo / Phaser (now Motion's mode) / Vocoder | 0.00 | 2026-11-16 |
| Multiband / Saturator / Chorus (now Motion's mode) | 0.01 | 2026-11-16 |
| Age, a record (then the Vinyl) | 0.02 | 2026-11-16 |
| Delay | 0.32 | 2026-11-16 |
| Reverb | 0.17 | 2026-11-16 |

| The paths' rings (known bug 85's review) | Share of a block of 128 at 48 kHz | From |
|---|---|---|
| 400 rings of 16,512 samples (a song of 200 tracks sending to 8 returns), reset: marked silent, nothing written | 0.1 % | 2026-11-16 |
| The same, cleared as a reset did before | 18.2 % | 2026-11-16 |

So the reset of a song of 200 tracks, each an instrument, an EQ and a compressor, takes about 4 % of the block that
starts; a delay or a reverb, which clear their lines, costs ten to twenty times what another device does.

### Benchmark: a sound made ready for the sampler as it loads, a minute of it in stereo

What the sampler does with a sound as it loads, on the message thread.

| For a minute of stereo sound at 48 kHz | Time | From |
|---|---|---|
| Its octaves made | 82.7 ms | 2026-11-16 |
| Its hits found | 207.7 ms | 2026-11-16 |

### Benchmark: a wavetable of 256 frames made

What the synth does with a table as it loads one, on the message thread: 256 frames of 2048 samples, each
a saw of all 1024 harmonics, read back as a table of 21 levels a frame, with what its warps leave off centre (each
level's area across its cycle at 128 places and its means bent at 49 bends, both exactly from the frame's harmonics,
`src/devices/wavetable/Wavetable.cpp`).

| A table of 256 frames, as it is made now (the best of three) | Time | From |
|---|---|---|
| Its levels, and the areas and bent means from its harmonics | 84.2 ms | 2026-11-16 |

What the areas and the bent means added, measured on 2026-11-15 **with other builds running on the machine**, so each
figure is the least of five runs, each the best of three; the same harness timed the table without the areas and the
bent means, in turns with it. The machine was busier for the second pair than for the first.

| A table of 256 frames, under load | Time | From |
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
| 48 kHz | 0.79 | 2026-11-16 |
| 96 kHz | 1.20 | 2026-11-16 |

The benchmark times one channel's spectrum. Since 2026-11-15 an analyzer's trace (`SpectrumTrace`) analyzes each
channel apart and shows the louder (known bug 120), so a trace costs twice this.

### Benchmark: a device's graph repainted, its curve kept or worked out again

Each graph painted 200 times into a software image, its values as they were ("Kept") and with a value changed before
each paint ("Worked out again", the analyzer refreshed with it). Before known bug 130's fix: measured on 2026-11-15 on
the PC above, in the `windows-profile` build, the editors' code as it was; after: the run of 2026-11-16.

| Graph, a repaint | Before | After |
|---|---|---|
| EQ, its values as they were | 0.92 ms | 0.42 ms |
| Reverb, its values as they were | 1.41 ms | 0.37 ms |
| Sampler's sound, its values as they were | 0.12 ms | 0.10 ms (as before the fix: its peaks were already kept for each width; drawing its columns as filled paths took 1.05 ms, so it draws them as lines, as before) |

Worked out again (2026-11-16): the EQ 0.75 ms, the Reverb 0.35 ms, the sampler's sound 0.10 ms.

### Benchmark: a live note's way through Phazite's engine

How soon a note played live is heard: the delay Phazite's engine adds, from the start of the block after the note to
the synth's sound above -60 dBFS. The engine places a live note as far into the next block as it came after the last
block started (`Engine::takeLiveEvents()`), so that notes keep their timing at a block's delay; the benchmark holds the
engine's clock (`HeldClock`), so that its note comes in as the last block started and what it measures is the engine's
own delay, the same on any machine, and it checks it: a limiter's latency for each limiter the note goes through or is
lined up with, none for one switched off.

| What | Value | From |
|---|---|---|
| A live note on a synth: heard after the next block starts | 2 samples, 0.04 ms (the synth's own rise) | 2026-11-16 |
| ... with a limiter on another track (delay compensation holds the played track back too) | 103 samples, 2.15 ms | 2026-11-16 |
| ... with that limiter switched off | 2 samples, 0.04 ms | 2026-11-16 |
| ... with a limiter on the master too | 204 samples, 4.25 ms | 2026-11-16 |

Before the clock was held, the note came in after the last block started by however long the test took between the two,
some tens of samples that changed from note to note: the run of 2026-11-16 recorded 70, 185, 115 and 292 samples, as if
the switched-off limiter delayed the note 45 samples. It delays nothing: a switched-off device's latency is left out of
the plan's delay compensation (`SongPlayer::addDevices()`), as the code says, and those 45 samples were the note's
place in its block.
| A live note's wait with blocks of 256 samples at 48 kHz (every note waits one block) | 5.3 ms | 9 |

### Benchmark: a project's CPU, stopped and playing

Songs of 20 tracks through Phazite's engine. The figure is the share of each 480-sample block that playing it takes,
on one thread and on four.

| Song | Stopped: 1 thread / 4 | Playing: 1 thread / 4 | From |
|---|---|---|---|
| 20 empty tracks | 0.39 % / 0.42 % | 0.39 % / 0.43 % | 2026-11-16 |
| 20 tracks, each a synth, an EQ, a compressor and a reverb, playing chords | 0.94 % / 0.52 % | 41.8 % / 11.3 % | 2026-11-16, the last run, on a quiet machine, after the fix below |
| 20 drum samplers, each a beat of a kick, a snare and sixteenth hats | 0.55 % / 0.39 % | 1.75 % / 0.84 % | 2026-11-16 |
| 20 plug-in synths in one helper (the tests' stand-in), playing chords | 1.21 % / 1.03 % | 1.43 % / 1.73 % | 2026-11-16 |

The stopped figure is the song's first seconds after it opens (20 blocks, then 500 timed: 5.2 seconds). The run of
2026-11-16 recorded 7.48 % / 2.61 % for the 20 tracks of synths: each reverb, which nothing had come into, played its two
seconds of tail from silence before it slept (`DeviceRunner` waited for a device's tail whether or not anything had come
in). That was not new: `main` (before the known-bugs round) measured the same, side by side with the branch on
2026-11-16 **while other agents' builds and benchmarks loaded the machine** (each three runs, `windows-profile`):

| 20 tracks of a synth, an EQ, a compressor and a reverb, stopped | 1 thread | 4 threads |
|---|---|---|
| `main` | 7.30, 12.04, 8.50 % | 2.38, 4.61, 3.00 % |
| The known-bugs branch, before the fix | 10.10, 7.32, 7.45 % | 4.04, 2.23, 2.23 % |
| After the fix: a device that has heard nothing since it was reset rests while the song is stopped without waiting for its tail | 1.10, 1.02, 1.17 % | 0.73, 0.55, 0.78 % |

The 1.39 % / 0.50 % that the audit recorded (sections 18 and 9) was measured with an earlier benchmark and reverb. A
stopped song's devices now work no block once their sound has ended, which `tests/PerformanceGateTests.cpp` counts for
each of Phazite's devices (see the gates); after Stop, as a chord ends, each rested after (in the gate, at 48 kHz in
blocks of 256, to a quarter of a second): the Delay 7.5 s (its echoes, then its tail), the Reverb 6.0 s (its decay to
silence, then its tail), the FM synth 3.8 s, the Drum Sampler and the Vinyl (now Age) 2.5 s (its noise's hold), the Phaser (now Motion's mode) 0.8 s,
and every other device 0.5 s. While the song plays, a device still waits for its tail after silence, so that one whose
sound starts within it plays as a render does (`docs/design/engine.md`).

The plug-in synths, stopped and playing, cost the same in `main` and in the branch, side by side on 2026-11-16 (stopped
1.13 to 2.28 % / 0.96 to 1.48 % against 1.15 to 1.23 % / 1.00 to 1.48 %; playing 1.44 to 2.21 % / 1.72 to 1.90 %
against 1.45 to 1.88 % / 1.74 to 1.87 %), under load: what the run of 2026-11-16 recorded above the 0.80 % / 0.79 %
and 1.10 % / 1.15 % of section 10 is not a change of the known-bugs round. No block's work changed on their path: a
plug-in never sleeps (it may play with nothing coming in), and each block is a round trip.

Muted tracks play on (their instruments and effects, for when they are heard again: known bug 78, 2026-11-15), so they
cost what heard ones do; Freeze frees them. Measured side by side on 2026-11-16, `windows-profile`:

| Song | Stopped: 1 thread / 4 | Playing: 1 thread / 4 |
|---|---|---|
| 20 tracks, each a synth, an EQ, a compressor and a reverb, playing chords | 7.48 % / 2.61 % | 42.97 % / 11.92 % |
| The same, every track muted | 7.49 % / 2.31 % | 41.77 % / 11.35 % |
| After the fix above, heard / muted (three runs each, under load) | 1.02 to 1.17 % / 0.55 to 0.78 %; 1.03 to 1.15 % / 0.56 to 0.71 % | 46 to 50 % / 12 to 15 %; 48 to 49 % / 13 to 14 % |

Every path into a sum keeps a ring of what came into it since 2026-11-15, a delay of none too (each block written into
it: two channels of the block's length). The same song heard, measured just before and after, `windows-profile`, **while
other builds ran**: 44.89 % / 18.79 % playing without the rings, 48.34 % / 14.85 % with them: within what varies from
one run to the next; with them, on 2026-11-16 on a quiet machine, 42.97 % / 11.92 %. The rings' memory: 128 KB a ring
at 48 kHz (16,384 samples on two channels), 512 KB at 192 kHz.

The paths from one point of a strip share one ring since 2026-11-15 (its tap: a track's sends after its fader read its
sound before its mute, its way to the master its output), which the strip writes once a block; before, each path had
its own. `Benchmark: 200 tracks each sending to 8 returns` (each track a Synth holding a chord, 8 returns with an EQ
each, one track's 64 samples of latency lining the others up; blocks of 10 ms played back to back), two runs of each,
the ring per path switched back on for the "before" runs, `windows-profile`, 2026-11-15 (the two to be compared with
each other), and the run of 2026-11-16 on a quiet machine:

| | Rings | Their memory, 48 kHz / 192 kHz | Playing at 48 kHz, 1 thread / 4 (mean) | At 192 kHz, 1 thread / 4 (mean) |
|---|---|---|---|---|
| A ring per path | 1,808 | 226.9 MB / 904.9 MB | 139.2 and 140.1 % / 47.6 and 50.0 % | 561.0 and 551.1 % / 200.5 and 182.1 % |
| A ring per tap | 408 | 52.7 MB / 210.2 MB | 145.7 and 131.0 % / 50.3 and 45.5 % | 517.9 and 635.3 % / 165.1 and 206.6 % |
| A ring per tap, 2026-11-16 | 408 | 52.7 MB / 210.2 MB | 96.4 % / 26.4 % | 372.8 % / 101.1 % |

The block's cost is the synths' (within what varies from one run to the next either way); the memory falls by
three quarters. A render keeps no room in its rings, and none for a path with no delay or for what it leaves out
(`tests/PerformanceGateTests.cpp` counts its samples): the stems benchmark below peaked at 138.5 MB for the whole test
process with a ring per path and room in renders, and at 130.8 MB now.

### Benchmark: the benchmark song's stems exported

The benchmark song (`tests/BenchmarkSong.h`) of 20 tracks and 16 beats, its 4 stems at 48 kHz, each a render of one
channel as it reaches the master. A render leaves out what it can't hear (2026-11-15): what doesn't reach the channel,
and a muted track that feeds nothing heard. The two ways measured on 2026-11-15, `windows-profile`, **while other
builds ran**; the last column is the export as it is now, on 2026-11-16 on a quiet machine.

| Song | Each render playing the whole song | Leaving out what it can't hear | Now, 2026-11-16 |
|---|---|---|---|
| As it is | 28.32 s | 13.75 s | 7.91 s |
| A quarter of its tracks muted | 28.44 s | 10.59 s | 5.86 s |

Each stem compiled as its turn comes (known bug 125, 2026-11-16): the export compiled every stem's player and plan on
the message thread before the first started, and held them all until it ended. The message thread's time in the call
that starts the export, and the most the process's memory grew by while it ran, the two builds one after the other as
for the song of 100 tracks below, **while other builds loaded the machine**; "now" is the run of 2026-11-16 on a quiet
machine:

| Song | Before the first stem starts: before / after | Memory's peak, above what it was: before / after | The export: before / after |
|---|---|---|---|
| As it is | 127 ms / 17 ms; now 14.0 ms | 112.5 MB / 32.1 MB; now 22.0 MB | 12.69 s / 10.43 s; now 7.91 s |
| A quarter of its tracks muted | 104 ms / 20 ms; now 13.2 ms | 112.5 MB / 28.7 MB; now 24.4 MB | 8.32 s / 8.23 s; now 5.86 s |

The song has 4 stems (its tracks are in buses): a song of more stems saved more.

| The whole process's CPU while it plays, blocks coming one each block's length | % of a core | From |
|---|---|---|
| 20 empty tracks: four threads / one | 9.4 / 2.3 | 2026-11-16 |
| 20 tracks of a synth, an EQ, a compressor and a reverb, chords: four threads / one | 53.5 / 41.4 | 2026-11-16 |
| The same, every track muted: four threads / one | 59.0 / 46.1 | 2026-11-16 |
| 20 drum samplers, a beat: four threads / one | 12.1 / 3.5 | 2026-11-16 |
| 20 plug-in synths in a helper (Phazite's process alone): one thread / four | 4.7 / 5.1 | 2026-11-16 |

Section 9 of the audit, before the changes that made the devices' song cost more to play (sections 15 and 19), had 7.4
for the 20 empty tracks on four threads and 39.4 / 30.8 for the 20 tracks of synths; section 10, two runs each, 5.1 and
2.7 / 13.3 and 10.2 for the plug-in synths.

### Benchmark: a song of 100 tracks, its blocks at 128 and 512 samples, its plan and an edit

The benchmark song (`tests/BenchmarkSong.h`) of 100 tracks, every one playing all the way through: an instrument on
each, in turn a Synth, an FM synth, a Drum Sampler and a Sampler, each with a part that suits it; an EQ
and a compressor after it, a saturator on every third track and a Motion (a chorus) on every fifth; a send to a reverb from every
track and to a delay from every other; buses of ten, each with a compressor; and automation on every fourth track's
volume and every Synth's cutoff. Ten seconds of blocks at 48 kHz, played one after another on the test's thread as the
audio device's thread plays them. The figure is each block's time as a share of the block's length, 2.67 ms at 128
samples and 10.67 ms at 512: over 100 % is a dropout.

Measured on 2026-11-16 on the PC above, from `windows-profile`, one run, on a quiet machine.

| Blocks of | Threads | Stopped: mean / 99th percentile / most | Playing: mean / 99th percentile / most |
|---|---|---|---|
| 128 samples | 1 | 7.6 % / 12.1 % / 20.8 % | 175.4 % / 253.0 % / 298.3 % |
| 128 samples | 4 | 4.2 % / 23.8 % / 36.3 % | 46.5 % / 65.7 % / 91.5 % |
| 512 samples | 1 | 4.8 % / 7.2 % / 9.2 % | 165.5 % / 196.4 % / 245.5 % |
| 512 samples | 4 | 2.5 % / 4.3 % / 5.2 % | 44.0 % / 54.9 % / 70.6 % |

| The message thread, with 100 tracks | Median | Most |
|---|---|---|
| A plan with nothing changed, given to the engine | 1.44 ms | 1.98 ms |
| A note added, with its new plan | 1.57 ms | 1.85 ms |
| A clip moved, with its new plan | 1.55 ms | 1.69 ms |
| A note added after every track's height changed | 1.54 ms | 1.97 ms |
| A whole compile, as a render's player makes it (every device made) | 85.8 ms | 91.7 ms |
| Save, after the song played and was edited, for a project file of 3,118 KB | 128.3 ms | |
| Save again, nothing changed | 110.6 ms | |

A save's parts, by temporary timers on the 40-track song of "saving and opening a big project" (2026-11-16, 1,353 KB):
the song written as text 17 ms, the file written and flushed to the disk 33 ms (`writeWholeFile()`), the copy kept to
compare with 2.5 to 4.8 ms, the backup 1.6 to 2.5 ms, the plug-ins' states stored 1.1 to 1.4 ms. The known-bugs round
added to a save only a test's hook and a debug build's check (`isLetThrough()`, `holdFolder()`); the 128 ms above
against 106 to 113 ms before is the disk's flush, which moves by as much from one save to the next (110.6 ms for the
save after it, nothing changed).

A plan compiles again only what changed (known bug 145, 2026-11-16). Measured from `windows-profile` with the build
before the change and the build after it run one after the other, **while other builds and a ctest run loaded the
machine** (so both are high; compare them with each other); "now" is the run of 2026-11-16 on a quiet machine:

| The message thread, with 100 tracks | Before | After | Now |
|---|---|---|---|
| A plan with nothing changed, given to the engine | 45.3 ms (most 54.4) | 1.95 ms (most 3.2) | 1.44 ms (most 1.98) |
| A note added, with its new plan | 45.5 ms | 2.04 ms | 1.57 ms |
| A clip moved, with its new plan | 46.6 ms | 2.16 ms | 1.55 ms |
| A whole compile, as a render's player makes it (every device made) | 219 ms | 114 ms | 85.8 ms |

Where a compile's time went before, by temporary timers around its parts (a plan with nothing changed, another run under
more load, 97.8 ms in all): reading every device's values from the song again, 93.1 ms, half of it to see whether the
values changed the device's latency and half to set them, each value's parameter name made a `juce::Identifier` from a
string (a lookup in JUCE's pool of names, under a lock); everything else 4.7 ms (the channels and their order 0.5, the
inputs and their delays 1.0, the notes and audio 0.2, the faders, modulation and plug-ins told what moves 0.4,
audibility 0.2, the rings 0.5, the automation 0.1). Now a device's values are read once its channel changed, each name
made once for each kind of device (what made the whole compile twice as fast); a track's notes and audio once its clips
changed; a curve once its points did. What joins the channels is worked out whole each time (2 ms at 100 tracks, under
load).

Measured again after the change's review (the channel marked only for a change of its devices, not of its height, mute
or a plug-in's stored state), the same two builds one after the other under heavier load: a plan with nothing changed
88.5 ms before and 3.2 ms after, a note added 90.5 and 3.4 ms, a clip moved 90.4 and 3.4 ms; and a note added after
every track's height changed, 4.3 ms (before the review it read every device of the song again, as a whole compile
reads them); on a quiet machine on 2026-11-16, 1.54 ms.

What it says of scale:
- **One thread can't play 100 busy tracks:** it takes about 1.7 times each block's length, at either block size.
- **Four threads use under half of each block,** and the worst block of the run took 92 % of its length at 128
  samples. So the limit is near 100 busy tracks on four threads at 128 samples, and near 50 on one.
- **An edit compiles what it changed** (2026-11-16): at 100 tracks a note added or each step of a drag takes about
  1.5 ms of the message thread, where it took 40 ms, more than two frames, compiling the whole song.

### Benchmark: an everyday edit's block while its paths fade, on the benchmark song

Known bug 167's review: an edit that changes where sound goes while the song plays (here a send taken away, then given
back by Undo, on each of 40 tracks in turn) fades the path that leaves out on the plan played, which plays in place of
the new plan for those samples (never beside it), and the new path fades in by itself. The block in which the edit's
plan is taken, against the ten blocks before each edit, on the benchmark song of 100 tracks (as above) playing at 48
kHz in blocks of 480 on one thread; each a share of the block's 10 ms. Run of 2026-11-16, on a quiet machine.

| 100 tracks, 48 kHz, blocks of 480, one thread | Mean | 99th percentile | From |
|---|---|---|---|
| The blocks around the edits | 166.4 % | 200.6 % | 2026-11-16 |
| The block that takes a send taken away or given back | 167.2 % | 211.3 % | 2026-11-16 |

The edit's block costs what the others do, within what varies from block to block: the fades replace the old plan's
work for their samples rather than add a second plan's. (One thread can't play this song in real time, as the section
above says; the comparison is what matters here.)

### Benchmark: a song of 500 audio clips, its plan and an edit

50 tracks of ten audio clips, each clip a file of its own (a song of recordings), half a second long, in the project's
Audio folder: a clip moved and the tempo changed (which places every clip again), each with its new plan through the
song's session, and a whole compile as a render's player makes it (every file opened). Known bug 124: every plan asked
the disk about each clip's file and made the tempo map several times for each clip. Measured 2026-11-16 as the song of
100 tracks above, the two builds one after the other **while other builds loaded the machine**; "now" is the run of
2026-11-16 on a quiet machine (the median; the most in brackets):

| The message thread, 500 clips of 500 files | Before | After | Now |
|---|---|---|---|
| A clip moved, with its new plan | 52.6 ms | 4.0 ms | 3.38 ms (3.84) |
| The tempo changed, with its new plan | 61.8 ms | 7.5 ms | 5.55 ms (5.91) |
| A whole compile, as a render's | 291 ms | 237 ms | 188 ms (194) |

A track's audio is placed again only once its clips changed (or the tempo, for every track), with one tempo map for
the plan; the disk is asked about a file again only for a track whose clips changed. A whole compile opens every file,
which is most of its time.

### Benchmark: a loop's wrap and a jump on a song of 200 tracks

Known bug 85. Measured on 2026-11-16 on the PC above, from `windows-profile`: each figure is a block's time as a share
of a block of 128 samples at 48 kHz (2.67 ms). The figures of the run on a quiet machine are marked "2026-11-16";
the rest were measured the same day **while other agents' builds loaded the machine**.

The wrap's own work, alone (`Benchmark: a loop's wrap's own work`), the median of 50, the way before the change and the
way now each timed in the run of 2026-11-16: the notes sounding at the loop's start found on 200 tracks of 1,024 notes
each (a chord of four on every beat for 256 beats), and a block played from the loop's start by 40 clips of a stereo
file at 44.1 kHz.

| A loop's wrap, its own work | Before | Now |
|---|---|---|
| The notes sounding at its start found on 200 tracks | 8.9 % (a look through every note before it) | 0.5 % (each track's index of them) |
| 40 resampled clips playing their first block from its start | 36.8 % (each resampler started from nothing) | 0.9 % (each one's spare, made ready a little a block) |

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
blocks around moved by a quarter from run to run, so the loaded figures only show the order of what the wrap and the
jump add; the last two rows are the run of 2026-11-16 on a quiet machine.

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
| 2026-11-16, as the row above | 1 | 349 % | 411 % (+63) | 377 % (+29) |
| 2026-11-16, as the row above | 4 | 95 % | 106 % (+11) | 105 % (+10) |

The blocks after such a jump, each by itself (a temporary timer in the engine, the means of 20 jumps), in milliseconds of
a 2.67 ms block, on four threads: before the hold, steady 7.53, the fade's first block 8.76 (its 40 clips' resamplers
started there: 2.25 ms of the strips' tasks' time) and the landing 7.89 (the resets of every device: 2.26 ms of the tasks'
time); now steady 6.30, the fade's first 6.74, a block held 3.19 (silent but for the fade's last 16 samples), the landing
5.93, then 4.4 to 4.9 while the spares are made ready again for the loop's start. On one thread, the same shape: steady
22.0, fade 20.2, held 5.6, landing 18.6.

Play, and a jump while the song plays, reset every device and every path's ring, which starts the song from silence
(`Benchmark: each device's reset`, 2026-11-16): a ring's reset writes nothing now, its past silence: 400 rings of a
third of a second at 48 kHz
in 0.1 % of a block, where clearing them, as a reset did before, takes 18.2 %. A device's own reset takes 0.02 % of a
block at most for the instruments, 0.32 % for the delay (its seconds of buffer) and 0.17 % for the reverb, 0.02 % or
less for the rest: which would make about 4 % for the song's 600 devices; measured in the landing block under load,
with what each runner resets of its own, they took 1.7 ms of one thread (64 % of a block; 2.3 to 2.7 ms of the tasks'
time on four, shared by them), and the chase of every track 0.3 ms.

A reset that writes nothing of a delay line was tried and taken back (2026-11-20, the audit of the threads): a line
counted what came in since its reset and read what was older as silence, which took the Delay's reset from 0.35 % of
a block to none and the Reverb's from 0.6 % to none (run with other builds going on the PC, each build twice in turn),
but put a compare on every read of a line: the Reverb went from 0.91 to 1.29 % of a core (its plate from 0.42 to
0.59, its hall from 1.51 to 1.98), the Delay from 0.23 to 0.27, the Compressor from 0.45 to 0.51, the Multiband from
1.44 to 1.67, Motion's chorus of three voices from 0.37 to 0.44. A cost on every sample for as long as a device plays,
against a tenth of one block at Play for ten Delays and ten Reverbs: the lines are cleared whole, as they were. The
EQ's linear phase keeps its own count, which costs nothing for each sample (its history read in two runs, its
partitions summed up to those filled).

The message thread's wait as a new plan keeps each clip's start in its file (`AudioFile::keep()`), while a reading
thread reads a file whose reads take 20 ms each, as a cold read of a slow or busy drive can
(`Benchmark: what the message thread waits for`): before, a median of 25 ms and as long as 1.2 s, as the reading thread
held the file's lock across each read and took it again for the next; now (2026-11-16) a median of 0.002 ms and at
most 0.006 ms, keep() taking a lock of its own that no read holds.

The chase's index of the controllers (`Benchmark: a long take dense with pressure`, 2026-11-16): each controller's
changes a list of places among the events, 4 bytes a change. A take of a minute, ten keys' pressure at 200 values a
second (120,000 values): a note added and the song compiled in 17.3 ms (the median), its sequence holding 3.2 MB; five
minutes at the bound a take keeps to (a thousand values a second for each key, 3 million): the sequence made in 196 ms,
holding 80 MB, of which 72 MB are the events themselves. The chase at a wrap on a track of sixty controllers with 2,000
changes each and ten keys' pressure: 0.037 % of a block of 128 at 48 kHz.

### Benchmark: saving and opening a big project

| A song of 40 tracks and 10,240 notes, of Phazite's own devices | Time | From |
|---|---|---|
| Save, for a project file of 1,353 KB | 48.1 ms | 2026-11-16, the last run, on a quiet machine |
| Save again | 54.6 ms | 2026-11-16, the last run, on a quiet machine |
| Autosave | 39.8 ms | 2026-11-16, the last run, on a quiet machine |
| Open | 47.9 ms | 2026-11-16, the last run, on a quiet machine (62.6 ms before that day's fix) |

Open took 28 ms when the audit measured it (section 12, 2026-11-04). Since then the project file holds each property
on a line of its own (2026-11-09: 1,353 KB for this song), whose parse by JUCE (`juce::XmlDocument`) and the tree made
of it take about 27 ms of the open alone, and the loader's checks (`makeConsistent()`) grew with the format audit
(2026-11-09) and the known-bugs round (numbers held as Phazite writes them, switches and IDs, exponents; known bugs 170
and 179). Those checks took 28 ms of the 62.6 ms; on 2026-11-16 their waste was taken out, every check kept: a type
looked up by its name's text in every walk (now by its pooled pointer), a list of each element's properties copied to
find the few that name an ID, a set's node for each note's ID and each element's keys, every row of the tables scanned
for each element, a copy of each number's text to read it, and each number written as text to see whether its file
wrote it as Phazite would (now told from the text itself, for the forms Phazite writes: `isPlainlyAsWritten()`), and a
second count of the song's IDs. Before and after, the two builds run in turns three times on the busy machine:

| | Before | After |
|---|---|---|
| Open, 40 tracks | 63.9 to 107.4 ms | 53.7 to 102.9 ms |
| `makeConsistent()` of 100,000 notes, the median of 5 | 372.6 to 478.1 ms | 189.4 to 224.2 ms |
$1
On a quiet machine, the last run of 2026-11-16: Open 47.9 ms, `makeConsistent()` 179.1 ms, `joinSong()` 526.5 ms.

What is left of an open is JUCE's parse and tree (about half), the checks' walks of every element (each a few
properties looked up and read), and the song's first plan.

### Benchmark: what a branch switch and a merge spend on the message thread, by part

The 32-track benchmark song of 64 beats, `windows-profile` (known bug 126): before, three runs, measured 2026-11-15;
after, the run of 2026-11-16 (the switch back to the first branch in brackets). Before, both commands read their songs,
split the song left and worked the merge out on the message thread, in the call; now they read and work out on the
history's thread, and the message thread is kept by the call and by the message that takes what was read in (the song
made the other branch's, its plan compiled; the merge's list shown).

| | Before: the call | After: the call | After: the longest message after it | After: the whole, start to end |
|---|---|---|---|---|
| A branch switch | 280 to 310 ms | 1.1 ms (1.2) | 78.0 ms (79.4) | 212.7 ms (197.7) |
| The start of a merge | 208 to 225 ms | 1.3 ms | 4.3 ms | 175.9 ms |
| Songs read, and split, on the message thread (switch, merge) | 2, 2 | 0, 0 | | |

A commit's song read, which now passes the loader's checks as it is joined (`joinSong()`), takes 53 ms on its thread
(2026-11-16; 61 to 80 ms before the checks, 2026-11-15); the song split into pieces 14 ms (33 to 37 ms before). What
still runs on the message thread of a DAWproject import as a branch: the commit's song made a song of its own
(`makeSong()`, which belongs there) with what came back taken in, its audio copied out of the file
(`takeDawProject()`): 22 ms for the song made (2026-11-16), plus the copy, which depends on the file.

The same loader on a song of 100,000 notes (`Benchmark: a song of 100,000 notes made consistent as it opens`,
2026-11-16, the median of 5): `makeConsistent()` 341.6 ms, `joinSong()` 698.7 ms on the quiet machine; 189 to 224 ms and
524 to 676 ms after that day's fix, on a busy one (where the build before it took 373 to 478 and 713 to 882 ms; "saving
and opening a big project" above).

### Benchmark: rendering audio clips stretched and pitched

| A clip of stereo sound | Rendered in | Times as fast as it plays | From |
|---|---|---|---|
| 8 s, stretched from 120 to 100 BPM | 910 ms | 8.8 | 2026-11-16 |
| 8 s, pitched up 3 semitones | 979 ms | 8.2 | 2026-11-16 |
| 60 s, stretched from 120 to 100 BPM | 6,763 ms | 8.9 | 2026-11-16 |
| 60 s, pitched up 3 semitones | 7,158 ms | 8.4 | 2026-11-16 |

### Benchmark: audio clips of files at another rate than the engine's

A clip of a file at another sample rate, resampled as it plays on a 48 kHz engine; the figure is for one stereo clip
of 64 playing at once.

| File | % of a core, a clip | From |
|---|---|---|
| 48 kHz, as the engine's (nothing resampled) | 0.002 | 2026-11-16 |
| 44.1 kHz | 0.226 | 2026-11-16 |
| 96 kHz | 0.163 | 2026-11-16 |

### Benchmark: a loop previewed in time, until it can start

Four bars, stretched from 120 to 100 BPM: ready to start in 99.5 to 104.4 ms, and all of it made in 999 to 1,008 ms
(three tries, 2026-11-16).

### Benchmark: the UI's work on a big song

The message thread's work on a song of 16 tracks, 256 clips and 16,384 notes, in the whole window.

| What | Time | From |
|---|---|---|
| A parameter's change, as the views handle it | 0.03 ms | 2026-11-16 |
| The whole window painted, its first paint | 20.7 ms | 2026-11-16 |
| The whole window painted, its second paint | 7.2 ms | 16 |
| An 11-pixel column of the window, as a playhead repaints it | 0.79 ms | 2026-11-16 |
| ... of the playlist / of the piano roll | 0.23 / 0.15 ms | 2026-11-16 |
| The check for unsaved changes after a notification that changes nothing: the first / the mean of 100 | 0.01 / 0.00 ms | 2026-11-16, after the fix |
| ... after a note added and taken back (its clip compared) | 0.04 ms | 2026-11-16, after the fix |
| ... after a track moved and moved back (every track compared) | 8.29 ms | 2026-11-16, the last run, on a quiet machine |
| Save | 79.7 ms | 2026-11-16, the last run, on a quiet machine |
| Autosave, on the message thread | 3.9 ms | 2026-11-16, the last run, on a quiet machine |
| A note moved a step, with what follows | 3.62 ms | 2026-11-16, the last run, on a quiet machine |

The note's row moves by 2 ms from run to run of one build. Save, which took 103 ms in section 12 and 145 ms in section
16, took 91.2 ms and then 79.7 ms in the two runs of 2026-11-16.

The check for unsaved changes (the title's star and Auto Save ask it after every notification) took 22 ms in the run
of 2026-11-16, 0.78 ms in section 7. Two causes, both older than the known-bugs round: since the versions came
(2026-11-04), the comparison with the saved copy writes every value of both as text to compare them (`isSameValue()`,
`src/model/base/SongText.cpp`), a `juce::String` each; and any notification on a track compared every track, as the
song's element it is in is the track list. Now the values are compared without making text (the same text is still
what decides), and a property that notified is compared alone, an element whose elements changed whole, each at its
place in the saved copy; past 64 notifications at once, the track each is in. Before and after, the two builds run in
turns three times on the busy machine:

| The check for unsaved changes | Before | After |
|---|---|---|
| After a notification that changes nothing: the first / the mean of 100 | 22.1 to 34.4 / 24.5 to 27.6 ms | 0.01 to 0.02 / 0.00 ms |
| After a track moved and moved back, every track compared | 21.2 to 35.6 ms | 8.1 to 9.9 ms |

### Benchmark: frosted glass behind popups over a big song

What a popup's glass costs over the song above, at 100 %: taking what is behind it once as it opens or moves, and each
paint of the glass after that.

| A popup | Taken as it opens | The glass painted | From |
|---|---|---|---|
| A tooltip, 220 x 30 | 0.49 ms | 1.35 ms | 2026-11-16 |
| A menu, 220 x 320 | 1.94 ms | 0.85 ms | 2026-11-16 |
| The command search, 530 x 480 | 5.30 ms | 1.17 ms | 2026-11-16 |

Section 14 of the audit had 0.33 / 1.13 ms, 1.73 / 0.69 ms and 4.71 / 1.08 ms. Nothing of the known-bugs round is in
a popup's glass; the build before the round's last fixes and the build after them, run in turns on a busy machine
(2026-11-16), both took 0.62 to 0.73 ms for the tooltip: the machine's load, not a change.

### Benchmark: scrolling and zooming a big song

The playlist with 100 tracks, 2,400 clips and 38,400 notes, in the whole window.

| What | The message thread's work | From |
|---|---|---|
| A wheel notch, scrolling down / up | 0.39 / 0.36 ms | 2026-11-16 |
| Shift and a wheel notch, scrolling in time | 0.59 ms | 2026-11-16 |
| Ctrl and a wheel notch, zooming in / out | 0.67 / 0.66 ms | 2026-11-16 |
| The mouse moved a pixel over the tracks | 0.13 ms | 2026-11-16 |
| The mouse moved from one half of a clip to the other | 0.18 ms | 2026-11-16 |
| A clip moved a beat, as each step of a drag moves it | 2.43 ms | 2026-11-16 |
| The playlist painted, after each of the above | 1.17 to 2.00 ms; 3.21 ms after the clip's move | 2026-11-16 |
| Clip components in the playlist | 12, those in view | 2026-11-16 |

Section 12 of the audit had 0.27 ms for a wheel notch, 0.53 ms for a zoom, 8 clip components. The clip's move took 3.2
to 3.3 ms in section 12 (0.8 ms of it the new plan and 1.5 ms the playlist's work) and 8.1 ms in section 16; 2.43 ms on
2026-11-16, since a plan compiles again only what an edit changed.

What a wheel notch costs (a sampling of the message thread over 300 notches, 2026-11-16): about half is the playlist
placing its clips (`layOutClips()`, which works out the bounds of every clip of the song, 2,400 here, to find those in
view), a quarter the mouse's move that follows the scroll (`getClipAt()`, which looks through every clip too), and a
tenth the track headers made and deleted as their rows come into and leave the view, which the mixer and playlist's
change of the known-bugs round brought (only what is in view is made, with one row more at either end, which the Tab
key goes on to: the 12 clip components here, 8 before). That tenth is the price of the playlist of 200 tracks making 19
headers rather than 200; the rest is older, and grows with the song's clips (in the backlog).

### Benchmark: the mixer of 200 tracks

200 tracks, each with a synth and an EQ, in a mixer 1600 pixels wide (about a dozen strips in view). The song's plans
aren't compiled (its session is lent, as to a render), so that the times are the mixer's work alone; a strip shows a
change at its next tick, eight times a second. Measured on 2026-11-15 from `windows-profile` on the PC above, **while
other builds loaded the machine**, before and after the fix of known bugs 127 and 144 (only the strips in view, and
only the changed channel's brought up to date). "Now" is the run of 2026-11-16 on a quiet machine.

| What | Before | After | Now |
|---|---|---|---|
| Opened: its strips made and laid out | 546 ms | 16.5 to 16.8 ms | 5.6 ms |
| Strips made / components | 200 / 6,025 | 13 / 415 | 13 / 415 |
| At rest: a tick of the strips, eight a second | 42.7 ms | 1.5 ms | 0.83 ms |
| A device added to a track in view: the change heard / shown at the next tick | 0.20 / 84.4 ms | 0.25 / 0.5 to 4.2 ms | 0.41 / 0.58 ms |
| A device added to a track out of view: the same | 0.24 / 171 ms | 0.29 to 0.68 / 0.9 to 2.0 ms | 0.35 / 0.28 ms |
| A fader's drag: a step / the next tick | 19.7 / 18.1 ms | 0.10 to 0.63 / 1.1 to 2.5 ms | 0.76 / 0.11 ms |
| Scrolled a strip sideways | 0.02 ms | 1.6 to 2.7 ms, a strip made as it comes into view | 0.72 ms |
| The scroll bar's thumb dragged end to end in 60 events: each event / the worst | | 6.9 / 11.1 ms; 9.2 / 33.7 ms under a heavier load | 4.85 / 10.68 ms |
| ... each event with what it left for the next turns of the message loop | | 9.7 ms; 18.4 ms under the heavier load | 5.00 ms |

What is left of a tick, about 0.06 ms a strip, is the model's lookups of each strip's values by ID (known bug 122).

A device added in view and a fader's step move from run to run more than "after" and "now" differ: the builds before
and after the round's last fixes, run in turns on a busy machine (2026-11-16), took 0.21 to 0.99 ms for the device
added in view and 0.37 to 0.94 ms for the fader's step, either build.

The thumb's drag was measured after the review's round (2026-11-15), whose first version made every strip a jump brought
into view, and deleted every one it took out, at once: 10.5 ms an event and 24 to 34 ms at the worst, over a frame. A jump
now makes and deletes what 6 ms of a frame can bear (two strips at least), the rest at the next turns of the message
loop; a strip also reads its channel once as it is made (twice before). Under load the two strips it always makes can
take a frame by themselves.

### Benchmark: the playlist of 200 tracks and 4,800 clips

200 tracks, each with 24 clips of 4 bars and 16 notes (4,800 clips, 76,800 notes), in a playlist 1600 by 900 pixels,
its plans not compiled, as above. Before and after measured the same day, the same way, **under load**; "now" is the
run of 2026-11-16 on a quiet machine.

| What | Before | After | Now |
|---|---|---|---|
| A note added to a clip in view: the work / asked to repaint | 306 ms / 1400 x 900 px | 2.9 to 3.0 ms / the clip, 384 x 47 px | 1.37 ms / 384 x 47 px |
| A note added to a clip out of view | 302 ms / 1400 x 900 px | 3.5 to 3.7 ms / nothing | 1.34 ms / nothing |
| A clip moved a beat, as a drag's step moves it | 294 ms / 1400 x 900 px | 3.5 to 3.9 ms / its row's part and the overview, 1400 x 184 px | 1.42 ms / 1400 x 184 px |
| A wheel notch, scrolling down / up | 9.1 / 1.3 ms | 7.3 to 7.5 / 1.0 ms | 4.52 / 0.64 ms |
| A track renamed | 326 ms | 130 ms | 13.3 ms |
| A track added at the end | 384 ms | 162 ms | 16.7 ms |
| Headers / clip components / components | 200 / 72 / 1,506 | 19 / 72 / 239 | 19 / 72 / 239 |
| The rows' scroll bar's thumb dragged top to bottom in 60 events: each event / the worst | | 4.5 / 7.9 ms; 6.1 / 17.0 ms under a heavier load | 0.77 / 0.98 ms |
| ... each event with what it left for the next turns of the message loop | | 23.8 ms; 141 ms under the heavier load | 9.28 ms |

What was left of a track renamed or added, after the change, was the playlist reading every clip again (`getClips()`,
about 0.6 ms a track of 24 clips, 122 ms for the 200), which was the model's lookups (known bug 122): a clip's
information looked its track up by ID, and an audio clip itself three times. The song's index (below) took
`getClips()` for the 200 tracks to 5.5 ms. A clip moved still reads its own track's clips again.

A jump of the rows (the thumb dragged) makes the headers, automation lanes and clips that come into view as 6 ms of a
frame can bear (four at least), the rest at the next turns of the message loop: before, the first version made them all
at once, 16 ms an event and up to 230 ms. What it leaves is mostly the clips' notes read from the song
(`getNoteShapes()`: about 0.45 ms a clip of 16 notes before the song's index, 0.0025 ms with it).

### Benchmark: a song scrolling continuously as it plays, each frame painted as the window paints it

600 frames of the example song House Groove at 22 pixels a beat and 1920 x 1080, painted through Direct2D.

| Each frame | Mean / 95th percentile | From |
|---|---|---|
| Its work: the song's position shown, and what that posts | 0.12 / 0.28 ms (0.38 / 0.55 ms until the scroll bar kept the content's end it had, the same day) | 2026-11-16 |
| Its paint | 4.46 / 4.81 ms | 2026-11-16 |
| ... of which the lanes, with their clips and automation lanes | 1.23 / 1.39 ms | 2026-11-16 |
| ... of which the MIDI clips | 0.48 / 0.61 ms | 2026-11-16 |
| ... of which the audio clips | 0.35 / 0.49 ms | 2026-11-16 |
| ... of which the ruler, the sections' strip and the chord lane | 0.68 / 0.75 ms | 2026-11-16 |
| The first frame's paint, which makes the clips' pictures | 36.3 ms | 2026-11-16 |
| Frames whose work and paint took more than half a frame (8 ms) | 2 of 600, the first among them | 2026-11-16 |

### Benchmark: the mouse moving over the playlist while the song scrolls continuously, at the display's pace

1,200 frames paced at 60 a second, the mouse moving over the track headers and the lanes, four moves a frame.

| What | Value | From |
|---|---|---|
| A move of the mouse over the playlist, mean / most: without the window's tooltips / with them | 0.01 / 0.31 ms; 0.01 / 0.07 ms | 2026-11-16 |
| A frame's work with its moves, mean / 95th percentile / most: without tooltips / with them | 0.46 / 0.86 / 2.40 ms; 0.46 / 0.86 / 1.70 ms | 2026-11-16 |
| A frame's paint, mean / 95th percentile / most: the mouse still / moving without tooltips / with them | 6.71 / 9.36 / 32.2 ms; 9.08 / 10.45 / 14.5 ms; 9.01 / 10.64 / 18.0 ms | 2026-11-16 |
| Frames whose work and paint took more than 8 ms, of 1,200: the mouse still / moving without tooltips / with them | 482; 1,200; 1,128 | 2026-11-16 |
| Frames missed: the mouse still / moving without tooltips / with them | 1; 0; 1 | 2026-11-16 |
| A tooltip shown over the lanes, on frosted glass: median / most, over clips kept as pictures / painted afresh | 0.27 / 0.62 ms; 0.28 / 0.39 ms | 2026-11-16 |

Section 16 of the audit had a move's most at 1.0 ms, a frame's work at 0.56 / 1.60 / 2.2 ms, and no frame missed.

The mouse adds nothing to a frame's paint (looked for on 2026-11-16, after the run above). The paint's column was the
window's paint into the benchmark's Direct2D image and, as that image's `juce::Graphics` goes, JUCE copying the image
back from the graphics card (its persistent backup), which waits for the card and which a window's frame doesn't do;
that copy grows as a run goes on, the mouse moving or not. With the mouse still once more after it moved, and the
window's paint timed on its own (the benchmark's columns since; "frames over 8 ms" now count the work and the window's
paint), on a busy machine:

| 1,200 frames, three runs | The window's paint, mean / 95th percentile | With the image copied back, mean | Frames whose work and the window's paint took over 8 ms | Frames missed |
|---|---|---|---|---|
| The mouse still | 1.81 to 2.12 / 2.30 to 2.68 ms | 7.06 to 7.75 ms | 1 to 2 | 2 to 3 |
| The mouse moving, without tooltips | 1.74 to 1.96 / 2.19 to 2.51 ms | 9.27 to 9.74 ms | 0 to 2 | 0 to 2 |
| The mouse still again | 1.76 to 1.98 / 2.21 to 3.01 ms | 9.32 to 9.79 ms | 0 to 1 | 1 to 2 |
| The mouse moving, with the window's tooltips | 1.77 to 1.88 / 2.29 to 2.49 ms | 9.13 to 9.37 ms | 1 to 2 | 1 to 3 |

The frames missed here are those during which the busy machine ran something else (a frame's work and its window's
paint stay near 2.5 ms); on the quiet machine the run of 2026-11-16 missed 0 or 1.

The areas each frame asks to repaint are the same with the mouse moving as with it still, but for a header's small
part as the mouse goes over it. A sampling of the message thread found the window's paint about a fifth of the paint's
column, and the copy back the rest.

### Benchmark: the time bar's overview dragged, at each of the mouse's reports and at the display's frames

*The owner, 2026-11-16: "when scrolling the horizontal bar on a playlist song, it can be visually choppy/laggy", the
song playing, either scroll bar dragged, in the release build.* House Groove at 22 pixels a beat and 1920 x 1080, the
overview's part in view dragged 480 pixels along the bar and back, a pixel at each of the mouse's reports, sixteen
reports a frame (a pixel of the bar moves the song 6.1 pixels). JUCE takes every report Windows gives (the owner's
mouse gives up to a thousand a second), and a frame paints once no report is waiting: a frame bears its paint and the
work of every report since the last.

| `windows-profile`, 2026-11-16: mean / 95th percentile / most | Each report moving the view (until that day) | The view moved at the display's frames (now) |
|---|---|---|
| A report of the mouse | 0.45 / 0.62 / 1.64 ms | 0.02 / 0.06 / 1.61 ms |
| A frame's own work: the view moved, and what that posts | 0.02 / 0.06 / 0.12 ms | 0.09 / 0.15 / 0.55 ms |
| A frame's paint of what was asked to repaint | 3.94 / 6.01 / 6.82 ms | 3.33 / 5.80 / 6.88 ms |
| A frame's reports, work and paint at 125 reports a second (2.1 a frame): mean / at the 95th percentiles | 4.90 / 7.36 ms | 3.47 / 6.07 ms |
| ... at 500 (8.3 a frame) | 7.74 / 11.23 ms | 3.62 / 6.43 ms |
| ... at 1,000 (16.7 a frame) | 11.53 / 16.39 ms | 3.82 / 6.91 ms |

Two things made a report cost 0.45 ms, and neither was laying out or painting the song. Nine tenths of it was the
scroll bar brought up to date at every move of the view: it asked the model where the content ends, which looks
through the whole song (its clips, sections and chords; 0.39 ms for House Groove, more for a longer song). A move of
the view at each frame (a scroll by hand on its way, the playhead followed continuously) now keeps the end the scroll
bar has, which the editor's tick reads again a few times a second. And every report moved the view, fifteen of sixteen
to a place no frame showed: a scroll by hand now says where the view is to go (`TimelineEditor::scrollViewTo()`,
`scrollRowsTo()`), and the next frame takes it there, gliding sideways by parts of a pixel. `windows-debug`, before:
5.73 ms a report and 24.3 ms a paint, 120 ms for a frame's worth at a thousand reports a second.

At a thousand reports a second a frame's reports and paint took two thirds of its 16.7 ms on average before, and
all of it at the 95th percentiles; now a quarter, and under half.

### Benchmark: starting up, and the window at rest

| What | Value | From |
|---|---|---|
| The services, the commands and the window's content, at startup | 12.2 ms | 2026-11-16, the last run, on a quiet machine (20.0 ms in that day's first run: the row swings with the process's first fonts) |
| The first start's project opened and the window painted | 22.3 ms | 2026-11-16, the last run, on a quiet machine |
| The example song House Groove (64 bars) opened from Help and painted | 116.4 ms | 2026-11-16, the last run, on a quiet machine |
| At rest for 5 seconds, an example song open and stopped: what was asked to repaint | nothing | 2026-11-16 |
| At rest: the process's CPU | 1.25 % of a core | 2026-11-16, the last run, on a quiet machine |
| At rest: the message thread's CPU | 0.94 % of a core | 2026-11-16 |

The services, the commands and the window's content took 12.7 ms in section 14 of the audit. That row is the first
window made in the process, which is also the first text shaped and the first fonts read: the same made again 25 times
in one process (2026-11-16, a busy machine) took 12.7 to 29.7 ms, so one run's row moves by more than its change since
section 14. A sampling of those starts found no service or view of the known-bugs round dear (the library's listing
threads start only as a folder is listed); the largest parts were text laid out and shaped for the first time (the
Project view's lists, the tabs), every icon read from its SVG again for each button (about an eighth), and, here only,
the piano roll asking Windows where the mouse is as it starts (`NoteGrid::cancelGesture()`, the same before the round),
a call that is slow on this test machine's desktop. Each icon is now read once and copied for each button
(`createIcon()`, `src/ui/theme/Icons.cpp`). The two builds in turns, three times each, on the busy machine:

| | Before | After |
|---|---|---|
| The services, the commands and the window's content, the first | 23.1 to 132.8 ms | 21.8 to 31.2 ms |
| ... again, the median of 10 | 21.4 to 23.4 ms | 18.5 to 21.6 ms |

### Benchmark: a clip of 5,000 notes edited in the piano roll, and what views read of a song of 4,800 clips

Known bug 122: the model found each track, clip and note by looking through the song at every call, several times for each
note an edit touched, and the piano roll read every note of its clips, and the other tracks' notes, at each move of the
mouse. Since 2026-11-15 the song keeps an index of where its tracks, clips and notes are by their IDs
(`src/model/base/internal/SongIndex.h`), and the piano roll finds what is under the mouse among the notes it paints.

The first table: the piano roll in a window (`PianoRollScreen`), a clip of 5,000 notes, four a beat over twelve rows,
with another track's 5,000 as ghosts; each through the mouse or the command, the song's changes delivered after it, the
engine's new plan included. The second: the model's reads a view makes in a song of 200 tracks of 24 clips of 16 notes.
Before and after measured 2026-11-15, the `windows-profile` build, **on a machine other builds shared**; "now" is the
run of 2026-11-16 on a quiet machine.

| What | Before | After | Now |
|---|---|---|---|
| The mouse moved over the notes (hover) | 7.96 ms | 5.5 ms (finding the note under the mouse 0.013 ms of it; the rest is the hint and the cursor, which change at every move here, the notes a fifth of a pixel apart: not measured further) | 2.88 ms |
| ... over empty rows, where ghosts are looked for | 5.36 ms | 0.56 ms | 0.41 ms |
| A step of a rectangle selecting across the clip | 122.5 ms | 2.9 ms | 2.53 ms |
| A step of a drag of 500 notes | 22.1 ms | 4.5 ms | 3.26 ms |
| ... its Undo | 18.0 ms | 7.1 ms | 7.54 ms |
| Select All | 340.8 ms | 3.2 ms | 3.09 ms |
| Transpose Up, every note / its Undo | 2,377 / 356 ms | 22 / 12 ms | 21.9 / 9.5 ms |
| Quantize, every note / its Undo | 2,500 / 530 ms | 58 / 20 ms | 38.4 / 18.0 ms |
| Duplicate, every note / its Undo | 6,286 / 805 ms | 37 / 9.5 ms | 38.2 / 8.7 ms |
| Delete, every note / its Undo | 1,944 / 408 ms | 27 / 15 ms | 23.6 / 11.9 ms |
| Every track's clips (`getClips()`, 200 calls) | 253 ms | 5.9 ms | 5.49 ms |
| A clip's info by its ID (`getClip()`) | 0.173 ms | 0.0014 ms | 0.0013 ms |
| A clip found by its ID (`findClipState()`), alone | not measured | 350 to 440 ns, of which the check of the answer against the tree (it still has the ID, on a track in the track list) is about 250: 90 to 170 ns without it | 163 ns, with the check |
| A track's row (`getTrackRow()`) | 0.091 ms | 0.0026 ms | 0.0020 ms |
| A clip's note shapes (`getNoteShapes()`) | 0.127 ms | 0.0030 ms | 0.0025 ms |
| A clip moved, as a drag's step moves it (the model alone) | 0.134 ms | 0.0034 ms | 0.0027 ms |

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
| The tests' stand-in effect (a gain) | 1.6 / 2.9 / 30 µs | 35.3 / 102 / 1037 µs | 0.88 % of a core (0.59 % in the helper) | 1.3 MB empty, 1.4 MB with it | 2026-11-16 |
| Surge XT Effects | 21.0 / 67 / 147 µs | 54.1 / 142 / 251 µs | 1.46 % (1.46 % in the helper) | 17.3 MB | 2026-11-16 |
| Surge XT, a chord held | 80.1 / 200 / 1037 µs | 115.6 / 242 / 926 µs | 1.03 % (3.37 % in the helper) | 42.7 MB | 2026-11-16 |

A round trip costs about 34 microseconds a block (28 in section 10 of the audit). The benchmark also times a waiting
thread that spins for 30 µs, or until the helper answers, before it sleeps: each is slower than sleeping at once (a
median of 56 to 67 µs for the stand-in, 74 to 86 µs for Surge XT Effects, 136 to 147 µs for Surge XT) and costs more
of a core here.

The 34 against 28 microseconds is not a change of the known-bugs round: `main` and the branch, run side by side on
2026-11-16 **while other agents' builds and benchmarks loaded the machine**, gave the same (the stand-in in a helper, a
median of 23.7 to 34.6 µs in `main`, 34.3 to 35.3 µs in the branch; Surge XT Effects 57.3 and 63.8 µs against 29.5 to
52.7 µs; Surge XT 90.6 and 153.6 µs against 91.3 to 121.9 µs). This benchmark plays the helper's channel directly
(`sandbox::RemoteInstance`), and nothing on that path changed in the round; section 10 measured on an older build and
Windows.

### Benchmark: eight sandboxed synths in a block at 128 samples

Eight Surge XTs, a chord each, in a block of 128 samples.

| How they are played | Median | 99th percentile | Worst | CPU | Memory | From |
|---|---|---|---|---|---|---|
| In Phazite's process, one after another | 479 µs | 735 µs | 3.4 ms | 18.6 % of a core | | 2026-11-16 |
| In Phazite's process, on the engine's workers | 251 µs | 389 µs | 3.9 ms | 67 % (the whole process) | | 2026-11-16 |
| A helper each, each waited for in turn | 493 µs | 816 µs | 8.1 ms | 2.3 % here, 22.0 % in the helpers | 339 MB | 2026-11-16 |
| A helper each, all handed over first, then waited for | 275 µs | 362 µs | 1.7 ms | 1.9 % here, 66 % in the helpers | 339 MB | 2026-11-16 |
| One helper with all eight, each waited for in turn | 489 µs | 855 µs | 3.8 ms | 2.6 % here, 17.3 % in the helper | 244 MB | 2026-11-16 |
| One helper with all eight, handed over first | 259 µs | 338 µs | 1.2 ms | 3.2 % here, 37 % in the helper | 245 MB | 2026-11-16 |

The helpers' CPU varies most from run to run: side by side on 2026-11-16 under load, a helper each, handed over first,
47.8 to 51.0 % in `main` against 36.9 to 57.9 % in the branch, and one helper with all eight 43.7 to 50.5 % against 32.4
to 51.7 %. The 66 % and 37 % above, against the 54 % and 45 % of sections 13 and 10, are within that.

### Benchmark: what the browser's search and the commands that read audio files keep the message thread for

`tests/MessageThreadWorkBenchmarks.cpp` (known bugs 128 and 123): before, measured 2026-11-15; after, the run of
2026-11-16. Before: the browser searched the whole index on the message thread at each key, and an import, Normalize
and Quantize Audio read their audio there.

The browser's search, typing "kick deep" key by key into a library of 50,000 sounds (empty files in sample packs'
folders, indexed by the library) and an index of 500,000 made in the test:

| What | Before | After |
|---|---|---|
| 50,000 sounds: the message thread, per key | 60 to 83 ms (the whole search) | 0.05 to 0.08 ms (the search started; the rows made once its results are in) |
| 50,000 sounds: until the results show | the same | 4.4 to 11.3 ms |
| 50,000 sounds: the search itself, wherever it runs | 60 to 83 ms | 2.6 to 4.1 ms (search keys made with the index) |
| 500,000 sounds: the search itself, on its own thread | (the index stopped at 50,000) | 24.6 to 33.7 ms |

The browser's filters' counts, which a filter's menu shows (`the browser's filters' counts and its filtered places`,
2026-11-16, both ways timed in the run): before, counted on the message thread as the menu opened; now counted on the
search's thread, the menu opening once they are in.

| Words, filters | Before: the message thread (ms) | Now: the call (ms) | Now: the counts there after (ms) |
|---|---|---|---|
| none, none | 9.8 | 0.06 | 11.2 |
| "kick", none | 3.3 | 0.00 | 5.2 |
| none, Type Kick | 9.9 | 0.01 | 16.9 |
| "deep", Type Kick | 3.9 | 0.01 | 4.3 |

The places a filter leaves, each with how many of its sounds have its values (the Type filter set to Kick, in the same
library): before, listed on the message thread at each filter set and each change of the index, a walk of the index for
each place, 18.7 ms; now listed on the search's thread in one walk, the call 0.01 ms, the places there after 20.3 ms,
the tree showing the places it showed until then.

A folder of 20,000 sounds (empty WAV files) opened in the browser (`a folder of 20,000 sounds opened in the browser`,
2026-11-16, on a local SSD, both ways timed in the run): before, read from the disk on the message thread as it opened;
now, from the index where it holds the folder whole (the rows made from it on the message thread), and read again on a
listing thread all the same, to show what changed outside since; else read on a listing thread alone.

| How | The message thread (ms) | The rows there after (ms) |
|---|---|---|
| Before: read on the message thread | 351.6 | the same |
| Now: from the index | 9.98 | 10.1 |
| Now: the same folder read again in the background, whether it changed | none | 145.2 (nothing differs: the rows stay) |
| Now: read on a listing thread alone | 0.12 | 144.2 |

A folder of 1,000 presets the index doesn't hold (a library larger than it holds, or the first seconds of a start), each
preset's header read for its row on the listing thread (`a folder of 1,000 presets the index doesn't hold`, 2026-11-16,
on a local SSD): 163.7 ms until the rows are there, the message thread free meanwhile. So the rows come once with each
preset's kind, not twice.

A ten-minute stereo recording at 48 kHz, 24-bit (173 MB), with a file of the same name in the project already:

| Command | Before: the message thread | After: the call | After: the message thread in all | After: done after |
|---|---|---|---|---|
| Import (the file compared with the one there) | 512 ms | 0.07 ms | 5.6 ms | 71 ms |
| Normalize | 470 ms | 0.50 ms | 6.6 ms | 203 ms |
| Quantize Audio (its hits found first) | 4,247 ms | 0.19 ms | 98.8 ms | 2,086 ms |

"The message thread in all" is the call and everything the message thread ran until the change was made: the change
itself, the plan it compiles, the clip's render it starts, and the other timers. The reading is on a thread of its own;
the window takes no input meanwhile, and a job that takes longer than a second shows its progress window with Cancel.

Quantize Audio's change itself, on a ten-minute clip (`Quantize Audio's edit of 1,000 and 5,000 hits`): the edit, then
what the message thread runs for a second after it (the plan, the render's recipe). Most of what came after was the name
of the clip's render, a key of thousands of points built by appending to a string, which copied it whole at each point,
several times for each plan; written into a stream now (`getRenderFile()`, `src/model/arrangement/AudioClips.cpp`).

| Hits | The edit (ms) | After it, before (ms) | After it, now (ms) |
|---|---|---|---|
| 1,000 | 2.3 | 181 | 55.6 |
| 5,000 | 9.1 | 381 | 61.8 |

The edit and "now" are the run of 2026-11-16.

A sound tried on a drum pad by Swap (`a sound tried on a drum pad by Swap`, 20 tries): median 0.07 ms before and after
when it was first measured (a try copies nothing: the sound kept is copied in by its keep, in the background); on
2026-11-16, median 0.41 ms, slowest 0.53 ms. A sampling of 2,000 tries (2026-11-16) found nearly all of a try in the
pad's sound read on the message thread as the song's player takes the sound (`loadPadSound()`, `src/model/devices/DrumKit.cpp`,
as before the known-bugs round), most of that in the file system's calls (a file's size and attributes asked, its bytes
read): the test's sounds are written just before they are tried, which the system's indexer and virus scanner look at
first. So the change is the disk's, not Phazite's; a pad's sound read on the message thread is in the backlog.

## The gates

Timings can't be compared with stored numbers on a shared or another machine, so two kinds of gate fail a change that
makes Phazite slower without comparing a time with a number.

**The performance gates** (`tests/PerformanceGateTests.cpp`) run in every ctest run, on every preset. They count work,
which is the same on any machine (`tests/PerformanceCounts.h`):
- each edit gives the engine one plan, however many changes it makes in one turn of the message loop, and a value set
  by hand, a mute, a selection, the playhead moved or a save gives it none (`PlanCounter`);
- a new plan keeps what didn't change: a note added on one track compiles that track's notes again and no other's,
  and every device sounds on;
- an edit's plan reads the values of the devices on the channel it changed and no others, the automation curve it
  changed and no others (none for a note, a clip moved or the tempo), and asks the disk about the files of the track
  whose clips it changed, each once (none for the tempo), and about every file the song plays, each once, only once a
  second has passed since it last did (known bug 198); view state (a grid, a lane shown, a device's window moved)
  gives no plan; a track's height, a mute, a device's window moved or a plug-in's state stored (as every save stores
  it) leave the next note's plan reading no device's values; and an export of stems compiles its first stem alone
  before it starts (`countDevicesRead()`, `countCurvesRead()`, `countFilesAsked()`; known bugs 124, 125 and 145);
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
  track's and a clip's elements counted too (`countVisited()`); 14,988, 1,229,268 and 22,368 before, the walks by ID alone);
  and a step of a drag of 500 notes, or a paste of 500 past the clip's end, looks through its track's clips once, not
  once for each note: on a track of 200 clips under 1,000 elements more than on one of 20 (2,521 and 2,701 for the drag
  step, 1,020 and 1,200 for the paste, measured 2026-11-16; 13,501 and 103,501, 4,020 and 26,700 before).
- the piano roll's hover reads no notes from the song, and ten steps of its selection rectangle fewer than its clip
  holds, choosing each note once (`countNotesRead()`, `Selection::getNoteComparisons()`; 0 and 362 of 500 measured,
  15,000 and 9,181 before).
- an import, Normalize and Quantize Audio read no byte of audio on the message thread (`AudioWork::getMessageThreadReads()`,
  known bug 123), and a key typed in the browser's search looks through none of the library there
  (`Library::getItemsSearchedOnMessageThread()`, in `tests/BrowserTests.cpp`; known bug 128), nor does a filter's menu
  as it counts what it offers (in `tests/BrowserFilterTests.cpp`).
- a branch switch, a whole version's restore, the start of a merge and Merge read, split and write no song on
  the message thread (`VersionStore::countSongsRead()`, `countSongsWritten()`, `countSongsSplit()`; known bug 126).
- a stopped song's devices work no block once their sound has ended, each of Phazite's own (an instrument alone, an
  effect after a synth, and a reverb an LFO modulates): none in a second as the song opens, nothing played, and none in
  a second after Stop once each has rested (`DeviceRunner::getBlocksWorked()`; until 2026-11-16 a reverb that had heard
  nothing still played its two seconds of tail as a project opened, and the vinyl never rested).
- the check for unsaved changes (the title's star, Auto Save) compares no element after a notification that changes
  nothing, only the property that notified, and only its clip after a note added and taken back: 321 elements of the
  3,713 the song of 16 tracks holds, measured 2026-11-16; before, every track, whose every value it wrote as text
  (`Project::countElementsCompared()`).
- the loader's checks (`makeConsistent()`) of a song read from its text make fewer allocations than a tenth of its
  notes, and leave Phazite's own text as it is: 248 for 4,000 notes, measured 2026-11-16; about seven a note before
  (a text for each number compared, a list of each element's properties, a set's node for each note's ID and for each
  element's keys).

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

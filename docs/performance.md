# Performance

Phazite's performance targets, its benchmarks, and the latest number recorded for each.

**The numbers on this page are the latest that the performance audit records. None was measured again for this page.**
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
| Audio: no dropouts at a buffer of 128 samples (about 3 ms) with a typical electronic project, 20 to 40 tracks and a dozen third-party synths, on a mid-range laptop | On the PC above, with room to spare. Not measured at 128 samples for Phazite's own devices, and not on a laptop |
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

## Measured outside the benchmarks

| What | Value | How the audit has it |
|---|---|---|
| A key to its sound through Bluetooth headphones | about 170 to 300 ms | The stages added up, section 1: the link itself is about 150 to 250 ms, from published measurements |
| A key to its sound through a wired output, Windows' shared mode, 480 samples | about 20 to 50 ms | An estimate, section 1 |
| The same with a low-latency output at 128 to 256 samples | about 8 to 15 ms | An estimate, section 1; to be measured by hand |

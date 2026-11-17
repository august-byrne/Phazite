# Performance

*Design section 20.*

Phazite is meant to feel instant and to stay light: no dropouts at small buffers, a window that draws only what
changed, and a short delay between a key and its sound. This page holds the targets and the rules that follow from
them. What has been measured, and how, is in [../performance.md](../performance.md).

## Status

| Feature | State | Note |
| --- | --- | --- |
| Real-time rules for the audio thread, checked by an audit | Built | [../realtime-audit.md](../realtime-audit.md) |
| Several threads play each block | Built | |
| Audio files read from disk ahead of where they play | Built | |
| A display repaints only when what it draws moved | Built | |
| Saving and autosaving written in the background | Built | |
| First run picks a low-latency setup | Built | Windows; not yet tried by a person on many devices |
| Low Latency switch | Built | |
| Bluetooth outputs named, with what they cost | Built | |
| Devices plugged in offered in a notice | Built | |
| Latency and delay compensation shown in the audio settings | Built | |
| CPU load in the status bar and on each mixer strip | Built | |
| Freeze Track and Bounce in Place for heavy tracks | Built | [projects-saving-export.md](projects-saving-export.md) |
| Benchmarks of the devices, the engine (a song of 100 tracks among them), saving and the UI | Built | Run by hand; not part of `ctest` |
| Gates that fail a change that makes Phazite slower | Built | Counts of work in every `ctest` run; the benchmark gates and an hour's long run nightly |
| The targets below, measured on a mid-range laptop | Partly built | Measured on the development PC only |
| Tracks that aren't being edited pre-rendered in the background | Planned | After v1; no roadmap step |

## Targets

| Area | Target |
| --- | --- |
| Audio | No dropouts at a 128-sample buffer (about 3 ms) with a typical electronic project: 20 to 40 tracks and a dozen third-party synths, on a mid-range laptop |
| UI | 60 frames a second while scrolling and zooming the playlist and the piano roll with 100 or more tracks; near-zero CPU when idle |
| Startup | Under 3 seconds to a playable project on an SSD |
| Memory | Long audio streamed from disk rather than loaded into memory |

## The audio thread

The audio thread is the audio callback and everything it calls: Phazite's engine playing a block with its worker
threads, every device's processing, and a helper's thread that plays a plug-in's blocks. On it:

- no memory is allocated or released;
- no lock is taken and nothing is waited for;
- nothing is posted to the message thread;
- no file, network or log is touched;
- no exception is thrown and no loop is unbounded.

Threads talk through lock-free queues and atomic values. What the audio thread plays is handed to it whole and freed
on the message thread. Parameters are read from what the engine holds of them, never from the song. The rules are
in full in `CLAUDE.md` and in [CONTRIBUTING.md](../../CONTRIBUTING.md).

**Checked, not assumed.** The [real-time audit](../realtime-audit.md) counts allocations on the audio thread in
guarded scenarios, which fail if steady playback allocates, and lists what is known of the code Phazite builds on.
Every one of Phazite's devices has a guarded scenario. The sanitizer builds (AddressSanitizer and ThreadSanitizer)
run the tests in CI.

## How the engine stays fast

The engine's design is in [engine.md](engine.md). The parts that matter for speed:

- **Worker threads** help play each block, each strip a task, at the audio thread's priority.
- **Audio files are read ahead** of where they play by a reading thread, so the audio thread never waits for the
  disk, and a long file is never loaded whole.
- **The song is compiled into a plan** on the message thread. An edit hands the engine a new plan; a value set by
  hand reaches what plays at once, without a new plan.
- **A plug-in's block has a deadline** of its own length, so a slow plug-in makes its own blocks late and never the
  song's ([plugin-hosting.md](plugin-hosting.md)).
- **A device sleeps on silence** where it can.
- **Stretched, pitched and reversed audio clips play from renders** made in the background, not stretched as they
  play.

## The window

- **Nothing repaints in silence.** Meters, the playhead and other values that change many times a second are read
  from small read-only sources, polled by one ticker for all displays, or, for the playhead while playing, at the
  display's frames. A display repaints only when what it draws moved.
- Such values are never stored in the project or sent as change messages.
- A component that paints all of itself in opaque colours says so, so that what is behind it doesn't repaint with
  it.
- A view of many items, such as the playlist's clips, makes components only for those in view.
- The playlist and the piano roll scroll continuously while they follow the playhead: what is on the time line moves
  by parts of a pixel, and what is beside it stays put.
- **A scroll by hand costs a frame, not a report of the mouse.** A mouse reports up to a thousand times a second,
  and JUCE hands on every report; the time line's editors keep where a drag of the overview or of the rows' scroll
  bar, the wheel or a pan has taken the view, and move it there at the display's next frame, once
  (`TimelineEditor::scrollViewTo()`). A window that gets no frames moves it at once.

## Off the message thread

- The project file, the autosave and the history's files are written in the background from a copy of the song, so
  the window and what plays don't wait for a save.
- Exports, freezes, bounces and consolidations render on a thread of their own, with a progress window and Cancel.
- The changes since the last version, the hits in an audio file and the library's loop data are worked out in the
  background.
- No thread is ever ended by force. A thread is woken or cancelled, then joined.

## Heavy tracks

- **Freeze Track** plays a track from a file of its sound, so its devices take no CPU.
- **Bounce in Place** renders a track onto a new audio track.
- **CPU meters** show where the time goes: the engine's load in the status bar, and a bar on each mixer strip
  ([mixer.md](mixer.md)).

## Latency

- **First run:** on Windows, Phazite picks "Windows Audio (Low Latency Mode)" on the default output, with the
  largest buffer from 128 to 256 samples that the output offers. Where the output offers none, it keeps Windows'
  shared mode with the device's own buffer. The setup then becomes the chosen one, which the settings keep.
- **Phazite does not pick ASIO® by itself.** An ASIO driver takes its device from every other app, and which driver
  belongs to the output in use is only a guess from names. Where ASIO drivers are installed, the audio settings say
  that an audio interface's own driver gives less delay.
- **Low Latency** (`transport.lowLatency`, a switch in the transport bar), off at first: while it is on, what is
  played live has no delay compensation to wait for. Devices that look ahead by more than a millisecond, Phazite's
  own and plug-ins that report latency, are left out of what plays, and the output's buffer shrinks where the device
  offers a smaller one. Turning it off puts the buffer back. Nothing of it is saved.
- **Bluetooth outputs:** the audio settings and the welcome window say when the output is Bluetooth and what it
  costs, about 150 to 250 ms between a key and its sound that no driver reports, and suggest a wired output for
  playing and recording.
- **Devices plugged in:** an output or an input that appears while Phazite runs, and that it doesn't play or record
  through already, is offered in a notice in the bottom right corner of the window: "Headphones (USB) is connected.
  Play through it?" (for an input, "Record from it?"), with **Switch** and a button that closes it. A Bluetooth
  output says what it costs. Everything goes on working while it shows. Left alone, it goes after 15 seconds counted
  while Phazite is in front and the pointer isn't over it, and at once when its device goes or another driver is
  chosen. At most three show. Switch does what choosing the device in the audio settings does. Devices that are
  there as Phazite starts, or as another driver is chosen, are not offered.
- **Shown:** the audio settings show the buffer in samples and milliseconds, the output's latency as its driver
  reports it, how long a played note takes to sound, and the song's delay compensation with the devices that cause
  it.
- **Recordings** are placed by the latencies the driver reports ([recording.md](recording.md)).

## Measuring

- The benchmarks (`tests/Benchmarks.cpp`, run with `phazite_tests --test-suite=benchmark --no-skip` from the
  `windows-profile` build) time the devices, live notes, a project stopped and playing, a song of 100 tracks, saving
  and the UI's work. `tests/SandboxBenchmarks.cpp` times plug-ins in helpers.
- Performance work runs them before and after, and records the numbers in [../performance.md](../performance.md).
- Gates fail a change that makes Phazite slower, without comparing a time with a stored number: counts of the work an
  edit makes, in every test run (`tests/PerformanceGateTests.cpp`), and costs as ratios to a fixed reference within
  one run, nightly (`tests/BenchmarkGates.cpp`). The long run plays an hour of music while the song is edited
  (`tests/LongRun.cpp`). [../performance.md](../performance.md#the-gates) says what each can and can't catch.
- While the song plays, the display's frames are logged, and what they did is written to Phazite's log as the song
  stops.

## Decisions

- The targets name a mid-range laptop and a 128-sample buffer, because that is where a producer notices dropouts.
- The first run picks Windows' low-latency shared mode, not ASIO: it works with every output and takes the device
  from no other app.
- Low Latency is a switch the user turns on to play and off to mix, rather than something Phazite guesses.
- Look-ahead devices are bypassed under Low Latency rather than played late, so that what is heard is in time.
- A plugged-in device is offered, never switched to by itself, and the notice asks for nothing.
- Audio clips are stretched offline into renders rather than as they play: the best stretcher measured is too slow
  for the audio thread ([engine.md](engine.md)).
- Plug-ins are always hosted in helpers; the round trip measured cheap enough ([plugin-hosting.md](plugin-hosting.md)).
- Allocations on the audio thread are counted by a test, because reading the code does not find them all.

## Not built

- Background pre-rendering of tracks that aren't being edited: Planned, after v1.
- Plug-in scanning in the background as Phazite starts: Planned; scanning runs when the user asks for it.
- The targets measured on a mid-range laptop, and with a dozen third-party synths: Planned.

## In the code

- `src/engine/core/Engine.h`, `Plan.h`, `Handover.h`, `Workers.h`: the engine, what it plays, and how it is handed
  over.
- `src/engine/audio/FileReader.h`: files read ahead of where they play.
- `src/model/playback/internal/PlaybackHost.h`: the engine on the audio device, and the Low Latency switch.
- `src/model/engine/AudioDevices.h`: the first run's setup and Bluetooth outputs; `AudioDeviceWatch.h`: devices
  plugged in.
- `src/ui/shell/DeviceNotices.h`, `NoticeCard.h`: the notices; `src/ui/shell/SettingsPanel.h`: the audio settings.
- `src/ui/components/DisplayTicker.h`: the displays' one timer and what tells them whether to repaint.
- `src/ui/timeline/FrameLog.h`: the display's frames while the song plays.
- `src/model/base/BackgroundJobs.h`: jobs in the background.
- Tests: `tests/RealtimeAuditTests.cpp`, `tests/Benchmarks.cpp`, `tests/SandboxBenchmarks.cpp`,
  `tests/PerformanceGateTests.cpp`, `tests/BenchmarkGates.cpp`, `tests/LongRun.cpp`, `tests/ScrollingFrameTests.cpp`,
  `tests/AudioDevicesTests.cpp`, `tests/DeviceNoticesTests.cpp`.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

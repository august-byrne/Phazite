# Recording and Capture

*Design section 17.*

Recording puts what is played into the song: the notes played on MIDI keyboards and the computer keyboard, and the
sound that comes into the audio device's inputs. Capture makes a clip of the notes that were just played, without
Record having been pressed.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Live MIDI plays one track, without arming it by hand | Built | |
| MIDI recording, with Overdub and Quantize Recordings | Built | |
| Capture: the last 30 seconds of MIDI as a clip | Built | |
| An arm switch on every track that records | Built | |
| Audio recording from an input, one channel or a pair | Built | Not yet tried by a person with an ASIO® interface |
| Hearing the input: while armed, always or never | Built | |
| Record an Input on a New Track | Built | |
| Count-in | Built | |
| Metronome on the time signature's beats | Built | |
| A click grouped into dotted quarters for compound meters | Planned | No roadmap step |
| Punch In over the looped range | Built | |
| Takes round a looped range, with Next Take and Previous Take | Built | |
| Moving the playhead while recording | Built | |
| A clip that grows on each track as it records | Built | |
| Recordings placed where they were played, to the sample | Built | Recording correction for a driver that reports its delays wrongly |
| Recordings follow the song's tempo | Built | |
| A drive that stalls leaves silence, and the take stays in time | Built | |
| An input meter in the input list, and a clip mark at 0 dBFS | Planned | No roadmap step |
| Hearing the input only inside the punch range | Planned | No roadmap step |
| Several MIDI tracks armed at once | Planned | No roadmap step |
| Take lanes and comping | Planned | After v1, item 4 |
| Audio Capture | Planned | After v1; no roadmap step |

## What records

Every track that records has an arm switch, beside its mute and solo switch in its playlist header and in its mixer
strip. It is one switch shown in both places. Folders and returns have none.

- **A track with an instrument records MIDI.** Live MIDI, from every MIDI keyboard and the computer keyboard, plays
  one track: the selected track with an instrument, or the first track with one. That track's arm switch is lit
  without a click. A click on another instrument track's switch sends live MIDI there, armed. A click on the lit one
  stops that track recording MIDI; it still plays, and records again when it is clicked or when live MIDI moves to
  another track.
- **A track without an instrument records an audio input.** A click arms or disarms that track alone. On a track
  with no input yet, the click opens the input list instead, and choosing an input arms the track. A right-click
  offers the inputs, when the input is heard, and **Arm Only This Track**. **Arm Track** (`track.arm`) arms the
  selected tracks.
- Arming is not saved and is not an undo step. A project opens with nothing armed, so that a microphone that is
  heard can't feed back as it opens.

What a track records shows under its name: "In 1", "In 1+2" or "MIDI". While Record records onto it, the text turns
to the danger colour and reads "Recording In 1". The mixer strip says "Armed: In 1" or "Armed: MIDI" where it says
what feeds the channel.

## Inputs and hearing them

- **The input list** has each of the audio device's input channels alone ("In 1"), then each pair ("In 1+2"), the
  one chosen last on any track first, and **No input**. New tracks start with no input.
- The inputs are those of the audio device chosen in the audio settings. Phazite opens an input's channels while a
  track needs them and closes them again after, leaving open those the audio settings opened.
- A track's input is saved with the project. Changing it is one undo step, "Change Input".
- **Hearing the input** (monitoring) has three settings: **While armed** (the default), **Always**, and **Never**,
  for an audio interface that plays its input to its player itself. The setting is saved, and changing it is one
  undo step. The input is heard through the track's devices and fader.

## Record

- **Record** (`transport.record`, R) records what is played on MIDI onto the track live MIDI plays, and the inputs
  of the armed tracks, all as one undo step, "Record".
- It starts from where playback starts, after the count-in when that is on, or from where the song is if it plays
  already.
- Its tooltip names what it records onto: "Record onto Lead (MIDI) and Vocals (In 1)".
- **With nothing to record onto, Record is not greyed out.** A click opens its menu, headed "Nothing records yet".
  **Record an Input on a New Track** lists the inputs; choosing one adds a track called "Audio" ("Audio 2", and so
  on) after the selected track, with that input, armed and selected, as one undo step, "Add Audio Track".
- The song's own loop does not loop a recording, which runs on past the song's end. A range looped on purpose does.
- Record's options, in the menu beside its button: **Count-In** (`transport.countIn`), **Punch In**
  (`transport.punch`), **Overdub** (`transport.overdub`), **Quantize Recordings** (`transport.quantizeRecordings`)
  and Record an Input on a New Track. Count-in, Overdub and Quantize Recordings are the app's settings, for every
  project.

### MIDI

- **Overdub**, on by default, adds the notes played to the clip already there: the first of the track's MIDI clips
  that they overlap, or else a new clip over what was recorded. With Overdub off, the notes take the place of the
  clips there.
- **Quantize Recordings** moves recorded notes to sixteenths as they go in. It is off by default.
- A note played live is placed where it was heard: earlier by the output's delay and the song's delay compensation.

### Audio

- **Files:** each input is written as a WAV file, 24 bits at the device's rate, into the project's `Audio` folder
  (the untitled project's, before its first save), named "*track* Take *n*.wav".
- **In time:** Phazite's engine knows the song's sample its take started on. What came in is moved earlier by the
  delays the device's driver reports for its input and output, and by the song's delay compensation. The audio
  settings' **Recording correction**, in milliseconds, 0 by default, moves it further, for a driver that reports its
  delays wrongly.
- **Following the tempo:** a recording comes in following the song's tempo, from the tempo it was played at, so that
  it keeps to its beats when the tempo changes later. One recorded where the tempo changes keeps its own speed.
  Nothing is rendered until the tempo changes.

## While it records

A clip grows on each track that records, in the track's colour with its edge in the danger colour, tagged
"Recording" ("Recording take 2" round a loop).

- It runs from where recording started to the playhead's line. Round a loop it starts where this time round began;
  punched in, at the punch's start.
- Round a range that recording started before, once it has gone round, what was played before the range shows
  beside it, whole, as the clip it will be.
- An input's waveform is drawn as it comes in.
- Notes show as they are played, a held note growing, from the first note played. Round a loop they fold in as they
  will be kept.
- When recording stops, the take's clips take its place.

## Punch In

With **Punch In** on, Record keeps only what is played in the looped range, in place of what was there.

- The take does not loop: it plays straight through, and the loop's switch stays on.
- The punched clip's edges cross-fade with what was there over 10 ms.
- While the loop follows the whole song there is nothing to punch into, and Punch In is off.
- While it is on, the loop shows red in the rulers.
- The switch is saved with the project, as the loop is, and is not an unsaved change.

## Takes

Recording round a range looped on purpose, of any length, makes one clip over the range with a take for each time
round.

- **The last whole take plays.** A take cut short by Stop is kept, but the last whole one plays; if there is no
  whole one, the last one plays.
- **Started before the range:** what was played before the range becomes a clip of its own, where it was played. The
  first time round from the range's start is the range clip's first take.
- **Started within the range:** the first, partial time round is a take too. Its file starts with silence from the
  range's start, so that every take lies where the range does, each as long as it.
- **Stopped before the playhead first reaches the range's end:** the take is one clip of what was played, where it
  was played.
- **Notes** go the same way: those played before the range go into a clip of their own, and those of the times round
  fold into the range's clip.
- The clip's tag shows "Take 2/3". Its menu lists the takes; choosing one is one undo step, "Choose Take".
  **Next Take** (`clip.nextTake`) and **Previous Take** (`clip.previousTake`) step through them; the Phazite keymap
  gives them no key.
- One rule says which time round a moment of a take is on, for its audio, its notes and what shows while it records
  (`TakeLoop` in `src/model/transport/internal/AudioRecorder.h`).

## Moving the playhead while recording

Recording goes on after a jump. What was recorded so far is kept as a take, as Stop would keep it, in a "Record" undo
step of its own, and recording goes on from the new place as another take
(`PlaybackSession::locateWhileRecording()`). The new take records the inputs of the tracks the first one recorded; a
track armed since then is not recorded until recording has stopped and started again.

A press in the ruler moves the playhead once. A drag in the ruler takes it no further than its press while a take
records, because every move would end one take and start another.

## A drive that stalls

Each input has a buffer of 8 seconds between the audio thread and the thread that writes its file.

- Past that, what comes in is lost. The audio thread notes where and how much in a small queue beside the buffer,
  without a lock or an allocation.
- The writing thread writes silence there, so the take stays in time and recording goes on.
- Once the clips are placed, one error, "Part of the recording was lost", names each track that lost sound, where
  each gap starts and how long it is.
- Stop waits for the writing thread to finish what it is writing, however long the drive takes, and so does the
  autosave that a recording's end starts. A thread ended in the middle of a write can leave the file broken.

## Capture

Phazite keeps the last 30 seconds of what MIDI keyboards and the computer keyboard played, recording or not.
**Capture** (`transport.capture`, Ctrl+Shift+C) turns them into a clip on the track live MIDI plays, as one undo
step, "Capture", and selects it.

- While the song plays, the notes stay where they were played in it.
- While it is stopped, the clip goes at the bar the play start is in, or after the clips already there, with the
  first note on the bar's first beat.
- In a song without clips, Capture first sets the song's tempo from what was played, when it can tell: a whole
  number of beats a minute between 80 and 160, from at least four notes over at least a second.
- The clip runs whole bars, and its notes are quantized to sixteenths when Quantize Recordings is on.

## Metronome and count-in

- **The metronome** (`transport.metronome`, O) is Phazite's own click, synthesized as it plays. It clicks on each of
  the time signature's beats, the note it counts: six eighths a bar in 6/8, two half notes in 2/2. Each bar's first
  click is louder.
- The song keeps whether the metronome is on, as it keeps the loop. Switching it is not an undo step.
- **The count-in** is a bar of clicks before recording starts from a stop, on the same beats, at the tempo there,
  while the song waits. It is heard with the metronome off too.

## How it works

Phazite's engine takes the audio device's inputs in its own callback and plays them through the tracks that hear
them. From the first sample the song plays after Record, after the count-in, it copies each armed track's input into
a buffer of its own that no lock or allocation touches. A thread of Phazite's writes that buffer into the file, and
keeps the loudest sample of every stretch for the waveform the playlist draws. The notes played live come out of the
engine with the moment they came in. The [real-time audit](../realtime-audit.md) has guarded scenarios for a take
being recorded and for an input that falls a whole buffer behind.

Joining clips, warping and quantizing recorded audio are described with the playlist and the audio clip editor:
Consolidate in [playlist.md](playlist.md), warp markers and Quantize Audio in [piano-roll.md](piano-roll.md).

## Decisions

- Arming is per track, not exclusive: a click arms one track and leaves the others, and Arm Only This Track is the
  exception. Several inputs are often recorded at once.
- Live MIDI follows the selected instrument track by itself, so that playing a track never needs a click on its arm
  switch first.
- Arming is not saved, so that opening a project can never start feedback through a monitored microphone.
- A new audio track starts with no input, so that nothing is recorded or heard by accident.
- Record never makes a track by itself. With nothing to record onto, it asks what to record.
- Punch In uses the loop's range rather than a range of its own: one range to set, shown red.
- Every time round a looped range is kept, a partial first one too, and what was played before the range is kept as
  a clip of its own. Nothing played is thrown away.
- Sound lost to a slow drive becomes silence in place, and recording goes on. Stopping the recording with an alert
  would lose the rest of the performance.
- Recording goes on after the playhead is moved, keeping the take so far.
- Next Take and Previous Take have no key in the Phazite keymap: T is the Mute tool.
- The count-in, Overdub and Quantize Recordings belong to the app, not to a project.

## Not built

- An input meter in the input list, and a mark on a clip that stays lit after its input reached 0 dBFS: Planned.
- Hearing the input only inside the punch range: Planned.
- Several MIDI tracks armed at once: Planned.
- A metronome click grouped into dotted quarters for compound meters: Planned.
- Take lanes and comping: Planned, after v1.
- Audio Capture: Planned, after v1.
- A setting that keeps the monitoring chain's latency in the recorded audio: Planned, if a monitored chain needs it.

## In the code

- `src/model/transport/RecordingModel.h`, `RecordingValues.h`: recording as the UI reaches it (inputs, arming,
  monitoring, punch, takes, what is being recorded now, `addTrackForInput()`, `describeRecordTargets()`).
- `src/model/transport/internal/Recording.h`: Record, Overdub, Quantize Recordings, Punch In, Arm Track, Next and
  Previous Take.
- `src/model/transport/internal/Take.h`: a take recorded through Phazite's engine, and where its clips go.
- `src/model/transport/internal/AudioRecorder.h`: arming, the device's inputs the tracks need, `TakeLoop`.
- `src/model/transport/internal/Capture.h`, `Metronome.h`, `Playback.h` (`movePlayhead()`).
- `src/model/playback/internal/PlaybackSession.h`: `startTake()`, `locateWhileRecording()`.
- `src/model/engine/internal/LiveInput.h`, `AudioIO.h` (the recordings' options and the Recording correction).
- `src/engine/core/Recording.h`: what the engine records of a take; `src/engine/transport/Click.h`: the click.
- `src/ui/components/ArmButton.h`, `src/ui/playlist/PlaylistRecording.cpp`, `src/ui/shell/TopBar.h` (Record and its
  menu).
- Tests: `tests/RecordingTests.cpp`, `tests/AudioRecordingTests.cpp`, `tests/PlaylistTests.cpp`,
  `tests/MetronomeTests.cpp`.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

# Phazite's engine

*Design section 23.*

Phazite plays, records and renders its songs with code of its own. It builds on JUCE for the audio and MIDI devices,
audio files, the plug-in formats and the windows, and on nothing else for playback. This page describes the three
parts and how they meet: the song, which is Phazite's own document; the engine, which plays a plan; and the step
between them, which compiles the song into what plays. The layers and where the code lives are in
[../architecture.md](../architecture.md).

## Status

| Feature | State | Note |
| --- | --- | --- |
| The song as Phazite's own document, with one undo history | Built | |
| Playback by Phazite's own engine | Built | |
| The same sound at any buffer size | Built | |
| Delay compensation on every path, sends and sidechains too (a key that comes later waited for) | Built | |
| Several threads playing each block | Built | |
| Audio files streamed from disk, and resampled where their rate differs | Built | |
| Tempo changes, ramps and time signatures along the song | Built | |
| Recording through the engine, known to the sample | Built | Not yet tried by a person with an ASIO® interface |
| Offline renders the same to the last bit on any number of threads | Built | |
| Plug-ins in helper processes | Built | [plugin-hosting.md](plugin-hosting.md) |
| Phazite's own devices, engine-neutral | Built | [devices.md](devices.md) |
| Audio clips stretched, pitched and reversed | Built | Offline, with one stretcher |
| Browser previews on an engine of their own | Built | |
| Other stretch modes (slices at the hits, re-pitch) | Planned | No roadmap step |
| Tracks pre-rendered in the background | Planned | After v1; no roadmap step |

## The song

The song is Phazite's own document: its tempo map, tracks and folders, clips with their notes, devices, containers,
sends, automation, modulators and macros, sections and the chord lane.

- **IDs.** Each item has an ID that the song gives. An ID is never given twice and never taken back by an undo.
  Everything outside the model names tracks, clips, notes, sections and devices by these IDs.
- **One schema.** Every name the song holds is defined once. A project file is the song alone, saved as XML
  ([projects-saving-export.md](projects-saving-export.md)).
- **One undo history.** The song keeps its own undo history, in which every change is one named step. Undo puts
  back what plays as well, the values set by hand included.
- **What is not an undo step:** mute and solo, freezing a track, the loop, and view state.
- **A device's own state.** What a plug-in holds of its own (its settings, program and channel layout) is stored
  into the song outside the undo history, before the song is saved or copied. A change made in the plug-in's window
  counts as an unsaved change.
- **Unsaved changes are found by comparing.** A value written again as another type notifies without changing
  anything, so the project keeps a copy of the song as opened or last saved and counts a change only when what
  counts differs from it.
- **Aliases.** Each clip of an alias group holds the group's notes in its own element, so an edit changes every
  clip of the group in the same undo step.
- **Time edits** make a section longer or shorter rather than cutting it in two.

### Time

- **Positions are beats,** and a beat is a quarter note whatever the time signature, as MIDI and DAWproject count
  them. A tempo counts quarter notes a minute. So clips, notes, automation and sections stay on their beats when
  the tempo or the time signature changes.
- **A time signature sets two things only:** how long its bar is (numerator x 4 / denominator quarter notes: 3 in
  6/8, 3.5 in 7/8, 4 in 2/2), and its own beat, the note it counts (4 / denominator quarter notes). The grid's Beat,
  the inspector's bar and beat, the count-in and the metronome go by that beat.
- **The tempo map** holds each tempo and time signature from a beat on. A tempo holds until the next and then
  jumps, or ramps to it, straight or bent. The engine plays ramps exactly, and bent ones in straight pieces.
- **A device's rate in time with the song** is a note value, in quarter notes, or so many of the song's bars, each
  as long as its time signature makes it. A "1 bar" LFO or delay is three quarter notes in 6/8, and a bar's LFO
  starts its cycle with each bar, across a change of time signature too.
- **A plug-in's playhead** has the song's place in quarter notes, its time in samples, the time signature, and the
  bar it is in. While the song is stopped, every piece of a block is where the playhead is, but after a Stop that sends
  the playhead back (to where playback started, or from the song's end): then what plays stays where the song stopped
  (below, "The transport").

## From the song to what plays

The model compiles the song into a **plan**: each track's notes, audio clips, devices, automation and fader, its
sends, and the buses and the master they reach.

- **The engine is handed each plan whole** and frees nothing on the audio thread. The old plan is freed on the
  message thread.
- **What sounds, sounds on.** What plays from one plan to the next (each strip and channel, each device, each
  modulator) is kept by the song's IDs, so a note rings on through an edit.
- **A value set by hand reaches what plays at once,** without a new plan: a device's parameter or switch, a
  channel's volume, pan, mute and solo, and a plug-in's parameter. View state (a track's height, the grids, a lane
  shown, where a device's window was) makes no plan at all.
- **A new plan compiles again only what changed.** The player listens to the song itself: each track's notes and
  audio clips are kept until that track's clips change (its audio until the tempo changes too), and each of Phazite's
  own devices keeps the values the plan before read until something of its devices changes (not the channel's own
  height, mute or volume, nor a plug-in's stored state); what joins the channels (routing, delay compensation, sends,
  audibility) is worked out whole each time, being cheap. What the player keeps of a sound file is its identity on the
  disk as it was read (its path, size and modification time: `getFileIdentity()`, `src/model/engine/AudioFiles.h`).
  The disk is asked about a file again for a track whose clips changed, and for every file a plan plays once a second
  has passed since anything last asked (`trustFilesMilliseconds`), each once a plan however many clips play it, and the
  tempo map made once: a file another program writes again in its place plays as it is now from the first plan after
  that second, made for whatever reason, the audio of every track that plays it made again; within the second, as it
  was. A clip's render is named for its source's identity (`getRenderFile()`), so a source written again asks for a
  render of its own, which the project's renders make (they hear of the change), and plays it once it is made (silent
  meanwhile, as any render not made yet); the old render is asked for no more, and goes as the cache is purged when the
  project closes. A change of a source's modification time alone (files restored without their times) makes its
  stretched clips' renders anew too. A clip's waveform follows its file the same way: a paint never asks the disk, and
  the waveform shows the file again once a plan (or anything else that asks) finds it changed. Each job of a render
  (an export's stems, one after another) makes the clip renders it needs as its turn comes. Phazite's own renders, recordings, consolidations and imports take names that are free; a freeze
  writes its new file over the track's frozen one in its place, which the player asks about at every plan; and a writer
  that writes where a song played a file before (a DAWproject's audio unpacked into the project) tells the players to ask
  again at the next plan (`noteFilesWritten()`; known bug 198). The disk is asked only as a plan is made: a file written
  in its place while nothing is edited plays on from the old reader until the next plan. A sampler's sounds are read as
  they are loaded, and not again. While the tests run, kept parts are compared with a read of the song and of what the
  player knows of each file (`SongPlayer::setChecking()`), after every edit, Undo and Redo the undo check makes and
  every random edit. A render compiles each of its jobs (an export's stems) as its turn comes, on the message thread,
  and lets go of the one before.
- **Each open project has a playback session.** It gives the engine the song again as it changes, owns the song's
  plug-in instances and its recorder, plays the transport, takes live MIDI into the song, and keeps the take it
  records.
- **The engine context** owns the audio device, the plug-in host and the engine on the device. It is made once, as
  the app starts, and outlives every project.
- **A second engine** beside the song's plays the browser's previews and the startup sound, so a preview never
  disturbs the song.

## The playback engine

The engine is engine-neutral: it knows nothing of the song model. It uses JUCE's core, audio and DSP modules,
r8brain-free-src and Phazite's devices only.

- **The transport** is driven by the message thread through the state it last asked for, in atomic values that can't
  overflow however fast it asks, and read back through the engine's playhead. The window reads where the playhead was
  at its frames' own times, from a clock smoothed over the last blocks.
- **Stop leaves what rings on as it was.** A Stop that sends the playhead back (Stop going back to where playback
  started, and the song's end) asks the engine for a return (`Engine::returnTo()`), not a locate: the playhead goes
  back, but what plays stays where the song stopped (every automated value, a device's parameters, a fader's volume and
  pan, a send's level, a chain's fader and crossover, and the song's place the devices and the modulators are given),
  so that a reverb's tail through a filter its lane had closed rings on closed, rather than blip as the lane's value at
  the start opens it again. FL Studio leaves its controls at their last values as it stops, and Ableton Live's song
  stays where it stopped. The values go to the playhead when the song plays again, or when the playhead is put
  somewhere by hand (a click in a ruler, Go to Start: a locate). What the transport itself does (the playhead shown,
  the audio clips' reading ahead and their fade after the stop) follows the playhead. A render never stops and
  returns.
- **Fades at Stop, at a jump and at the loop's end,** as other DAWs make them: a jump while playing fades what sounds
  out over 3 ms, then in after it; a stop and the loop's end fade out the audio clips they cut, the rest ringing on as
  its notes end. A stop asked while a jump fades out waits for the fade to end, and stops where the jump lands. A song
  played without them plays as its render does, to the last bit.
- **Chase, where playback starts, jumps or goes round the loop:** each track's notes sounding there start, from where
  each started, and before them its clips' controllers, bend and channel pressure are set to their last values before
  there, as Cubase and Ableton Live chase them (a plug-in with the mod wheel on its filter, a note started in the middle
  of a bend). Where the clips hold no value before there, the bend goes to its middle and the pressure to none, and the
  controllers that MIDI's Reset All Controllers resets (RP-015: the mod wheel, breath, the foot and the pedals to none,
  expression to full) to that; any other controller they change only later (a volume, a pan) is left as it is, having
  no value of its own to go back to. A note's own pressure is set where it changed since that note started. Both are
  found by an index made with the plan (each track's notes sounding at a place, and its controllers' values at a
  place), so that the block that starts, lands or goes round does no more than a few hundred steps for a track. Where
  playback starts or lands, every device starts from silence, and so does every path's ring, which a reset leaves as it
  is in memory, its past silence. A track whose clips hold no controller gets none: only its notes. A controller that
  changes exactly where playback starts is sent once, its own value. Each MIDI channel's controllers, bend and pressure
  are its own, chased and reset on that channel. A program change is chased too, but sent only where it differs from
  the program the instrument was sent last on that channel (from the song or played live; each strip keeps these from
  one plan to the next, for the instrument they were sent to: another instrument in its place, or a plug-in loaded
  again after a crash, has been sent none), so that a plug-in that loads samples for a program doesn't load them again
  at every round of a loop; before a channel's first program change in the clips nothing is sent, and the instrument
  keeps the program it has.
- **MIDI channels.** A note and a controller play on their MIDI channel (the song's 1 to 16, the events' 0 to 15:
  `devices::NoteEvent::channel`): from the song, from a MIDI input as it was played (the computer keyboard and a note
  drawn in the piano roll are on channel 1), into a take. Two notes of one pitch on two channels both sound, each to
  its own end; on one channel the second ends the first, as MIDI can't play a note twice at once. All notes off and a
  slide are every channel's. Phazite's own instruments take every channel as one and ignore program changes: as they let
  go of a pitch at its first note-off, their runner (`DeviceRunner`) keeps which channels hold each pitch and hands one
  a note's end only where no other channel still holds it, so a pitch held on two channels sounds until the later end.
  A plug-in gets each message on its channel ([plugin-hosting.md](plugin-hosting.md)).
- **Audio clips at another rate** keep a second resampling, made ready a little each block ahead of where they play
  from next without playing on to there (the loop's start, where playback will start, their own start half a second
  ahead), so that the loop goes round without starting a resampler from nothing (it takes 832 samples of a 44.1 kHz file
  before one gives anything out at 48 kHz). Where a jump lands among clips not ready there, the playhead waits there,
  silent, for whole blocks while they make it ready a share of each block, never waiting for the disk (a clip whose
  file isn't read there plays as silence and fades in once it is), and then plays as it would have at once, later by
  those blocks. Live notes played meanwhile, and those played in the last moments before it landed, play where it
  lands; a take after the jump starts there, in time with what is heard ([recording.md](recording.md)).
  - The hold lasts 20 ms at most, in whole blocks; a block longer than 20 ms is never held. At the bound, the landing
    plays from there with the clips not ready yet started from nothing, as a jump did before holds: a 192 kHz file
    on a 44.1 kHz engine at small blocks can need more than 20 ms of shares and get there.
  - Never held: the loop's wrap (made ready ahead), a jump during a count-in (which makes them ready itself), a jump
    while stopped, a render, and a jump in a run: one asked for while it is held, which is taken at once, or within a
    tenth of a second of the jump before, as the playhead dragged while the song plays makes them. Those play from
    where they land at once, from what is ready, as before holds; so a run of jumps is silent for 20 ms at most, at
    its first.
- **A change of delay compensation while playing** (a plug-in with latency that loads or joins, a device switched
  that has one: the alignment of what goes on playing moved) fades what goes out over 3 ms on the plan before, which
  the engine keeps playing meanwhile, and in over 3 ms on the new one: plans that come during the fade make one dip.
  Every such change dips, whether or not the channel it comes from is heard: what listens to a channel that isn't
  heard (a send before a muted track's fader, a soloed return, a sidechain) would hear its delay jump, and a 6 ms dip
  nobody hears is the lesser fault. Everyday edits touch nothing else: a path that goes (a track deleted or moved to
  another bus, a send taken away or switched before or after its fader) fades out alone over 3 ms on the plan before,
  everything else playing through it sample for sample, and a path that comes (a send added, a track moved into a bus)
  fades in alone. The plan before plays in place of the new one for those samples, never beside it. Each leaving path
  starts its own fade as the engine takes the plan it left in (one deleted during another's fade fades from full level),
  stays silent after it while the plan before is kept, and when the compensation moves during a leave the mix stays
  dipped until the new plan's paths that read what a leaving path went into (a return, a bus, and what they go into),
  where their delays grew, read nothing again from before the leaving paths' fades ended (a return's sound held what
  they still sent it as they faded): the leave's fade, the growth and the fade in at most after the last such edit,
  and a jump ends it at once. Live, every path into a sum reads a ring of
  what came into it, delayed or not, kept from plan to plan, with room for a third of a second more than its delay
  (16,384 samples at 48 kHz, 65,536 at 192 kHz), so that a delay that grows or shrinks within that plays on from the
  past it holds. The paths from one point of a strip share one, its tap, which the strip writes once a block (a
  track's sends after its fader and the sidechains that listen to it read its sound before its mute; its way to its
  bus or the master its output; a pad's track its sampler's part): 200 tracks sending to 8 returns keep about 400
  rings, not 1,800. One that grows past its ring gets a longer one, which copies nothing: it reads what predates it
  from the ring before until it holds as much itself, so a block's work is bounded by its length. What a ring can't
  hold yet (a new path, a send added; or past a ring's reach) plays nothing and then fades in by itself, the rest of
  the mix playing on. A render (an export, a stem, a bounce, a freeze) plays the song once as it is: a path with no
  delay has no ring, one with a delay a tap as long as the longest delay from there and a block, and nothing is made
  for what it leaves out.
- **Mute silences a channel's output only,** fading over 5 ms: its sends before the fader go on; its instruments and
  effects play on, as in Ableton Live and most DAWs, so that a note held, a reverb's tail and a delay's echoes are
  heard at once as the channel is heard again ([mixer.md](mixer.md#mute-and-solo)). A muted track costs the CPU it
  costs unmuted live; Freeze frees it, and a render leaves out what it can't hear.
- **Control points.** Automation and modulation move what they move on the song's control points, one every 32
  samples from its start. So a song plays the same at any buffer size
  ([automation-and-modulation.md](automation-and-modulation.md)).
- **Delay compensation** lines up every path to the master, the sends' and the sidechains' too. The Low Latency
  switch leaves it off for what is played live ([performance.md](performance.md)).
  A sidechain that comes later than the device's own sound (a kick through a linear-phase EQ keying a compressor on a
  bass) can't be heard earlier: the device's own sound waits for it instead, held back by a delay before the device
  (`Plan::Device::inputDelay`), and its track comes out as much later, which the rest of the song is lined up with, so
  that a duck stays on its kick whatever is on the kick's track. Live, a device with a sidechain keeps that ring
  whether it waits or not, so that a wait that comes (the EQ switched to Linear while the song plays) plays on from
  the past under the dip. What is held back is the strip's own sound, never a path that comes or goes: choosing a
  sidechain or taking it away while the song plays leaves the device's sound as it was, with no fade of its own; where
  it had waited (the sidechain taken away, its key's track or the device deleted), the sound comes that much earlier,
  across the dip a plan that lines its paths up otherwise makes. A
  sidechain chosen while the song plays whose key already comes late has no past to wait from: its device's sound
  is silent for as long as the wait, once, and fades in. An instrument's sound isn't held back (it takes notes): its
  sidechain is lined up where it can be; nor is a device's that is off, which doesn't play.
  A device that sound comes into late (through devices before it, or the strips a bus adds up) is given the song's
  place less that latency, for its automation and for what it counts from the place itself (an LFO in time with the
  song, its grid of control steps, a noise that starts with the song), so that each meets the sound that belongs
  there; a strip's modulators are not moved so.
- **Worker threads** help play each block, each strip a task, at the audio thread's priority.
- **Audio files** are read ahead of where they play by a reading thread, so the audio thread never waits for the
  disk: ahead of where they play, of where playback will start and of the loop's start, a version's bounce compared
  with the song too. A jump into a part of a long file not read yet can't be read ahead of: as other DAWs do, playback
  goes on and the clip fades in when its file is read there. The jump asks for that part first, ahead of everything
  else, and wakes the reading thread at once, so the silence lasts as long as the read (a fraction of a millisecond
  from the system's cache, a disk's seek and read from a cold file). A file at another rate than the engine's is
  resampled as it plays. What a file of floats holds that isn't a number (NaN, an infinity) plays as silence, sample by
  sample.
- **Live notes** come from any thread through a lock-free queue. The engine keeps the notes played live in the last
  30 seconds, for Capture.
- **Recording** is the engine's too: it knows the sample a take started on, and copies the armed inputs into buffers
  that a thread of Phazite's writes into files ([recording.md](recording.md)).
- **The metronome's click** is synthesized, and comes out as late as the song's sound does (its delay
  compensation), the count-in's too. The master's loudness is measured by EBU R128 ([mixer.md](mixer.md)).
- **One output channel** (a mono device, or one channel chosen) plays both sides of the mix, each at half.
- **A version's bounce** is heard in place of the song by a part of the plan that plays it after the master, in time
  with the playhead ([versions.md](versions.md)).

### Offline

Exports, stems, freezes, bounces and consolidations play the song, or a part of it, offline on a thread of their
own, through the same engine. The result is the same to the last bit however many threads play it. While the open
song renders, the render has the song's plug-ins and playback stops.

## Devices

Phazite's own instruments and effects are engine-neutral DSP: each knows only how it is described and what it does
with a block ([devices.md](devices.md)). The engine plays each through a runner, which gives every device the same
surroundings:

- it glides each continuous parameter from one block's value to the next, and moves the automated and modulated
  ones on the control points;
- it applies an effect's mix, with the sound coming in delayed as much as the effect delays its own;
- it plays a mono channel in stereo;
- it lets a device sleep on silence, but never an instrument while it holds a note or its sustain pedal; asleep, its
  parameters still play their modulation, so that what shows them moves. A device sleeps once what comes in and what
  it plays have been silent for a quarter of a second and for as long as the tail it reports: a reverb rings out its
  decay, a delay its echoes. While the song is stopped, one that has heard nothing since it was last reset or made ready
  (a project just opened, a device added or switched on) has nothing to ring with, and sleeps after the quarter of a
  second alone; only what can sound counts as heard (a note's start, sound coming in, a pad its editor played), not an
  all-notes-off, a note's end, a bend, a slide, pressure or a controller alone (the stop of a project closed as it
  played reaches the next project's instruments as an all-notes-off). A device is reset by Play, by a jump while the
  song plays and by being switched on; a jump while the song is stopped resets nothing, so what rings on before it is
  waited for. While the song plays, every device
  waits for its tail, so that one whose sound starts within it plays as a render does (a render never sleeps, and a
  reverb's sway runs on through silence). A muted track's devices sleep the same way. Age, whose noise
  plays with the song whatever comes in, sleeps only while the song is stopped, and wakes as it plays. Asleep, a device
  wakes for sound, a note's start or what waits in it to play (a pad its editor played); and a device that a change of
  its values alone can make sound (the Filter, which a lane can switch to Driven at a Resonance that sings) asks to be
  woken when they change (`Processor::wakesWhenValuesChange()`): its runner compares the values it would give it in each
  block, on each of the block's control points, automation and modulation applied, with those it gave it last before it
  fell asleep, and, where they differ and the device says it would sound for the new ones (`Processor::wakesFor()`: the Filter, only
  for values that make it Driven, so that an LFO on a Clean filter's cutoff doesn't keep it at work on a silent track),
  wakes it for the block, after which it stays awake as long as for sound come in. Other devices are never compared,
  and sleep through a change of their values.
  `tests/PerformanceGateTests.cpp` counts the blocks each of Phazite's devices works once stopped, which are none once
  its sound has ended.

A container of devices plays as one device, its chains each in a buffer of its own. A part of a device's sound can
play on a mixer channel of its own: a drum sampler's pad on a track of its own.

**Plug-ins run in helper processes:** `PhaziteHost`, one for each plug-in file, each block a round trip through
shared memory with a deadline. A plug-in that crashes or hangs takes only its own helper with it. The engine plays a
plug-in through the same runner as its own devices, and names its parameters by the plug-in's own IDs
([plugin-hosting.md](plugin-hosting.md)).

## Stretching and pitching audio clips

A clip that follows the song's tempo, is pitched or is reversed plays a render that Phazite makes in the background
with Rubber Band Library's R3 engine, into the project's `Cache` folder.

- A clip under the song's tempo changes, or with warp markers, is rendered along a time map.
- A clip that needs none of this plays its file as it is. A recording that follows the tempo is not rendered until
  the tempo changes.
- The browser's previews stretch the same way, rendered into memory as a loop starts and played as soon as a
  quarter of a second is ready, the render keeping ahead, so that a preview sounds as its clip will.
- Tests check that a stretched drum loop plays every hit once, on its beat, and that a held chord stays clean
  between its notes (`tests/StretchTests.cpp`).

## Decisions

- **An engine of Phazite's own, on JUCE alone.** Phazite owns its song model, its playback, its plug-in hosting and
  its renders, so that each can be fixed and measured where it is. Nothing else is built on for playback.
- **The song is the model's; the engine plays plans.** The engine and the devices know nothing of the song, so
  that either side can change, or be replaced, without the other.
- **Beats are quarter notes everywhere,** and a time signature only sets the bar's length and the counted beat.
  Everything that keeps time then agrees, in every meter.
- **Control points every 32 samples from the song's start,** so that what a song sounds like does not depend on
  the audio device's buffer.
- **Plans are handed over whole,** and what sounds is kept by ID, rather than the engine being edited in place: the
  audio thread never sees a half-made change.
- **Values set by hand bypass the plan,** so a knob is heard at once.
- **Stretching is done offline, by one stretcher.** Rubber Band's R3 engine was the one stretcher measured that did
  well on everything: no drum hit played twice, each hit on its beat, and the cleanest held chords. It is too slow
  to run as the song plays, so clips play renders. Stretchers that cut the sound into overlapping pieces play some
  drum hits twice when slowing a loop down; slicing at each hit leaves gaps and can't stretch a held note.
- **Previews stretch as clips do,** so that a sound auditioned in the browser is the sound that lands in the song.
- **Renders use the same engine as playback,** so an export is what was heard.
- **Rubber Band Library is GPL-2.0-or-later,** which Phazite's AGPL-3.0-or-later allows
  ([privacy-licences.md](privacy-licences.md)).

## Not built

- Other stretch modes, such as slicing drums at their hits or re-pitching: Planned.
- Tracks that aren't being edited pre-rendered in the background: Planned, after v1.
- Audio Units and CLAP in the helper: Planned, after v1 ([plugin-hosting.md](plugin-hosting.md)).

## In the code

- `src/model/base/Song.h`: the song, its undo history, `Song::makeID()`, `Song::storeDeviceStates()`.
- `src/model/base/internal/Schema.h`: every name the song holds.
- `src/model/commands/internal/Undo.h`: undo steps, `phazite::undo()` and `redo()`.
- `src/model/transport/TempoValues.h` (`TimeSignature`, `Bars`), `src/engine/core/TempoMap.h`,
  `src/devices/dsp/NoteValues.h`: time.
- `src/model/playback/internal/SongPlayer.h`: the song compiled into plans.
- `src/model/playback/internal/PlaybackSession.h`: an open song's session.
- `src/model/playback/internal/PlaybackHost.h`: the engine on the audio device, with the previews' engine beside it.
- `src/model/playback/internal/OfflineRender.h`: the song played offline.
- `src/model/engine/EngineContext.h`, `internal/EngineContext.h`, `internal/AudioIO.h`: the engine context and the
  audio and MIDI devices.
- `src/engine/core/`: `Engine.h`, `Plan.h`, `Handover.h`, `Workers.h`, `Recording.h`, `PlayheadClock.h`,
  `EventQueue.h`.
- `src/engine/nodes/`: `DeviceRunner.h`, `DeviceNode.h`, `Strip.h`, `Channel.h`, `Container.h`, `PartOutput.h`,
  `LoudnessMeter.h`, `Comparison.h`.
- `src/engine/audio/` (`FileReader.h`, `AudioRegion.h`, `Resampler.h`), `src/engine/automation/Curve.h`
  (`controlPointSpacing`), `src/engine/modulation/ModulatorNode.h`, `src/engine/transport/Click.h`.
- `src/devices/`: the devices' DSP; `src/devices/stretch/Stretch.h`: stretching.
- `src/model/arrangement/internal/ClipRenders.h`, `AudioClips.h`: clips' renders;
  `src/model/engine/internal/PreviewLoop.h`: previews.
- `src/sandbox/`, `src/host/`, `src/model/plugins/internal/`: plug-ins in helpers.
- Tests: `tests/EngineTests.cpp`, `tests/EnginePlaybackTests.cpp`, `tests/PlaybackTests.cpp`,
  `tests/GoldenRenderTests.cpp`, `tests/StretchTests.cpp`, `tests/RealtimeAuditTests.cpp`.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

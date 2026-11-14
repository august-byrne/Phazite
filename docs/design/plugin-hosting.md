# Plug-in hosting and stability

*Design section 19.*

Phazite hosts third-party plug-ins in helper processes, never in its own. A plug-in that crashes or hangs takes its
helper with it, and Phazite, the song and the other plug-ins go on.

## Status

| Feature | State | Note |
| --- | --- | --- |
| VST3 plug-ins, instruments and effects | Built | Windows and Linux |
| Every plug-in in a helper process | Built | The only way Phazite hosts plug-ins |
| Scanning in a helper, a file at a time, with a time limit | Built | |
| A crashed or hung plug-in shown as "Crashed", and reloaded | Built | |
| A plug-in that crashes as it loads kept off | Built | |
| Open with Plug-ins Off, after a crash of Phazite's | Built | |
| A plug-in's own window inside Phazite's | Built | |
| Parameters named by the plug-in's own IDs | Built | |
| A plug-in's state kept as its `.vstpreset` | Built | Not yet tried by a person with a preset going to another DAW and back |
| Notes ended on Stop, whatever the plug-in takes | Built | |
| A plug-in that fails during an export, freeze or bounce fails it | Built | |
| A choice of how plug-ins share helpers | Declined | One helper for each plug-in file measured cheap enough |
| Containing a hostile plug-in | Declined | The sandbox is for crashes and hangs |
| Audio Units on macOS | Planned | After v1, item 11 |
| CLAP | Planned | After v1, item 1 |
| LV2 on Linux | Planned | No roadmap step |

## Formats

- Phazite hosts VST3 plug-ins, on Windows and Linux.
- Audio Units come with macOS.
- CLAP comes after v1. JUCE doesn't host CLAP, so the helper program needs a plug-in format of its own for it, built
  on the MIT-licensed CLAP SDK.
- The device list offers Phazite's own devices and the plug-ins that scanning found ([devices.md](devices.md)).

## Scanning

**Scan for Plug-ins** (`options.scanPlugins`, in the settings' menu at the activity bar's foot) finds the plug-ins.

- Plug-ins are scanned in the helper program, a file at a time. The helper loads the plug-in's code to find what the
  file holds, so one that crashes or hangs as it is scanned takes only the helper with it. Phazite's own process
  loads no plug-in.
- Each file has 60 seconds. One that takes longer is left out, its helper ended, and later scans skip it.
- A crash is tried once more with a fresh helper, in case the plug-in scanned before it was to blame.
- A scan that is cancelled ends the helper at once and blocks nothing.
- The settings keep the list of plug-ins found.

## Helpers

- The helper program is `PhaziteHost`, next to the app.
- **One helper for each plug-in file.** Every instance of a plug-in is in that plug-in's helper, each with an audio
  thread of its own. The helper starts as the plug-in's first instance opens and quits as its last goes.
- Phazite starts a helper with the name of a pipe of its own, and checks that the helper speaks the same version of
  their protocol. Each request to it is answered within a time limit or not at all.
- **A block is one round trip through shared memory.** Phazite's engine hands the helper the block: its audio, its
  sidechain, its MIDI, its parameters' changes at their samples, and where the song is. The helper's thread plays it
  in place and answers. Both sides are real-time safe.
- **Each block has a deadline of its own length in real time,** a millisecond at least (a part of a block the engine
  cuts short at a loop's end has a round trip's time), so a plug-in that is slow makes its own blocks late and never
  the song's. A block the helper has not answered in time is silent: an effect's sound fades out over 5 milliseconds
  (its input, as late as the plug-in would have played it), an instrument's stops, and the plug-in's sound fades in
  again as it plays again. An effect is not passed through dry: a limiter or a fully wet reverb passed dry is worse
  than a gap. The notes of a block that could not be handed over go with the next that is, so that no note that ended
  sounds on.
- Phazite reads back only what it handed over (the block's channels and samples), never what the helper's side of the
  shared memory says: a block that says otherwise is as one the helper did not play.
- A plug-in's switch is a switch: a parameter of two steps, as JUCE's VST3 host tells a VST3's switch, with the
  plug-in's own texts for its two values.
- The helper plays a block in pieces between its parameters' changes, so automation and modulation land on their
  samples, and gives the plug-in a playhead with the song's place, time signature and bar.

## Crashes and hangs

- **A crash:** a thread of Phazite's watches each helper's process. When the helper goes, its plug-in's instances
  are crashed, and nothing else is.
- **A hang:** a helper whose message thread has not answered its heartbeat for 10 seconds, or whose audio thread has
  kept a block for 5 seconds, is ended, and its instances are crashed as a crashed helper's are. Nothing of Phazite's
  waits for it.
- **What shows:** the plug-in's slot reads "*name* - Crashed" in the danger colour. It plays nothing: an effect's
  sound fades out, as for a late block.
- **Reloading:** a click on the slot, or **Reload** in its menu, opens the plug-in again in a new helper with the
  state the song holds for it: the state it was last given or last asked for. Its window makes its editor again.
- **A plug-in that crashes its helper as it loads stays off,** at the next start too, until it is reloaded by hand,
  so that it can't take time from every project that holds it. The settings keep that list.
- **A missing plug-in** (not installed on this computer) stays in the song as it was saved, silent, until scanning
  finds it.
- **Nothing is left behind by a crash of Phazite's own:** on Windows the helpers are in a job of Phazite's that ends
  them as Phazite ends, however it ends (what a plug-in starts, such as a browser for its manual or a licence manager,
  is left out of the job and lives on, as it would beside a plug-in in any host); on Linux and macOS each helper watches for
  Phazite to go and ends a second after, if the pipe's loss hasn't quit it by then (its message thread may be a plug-in's
  hostage). A channel's shared memory goes with its last user: Windows keeps a mapping only while a process has it, and
  on Linux and macOS Phazite unlinks its name as soon as the helper has opened it.
- **A crash of Phazite's own:** if Phazite did not close properly, the next start asks before it opens the project
  that was open: **Open**, **Open with Plug-ins Off**, or **Start a New Project**. With its plug-ins off, the
  project's plug-ins stay as they were saved, unloaded and silent; opening the project again turns them on.
- Autosave and Auto Save keep the song itself ([projects-saving-export.md](projects-saving-export.md)).

## Stop ends every note

The helper keeps which notes it gave a plug-in that have not ended. Where every note is to end (as playback stops or
jumps, and on a reset), it ends each with a note-off of its own. A plug-in need not take the "all notes off" and
"all sound off" controllers, and a note-off it always takes.

## Renders

An export, a freeze or a bounce waits for a plug-in instead of skipping its block (a block of live playback the
plug-in is still on too), so the file holds what the plug-in plays. Plug-ins are told that they play offline (JUCE's
`setNonRealtime()`, a VST3's process mode) before the render's first block, so that one that streams from disk or
loads in the background has what it plays in time, and that they play live again as the render gives them back
(finished or cancelled), so that the first live block doesn't wait for the switch. A render does not wait for ever:
a plug-in whose helper goes while the song renders, or that keeps a block for 30 seconds, fails the render, which
names the plug-in ("*name* crashed while Phazite rendered. Reload the plug-in, then render again."); one that had
crashed before fails it at its start ("*name* has crashed. Reload the plug-in, then render again."), unless it is
switched off, as it isn't heard then. Nothing is written. Cancelling a render does not wait for a plug-in, not even
one to be prepared for the engine's rate again while it keeps the render's block: its channel waits for the block
(the engine skips its blocks meanwhile), and a plug-in that keeps it as long as a hang takes is ended. A crashed
plug-in keeps the rate the engine last asked of it, which its reload opens it at.

While the open song renders, the render has the song's plug-ins, and playback stops. A song rendered beside the open
one (an older commit's bounce) gets plug-in instances of its own.

## Parameters and state

- **Stable parameter identity:** the song names every plug-in parameter it refers to (its automation, what
  modulates it, its value set by hand, its automation lane) by the plug-in's own ID for it, never by its index. A
  plug-in update that reorders its parameters can't move saved automation onto another. The helper finds each
  parameter by that ID.
- **Automation and modulation** move a plug-in's parameters on Phazite's engine as they move its own devices': the
  block goes to the helper with their values on each control point
  ([automation-and-modulation.md](automation-and-modulation.md)).
- **A plug-in's own state** (its settings, program and channel layout) is stored into the song before the song is
  saved or copied. It is outside the undo history, and a change made in the plug-in's window counts as an unsaved
  change. A VST3's state is kept as the format's own preset file, a `.vstpreset`.
- **Latency** that a plug-in reports goes into the song's delay compensation ([engine.md](engine.md)).

## Plug-in windows

A plug-in's editor is a window of its helper's, shown inside a window of Phazite's, which the helper places over it
again as it moves or is resized. Phazite never calls into the helper's window, so a helper that hangs can't hang
Phazite's window. The keys Phazite takes over a plug-in window (play, stop, the computer keyboard) are handed back
by the helper, unless **Send All Keys to the Plug-in** is on for that plug-in
([commands-keys-help.md](commands-keys-help.md)).

## Decisions

- Every plug-in runs in a helper, always. One plug-in must not be able to close Phazite and lose the song, and a
  round trip to a helper measured cheap enough to make it the only way ([../performance.md](../performance.md)).
- One helper for each plug-in file, rather than one for all or one for each instance: a crash takes one plug-in's
  instances, and memory stays reasonable. There is no setting for it.
- The sandbox exists to survive crashes and hangs. It does not contain a hostile plug-in: a helper runs with the
  user's rights, and a plug-in the user installs is trusted as any program they install is.
- A hung helper is ended rather than waited for, after 10 seconds without an answer or 5 seconds on one block.
- A plug-in that crashed as it loaded stays off until the user reloads it, rather than being tried at every start.
- A render fails, naming the plug-in, rather than writing a file with a silent part.
- A late, busy or crashed effect fades to silence rather than passing its input through dry (the owner, 2026-11-14).
- The helper ends notes with note-offs of its own, because plug-ins need not obey "all notes off".
- Parameters are named by the plug-in's IDs, not their order.
- Scanning has a time limit for each file, so that one plug-in can't stall the scan.

## Not built

- Audio Units on macOS: Planned, after v1.
- CLAP: Planned, after v1.
- LV2 on Linux: Planned.
- A choice of how plug-ins share helpers (all together, by vendor, one each): Declined.
- A sandbox that limits what a plug-in may do on the computer: Declined.

## In the code

- `src/model/plugins/internal/PluginHost.h`: the host that starts a helper for each plug-in file, the reload, the
  plug-ins kept off, `setLoadsPlugins()`.
- `src/model/plugins/internal/PluginInstances.h`: a song's plug-ins, each by its device's ID, and their status.
- `src/model/plugins/internal/SandboxedInstance.h`, `SandboxedProcessor.h`: an instance, and what the engine plays
  of it, with its deadline and its failure in a render.
- `src/model/engine/internal/PluginScanner.h`: the scan (`pluginScanSecondsEach`).
- `src/sandbox/core/`: `SharedMemory.h`, `BlockChannel.h`, `Messages.h`, `PluginInfo.h`, `Vst3State.h`.
- `src/sandbox/client/Helper.h` (a helper process, its time limits and the hang limits), `RemoteInstance.h`.
- `src/host/core/Host.h`, `Instance.h`, `Editor.h`; `src/host/app/`: the helper program's `main` and its formats.
- `src/app/PluginWindow.h`, `src/app/SandboxedEditor.h`: a plug-in's window.
- `src/model/devices/internal/Parameters.h`, `src/model/devices/ParameterRef.h`: parameters by reference.
- Tests: `tests/SandboxTests.cpp`, `tests/SandboxedPluginTests.cpp`, `tests/PluginParametersTests.cpp`,
  `tests/PluginWindowTests.cpp`, `tests/Vst3StateTests.cpp`; the stand-in plug-ins are in `tests/host/`.
- [../juce-workarounds.md](../juce-workarounds.md) lists where the hosting works around JUCE.

VST is a registered trademark of Steinberg Media Technologies GmbH.

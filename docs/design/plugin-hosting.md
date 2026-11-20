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
| Plug-ins opened, added, reloaded, saved and closed without the window waiting for them | Built | |
| Stores, preparing, plug-in windows opened and closed, and values' texts without the window waiting | Built | Linux's window closing not yet tried on a Linux desktop |
| What a plug-in tells checked where it comes in: no value that is no number, no text of any length | Built | |
| A slow plug-in waited for in a render, named after 5 seconds, with Cancel | Built | |
| A plug-in's window as large as the plug-in at any Interface Size | Built | |
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
  thread of its own. The helper starts as the plug-in's first instance opens and quits as its last goes; nothing waits
  for it to quit: it is asked to, on a thread of Phazite's beside the others that go, and ended a second later if it
  hasn't.
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
- **A block that comes while the plug-in is being set** (given a state or a program, or switched to or from playing
  offline, on the helper's message thread, under the plug-in's lock) waits for it half the block's length at most, which a
  quick preset is set within (so that auditioning presets as the song plays doesn't cut the sound), and is then answered as
  one it didn't play, rather than kept until that is done: silent as a late block is, its notes and the values its parameters' changes left them at going
  with the next block it plays, and no hang counted. A sampler that reads its library as it is given a state plays again once it has. A render's block waits for it.
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
  waits for it. While a plug-in is given a state or a program, its helper's message thread isn't taken for hung for not
  answering, for up to a minute (as long as a plug-in may take to load); past that, the heartbeat's 10 seconds later, it is.
- **What shows:** the plug-in's slot reads "*name* - Crashed" in the danger colour. It plays nothing: an effect's
  sound fades out, as for a late block.
- **Reloading:** a click on the slot, or **Reload** in its menu, opens the plug-in again in a new helper with the
  state the song holds for it: the state it was last given or last asked for. It reloads in the background, as a
  project's plug-ins load: its slot reads "*name* - Loading" meanwhile, it stays silent, and one that doesn't reload says
  why once it is done. Its window makes its editor again.
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

## Opening, saving and closing

Phazite's window never waits for a plug-in as a project opens, saves or closes, as a plug-in is added, reloaded or its
window opens, or as its values' texts are written and read, as most DAWs' windows do.

- **Opening:** the plug-ins of a project that opens load in the background, one at a time in each helper and the
  helpers beside each other, each plug-in's time limit counted from when its own load begins. The song is there at once,
  and plays and edits meanwhile; each plug-in comes in as it is ready, its sound fading in over 5 milliseconds. The
  Project view lists each as "loading" until it has, and its slot reads "*name* - Loading". A plug-in added, one the song
  gets back (an undo, a redo), one scanning finds and one reloaded load in the background in the same way, whenever they
  come; what the song gives it meanwhile (a preset's state, an undo's) is what it comes in with. One that a closed project
  or an undo no longer holds is abandoned before it opens, or closed as soon as it has.
- **What is heard while a project loads:** a plug-in still loading is silent in its place, as a late or crashed one is:
  an instrument plays nothing, and an effect silences what passes through it (an effect that can't play is silent, never
  dry), so the strip it is on is silent until it has loaded (on the master, the whole song: right for a limiter). A
  plug-in that isn't installed on this computer is left out, as it always is.
- **A helper that crashes as a plug-in loads** marks that plug-in as one that crashes as it loads; another plug-in that
  was in the same helper, or loading in it, is tried once more in a fresh helper first.
- **Renders:** an export, a freeze or a bounce started while plug-ins still load fails at its start, naming them ("*name*
  is still loading. Render again once the project's plug-ins have loaded."), one the song got a moment before included. A
  render of a song no session plays (an older version's bounce, a DAWproject of an old version) opens its own plug-ins in
  the background the same way, and begins once they are in.
- **Saving:** a save asks every plug-in for its state at once, and writes once all have answered, or once a plug-in has
  taken 10 seconds; one that hasn't answered keeps the state it last gave, and a notice names it. The save is never lost
  over one plug-in. A save or a close that waits more than a second shows the progress window, "Waiting for the
  settings of *name*", whose Cancel goes on without the plug-ins still to answer, as if their time were up; a close's save
  takes the states its wait gathered rather than asking again. Auto Save and the autosave ask only the plug-ins whose
  state may have changed since they last gave it: a value or a program set, by Phazite, in the plug-in's window or by
  MIDI, its settings said to have changed (a VST3's `setDirty()`), or its window open since (a plug-in needn't say what
  its window changes, a sample loaded say); and they ask again one that didn't answer the last time.
- **Commands that read the plug-ins' states** (Save Version, a branch switched to, discard, restore, merge
  and cancel merge, a snapshot restored, Save As, DAWproject export; Delete and Duplicate of tracks that hold a plug-in
  or a drum sampler's pad tracks, and grouping devices among which one is, but not of notes, clips or a time range) run
  once the plug-ins that said they changed have given their states, as a save does. So do the other ways in to the same
  (`ProjectContext::withPluginStates()`, the one gate the registry's commands go through too): a preset loaded into a
  device or saved, from the browser's menus, a plug-in's window or a track dropped into the browser; a chain duplicated
  in a container; a line's Discard Changes in the Versions view; a branch merged from the branch picker; a DAWproject imported as a
  branch. A wait of more than a second shows the progress window, "Waiting for the settings of *name*", with **Go On
  Without Them** (the command runs with what each plug-in gave last) and **Cancel** (the command is dropped, changing
  nothing; Escape is Cancel). Meanwhile no other command that changes the song runs, and the window takes no input (a
  modal component of the session's own, never shown, until the wait is over or the progress window takes over), so no
  drag changes the song under it; a key let go meanwhile is told to the window as the wait ends, so that neither a
  shortcut nor a note of the computer keyboard is taken for still held. A command says so with `needsPluginStates`, or
  `readsPluginStates` for one that asks what is selected as it runs; a macro asks once before it starts. A preset's
  Swap in the browser needs none: it swaps only Phazite's own devices.
- **Other stores** (a drag that copies, a device's own buttons, Auto Save's snapshot, a plug-in's window closed) wait for
  no plug-in: each writes the state a plug-in gave last, and asks those that said they changed, whose answers are
  written in as they come (into the device an undo brings back, if it was taken away, unless the instance playing it
  now changed since it opened); the versions count a plug-in's reports only as far as the state they hold has them, so a
  late answer shows as a change since the last version. So that what a plug-in gave last is its own, one that says it changed is
  asked for its state once it has been quiet for a quarter of a second, or a second after its first change at the
  latest as changes go on, and no sooner after the last ask than ten times as long as that answer took (a 20 MB state
  measured 48 ms an answer, 7 ms of it the message thread's copy).
- **A plug-in reloaded after a crash** opens from the state it gave last, the user's latest settings. If it crashes
  again within 5 seconds of a reload, the next reload opens it from what the project holds, and says so, keeping its own
  state aside: the reload after that tries its own again, and a reload that fails keeps it as it was.
- **A sample rate or a block size changed:** a plug-in's channel for it is asked of its helper without waiting; the
  engine skips the plug-in (silent) until it has come, and a render waits for it, failing if the helper refused it.
- **Closing:** a project's plug-ins are let go of at once; each helper quits on its own, without anything waiting for it.
  Windows ending its session (a shutdown or a sign-out) while a close waits for the plug-ins' settings goes on without
  those still to answer, so that the save question comes before Windows ends Phazite; while a command waits for them,
  the close starts once the command has run or been dropped. A program that asks Phazite to close (the installer's
  update, through Windows' Restart Manager) ends no session: Phazite quits as it would from its own menu, and a close
  the user cancels leaves it as it was.

## Stop ends every note

The helper keeps which notes it gave a plug-in that have not ended. Where every note is to end (as playback stops or
jumps, and on a reset), it ends each with a note-off of its own. A plug-in need not take the "all notes off" and
"all sound off" controllers, and a note-off it always takes.

## Renders

An export, a freeze or a bounce waits for a plug-in instead of skipping its block (a block of live playback the
plug-in is still on too), so the file holds what the plug-in plays. Plug-ins are told that they play offline (JUCE's
`setNonRealtime()`, a VST3's process mode) before the render's first block, so that one that streams from disk or
loads in the background has what it plays in time, and that they play live again as the render gives them back
(finished or cancelled), so that the first live block doesn't wait for the switch. A render waits for a plug-in that
keeps a block for as long as it takes, as REAPER and Bitwig do, since it may only be slow: once it has waited as long as
a hang takes live (5 seconds), its window's progress bar names the plug-in ("Waiting for *name* (12 s)"), and its
Cancel is the user's answer. The render never keeps Phazite's window: it waits on a thread of its own, a little at a
time, and Cancel or closing the project ends the wait at once. A plug-in whose helper goes while the song renders
fails the render, which names the plug-in ("*name* crashed while Phazite rendered. Reload the plug-in, then render
again."); one that had crashed before fails it at its start ("*name* has crashed. Reload the plug-in, then render
again."), unless it is switched off, as it isn't heard then. Nothing is written. Cancelling a render does not wait
for a plug-in, not even one to be prepared for the engine's rate again while it keeps the render's block: its channel
waits for the block (the engine skips its blocks meanwhile), and a plug-in that keeps it as long as a hang takes is
ended. A crashed plug-in keeps the rate the engine last asked of it, which its reload opens it at.

While the open song renders, the render has the song's plug-ins, and playback stops. A song rendered beside the open
one (an older version's bounce) gets plug-in instances of its own.

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
- **A meter a plug-in has as a parameter** (a gain-reduction meter: a parameter that can't be automated, which the plug-in
  tells as it plays) changes nothing of the song: it isn't a value set by hand, nor an unsaved change.
- **Latency** that a plug-in reports goes into the song's delay compensation ([engine.md](engine.md)).
- **A value's text and a value typed** are asked of the helper without waiting: a value's text the plug-in wrote before
  shows at once, else the value as a number until the answer comes (never another value's text), one value of a parameter
  asked at a time, the newest asked for meanwhile next. A value typed is read by the plug-in while the window takes no
  input (the window's one hold, as for work that waits: play and stop still pass), for as long as that takes while its
  helper lives, the field showing what was typed meanwhile; after a second a progress window says Phazite waits for the
  plug-in, and Escape or its Cancel stops waiting (the value dropped, the old text shown again, a late answer
  dropped too). Phazite never sets a number the plug-in didn't give. The value is set only if the song's history
  hasn't moved meanwhile (an undo, a redo, another step) and the parameter is still there, and dropped otherwise, or if
  its plug-in or its helper has gone; what changes the song as it plays (automation written, a take recorded) leaves it
  alone.
- **What a plug-in tells is checked where it comes in:** a value that is no number (NaN, an infinity) is no change, a
  value past either end is at that end, every text (a name, a label, a program's name, a value's text) is 256 characters
  at most (an ID 128, each once: the first of IDs then the same keeps it, a later one is named by its order among them,
  "gain#2", as are one with none, "#1", and one named as the engine's mix, "mix#2"), an editor's side 16,384 pixels at
  most, a count never more than its message holds (a scan's 8,192 plug-ins from a file at most, what is past that said
  in the plug-in list's window), the values and gestures a helper tells between two turns of the message loop the last
  of each parameter (a gesture begun and ended meanwhile begins and ends, each begin paired with its end), its editor's
  keys a thousand at most, and the process ID its hello tells taken only if it is the child Phazite started, running the
  helper program. A plug-in may tell nonsense; nothing past the helper's messages sees it.

## Plug-in windows

A plug-in's editor is a window of its helper's, shown inside a window of Phazite's, which the helper places over it
again as it moves or is resized. It is as large on the screen as the plug-in makes it, whatever the Interface Size, and
fitted again as that changes. Phazite never calls into the helper's window, so a helper that hangs can't hang
Phazite's window. The window opens once the helper has made the editor, which it is asked for without waiting (and once
the plug-in has loaded, if it still loads), for as long as the helper lives (a big editor may take long; after 30 seconds a
helper whose message thread doesn't answer is taken for hung); meanwhile the device's window button shows it opening, and
says so after a few seconds; a second click stops waiting (an editor that comes then is dropped, and the next click asks
again), and closing it or deleting the device drops it. An editor
made for an instance the song no longer has (an undo replaced it meanwhile) is dropped, and the instance there now asked.
An open window whose device comes back as another instance (a version brought back, Discard, a branch switched to in
place) stays, saying the plug-in loads, and shows its editor once it is in. The helper's window sits in a window of Phazite's own (a
socket) and tells Phazite's window nothing of what happens in it; as the plug-in's window closes, the socket is taken
out of Phazite's window, hidden, and the helper is asked to close its editor without anything waiting, the socket going
once the helper's window has left it. A window destroyed with a child of another process's in it waits for that
process's thread on Windows, which kept Phazite 2 seconds at every close, and as long as a hung helper lived; on Linux
the close waited up to 2 seconds for the helper to take its window out, which Linux's socket, an X window of Phazite's
own, makes needless: it is taken out of Phazite's window as soon as the editor stops showing, and goes only once the
helper's window has left it or the helper has gone (without a display to make one with, the close waits as before). The
keys Phazite takes over a plug-in window (play, stop, the computer keyboard) are handed back by the helper, unless **Send
All Keys to the Plug-in** is on for that plug-in ([commands-keys-help.md](commands-keys-help.md)): the helper is told
each in every form a key of the layout in use comes in that Phazite's own window matches it by (AZERTY's & key for a
shortcut on 1), again as another layout is switched to, and on Windows by its place (its scan code) too, Shift forms
included: the helper matches a key that has a place by its place alone and hands it back by it, whatever its own
thread's layout, so that a key it took always means to Phazite what Phazite asked for.

## Decisions

- Every plug-in runs in a helper, always. One plug-in must not be able to close Phazite and lose the song, and a
  round trip to a helper measured cheap enough to make it the only way ([../performance.md](../performance.md)).
- One helper for each plug-in file, rather than one for all or one for each instance: a crash takes one plug-in's
  instances, and memory stays reasonable. There is no setting for it.
- The sandbox exists to survive crashes and hangs. It does not contain a hostile plug-in: a helper runs with the
  user's rights, and a plug-in the user installs is trusted as any program they install is.
- A hung helper is ended rather than waited for, after 10 seconds without an answer or 5 seconds on one block, while
  it plays live. A render waits on, naming the plug-in, until the user cancels it: no limit of a render's, nor a
  setting, can tell a slow plug-in from a hung one (2026-11-15, known bug 42).
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

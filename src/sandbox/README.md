# src/sandbox

What Phazite shares with the helper processes that host plug-ins, engine-neutral like `src/engine/` and compiled into
`phazite_core`. It uses JUCE's `juce_core`, `juce_events` and `juce_audio_basics` and `src/devices/`, and nothing of the
layers above. The model (`src/model/plugins/`) and the helper program (`src/host/`) both include it
([docs/architecture.md](../../docs/architecture.md)).

## core/

What both sides use.

| File | What it holds |
|---|---|
| `SharedMemory.h` | Memory two processes share by name, and the bells between them: a counter one side moves on and the other waits on, woken by the operating system only when it sleeps. |
| `BlockChannel.h` | The channel a plug-in instance's blocks go through: one round trip a block, with a deadline. |
| `Messages.h` | The control channel's messages. |
| `PluginInfo.h` | What a helper tells of a plug-in it hosts: its parameters by their own IDs, and its programs; and the checks of what a helper tells, where it comes in (a value that is no number, a text of any length, a size). |
| `Vst3State.h` | A VST3's state as its format's own preset file, a `.vstpreset`, as the song keeps it. |

## client/

Phazite's side.

| File | What it holds |
|---|---|
| `Helper.h` | A helper process, started with the name of a pipe of Phazite's, its requests answered within a time limit (or sent without waiting, their answers taken as they come), watched by a thread of its own; on Windows in a job that ends it as Phazite ends; the process ID it tells taken only as that of the child Phazite started, running the helper program. |
| `RemoteInstance.h` | A plug-in instance in a helper: what Phazite asks of it, each asked without waiting and read from its answer as it comes. |

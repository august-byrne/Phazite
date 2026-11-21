# src/host

The helper program that hosts plug-ins for Phazite: `PhaziteHost`, next to the app, which starts it. The code is in the
`phazite_host` static library, which `PhaziteHost` and the tests' helper (`phazite_test_host`, with the tests' stand-ins
in `tests/host/`) link; it links `phazite_core` for JUCE and the sandbox. It includes `src/sandbox/` and
`src/devices/`, and nothing includes it ([docs/architecture.md](../../docs/architecture.md)).

## core/

| File | What it holds |
|---|---|
| `Host.h` | The pipe to Phazite and its requests, handled on the helper's message thread. |
| `Instance.h` | A plug-in it hosts, and the thread that plays its blocks, in pieces between its parameters' changes, each with the song's place, time signature and bar on its playhead, a live block answered unplayed while the message thread sets the plug-in (a state, a program); and what the plug-in changes itself, told to Phazite from the message thread, but for a meter's changes; and the program a MIDI program change asks for, which the message thread sets as the song's. |
| `Editor.h` | The plug-in's editor, in a window of the helper's inside Phazite's where Phazite places it, which tells Phazite's window nothing of what happens in it (so that the helper waits for nothing of Phazite's), catching the keys Phazite takes over it. |
| `Platform.cpp` | What the helper asks of the operating system (to be kept out of Windows' power throttling, without a window of its own), and its watch for Phazite's end on Linux and macOS. |

## app/

| File | What it holds |
|---|---|
| `HostMain.cpp` | The helper's `main`: started by Phazite with the name of a pipe to connect to, it shows no window of its own and runs until Phazite asks it to quit or goes. |
| `Formats.cpp` | The plug-in formats it hosts, those JUCE is built with (`juce::addDefaultFormatsToManager()`): VST3. Audio Units on macOS: Planned. |

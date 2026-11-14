# src/app

The app: it starts Phazite, makes the main window and the services, and connects them. Everything here but `Main.cpp`
is in the `phazite_app` static library, which the tests link too; the `Phazite` program is `Main.cpp`. The app may use
`src/ui/`, `src/services/` and `src/model/` ([docs/architecture.md](../../docs/architecture.md)).

| File | What it holds |
|---|---|
| `Main.cpp` | Startup: the engine context, the services and the main window; and the `PhaziteRunning` inter-process lock (a mutex on Windows) that the installer's silent update waits on. |
| `MainWindow.*` | The main window, with its menus and dialogs. |
| `ProjectSession.*` | The open project: it owns the `ProjectContext` and is the only code (with the tests) that changes which song it holds. |
| `AppServices.*` | The services the app owns and hands to what needs them: the engine as the UI reaches it, the settings, recent projects, the Phazite Sounds download, the library and the previewer. |
| `PluginWindow.*` | Devices' windows: a plug-in's own editor, with the Plug-in menu's presets and its Send All Keys to the Plug-in switch (whose services the main window sets), and beside the menu, for a plug-in with a sidechain, its key's picker; and, when the user wants one, one of Phazite's own devices' editors out of the Devices view. Containers show in the Devices view only. |
| `SandboxedEditor.*` | A plug-in's editor in its helper process as a plug-in's window shows it: the helper's window inside it, with the keys Phazite takes over it. |
| `ClipWaveforms.*` | The audio clips' waveforms for the playlist and the audio clip editor, drawn from Phazite's own cache of their files' levels (JUCE's thumbnails). |
| `AppCommands.*` | The registration of the commands that work on the open song, in the order the app uses. |
| `RunMarker.*` | Whether the last run closed properly. |

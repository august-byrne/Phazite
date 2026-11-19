# src/services

What the app keeps outside any project, compiled into `phazite_core`. Services may use `src/model/`; the model never
uses them ([docs/architecture.md](../../docs/architecture.md)).

| File | What it holds |
|---|---|
| `Settings.*` | Phazite's settings (the `phazite*` keys): every key in one list, with its default and when it is saved. |
| `RecentProjects.*` | The recent projects. |
| `Library.*` | The browser's library: sounds and presets, search and the filters' counts (both on a thread of their own as the browser asks), their tags and keys from their names, folders and loop data (the loop data read once by its index and kept beside the settings, in `Library facts.txt`), favorites and collections kept by file, and the presets' folders (Phazite's own presets, next to the app, and the user's), which it lists as one where the browser shows them so. |
| `BrowserModel.*` | What the browser shows of the library, without its window: the places, what each holds, the presets under each device, the containers' rows and Track Presets (`PresetShelf`, Phazite's and the user's as one), Other Presets (the user's of no device listed), search results, the filters and what each offers, and the rows' texts. |
| `SoundPack.*` | The Phazite Sounds download, pinned by its release's address, size and SHA-256 (`SoundPack::getPinnedRelease()`). |
| `Updates.*` | The update check: the latest of Phazite's GitHub releases, asked only when the user asks or has turned on the check as Phazite opens, and its installer taken only from those releases, over HTTPS, checked by its size and SHA-256, and again before it starts. |

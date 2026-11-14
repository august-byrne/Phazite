# resources

The data files Phazite is built with. Most are compiled into the program as `PhaziteData` (`juce_add_binary_data` in
`CMakeLists.txt`); the fonts and the presets are copied next to the app after each build.

| Path | What it holds | How it ships |
|---|---|---|
| `keymaps/` | The keymaps: Phazite's own, FL Studio's and Ableton Live's default shortcuts, as JUCE key descriptions. | Compiled in |
| `outlines/*.outline.json` | Song outlines: a song's sections with their usual lengths, an energy level and a tip each. Every file named so here is one, listed in `CMakeLists.txt`. | Compiled in |
| `progressions/progressions.json` | The chord progressions. | Compiled in |
| `songs/*.mid` | The notes of example songs that others made and shared, each with its notice beside it (`songs/<name>.txt`). | Compiled in |
| `icons/lucide/` | Lucide's icons, with their licence (`LICENSE`). | Compiled in |
| `logo/phazite.svg` | Phazite's logo, its own drawing, from which CMake also makes the app's icon as it configures. | Compiled in |
| `licenses/notices.json` | The one list of the notices of the code and fonts from others that Phazite is built with; compiled in for the About box too. | Compiled in |
| `licenses/*.txt` | The notices' texts that no submodule holds. `cmake/PhaziteNotices.cmake` puts each entry's text into `licenses/` next to the app, with Phazite's own, and writes `NOTICES.txt`. | Next to the app |
| `fonts/Inter/` | The UI font, Inter, with its OFL text. | Copied next to the app into `fonts/` |
| `presets/<device>/` | The presets that come with Phazite, one folder per device ([docs/presets.md](../docs/presets.md)), which the browser and the devices' Presets menus list. | Copied next to the app into `presets/` |

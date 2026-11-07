# Third-party code

Phazite is licensed under the GNU Affero General Public License, version 3 or (at your option) any later version (AGPL-3.0-or-later): see [LICENSE](../LICENSE). Each of its source files says so in its first line: `// SPDX-License-Identifier: AGPL-3.0-or-later`. It is built with this code from others:

| Component | License | Role |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 9.0.5 (`external/JUCE`, at `d60b7a78`) | AGPL-3.0, or commercial | App, audio-device, audio-file, DSP and plug-in-hosting framework, compiled into the app and its helper program (`PhaziteHost`) |
| VST 3 SDK (bundled with JUCE) | MIT | VST3 plug-in hosting, compiled in |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) 4.0 | GPL-2.0-or-later, or commercial | Stretches and pitches audio clips and the browser's previews (its R3 engine), compiled in; the license ships next to the app (`licenses/RubberBand.txt`) |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) 7.6 (`external/r8brain-free-src`, at `cb2abb99`), with its bundled Ooura FFT | MIT (the Ooura FFT: free for any use, with credit) | Resamples audio files at another rate than the engine's as they play (`src/engine/audio/Resampler.cpp`), header-only, compiled in; the license ships next to the app (`licenses/r8brain-free-src.txt`). Sample rate converter designed by Aleksey Vaneev of Voxengo |
| ASIO® SDK 2.3.4 headers (bundled with JUCE) | GPL-3.0-only (Steinberg's GPLv3 option) | ASIO driver support on Windows, compiled in |
| [WindowsToolchain](https://github.com/MarkSchofield/WindowsToolchain) | MIT | Finds MSVC at build time on Windows; nothing compiled in |
| [doctest](https://github.com/doctest/doctest) 2.4.11 (`external/doctest`) | MIT | Unit-test framework; compiled into the tests only, not the app |
| [Inter](https://github.com/rsms/inter) 4.1 (in `resources/fonts/Inter`) | SIL Open Font License 1.1 | The UI font; shipped as separate files with its license text (`OFL.txt`), not compiled in |
| [Lucide](https://lucide.dev) 1.55.0 icons (in `resources/icons/lucide`) | ISC; the parts that come from Feather: MIT | The icons, compiled in; the license ships next to the app (`licenses/Lucide.txt`) |
| [libebur128](https://github.com/jiixyj/libebur128) | MIT | The loudness meter (`src/engine/nodes/LoudnessMeter.cpp`, compiled in) follows its K-weighting filters' analogue parameters and its gating histogram of 0.1 LU bins; its notice ships next to the app (`licenses/libebur128.txt`, from `resources/licenses/`) |
| [Visual Studio Code](https://github.com/microsoft/vscode) (Code - OSS): its 2026 Dark and 2026 Light themes and its Modern UI | MIT | The dark and light themes' colours and the window's sizes, corners, spacings and text sizes (`src/ui/theme/DesignTokens.h`; where each comes from: [vscode-look.md](vscode-look.md)), taken as values, no code; its notice ships next to the app (`licenses/VSCode.txt`, from `resources/licenses/`) |
| [Phazite Sounds](https://github.com/august-byrne/phazite-sounds): drums, 808s, loops, effects and foley, from VCSL, Virtuosity Drums, Big Rusty Drums, body_percussion, FreePats, Sonic Pi, EatMe and Kenney, and Phazite's own synthesis | CC0 1.0 (public domain) | The browser's free sounds, usable in any song without credit. Not part of Phazite: a separate download from their own repository, which Phazite fetches only when asked. Their `sources.json` says where each came from |

JUCE is also available under a commercial license; Phazite uses its open-source license, the AGPL-3.0. JUCE bundles further open-source code, such as zlib, libpng, libjpeg, FLAC, Ogg Vorbis, HarfBuzz, SheenBidi and LunaSVG with plutovg (its SVG reader since JUCE 9, both MIT): see `external/JUCE/LICENSE.md`, and the license files and source-file headers of the bundled code under `external/`.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media Technologies GmbH. Visual Studio Code is a trademark of Microsoft Corporation; Phazite isn't affiliated with or endorsed by Microsoft, and its themes don't carry VS Code's name.

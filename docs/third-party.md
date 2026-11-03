# Third-party code

Phazite is licensed under the GNU Affero General Public License, version 3 or (at your option) any later version (AGPL-3.0-or-later): see [LICENSE](../LICENSE). Each of its source files says so in its first line: `// SPDX-License-Identifier: AGPL-3.0-or-later`. It is built with this code from others:

| Component | License | Role |
|---|---|---|
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) | GPL-3.0-or-later, or commercial | Audio engine, compiled in |
| [JUCE](https://github.com/juce-framework/JUCE) 8 (inside Tracktion Engine) | AGPL-3.0, or commercial | App, audio-device and plug-in-hosting framework, compiled in |
| VST 3 SDK (bundled with JUCE) | MIT | VST3 plug-in hosting, compiled in |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) 4.0 | GPL-2.0-or-later, or commercial | Stretches and pitches audio clips and the browser's previews (its R3 engine), compiled in; the license ships next to the app (`licenses/RubberBand.txt`) |
| libsamplerate (bundled with Tracktion Engine) | BSD-2-Clause | Sample-rate conversion, compiled in |
| ASIO® SDK 2.3.4 headers (bundled with JUCE) | GPL-3.0-only (Steinberg's GPLv3 option) | ASIO driver support on Windows, compiled in |
| [WindowsToolchain](https://github.com/MarkSchofield/WindowsToolchain) | MIT | Finds MSVC at build time on Windows; nothing compiled in |
| [doctest](https://github.com/doctest/doctest) (bundled with Tracktion Engine) | MIT | Unit-test framework; compiled into the tests only, not the app |
| [Inter](https://github.com/rsms/inter) 4.1 (in `resources/fonts/Inter`) | SIL Open Font License 1.1 | The UI font; shipped as separate files with its license text (`OFL.txt`), not compiled in |
| [Lucide](https://lucide.dev) 1.55.0 icons (in `resources/icons/lucide`) | ISC; the parts that come from Feather: MIT | The icons, compiled in; the license ships next to the app (`licenses/Lucide.txt`) |
| [libebur128](https://github.com/jiixyj/libebur128) | MIT | The loudness meter (`src/engine/nodes/LoudnessMeter.cpp`, compiled in) follows its K-weighting filters' analogue parameters and its gating histogram of 0.1 LU bins; its notice ships next to the app (`licenses/libebur128.txt`, from `resources/licenses/`) |
| [Phazite Sounds](https://github.com/august-byrne/phazite-sounds): drums, 808s, loops, effects and foley, from VCSL, Virtuosity Drums, Big Rusty Drums, body_percussion, FreePats, Sonic Pi, EatMe and Kenney, and Phazite's own synthesis | CC0 1.0 (public domain) | The browser's free sounds, usable in any song without credit. Not part of Phazite: a separate download from their own repository, which Phazite fetches only when asked. Their `sources.json` says where each came from |

Tracktion Engine and JUCE are also available under commercial licenses; Phazite uses their open-source licenses (section 13 of both the AGPL-3.0 and the GPL-3.0 allows combining code under the two licenses). Both bundle further open-source code, such as choc, farbot, magic_enum, crill, rpmalloc, zlib, libpng, libjpeg, FLAC, Ogg Vorbis, HarfBuzz and SheenBidi: see `external/tracktion_engine/LICENSE.md`, `external/tracktion_engine/modules/juce/LICENSE.md`, and the license files and source-file headers of the bundled code under `external/`.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media Technologies GmbH.

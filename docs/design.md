# Phazite's design

Phazite is an open-source digital audio workstation for making music: a playlist, a piano roll and a mixer in one window,
its own instruments and effects, VST3 plug-ins hosted in helper processes, and a history of the song's versions. This
page is the way into its design. Each numbered section below is one page (or a few) under [design/](design/) that says
how that part of Phazite works now, what is built, what is planned and what was declined.

The section numbers are the ones the code's comments use ("docs/design.md, section 9"): they stay as they are, so a
comment leads here and from here to its page.

- What was decided, and why: [design/decisions.md](design/decisions.md).
- What waits for the owner's answer: [open-questions.md](open-questions.md).
- What comes next: [roadmap.md](roadmap.md). What was built: [changelog.md](changelog.md).
- How the code is laid out: [architecture.md](architecture.md).
- The document these pages were made from, whole, with its dated history, its decision log, its research summary and
  its sources: [archive/design-2026-11-13.md](archive/design-2026-11-13.md).

## 1. Vision and scope

What Phazite is, and what it leaves to others: [design/product.md](design/product.md).

## 2. Who Phazite is for

Music professionals, with music production first: [design/product.md](design/product.md).

## 3. Design principles

The twelve principles every part is held to: [design/product.md](design/product.md).

## 4. The window

The title bar, the transport bar, the panels and their views, the status bar and the look:
[design/window.md](design/window.md).

## 5. Building blocks

Tracks, clips, devices and how they fit together: [design/building-blocks.md](design/building-blocks.md).

## 6. The playlist

The song's tracks and clips along the time line: [design/playlist.md](design/playlist.md).

## 7. The piano roll

Notes, and the audio clip editor beside it: [design/piano-roll.md](design/piano-roll.md).

## 8. The mixer

Channels, sends, returns, buses and the sidechain: [design/mixer.md](design/mixer.md).

## 9. Devices, containers and presets

What a device is, containers, presets and Swap: [design/devices.md](design/devices.md). Each of Phazite's own devices
has a page with its parameters, how it works and its limits:

| Instruments | Effects |
| --- | --- |
| [Synth](design/devices/synth.md) | [EQ](design/devices/eq.md), [Filter](design/devices/filter.md) |
| [Wavetable](design/devices/wavetable.md) | [Compressor](design/devices/compressor.md), [Multiband](design/devices/multiband.md), [Limiter](design/devices/limiter.md) |
| [FM](design/devices/fm.md) | [Saturator](design/devices/saturator.md), [Age](design/devices/age.md) |
| [Sampler](design/devices/sampler.md) | [Delay](design/devices/delay.md), [Reverb](design/devices/reverb.md) |
| [Drum Sampler](design/devices/drum-sampler.md) | [Motion](design/devices/motion.md) (Chorus, Flanger, Phaser) |
| | [Vocoder](design/devices/vocoder.md), [Stereo](design/devices/stereo.md) |

The presets that come with Phazite: [presets.md](presets.md).

## 10. Automation and modulation

Lanes, write modes, modulators and macros: [design/automation-and-modulation.md](design/automation-and-modulation.md).

## 11. The browser

Sounds, presets, search, filters, preview and Swap: [design/browser.md](design/browser.md).

## 12. Commands, keys and help

The command registry, the keymaps, the command search, hints and lessons:
[design/commands-keys-help.md](design/commands-keys-help.md).

## 13. Getting started and song outlines

The welcome window, New Project, the guide, the example songs and the outlines:
[design/getting-started.md](design/getting-started.md).

## 14. Chords

The chord lane and the progressions: [design/chords.md](design/chords.md).

## 15. Sound generator

Planned for after version 1: [design/sound-generator.md](design/sound-generator.md).

## 16. AI and generative features

Planned for after version 1, or declined: [design/ai.md](design/ai.md).

## 17. Recording and Capture

Arming, takes, punch-in and Capture: [design/recording.md](design/recording.md).

## 18. Projects, saving and export

The project folder, saving, export, stems, freeze and bounce:
[design/projects-saving-export.md](design/projects-saving-export.md). The song's versions:
[design/versions.md](design/versions.md). The formats, written down for other tools:
[project-format.md](project-format.md) and [versions-format.md](versions-format.md).

## 19. Plug-in hosting and stability

VST3 plug-ins in helper processes, and what happens when one crashes:
[design/plugin-hosting.md](design/plugin-hosting.md).

## 20. Performance

The rules and the targets: [design/performance.md](design/performance.md). The numbers measured:
[performance.md](performance.md). What breaks the real-time rules and what is done about it:
[realtime-audit.md](realtime-audit.md).

## 21. Accessibility

Keyboard use, screen readers, contrast and scale: [design/accessibility.md](design/accessibility.md).

## 22. Privacy, licences and trademarks

What Phazite sends and stores, its licence and the code it is built with:
[design/privacy-licences.md](design/privacy-licences.md) and [third-party.md](third-party.md).

## 23. Phazite's engine

The song model, the playback engine and how the song becomes what plays: [design/engine.md](design/engine.md).

## 24. Code structure

The layers and their rules: [architecture.md](architecture.md).

## 25. Open questions

What waits for the owner: [open-questions.md](open-questions.md).

## 26. Decision log

The decisions that still matter: [design/decisions.md](design/decisions.md). The full log, with its dates, is in the
[archive](archive/design-2026-11-13.md).

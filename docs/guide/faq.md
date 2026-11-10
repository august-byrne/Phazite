---
title: Questions
description: What people ask about Phazite, from its license to the plug-ins it plays.
sidebar:
  order: 3
---

## Is Phazite free?

Yes. It's free and open source, under the GNU Affero General Public License, version 3 or later (AGPL-3.0-or-later), and all of its code is on [GitHub](https://github.com/august-byrne/Phazite).

## Can I sell music I make with it?

Yes. The license covers Phazite itself, not the songs you make with it. Phazite Sounds are in the public domain (CC0), yours to use in anything. The example songs are other people's music under Creative Commons licenses: if you build a song on one, credit its author as its notes say.

## What does pre-alpha mean?

Phazite works, but it is still being built: it changes from one build to the next, and projects, presets and settings saved now may not open in later builds. Export what you want to keep as audio.

## Which plug-ins does it play?

VST®3 plug-ins, each in a process of its own, so that one that crashes or hangs can't take Phazite or your song down. CLAP comes later, and Audio Units with the macOS version.

## Does it run on macOS or Linux?

Windows 11 x64 now. Linux builds and passes its tests, and a beta comes with the first releases. macOS comes after version 1.

## Do I need an account? Does Phazite go online?

No account. Phazite goes online only when you ask it to: to download Phazite Sounds, or to check for a newer version. [Privacy](privacy.md) has the details.

## Where are my projects?

In `Documents\Phazite\Projects`, unless you save them elsewhere. Your presets are in `Documents\Phazite\Presets`, and Phazite Sounds in `Documents\Phazite\Packs`.

## I'm coming from FL Studio. What's different?

Phazite keeps the playlist, the piano roll and the mixer, and makes each track one row in the playlist, one chain of devices and one mixer channel, with no channel rack. Choose FL Studio's shortcuts in the Welcome window, or later under **Keymap**, in the menu of the gear at the bottom left.

## Can I open songs from other DAWs?

DAWproject files, the open format that Bitwig Studio, Studio One and Cubase share: **File > Import DAWproject** opens one as a new project, so far without its plug-ins and sends. Phazite exports DAWprojects too, with your VST3 plug-ins and their settings, and its own devices bounced beside their notes.

## How can I help?

Try it, and [open an issue](https://github.com/august-byrne/Phazite/issues) for what tripped you up or an idea. [CONTRIBUTING.md](../../CONTRIBUTING.md) covers working on the code. Sounds for Phazite Sounds go to [its own repository](https://github.com/august-byrne/phazite-sounds), in the public domain only.

VST is a registered trademark of Steinberg Media Technologies GmbH. FL Studio, Bitwig Studio, Studio One and Cubase are trademarks of their owners, named only to say what Phazite works with.

# Privacy, licences and trademarks

*Design section 22.*

Phazite is free software under the AGPL-3.0-or-later. It works with no account and no network, ships only code and
fonts under compatible licences, and names other companies' products only in plain text. This page also covers how
Phazite is installed and updated, because that is the one place it goes online by itself.

## Status

| Feature | State | Note |
| --- | --- | --- |
| No account | Built | The activity bar's Account icon says there is none yet |
| No telemetry | Built | Phazite sends nothing about its use |
| No personal data in project files | Built | |
| Online only when the user asks | Built | Phazite Sounds, and the update check |
| Update check as Phazite opens | Built | Off until the user turns it on |
| Downloads checked against a SHA-256 | Built | |
| Third-party notices next to the app, in the installer and in the About box | Built | |
| About Phazite | Built | |
| Windows installer and uninstaller, with file associations | Built | Not yet tried by a person on a clean PC |
| Updating from inside Phazite | Built | Not yet tried by a person: no release is published yet (roadmap step 4.5) |
| Code signing, and checking an update's publisher | Planned | Roadmap step 4.3 |
| ASIO® and VST® logos | Declined | They are not under a free licence |
| Crash reporting | Planned | Opt-in if it is ever built; no roadmap step |
| Keys for optional cloud AI kept in the system's credential store | Planned | With the AI assistant (after v1, item 3) |

## Privacy

- **No account and no telemetry.** Projects and their history stay on the user's computer. Any future crash
  reporting is opt-in.
- **No personal data in project files.** A project file is the song alone, without the user's name, and the files
  it plays are kept by their paths within the project's folder.
- **A project's history names no one.** It knows each computer that writes it by an ID made at random, once, and
  kept in the app's settings, never by the computer's name or the user's ([versions.md](versions.md)).
- **Downloads:** Phazite connects to the internet only when the user asks it to download something. So far that is
  Phazite Sounds (Help > Phazite Sounds), from the release of its repository on GitHub that Phazite is built to
  install, over HTTPS. Phazite checks the download against the size and SHA-256 it was built with.
- **Updates:** Phazite asks whether a newer version is out only when the user asks (Help > Check for Updates...), or
  each time it opens once the user has turned on **Check for Updates When Phazite Opens**, which is off until then.
  A check reads one small file from Phazite's latest GitHub release and sends nothing but the request.
- **Plug-ins** are other people's programs. What a plug-in sends or stores is its own affair
  ([plugin-hosting.md](plugin-hosting.md)).
- **Settings** are a file in the user's application data. They hold no key or password.

## Licences

- **Phazite is AGPL-3.0-or-later.** Every C and C++ file starts with its SPDX line.
- **Compatibility:** only code under licences compatible with it is added: GPL-3.0, AGPL-3.0, LGPL-2.1-or-later,
  LGPL-3.0, MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, Zlib and BSL-1.0 (the Boost Software License, a lax
  licence the FSF lists as compatible with the GPL; the owner, 2026-11-21, for the immer library). The 4-clause BSD
  licence is not.
- **No proprietary code:** nothing under a proprietary licence alone is compiled in, and no dependency's proprietary
  option is turned on. The ASIO SDK headers bundled with JUCE are used under Steinberg's GPLv3 option, and the
  VST 3 SDK under the MIT licence.
- **What Phazite is built with** is listed in [../third-party.md](../third-party.md), with each part's licence and
  role.
- **Fonts** may also use the SIL Open Font License 1.1. They ship as separate files next to the app, not compiled
  into it, with their licence text and copyright notice. The UI font is Inter.
- **Sounds:** the program holds no sounds or samples. Phazite Sounds are CC0 1.0 and a separate download from their
  own repository. The metronome's click, the startup sound and the factory drum sounds are synthesized.
- **Example songs** that someone else made ship as their notes alone, under CC0 or CC BY only, credited in the
  song's description, its project's notes and the notices.
- **The formats' specifications:** [../project-format.md](../project-format.md) and
  [../versions-format.md](../versions-format.md) are under the AGPL, as all of Phazite is, and each says that
  implementing its format, in software of any licence, open or closed, needs no permission: copyright covers the
  text, not the format.

### Notices

- **One list:** `resources/licenses/notices.json` names everything from others that ships, compiled into the app
  and its helper or a font next to it, JUCE's bundled libraries included, each with its version, licence, copyright
  and use.
- **Next to the app:** as it configures, CMake copies each one's text into `licenses/` next to the app, with
  Phazite's own licence and the GPL, version 3, that the ASIO SDK is used under, and writes `NOTICES.txt` from the
  list.
- **In the installer:** the installer shows `NOTICES.txt` on its first page, and installs them all.
- **A test** checks that every notice has its text, and that nothing else is in the folder.

### About Phazite

Help > About Phazite shows:

- the logo and the version; **Copy Version** copies it with the system's name, for a report;
- "Copyright (C) 2026 the Phazite contributors";
- the notice the AGPL asks every interactive program under it to show: that it comes with no warranty, that it may
  be shared and changed under the licence, and where the licence is;
- **Source Code**, a link to the source;
- every licence in a list, Phazite's first, with the text of the one chosen beside it;
- the trademarks' sentences under them.

## Trademarks

- VST is a registered trademark of Steinberg Media Technologies GmbH.
- ASIO is a registered trademark of Steinberg Media Technologies GmbH.
- Visual Studio Code is a trademark of Microsoft Corporation. Phazite isn't affiliated with or endorsed by
  Microsoft, and its themes don't carry VS Code's name.
- Other product names in Phazite and its documentation are trademarks of their respective owners. They are used
  only to say what Phazite works with, to compare, or to say what a sound is like, and Phazite isn't affiliated with
  their owners.

**Naming ASIO and VST.** Phazite names both in plain text, as it must to say which driver or plug-in format to
choose. Documentation that names ASIO writes "ASIO®" at its first mention and carries the sentence above. The About
box, `NOTICES.txt` and the documentation carry both sentences.

**No ASIO or VST logos.** Phazite uses the ASIO SDK under the GPL, version 3, and the VST 3 SDK under the MIT
licence. Under those, using the names or the logos is optional, and if they are used, Steinberg's usage guidelines
apply. The logos are not under a free licence, so shipping them would break the rule that everything Phazite ships
is. If Steinberg asks for the logo where ASIO is configured, it can be added there.

**Presets after classic instruments** are named by their sound, never by a trademark
([../presets.md](../presets.md)):

- "808" and "909" are used: they have become the words for those sounds.
- The makers' model names and company names are never used, and neither are near-variants of a mark.
- Every other classic is named by its sound: Acid Bass, Tine E-Piano, String Machine.

## Installing and updating

### The installer

- The Windows installer is made with Inno Setup from `packaging/windows/Phazite.iss`, through CMake's `installer`
  target. It carries an uninstaller.
- **For the user who runs it, without administrator rights,** in `%LOCALAPPDATA%\Programs\Phazite`, so that an
  update needs no prompt for administrator rights. `/ALLUSERS` installs for every user.
- Its first page shows `NOTICES.txt`, with Next, not "I accept": the GPL family's licences don't ask to be accepted
  to use the program.
- **File associations:** a `.phazite` file opens in Phazite, with its icon. A `.dawproject` file gets Phazite in its
  Open With menu, which imports it, without taking the file type from the DAW the user has.
- One Phazite runs at a time. A file opened while Phazite runs goes to that Phazite, which asks to save the open
  project first.
- **The uninstaller** takes the app, its shortcuts and its associations away, and leaves the user's projects,
  presets, Phazite Sounds and settings.

### Updates

- A release carries `phazite-update.json`: its version, its page, and on Windows its installer's address, size and
  SHA-256. The `installer` target writes it.
- Phazite takes an installer only from its own GitHub releases, over HTTPS only, following redirects only to HTTPS,
  and never more bytes than the update file says.
- It checks the installer's SHA-256 once it has it, and again just before it runs it.
- It runs the installer only when the user clicks **Install and Restart**. The project is saved first if the user
  wants, Phazite closes, and the installer, which waits for Phazite to close, updates it silently and opens it
  again.
- **A newer release shows** as a badge on the settings' icon at the activity bar's foot, and in the update command's
  name ("Update to Phazite 0.2.0..."). It never opens a window by itself.

## Decisions

- No account and no telemetry: a DAW holds people's unreleased work, and nothing about it leaves the computer
  unasked.
- The update check is opt-in and sends nothing but the request.
- Updates come only from Phazite's own releases, checked twice by SHA-256, because an updater runs a program.
- The installer is per user and needs no administrator rights, so that updates are silent.
- Copyright is "the Phazite contributors", with no personal name.
- No ASIO or VST logos: they are optional under the licences Phazite uses and are not free to ship.
- "808" and "909" name sounds; makers' marks are never used in preset names.
- Fonts and sounds stay outside the program: fonts as files beside it, sounds as a separate download.
- The formats' texts stay under the AGPL, and say that implementing the formats needs no permission.

## Not built

- Code signing of the app and the installer: Planned (roadmap step 4.3). Until then Windows' SmartScreen warns of
  the installer. Once Phazite is signed, it also checks that an update is signed by the same publisher.
- A published release with its installer and update file: Planned (roadmap step 4.5).
- An installer for Linux: none; Phazite is built from source there ([../building.md](../building.md)).
- Crash reporting: Planned, opt-in.
- An account, and anything kept online: Planned, opt-in.
- A credential store for cloud AI keys: Planned, with the AI assistant, after v1.
- A trademark lawyer's look at the preset names before a release: not done.

## In the code

- `resources/licenses/notices.json`, `cmake/PhaziteNotices.cmake`: the notices and what puts them next to the app.
- `src/ui/shell/AboutPanel.h`, `src/model/engine/AppInfo.h`: the About box, the version and the copyright.
- `src/services/Updates.h`, `src/ui/shell/UpdatePanel.h`, `cmake/WriteUpdateFile.cmake`: the update check.
- `src/services/SoundPack.h`: the Phazite Sounds download and its pin (`SoundPack::getPinnedRelease()`).
- `src/services/Settings.h`: every setting, among them `updateCheck` and `computer`.
- `packaging/windows/Phazite.iss`, `tools/test-installer.ps1`: the installer and its check.
- `src/app/Main.cpp`: one Phazite at a time, and the `PhaziteRunning` mutex the installer waits for.
- Tests: `tests/AboutPanelTests.cpp`, `tests/UpdatesTests.cpp`, `tests/SoundPackTests.cpp`.
- [../third-party.md](../third-party.md): the code from others, and its licences.

# Building Phazite

Phazite has no downloads yet, so you build it from source. This page explains each step of the short version in the [README](../README.md#try-it), the build on Linux and WSL2, and what to do when something goes wrong. To work on the code, read [CONTRIBUTING.md](../CONTRIBUTING.md) next.

- [Windows](#windows)
- [Running Phazite](#running-phazite)
- [Troubleshooting](#troubleshooting)
- [Linux and WSL2](#linux-and-wsl2)

## Windows

Open PowerShell (press Start, type `PowerShell` and press Enter; don't choose "Run as administrator", because the setup script asks for administrator rights itself) and type the commands one line at a time.

### Requirements

| You need | Notes |
|---|---|
| Windows 11, 64-bit (x64) | The only tested system. Visual Studio 2026, which provides the compiler, doesn't support Windows 10. |
| winget | The Windows Package Manager, part of Microsoft's App Installer, which comes with Windows 11. |
| Administrator rights | For the installers. The setup script asks once, through one UAC (User Account Control) prompt. |
| Internet access | The Visual Studio Build Tools alone are about a 2 GB download. |
| About 15 GB of free disk space | The Build Tools and the Windows SDK take about 6 GB. A build folder takes from 1.5 GB (`windows-release`) to several GB (`windows-profile`, and `windows-debug` most, with their debug symbols). |
| A short folder path, not cloud-synced | Such as `C:\src\Phazite`, outside folders that OneDrive or a similar service syncs. |

**Why a short path?** Windows limits most paths to 260 characters, and the MSVC compiler and linker keep that limit even when long paths are turned on in Windows. The longest path a build creates inside the repository is 179 characters, so the repository folder's own path must stay under about 80 characters, or the build can fail with `fatal error C1081` (file name too long).

### 1. Get Git and the source

If `git --version` prints a version, skip the install. Otherwise install Git with winget (Windows may ask for administrator rights):

```powershell
winget install --id Git.Git --exact --source winget --scope machine
```

Then close PowerShell and open a new window, so that it finds `git`. Clone the repository into a short folder (git creates any missing parent folders), and run every later command from that folder, the repository root. Don't add `--recurse-submodules`: the submodules need a git setting from step 2.

```powershell
git clone https://github.com/august-byrne/Phazite.git C:\src\Phazite
cd C:\src\Phazite
```

### 2. Run the setup script

```powershell
tools\setup-windows.cmd
```

It runs [tools/setup-windows.ps1](../tools/setup-windows.ps1) without changing PowerShell's execution policy (you can also double-click it in File Explorer), and:

- installs or updates Git; CMake and Ninja, which generate and run the build; gitleaks, the secret scanner the git hooks run; and Visual Studio 2026 Build Tools (the MSVC C++ compiler and the Windows 11 SDK, without the Visual Studio IDE);
- sets two global git options for your user account: `core.longpaths true`, which lets git handle paths over 260 characters, and a rule that fetches JUCE over HTTPS (see step 3);
- turns on this clone's git hooks, which stop a commit or push that would publish a secret or a private email address ([CONTRIBUTING.md](../CONTRIBUTING.md#the-git-hooks));
- turns on crash dumps for `Phazite.exe` (Windows keeps the last 3);
- installs or updates VS Code's C/C++ and CMake Tools extensions, if VS Code is installed (it never installs VS Code itself).

It asks for administrator rights once (click Yes), then runs the installers in a second window that closes by itself. Don't close either window; wait until the first one prints its `Result:` line. A full run takes a few minutes on a fast connection, and up to an hour on a slow one. It is safe to run again: it installs what is missing and updates the rest.

| Switch | Effect |
|---|---|
| `-CheckOnly` | Only reports what is installed and whether this clone's git hooks are on. Changes nothing and needs no administrator rights. |
| `-SkipUpdates` | Installs what is missing, but leaves installed tools as they are. |
| `-WithTestPlugin` | Also installs Surge XT, an open-source VST3 synth, to try plug-in scanning with. |
| `-WithLlvm` | Also installs LLVM (clangd, clang-tidy, clang-format). |
| `-SkipVSCodeExtensions` | Doesn't install or update the VS Code extensions. |

The last line states the result, such as `Result: exit code 0 (done)`:

| Exit code | Normal run | With `-CheckOnly` |
|---|---|---|
| 0 | Done | Everything is present |
| 3010 | Done, but restart Windows, then run it again | Visual Studio is waiting for a restart |
| 1 | A step failed: read the `FAILED` lines and the log named after `Full log:` | Something is missing, or not yet on this window's PATH |
| 2 | Could not start (no winget, administrator rights refused, ...) | Could not check (for example, from 32-bit PowerShell) |

**When it finishes:** if it asks you to restart (exit code 3010, or a message saying so), restart Windows and run it again. Then **open a new PowerShell window** and `cd C:\src\Phazite`: windows that were already open, including this one, can't find the new tools yet. Quit VS Code too if it is running (File > Exit) and start it again from the Start menu. You're ready when `tools\setup-windows.cmd -CheckOnly` ends with `Result: exit code 0 (everything present)`.

### 3. Fetch the submodules

The submodules are the other git repositories Phazite builds with, pinned at exact commits: Tracktion Engine (with the JUCE commit it pins), Rubber Band Library and WindowsToolchain, about 0.2 GB. `--depth 1` skips their history:

```powershell
git submodule update --init --recursive --depth 1
```

Run the same command after every pull or branch switch. Step 2 must come first: Tracktion lists JUCE with an SSH address, which only works with an SSH key registered on GitHub; the setup script's git rule fetches it over HTTPS instead.

### 4. Configure and build

```powershell
cmake --preset windows-profile
cmake --build --preset windows-profile
```

Configuring finds the compiler and prepares a build folder, `build\<preset>`; building compiles and links Phazite. The first configure of a folder also builds JUCE's `juceaide` helper, so it takes a minute or two longer. A preset is a named set of build settings, in [CMakePresets.json](../CMakePresets.json) (`cmake --list-presets` lists them); use the same one in both commands:

| Preset | For |
|---|---|
| `windows-profile` | Playing and everyday testing: as fast as Release, with the debug symbols that make a crash dump readable. |
| `windows-release` | The optimized app alone, without debug symbols. |
| `windows-debug` | Stepping through code in a debugger. It plays audio 3 to 20 times slower ([performance audit](performance-audit.md)), so don't judge Phazite's sound or speed by it. |

Configure has worked when its last line is `-- Build files have been written to: ...`, and the build when its last line is a `Linking CXX executable` line (or `ninja: no work to do.`). Otherwise the first line with `CMake Error` or `error` names the cause: see [Troubleshooting](#troubleshooting). There's no Developer PowerShell or vcvars to run first: [cmake/toolchains/windows-msvc.cmake](../cmake/toolchains/windows-msvc.cmake) finds MSVC by itself.

## Running Phazite

| Preset | The app |
|---|---|
| `windows-profile` | `build\windows-profile\Phazite_artefacts\RelWithDebInfo\Phazite.exe` |
| `windows-release` | `build\windows-release\Phazite_artefacts\Release\Phazite.exe` |
| `windows-debug` | `build\windows-debug\Phazite_artefacts\Debug\Phazite.exe` |

Start it from File Explorer or PowerShell, or with F5 in [VS Code](../CONTRIBUTING.md#vs-code). The Welcome window opens at the first start, and the README's [First steps](../README.md#first-steps) say what to try. Your settings (audio and MIDI devices, the plug-in list, the theme and the shortcuts) are kept in `%APPDATA%\Phazite\Phazite.settings`.

- **Sound:** Phazite opens your default audio output. With an audio interface, choose **ASIO**® under **Options > Audio Settings** for the lowest delay; without an ASIO driver, [FlexASIO](https://github.com/dechamps/FlexASIO) (MIT) gives you one that plays through Windows' normal audio. Audio inputs are off until you turn them on there.
- **Plug-ins:** **Options > Scan for Plug-ins** scans the standard VST3 folders. The scan runs in a separate copy of Phazite, so a plug-in that crashes while being scanned doesn't take Phazite down. With no plug-ins installed, `tools\setup-windows.cmd -WithTestPlugin` installs Surge XT to try it with.
- **Crashes:** a crash leaves `Phazite.exe.<number>.dmp` in `%LOCALAPPDATA%\CrashDumps`. Only `windows-profile` and `windows-debug` builds have the symbols that make one readable. A dump holds Phazite's whole memory, so never attach one to a public issue: describe what you did instead.

## Troubleshooting

| Problem | Fix |
|---|---|
| `fatal: not a git repository`, `tools\setup-windows.cmd` "is not recognized", or CMake `Could not read presets` | This window isn't in the repository folder (new windows start in your user folder): run `cd C:\src\Phazite` and try again. |
| `winget` "is not recognized" | Install or update "App Installer" from the Microsoft Store, then open a new PowerShell window. |
| `git`, `cmake` or `ninja` "is not recognized" | The window is older than the install. Open a new one; for VS Code, quit it completely (File > Exit) and start it from the Start menu. Then run `tools\setup-windows.cmd -CheckOnly`: if it reports `MISSING`, run the full setup again; if a new window still can't find the tools, sign out of Windows and back in. |
| Configure finds no complete Visual Studio installation | Run `tools\setup-windows.cmd`, and restart Windows first if it said a restart is required. |
| Configure says a folder under `external` is empty or missing | The submodules aren't there: run [step 3](#3-fetch-the-submodules) (after step 2 on a new machine). |
| Fetching JUCE asks whether to trust `github.com`, or fails with `Permission denied (publickey)` or `Host key verification failed` | The HTTPS rule for JUCE is missing (answer `no` if asked). Run `tools\setup-windows.cmd`, then step 3 again. |
| `fatal error C1081` (file name too long) | The repository path is too long: clone again into a short one such as `C:\src\Phazite`. |
| `LNK1104` on `Phazite.exe` | Phazite, its plug-in scan or a debug session is still running. Close it and build again. |
| Errors after a Build Tools update (new MSVC) or a toolchain change | Run `cmake --preset <preset> --fresh` (the setup script prints this when MSVC changes). If it still fails, delete `build\<preset>` and configure again. |
| No sound | Choose an output in **Options > Audio Settings**. |
| A message from `pre-commit` or `pre-push` | See [the git hooks](../CONTRIBUTING.md#when-a-hook-stops-you) in CONTRIBUTING.md. |
| Anything else | The first line containing `error`, `fatal` or `FAILED` usually names the cause. If you're stuck, [open an issue](https://github.com/august-byrne/Phazite/issues) with that line and the command you ran. |

## Linux and WSL2

> **Partly tested.** CI builds all four Linux presets on Ubuntu 24.04 with the packages below, but nobody has run the app on Linux or set up WSL2 with these steps yet, so they may need fixes.

For WSL2, install Ubuntu 24.04 from a PowerShell opened with "Run as administrator", then restart Windows:

```powershell
wsl --install -d Ubuntu-24.04
```

After the restart, open Ubuntu with `wsl -d Ubuntu-24.04`; its first start asks you to create a Linux user name and password, which `sudo` asks for. Keep the clone in the Linux file system (such as `~/src`), not under `/mnt/c`. Install the packages before the first configure:

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build pkg-config \
    libasound2-dev libfreetype-dev libfontconfig-dev \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev
sudo apt install -y gdb libasound2-plugins   # optional: VS Code debugging, sound under WSL2
git config --global url.https://github.com/juce-framework/.insteadOf git@github.com:juce-framework/
git clone https://github.com/august-byrne/Phazite.git ~/src/Phazite
cd ~/src/Phazite
git submodule update --init --recursive --depth 1
cmake --preset linux-release
cmake --build --preset linux-release
build/linux-release/Phazite_artefacts/Release/Phazite
```

The `git config` line is the Linux version of the setup script's HTTPS rule for JUCE; run it once per machine (WSL keeps its own git settings). The presets use the system GCC (Ubuntu 24.04: g++ 13) and need CMake 3.28 or newer. The Linux build finds Linux VST3 plug-ins only (in `~/.vst3`, `/usr/local/lib/vst3` and `/usr/lib/vst3`, plus any folder listed in `VST3_PATH`). To commit, also set up the git hooks as [CONTRIBUTING.md](../CONTRIBUTING.md#the-git-hooks) describes.

| Preset | Build type | The app |
|---|---|---|
| `linux-release` | Release (optimized) | `build/linux-release/Phazite_artefacts/Release/Phazite` |
| `linux-debug` | Debug | `build/linux-debug/Phazite_artefacts/Debug/Phazite` |
| `linux-asan` | Debug with AddressSanitizer and UndefinedBehaviorSanitizer (memory errors, undefined behavior) | `build/linux-asan/Phazite_artefacts/Debug/Phazite` |
| `linux-tsan` | RelWithDebInfo with ThreadSanitizer (data races) | `build/linux-tsan/Phazite_artefacts/RelWithDebInfo/Phazite` |

Run the ThreadSanitizer build under `setarch -R`, which turns off address-space randomization for that one process (Ubuntu 24.04 and WSL2 kernels randomize more than the ThreadSanitizer in GCC 13 and 14 can start with). `TSAN_OPTIONS` passes it the known races in Tracktion Engine to leave out ([tools/tsan-suppressions.txt](../tools/tsan-suppressions.txt)), as the `linux-tsan` tests do:

```bash
TSAN_OPTIONS=suppressions=tools/tsan-suppressions.txt setarch $(uname -m) -R build/linux-tsan/Phazite_artefacts/RelWithDebInfo/Phazite
```

Under WSL2, the window appears on the Windows desktop through WSLg (WSL's built-in GUI support). JUCE has no PulseAudio backend, so for sound install `libasound2-plugins` and pick the ALSA device "Pulseaudio output" in **Audio Settings**. Expect no MIDI ports, and don't measure audio performance under WSL. Phazite can't download Phazite Sounds by itself on Linux yet: its button opens the [release page](https://github.com/august-byrne/phazite-sounds/releases) instead, and **Install from a File...** installs the zip.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

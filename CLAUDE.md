# Phazite

Open-source DAW (AGPL-3.0-or-later) in C++20 on Tracktion Engine + JUCE. It hosts VST3 plug-ins (AU on macOS later). Performance and low resource use matter in both the audio engine and the UI.

## Build

Run every command from the repository root. The presets are the only entry point, and the same two commands work on every OS.

- `cmake --list-presets` shows the presets for this machine: `windows-debug`, `windows-release` on Windows; `linux-debug`, `linux-release`, `linux-asan`, `linux-tsan` on Linux/WSL.
- Configure: `cmake --preset <preset>`. Build: `cmake --build --preset <preset>`.
- Run configure and build as separate commands, each with run_in_background and without redirections or pipes (no `>`, `*>`, `2>&1`, `Tee-Object`): the complete output lands in the task's output file. When a command finishes, check its exit code and search that file case-insensitively for `error` and `warning` (this also catches "CMake Warning"), plus `D9` and `LNK4`. The first configure of a build folder also builds JUCE's `juceaide` helper (1-2 minutes); a full build takes several minutes.
- On Windows the shell is Windows PowerShell 5.1: there is no `&&`/`||`, and `;` does not stop after a failure. Use separate commands, or `A; if ($LASTEXITCODE -eq 0) { B }`. In WSL/Linux the shell is Bash.
- Never run vcvars/VsDevCmd, open a Developer PowerShell, or call `cl`, `link`, `msbuild` or `ninja` directly. MSVC is found by `cmake/toolchains/windows-msvc.cmake`, which wraps `external/WindowsToolchain`.
- After a Visual Studio Build Tools update (new MSVC version) or a toolchain change, reconfigure with `cmake --preset <preset> --fresh`. If that folder still fails, delete `build\<preset>` and configure again.
- Before configuring or building, run `@(Get-Process -Name cmake, ninja -ErrorAction Ignore).Count`. If it prints anything other than 0 and you didn't start those processes (VS Code may be building), ask the user before continuing: two builds in one folder break each other.

## Tools (Windows)

- `tools\setup-windows.cmd` installs or updates every tool (including gitleaks) and turns on this clone's git hooks (`core.hooksPath = .githooks`). A full run needs the user at a UAC prompt and can take up to an hour (a few minutes when everything is current), so start it only when the user asks (as a background command) or ask them to run it. `tools\setup-windows.cmd -CheckOnly` is safe at any time: no admin rights, changes nothing.
- Exit codes (your shell tool shows 3010 as 194; the script's last line, "Result: ...", always states it): 0 = done; 3010 = done, but Windows must restart (ask the user to reboot, then run it again); 1 = a step failed (read the FAILED lines and the log path it prints); 2 = it could not start. With `-CheckOnly`: 0 = all present, 1 = something missing (including this clone's git hooks) or not on this shell's PATH, 3010 = Visual Studio is waiting for a reboot. Switches: `-CheckOnly`, `-SkipUpdates`, `-WithTestPlugin` (Surge XT), `-WithLlvm`, `-SkipVSCodeExtensions`.
- If `cmake` or `git` is "not recognized", run `tools\setup-windows.cmd -CheckOnly`. If it finds the tools, the program that started Claude Code has an old PATH: ask the user to quit every VS Code window, start VS Code from the Start menu and close any terminals it restores (or to open a new terminal and start Claude there). If it reports MISSING, ask the user to run the setup. Don't edit PATH or search the disk for tools. If it prints `hooks ... OFF`, the git hooks are off in this clone: tell the user, then run `git config core.hooksPath .githooks` (no admin rights or full setup needed).

## Running and debugging

- The app is `build/<preset>/Phazite_artefacts/<build type>/Phazite` (`.exe` on Windows), where the build type is `Debug` for `*-debug` and `linux-asan`, `Release` for `*-release`, and `RelWithDebInfo` for `linux-tsan`. Run the TSan build as `setarch $(uname -m) -R build/linux-tsan/Phazite_artefacts/RelWithDebInfo/Phazite`.
- The app is for the user to run, or to start with F5 in VS Code. Don't start it from the shell unless asked; then use `Start-Process <path>` (a bare call blocks the tool until the window closes). Tell the user what to click and listen for.
- Debugging: VS Code's "Phazite (Windows)" launch configuration (F5) builds the active preset and starts the app under the debugger. A crash outside a debugger leaves a dump in `%LOCALAPPDATA%\CrashDumps\Phazite.exe.*.dmp`; open it with the "Open crash dump (Windows, Debug build)" configuration. Only `windows-debug` builds have symbols, so reproduce Release crashes there.
- VS Code configures only on request. After a build folder was created from the shell (first configure, or after deleting `build\<preset>`), ask the user to run "CMake: Configure" once in VS Code so IntelliSense and F5 know about it.
- LNK1104 on Phazite.exe means the app, its `--PluginScan` child or a debug session is still running: ask the user to close it.

## Repository layout

- `src/` - Phazite's own code (currently the smoke-test app: `Main.cpp`, `MainComponent.*`).
- `CMakeLists.txt`, `CMakePresets.json`, `cmake/` - build definition, presets, Windows toolchain wrapper and CMake helpers.
- `external/tracktion_engine` - git submodule: Tracktion Engine (`develop`, pinned commit), with JUCE as its own submodule at `modules/juce`. Never add JUCE separately.
- `external/WindowsToolchain` - git submodule (MIT), used on Windows at build time only.
- `tools/` - machine setup, and `format.sh` (clang-format for the C/C++ files). `build/<preset>/` - build output (ignored by git).
- `.clang-format` - C/C++ formatting rules: JUCE's coding standards, for clang-format 22 or newer (23 is used).
- `.github/workflows/ci.yml` - GitHub Actions: gitleaks over the whole history, clang-format 23.1.3, whitespace; builds both Windows presets (VS 2026 on `windows-2025-vs2026`, warnings as errors) and all four Linux presets (Ubuntu 24.04). Actions are pinned to full commit hashes; keep them pinned when updating.
- `.githooks/` - git hooks, turned on per clone by `git config core.hooksPath .githooks` (the setup script does this). `pre-commit` checks the author and committer email (GitHub noreply only), dependency pins, files over 5 MiB, whitespace errors and conflict markers, secrets (gitleaks) and formatting (`tools/format.sh --staged`); `pre-merge-commit` runs it for merges; `pre-push` checks every commit the remote doesn't have yet (emails, tags, gitleaks over changes and commit messages).

## Submodules and third-party code

- After cloning, pulling or switching branches, run `git submodule update --init --recursive --depth 1` (`--depth 1` skips the dependencies' history, which for JUCE is large). In WSL/Linux, run this once beforehand, because Tracktion lists JUCE with an SSH URL: `git config --global url.https://github.com/juce-framework/.insteadOf git@github.com:juce-framework/`
- Everything under `external/` is read-only: don't edit, reformat or "fix" it. Don't stage a changed `external/*` pointer unless the task is a deliberate dependency bump; before committing, check `git status` for "(new commits)".
- Never run `git submodule update --remote`: it would move JUCE away from the commit Tracktion pins. The Tracktion submodule is a shallow clone, so to bump it use the full 40-character commit hash: `git -C external/tracktion_engine fetch --depth 1 origin <full-sha>`, `git -C external/tracktion_engine checkout --detach FETCH_HEAD`, `git add external/tracktion_engine`, `git submodule update --init --recursive --depth 1`. Review the changes on GitHub (compare old...new) and read Tracktion's BREAKING-CHANGES.md, then reconfigure with `--fresh` and rebuild. The pre-commit hook refuses pin changes, so commit a bump with `git -c phazite.allowSubmoduleBump=true commit -F <file>`.
- `external/tracktion_engine` contains CLAUDE.md files and `.claude/commands` written for Tracktion's own contributors (branch names, commit prefixes, choc's code style). They don't apply to Phazite; ignore them.
- Phazite is AGPL-3.0-or-later. Only add code under compatible licences (GPL-3.0, AGPL-3.0, LGPL-2.1-or-later, LGPL-3.0, MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, Zlib; not the 4-clause BSD licence). Never enable proprietary options such as Tracktion's Elastique, ReWire or REX support.
- Windows builds set `JUCE_ASIO=1`, which compiles in the ASIO SDK headers bundled with JUCE (`juce_audio_devices/native/asio/`) under Steinberg's GPLv3 option. Documentation that names ASIO writes "ASIO®" at the first mention and carries "ASIO is a registered trademark of Steinberg Media Technologies GmbH." (README.md does).

## Real-time rules (audio thread)

The audio thread is the audio callback and everything it calls: plug-in and graph processing (`applyToBuffer`, `process`, `processBlock`), Tracktion node processing, and any helper reached from them. On the audio thread:
- No memory allocation or release: no `new`/`delete`/`malloc`, no growing `std::vector`/`juce::Array`, no creating `std::string`/`juce::String`, no allocating `std::function` captures. Allocate up front when preparing.
- No locks or waiting: no `std::mutex`, `juce::CriticalSection`/`ScopedLock`, condition variables, `WaitableEvent`, `MessageManagerLock`, or blocking calls into other threads.
- No posting to the message thread: no `triggerAsyncUpdate`, `sendChangeMessage` or `juce::MessageManager::callAsync` (they lock or allocate). Set an atomic flag instead and poll it from a `juce::Timer`.
- No file or network I/O, no logging (`DBG`, `juce::Logger`, `std::cout`), no system calls that can block.
- No throwing exceptions, no unbounded loops.
- Threads communicate through lock-free queues (single-producer/single-consumer FIFOs such as `juce::AbstractFifo` or `choc::fifo::SingleReaderSingleWriterFIFO`) and lock-free `std::atomic` values (add `static_assert (std::atomic<T>::is_always_lock_free)` for anything wider than a pointer), not shared mutable state.
- Read parameters through their atomic value (`AutomatableParameter::getCurrentValue()`), never through `juce::ValueTree`, `juce::CachedValue` or other message-thread objects.
- Tracktion's built-in 4OSC synth locks and allocates on the audio thread. It is fine for the smoke test, but not a pattern to copy or a basis for performance measurements.

## Code conventions

- Every C/C++ file starts with `// SPDX-License-Identifier: AGPL-3.0-or-later`.
- C++20. Include module headers directly (`<juce_gui_basics/juce_gui_basics.h>`, `<tracktion_engine/tracktion_engine.h>`), not `JuceHeader.h`. Qualify `juce::`; use `namespace te = tracktion;`.
- Create and destroy Tracktion objects (Engine, Edit) on the message thread, and destroy every `te::Edit` before the `te::Engine`.
- Keep the build warning-free under JUCE's recommended warning flags (/W4 on MSVC). Don't silence a warning without saying why.
- Formatting comes from `.clang-format` (JUCE style). After editing C/C++ files, run `sh tools/format.sh` with the Bash tool (on Windows it uses the clang-format 23 bundled with VS Code's C/C++ extension), and check with `sh tools/format.sh --check`. Don't hand-align code or trailing comments that the formatter will undo; use `// clang-format off` / `// clang-format on` only where a hand layout carries meaning (such as a table).

## Committing

The repository will be public on GitHub, and a commit stays in its history even after a later commit deletes the file. The user's rule: nothing confidential may ever reach the remote. Before every commit:
- Read `git diff --cached` in full and check that nothing confidential is staged: API keys and tokens (including AI providers'), passwords, private keys, signing certificates, `.env` files, crash dumps, and personal data such as email addresses or local paths containing user names. If anything looks sensitive, stop and ask the user.
- Secrets never go in tracked files, including `.vscode/launch.json`, `CMakePresets.json` and `.claude/settings.json`. Keep them in environment variables or in files git ignores (`.env`, `CMakeUserPresets.json`, `.claude/settings.local.json`).
- The git hooks (`.githooks/`) also stop a commit, merge or push whose email isn't a GitHub noreply address, or whose changes or messages gitleaks flags. They don't replace reading the diff. Never skip them with `--no-verify` (commit or push), never change `core.hooksPath`, and never use `-c phazite.allowPublicEmail=true` unless the user asks; if a hook stops something, show the user its message and ask how to proceed. For a false positive the user has checked, add `gitleaks:allow` to that line, or add the printed `Fingerprint` line to `.gitleaksignore` where comments aren't possible.
- The pre-commit hook also refuses changed dependency pins (`external/`, `.gitmodules`), staged files over 5 MiB, trailing whitespace, leftover conflict markers and unformatted C/C++. Fix the cause; use `-c phazite.allowSubmoduleBump=true` only for a deliberate bump, and `-c phazite.allowLargeFiles=true` only when the user asks.
- Stage a new or changed hook with `git add --chmod=+x .githooks/<name>`: Windows records no executable bit, and git on Linux and macOS silently skips a hook without it. `git ls-files -s .githooks` must show mode 100755.
- In WSL/Linux the hooks need gitleaks 8.19 or newer from its GitHub releases (Ubuntu's own package is 8.16) and `git config core.hooksPath .githooks` once per clone.
- Commit messages: write them to a file and use `git commit -F <file>`. Windows PowerShell 5.1 splits a `-m` argument at embedded double quotes.
- Never add a remote or push unless the user asks.

## Before reporting work as done

- Build `windows-debug` (and `windows-release` for performance-related work) and report the commands, their exit codes and any warnings.
- After a push, check the CI run for that commit (https://github.com/august-byrne/Phazite/actions). The repository is public, so the GitHub REST API (`/repos/august-byrne/Phazite/actions/runs`) answers without signing in.
- For audio or UI changes, tell the user what to click and listen for, because the GUI can't be checked from the shell.

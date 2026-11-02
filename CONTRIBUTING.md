# Contributing to Phazite

Thank you for helping. Bug reports, testing and ideas are as welcome as code: for those, [open an issue](https://github.com/august-byrne/Phazite/issues). This guide is for working on the code.

- [Before you start](#before-you-start)
- [Setting up](#setting-up)
- [How the code is organized](#how-the-code-is-organized)
- [Tests](#tests)
- [Sending a change](#sending-a-change)
- [The git hooks](#the-git-hooks)
- [Dependencies and submodules](#dependencies-and-submodules)
- [Working with Claude Code](#working-with-claude-code)

## Before you start

- **Read the plans.** [docs/design.md](docs/design.md) says what Phazite should be and why, with the owner's decisions and the open questions; [docs/roadmap.md](docs/roadmap.md) is the step-by-step plan, each step with a "Done when" check; [docs/known-bugs.md](docs/known-bugs.md) lists the bugs found and not fixed yet, each with how to fix it.
- **Talk first about bigger changes.** For a new feature or a change to how something works, open an issue before writing code: the design may already have an answer, or a reason against it.
- **Pre-alpha means no backward compatibility.** Projects, presets and settings saved by an earlier build needn't open: there are no format versions or migrations until the owner says compatibility starts.
- **[CLAUDE.md](CLAUDE.md) holds the full rules.** It is written for Claude Code, but its sections on the architecture, the real-time rules, the code conventions and the submodules apply to everyone. This guide summarizes them.

## Setting up

Build Phazite first, as [docs/building.md](docs/building.md) describes: on Windows, `tools\setup-windows.cmd` installs everything and turns on the git hooks. For development, use `windows-debug` to step through code and `windows-profile` to play and measure.

### VS Code

1. Open the repository folder (File > Open Folder) and trust it when VS Code asks: the C/C++ and CMake Tools extensions stay disabled until you do. Install the recommended extensions when VS Code offers them (C/C++, CMake Tools and Claude Code, and WSL if you use it). Saving a C/C++ file formats it in the project's style.
2. Pick a preset with **CMake: Select Configure Preset** from the Command Palette (Ctrl+Shift+P).
3. Run **CMake: Configure** once, even for a build folder you made in a terminal: CMake Tools takes IntelliSense data and launch targets only from folders it configured itself.
4. Choose a launch configuration in the Run and Debug view and press F5:

| Configuration | What it does |
|---|---|
| Phazite (Windows) | Builds the active preset (stops if the build fails), then starts Phazite under the debugger. |
| Attach to Phazite (Windows) | Attaches the debugger to a running Phazite. |
| Open crash dump (Windows, Debug build) | Asks for a `.dmp` file from `%LOCALAPPDATA%\CrashDumps` and opens it with the `windows-debug` build's symbols. |
| Phazite (Linux / WSL, gdb) | Like the first, with gdb (untested). |

### The command line

- Run every command from the repository root: the presets are the only entry point. Configure with `cmake --preset <preset>`, build with `cmake --build --preset <preset>`, and test with `ctest --preset <preset>`.
- Don't build one preset from VS Code and a terminal at the same time: two builds in one folder break each other.
- After a Build Tools update or a toolchain change, reconfigure with `cmake --preset <preset> --fresh`; if that still fails, delete `build/<preset>` and configure again.
- Put personal presets in `CMakeUserPresets.json`, which git ignores. Each build folder gets a `compile_commands.json` for clangd and similar tools.

## How the code is organized

| Path | Contents |
|---|---|
| `src/app/` | The app: startup, the main window with its menus and dialogs, the open project, and the services it owns |
| `src/ui/` | What you see and touch, by area: the theme and design tokens, base controls, keys and keymaps, device editors, and one folder per view (playlist, piano roll, audio clip editor, mixer, browser, and the window's shell) |
| `src/services/` | What the app keeps outside a project: settings, recent projects, the browser's library and the Phazite Sounds download |
| `src/model/` | The project model, by area (commands, engine, project, transport, arrangement, notes, chords, mixer, automation, modulation, presets), without window code; the only code that uses Tracktion Engine |
| `src/engine/` | Phazite's own playback engine, which plays what the model compiles from the song, without Tracktion (being built: [tracktion-migration.md](docs/tracktion-migration.md), T2) |
| `src/devices/` | The DSP of Phazite's own instruments and effects, using JUCE's core and DSP modules only, so that a device could run on another engine |
| `resources/` | Keymaps, song outlines, chord progressions, presets, icons and the Inter font |
| `tests/` | The unit, golden and real-time tests, and the benchmarks (`phazite_tests`, with doctest) |
| `docs/` | The design, the roadmap, the known bugs and the audits |
| `tools/` | Machine setup, `format.sh`, `check-architecture.sh`, and `soundkit/`, which makes [Phazite Sounds](https://github.com/august-byrne/phazite-sounds) |
| `external/` | The submodules: Tracktion Engine (with JUCE), Rubber Band Library and WindowsToolchain. Read-only |
| `.githooks/`, `.github/` | The git hooks, and the continuous integration (CI) on GitHub Actions |

[CLAUDE.md](CLAUDE.md#repository-layout) describes every folder and file in detail.

### The layers

The code is layered so that the UI can change without touching the rules of editing, every edit can be tested without a window, and parts of the engine could be replaced later ([design, section 24](docs/design.md#24-code-structure)):

- **Dependencies point down:** the app uses the UI, the UI uses the services and the model, the services may use the model, the model may use the engine (`src/engine/`), and never the other way round. Every layer may use `src/devices/`, which uses none of them.
- **No window code below the UI**, and **Tracktion stays in the model**: the UI calls model functions that take and return Phazite's own types (IDs and plain values), never Tracktion's.
- **The UI decides nothing about the project:** it runs a command or calls a model function, and every change to a project is one named undo step.
- **Every user action is a command**, with a stable id such as `edit.undo`, a name and a description; default shortcuts go in the keymaps in `resources/keymaps/`, not in code.
- **Colors, sizes and fonts come from the design tokens** (`src/ui/theme/DesignTokens.h`), never literals.

`sh tools/check-architecture.sh` counts what these rules forbid and compares each count with `tools/architecture-budget.txt`; CI and the pre-commit hook run it. The counts may only go down: when your change lowers one, run it with `--update` and commit the new budget with the change.

### The audio thread

The audio thread is the audio callback and everything it calls, such as plug-in, device and Tracktion node processing. It must deliver every block of audio on time, so code there must not:

- allocate or free memory (`new`/`delete`, growing containers, creating strings, allocating `std::function` captures): allocate up front when preparing;
- take locks or wait for other threads (`std::mutex`, `juce::CriticalSection`, `MessageManagerLock`, ...);
- post to the message thread (`triggerAsyncUpdate`, `sendChangeMessage`, `callAsync`): set an atomic flag and poll it from a `juce::Timer` instead;
- do file or network I/O, log, throw exceptions or run unbounded loops.

Threads share data only through lock-free FIFOs and `std::atomic` values. Tracktion's own instruments and effects break these rules, so Phazite doesn't use them. After a change that touches playback, run the real-time audit (`phazite_tests --test-suite=realtime-audit`), which counts allocations on the audio thread; [docs/realtime-audit.md](docs/realtime-audit.md) lists what is known.

### Code style

- Every C/C++ file starts with `// SPDX-License-Identifier: AGPL-3.0-or-later`.
- C++20. Include JUCE's module headers directly (`<juce_gui_basics/juce_gui_basics.h>`), not `JuceHeader.h`, and qualify `juce::`.
- **Formatting** follows [.clang-format](.clang-format), JUCE's coding standards: braces on their own lines, `foo (x)` but `foo()`, `if (! ready)`, `Type* p`, 4-space indents, lines up to 140 characters. VS Code formats a file when you save it, `sh tools/format.sh` formats every changed file from a shell (Git Bash on Windows), and the pre-commit hook refuses unformatted files. On Linux, install clang-format 23 with pipx (`pipx install "clang-format==23.*"`) or from apt.llvm.org (`clang-format-23`): Ubuntu's own is too old.
- Keep the build free of warnings (`/W4` on MSVC; CI also builds with GCC, which warns about more). Don't silence a warning without saying why in a comment.

## Tests

`ctest --preset <preset>` runs `phazite_tests`, written with [doctest](https://github.com/doctest/doctest), once for each test file (named after it), 4 at once; it has passed when it prints `100% tests passed`. `-R <name>` runs some files only, and `-j <n>` runs more at once. To run some tests only, call the executable directly, for example `build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-case="*save*"`.

- **Every change comes with tests**, and a UI bug is reproduced in a test before it is fixed: `tests/UiTestHelpers.h` gives a window that isn't on any screen, with a mouse and keys that reach components the way JUCE delivers real events. A test that builds a song does it through the model, with `tests/TestSong.h`. Add each new test file to `target_sources(phazite_tests ...)` in `CMakeLists.txt`.
- **The golden tests** (`--test-suite=golden`) compare what Phazite saves, plays and registers as commands with committed files in `tests/golden/`. A refactoring must leave them as they are. When a change is meant to alter them, run that suite with the environment variable `PHAZITE_UPDATE_GOLDEN=1`, read `git diff tests/golden` in full, and say in the commit message why they changed.
- **The benchmarks** (`--test-suite=benchmark --no-skip`, from a `windows-profile` build) time the devices, playback, saving and the UI; ctest skips them. For performance work, run them before and after, and update [docs/performance-audit.md](docs/performance-audit.md).

## Sending a change

1. Fork the repository and make a branch for your change. Keep each commit to one change that builds and passes the tests.
2. Before you commit: `sh tools/format.sh`, `sh tools/check-architecture.sh`, a `windows-debug` build without warnings, and `ctest --preset windows-debug`.
3. Check what you're about to publish: read `git diff --cached` in full (see [No secrets](#no-secrets)).
4. Open a pull request against `main`, saying what changed, why, and how you tested it. For a change to the UI or the sound, say what to click and listen for.

Every push and pull request runs [CI](.github/workflows/ci.yml) on GitHub's machines: a secret scan of the whole history, formatting with clang-format 23, whitespace, the architecture budgets, and builds of both Windows presets (Visual Studio 2026) and all four Linux presets (Ubuntu 24.04, two with sanitizers), with warnings as errors and the tests run. The results are on the [Actions tab](https://github.com/august-byrne/Phazite/actions), with compiler messages as annotations.

### Licenses

Phazite is AGPL-3.0-or-later. Only add code under licenses compatible with it: GPL-3.0, AGPL-3.0, LGPL-2.1-or-later, LGPL-3.0, MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC or Zlib (not the 4-clause BSD license); fonts may also be under the SIL Open Font License 1.1. Never turn on Tracktion's proprietary options (Elastique, ReWire, REX). No sounds or samples go in this repository: they belong in [phazite-sounds](https://github.com/august-byrne/phazite-sounds), under CC0. Add every new dependency to [docs/third-party.md](docs/third-party.md).

### No secrets

Nothing confidential may reach the public repository: no API keys or access tokens, passwords, private keys, signing certificates, `.env` files, crash dumps, or personal data such as email addresses or local paths that contain your user name. A secret that reaches a commit stays in the history even after you delete the file; if one slips through, revoke it and make a new one. Keep secrets in environment variables or in files git ignores (`.env`, `CMakeUserPresets.json`, `.claude/settings.local.json`), never in tracked files such as `.vscode/launch.json` or `CMakePresets.json`.

### Your email address

Every commit records git's `user.name` and `user.email`, and pushing to a public repository publishes them for good. Before your first commit, turn on **Keep my email addresses private** under GitHub's Settings > Emails, and use the noreply address shown there: `git config --global user.email "<id>+<username>@users.noreply.github.com"`. A new address applies only to new commits, so check yours before you push with `git log --format="%h %ae %ce"`. The hooks refuse any other address.

## The git hooks

Three hooks in [.githooks/](.githooks) check your work before it is committed or pushed. On Windows, `tools\setup-windows.cmd` installs what they need and turns them on in its clone; in another clone, run `git config core.hooksPath .githooks` there.

- **`pre-commit`** runs before `git commit` (also `--amend`, and finishing a merge or cherry-pick after a conflict). It checks that the author and committer email is a GitHub noreply address; that the pinned dependencies (`external/`, `.gitmodules`) don't change; that no staged file is larger than 5 MiB; that there is no trailing whitespace or leftover conflict marker; that [gitleaks](https://github.com/gitleaks/gitleaks) finds nothing that looks like a secret; that C/C++ files follow `.clang-format`; and that the code keeps to the architecture budgets.
- **`pre-merge-commit`** runs the same checks for `git merge` and `git pull`.
- **`pre-push`** checks the emails and secrets of every commit the remote doesn't have yet, their messages and any tag you push, and that every commit that raises an architecture budget says why. It catches what the others can't see, such as commits made by `git rebase` or with `--no-verify`.

**On Linux and WSL**, install gitleaks 8.19 or newer from its GitHub releases (Ubuntu's own package is too old), then turn the hooks on once in each clone:

```bash
cd /tmp
curl -fsSLO https://github.com/gitleaks/gitleaks/releases/download/v8.30.1/gitleaks_8.30.1_linux_x64.tar.gz
curl -fsSLO https://github.com/gitleaks/gitleaks/releases/download/v8.30.1/gitleaks_8.30.1_checksums.txt
sha256sum --check --ignore-missing gitleaks_8.30.1_checksums.txt   # must print: OK
sudo tar -xzf gitleaks_8.30.1_linux_x64.tar.gz -C /usr/local/bin gitleaks
cd ~/src/Phazite
git config core.hooksPath .githooks
```

The hooks don't replace reading the diff: gitleaks can't recognize personal data or every kind of key, and can't see inside binary files. Git runs the hooks from your working tree, so before you check out someone else's branch, look at its changes to `.githooks/`.

### When a hook stops you

| Message | Fix |
|---|---|
| `gitleaks is not installed` (or `... is too old`) | Run `tools\setup-windows.cmd` on Windows, or install gitleaks as above. |
| `this commit would publish the ... email address ...` | Set your GitHub noreply address ([Your email address](#your-email-address)), then commit again. |
| `pre-push: ... has commits or a tag with these email addresses ...` | Fix those commits as the message says (`git commit --amend --reset-author`), then push again. |
| `-CheckOnly` prints `hooks ... OFF` | The hooks are off in this clone: run `git config core.hooksPath .githooks` there. |
| `these staged C/C++ files don't follow .clang-format` | Run `sh tools/format.sh` (or save each file in VS Code), stage the files again and commit. |
| `the staged C/C++ files couldn't be checked against .clang-format` | No clang-format 22 or newer was found: install VS Code's C/C++ extension, or run `tools\setup-windows.cmd -WithLlvm`. |
| `this commit changes the pinned dependencies` | A submodule pointer moved by accident (for example through `git commit -a`): follow the message to unstage it. |
| `these staged files are larger than 5 MiB` | Unstage them (`git restore --staged <file>`): audio files, builds and dumps don't belong in git. |
| `the staged changes have trailing whitespace or a leftover conflict marker` | Fix the lines it lists, stage the files again and commit. |
| `the staged code doesn't keep to the architecture budgets` | If a count went down, run `sh tools/check-architecture.sh --update` and stage `tools/architecture-budget.txt`; if one went up, change the code as the message for that rule says. |
| `this commit raises architecture budgets` | Budgets only go down. A deliberate move between layers needs the `Architecture-Budget-Raise:` line it prints in the message, and `git -c phazite.allowBudgetIncrease=true commit`; ask in your pull request first. |
| gitleaks flags something that isn't a secret | Add the comment `gitleaks:allow` to that line, or, in a file that can't hold comments, add the `Fingerprint` line it printed to `.gitleaksignore`. |

## Dependencies and submodules

| Submodule | Pinned at |
|---|---|
| `external/tracktion_engine` | `bb386171` on Tracktion's `develop` branch (2026-10-07; module version 3.5.0) |
| `external/tracktion_engine/modules/juce` | `37c894f8`, the commit Tracktion pins (JUCE 8.0.13 plus 7 commits on `develop`) |
| `external/rubberband` | `1d95888b` (Rubber Band Library 4.0) |
| `external/WindowsToolchain` | `1373361e` (v0.14.0 plus 11 commits) |

- Run `git submodule update --init --recursive --depth 1` after every pull or branch switch. Never run `git submodule update --remote`: it would move JUCE away from the commit Tracktion pins.
- Everything under `external/` is read-only: don't edit, reformat or "fix" it, and never add JUCE separately. Don't commit a moved submodule pointer (`git status` shows "(new commits)") unless the change is a deliberate dependency bump, which [CLAUDE.md](CLAUDE.md#submodules-and-third-party-code) describes.

## Working with Claude Code

- [CLAUDE.md](CLAUDE.md) holds the project instructions that Claude Code loads by itself, and [.claude/settings.json](.claude/settings.json) the shared configuration: it pre-approves the build commands, read-only git commands and the format and architecture checks, and keeps Tracktion's own CLAUDE.md files (written for Tracktion's contributors) out of Claude's context.
- Put personal notes in `CLAUDE.local.md` and personal settings in `.claude/settings.local.json`; git ignores both.

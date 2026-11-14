# tools

Scripts and small programs for setting up a machine, checking the code, CI and making things by hand.

| File | What it does |
|---|---|
| `setup-windows.cmd`, `setup-windows.ps1` | Installs or updates every tool on Windows and turns on the clone's git hooks; `-CheckOnly` checks without changing anything. The `.cmd` runs the `.ps1` without changing PowerShell's execution policy. [docs/building.md](../docs/building.md) and `CLAUDE.md`, "Tools (Windows)", describe its switches and exit codes. |
| `format.sh` | clang-format for the C/C++ files (`--check` checks, `--staged` checks what is staged). On Windows it uses the clang-format bundled with VS Code's C/C++ extension. |
| `check-architecture.sh` | Counts what the architecture rules forbid and compares each count with its budget in `architecture-budget.txt` (`--help` lists the rules; `--list <rule>` shows each match; [docs/architecture.md](../docs/architecture.md)). Among the rules, `ui-design-literals` counts colours, font sizes and corner radii written as literals in `src/ui` and `src/app` outside `src/ui/theme`, rather than taken from the design tokens (`src/ui/theme/DesignTokens.h`); layout arithmetic, line thicknesses, alphas and sizes worked out from tokens aren't counted. |
| `architecture-budget.txt` | The budget of each architecture rule: 0 for every rule but `ui-design-literals`, whose budget is the count there was when the rule came (33) and only falls as each literal moves into the tokens. |
| `clang-tidy.sh` | clang-tidy with the checks [.clang-tidy](../.clang-tidy) lists over Phazite's own files in a build's `compile_commands.json` (one configured without precompiled headers), several at once; exits 1 on any finding. CI's Clang job runs it; [docs/building.md](../docs/building.md#linux-and-wsl2) says how to run it on Windows too. |
| `tsan-suppressions.txt`, `lsan-suppressions.txt` | The sanitizers' suppression files, for known races and leaks in code Phazite can't change. |
| `ci-sanitizer-reports.sh` | For CI: each sanitizer report in a test run's output as one GitHub annotation. |
| `test-installer.ps1` | Installs an installer for this user, silently, into a folder of its own, checks its files, associations, shortcut and uninstaller's entry, then uninstalls it and checks that all of it went. CI runs it. |
| `read-versions.pl` | A reader of the versions' packs, written from [docs/versions-format.md](../docs/versions-format.md) alone, which CI runs on the test vectors. |
| `social-preview.ps1` | Draws the picture GitHub shows for links to the repository, `docs/images/social-preview.png`, from the README's picture of the window. It is uploaded by hand in the repository's settings. |
| `lmms-to-midi.ps1` | An LMMS project's notes as a MIDI file, for an example song that someone else made (`resources/songs/`). |
| `soundkit/` | `SoundKit.cpp`, the `phazite_soundkit` tool that renders and zips Phazite Sounds from the recipes in the phazite-sounds repository. Run by hand, as that repository's README says. |

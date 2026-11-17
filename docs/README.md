# Phazite's docs

Every document under `docs/`, by who reads it. Each source folder also has a `README.md` that lists its files; the
map of the layers is [architecture.md](architecture.md).

## For people trying Phazite

- [../README.md](../README.md): what Phazite is, a picture of the window, its status, the short build and first steps.
- [guide/](guide/): the user guide, which Phazite's website publishes. The website is a repository of its own,
  `phazite-site` (Astro with Starlight on GitHub Pages, <https://august-byrne.github.io/phazite-site/>), whose build
  copies the guide, the logo, `notices.json` and the icons from this repository, and fills its Download page from the
  latest release's `phazite-update.json`. It keeps its own pictures of the window and its parts, which the tests'
  `site-pictures` suite paints into it. The pages are Markdown with Starlight's front matter
  (title, description, sidebar order) and relative links, which the site turns into its own pages or GitHub's. Each
  page's `description` says what it covers, from getting started to the keys, the views, recording, versions,
  troubleshooting, the FAQ and privacy.
- [building.md](building.md): the build in full, on Windows and Linux, with the installer and troubleshooting.
- [images/](images/): the README's picture of the window (`phazite.png`, painted by the tests' `readme-picture` suite)
  and the picture GitHub shows for links (`social-preview.png`, drawn by `tools/social-preview.ps1`).

## For contributors

- [../CONTRIBUTING.md](../CONTRIBUTING.md): setting up, the layers, the real-time rules, the tests, sending a change and
  the git hooks.
- [../CLAUDE.md](../CLAUDE.md): the full rules, written for Claude Code; its sections on the architecture, the
  real-time rules, the code conventions, the tests and the submodules apply to everyone.
- [architecture.md](architecture.md): the layers and which may include which, how the song becomes sound, and the
  architecture rules.
- [third-party.md](third-party.md): the code from others that Phazite builds with, and its licences.

## Reference

- [design.md](design.md): the product design, an index by section number of the pages below.
- [design/](design/): one page per area of Phazite: how it works now, what is built, planned and declined.
- [design/devices/](design/devices/): one page per device of Phazite's own.
- [design/decisions.md](design/decisions.md): the decisions that still matter, each with its reason.
- [design/versions-cut.md](design/versions-cut.md): the design note of what the versions are cut down to (roadmap
  step S4.1), which the owner approved and steps S4.2 to S4.5 build.
- [project-format.md](project-format.md): the project file's format, for other tools, with its test vectors in
  `tests/golden/format/`.
- [versions-format.md](versions-format.md): the versions' format, for other tools, with its test vectors in
  `tests/golden/format/`.
- [presets.md](presets.md): the presets that come with Phazite: what they cover, the classic instruments they recreate
  and how they are named, how they are leveled, and how to check them.
- [performance.md](performance.md): the performance targets and the current benchmark numbers.
- [realtime-audit.md](realtime-audit.md): what is known to break the real-time rules, and what Phazite does about each.
- [juce-workarounds.md](juce-workarounds.md): every place Phazite's code works around JUCE, by its number, the code
  shaped by JUCE that is a correct pattern, and the open questions.
- [vscode-look.md](vscode-look.md): where each of the themes' colours and sizes comes from in VS Code.

## Plans

- [roadmap.md](roadmap.md): the status of each stage, the open steps with their "Done when" checks, and the risks.
- [changelog.md](changelog.md): one line per finished step.
- [hand-checks.md](hand-checks.md): the checks only a person can do, with the day each was last done.
- [backlog.md](backlog.md): open work that is not a bug, each row ready to become a GitHub issue.
- [known-bugs.md](known-bugs.md): bugs found and not fixed yet, each with why and how to fix it.
- [open-questions.md](open-questions.md): what waits for the owner's answer.

## Archive

[archive/](archive/) holds frozen history, never updated:

- `design-2026-11-13.md` and `roadmap-2026-11-13.md`: the design and the roadmap as they were before they were split.
- `versions-plan.md`: the plan the project's versions were built from.
- `dsp-audit.md`, `recording-audit.md`, `format-audit.md`, `source-control-audit.md`, `performance-audit.md`: the
  audits of the DSP, recording, the formats, source control and performance, with what was built from each.

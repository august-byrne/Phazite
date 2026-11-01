<!-- Thanks for the change. CONTRIBUTING.md says what a change goes through. -->

## What and why

<!-- What this changes and why. Link the issue it closes: "Closes #123". -->

## How it was tested

<!-- The tests you added or ran. For a change to the UI or the sound: what to click, and what to listen for. -->

## Checklist

- [ ] `sh tools/format.sh --check` and `sh tools/check-architecture.sh` pass
- [ ] The `windows-debug` build has no warnings, and `ctest --preset windows-debug` passes
- [ ] Golden files changed only on purpose, and the commit message says why
- [ ] New dependencies are in `docs/third-party.md`; the roadmap, known bugs and README are updated where this changes them

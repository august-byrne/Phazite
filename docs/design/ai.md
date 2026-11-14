# AI and generative features

*Design section 16.*

Phazite's generative features are to be suggestions made from what the project holds (its key, tempo, chords,
melody and sections), which the user hears, accepts and then owns. None of them is built: the first two, chord
suggestions and the [sound generator](sound-generator.md), are planned for after v1, and an assistant after those.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Chord suggestions: next chord, fill a section, replace, fit my melody | Planned | After v1 ([roadmap](../roadmap.md#after-10), "Chord suggestions") |
| Sound generator | Planned | After v1 ([sound-generator.md](sound-generator.md)) |
| A record in the project of what each generative feature made | Planned | With the first of them |
| The assistant, acting through commands | Planned | After v1 (item 3 of the roadmap's list) |
| Audio-to-MIDI | Planned | After v1 (item 10) |
| Automatic tags and "find similar" for the user's samples | Planned | After v1 (item 10; [browser.md](browser.md)) |
| Sound generation by a trained model of Phazite's own | Planned | After v1 (item 9) |
| A reference track's structure shown against the song's outline | Planned | No roadmap step |
| Text-to-song generation | Declined | Phazite assists; it doesn't write the song |
| Stem separation | Declined | Many tools already do it |

## Principles

1. **Useful because it is in a project.** Each feature reads the project's context: key, tempo, chords, melody,
   sections and outline style, the sounds already used, and the user's past choices.
2. **Assistive.** The user gets suggestions to hear and accept, owns the result and can edit it.
3. **Local first and opt-in.** Everything runs on the user's machine. Nothing leaves it unless the user later adds a
   cloud model themselves ([privacy-licences.md](privacy-licences.md)).
4. **Licence-clean.** Only models and data whose licences fit the AGPL.
5. **Transparent.** A suggestion says why it was made. The project records which generative feature made what
   (generator, version, seed), so that results can be reproduced and users can label their work accurately.

The first engines are algorithmic: the chord engine built on music theory, the sound generator on synthesis and
evolution. Trained models can come later, behind the same interface.

## Chord suggestions, as intended

**What it suggests:**

- **Next chord:** after any chord on the chord lane, four to six suggestions grouped as safe, colourful or
  surprising.
- **Fill a section:** three to five whole progressions for the selected section, suited to its role (verse, chorus,
  drop) and the outline's style.
- **Replace:** other chords for a selected one: substitutions, borrowed chords, and inversions for a smoother bass
  line.
- **Fit my melody:** chords that hold the melody's notes on strong beats, for a selected MIDI clip.

**How it presents them:**

- A slider from safe to adventurous sets how far suggestions stray.
- Every suggestion says why it was made: "contains your melody's C and E on beats 1 and 3", "borrowed from the
  parallel minor".
- A suggestion is heard in place, applied with one click, and is one undo step.

**The engine** scores candidate chords by how well they fit the melody, smooth voice leading, harmonic function and
cadences, the section's role, and weights for each outline's style. It is instant, explains itself, works offline
and needs no training data. It also learns the user's taste, on their machine, from the suggestions they accept.

**What exists that it would build on:** the chord lane, its numerals and keys, and the list of chords a key offers
at a place (`getChordChoices()`), which is the key's own chords and common ones from outside it, in a fixed order,
not a suggestion ([chords.md](chords.md)); and sections with their roles, from the outlines
([getting-started.md](getting-started.md)).

## The assistant, as intended

It acts through the command system: every action is shown before it runs and can be undone. It runs on a local
model or the user's own API key. It needs commands that take parameters and return results, which the commands
don't yet ([commands-keys-help.md](commands-keys-help.md)).

## Decisions

- **Assistance, not generation of whole songs.** Text-to-song is out of scope.
- **No stem separation:** other tools do it.
- **Algorithmic engines first.** A trained model is used only if the model, or the data to train one, has a licence
  that fits the AGPL. None suitable was found for drum sounds, and no chord dataset both large enough and suitably
  licensed; the candidates looked at are in the archived design document. A model under a licence that doesn't fit
  could at most be an optional download that each user accepts separately, never bundled.
- **Local and opt-in:** no feature sends anything off the machine by itself.
- **What was generated is recorded in the project.**
- **After v1,** not in it.

## Not built

Everything on this page is Planned, after v1, but text-to-song generation and stem separation, which are Declined.

## In the code

Nothing yet.

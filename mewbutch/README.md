# mewbutch

Butch accepts veteran cats from **any** adventure, as long as they came back
from a harder difficulty than he is currently asking for.

Requires [mewjector](https://github.com/githubuser508/mewjector). Drop
`mewbutch.dll` into `Mewgenics/mods/`.

## The problem

Every one of Butch's storage tiers pins the same adventure - act 3, chapter 4 -
and only raises the difficulty as you progress:

| tier | act | chapter | difficulty |
|---|---|---|---|
| normal | 3 | 4 | 0 |
| hard | 3 | 4 | 1 |
| crazy | 3 | 4 | 2 |
| impossible | 3 | 4 | 3 |

A cat is checked against all three independently, so a survivor of *any other*
adventure is refused no matter how brutal the run. If you like playing a variety
of adventures, Butch has nothing to say to you.

## What this changes

Difficulty is tested first, and a run harder than the tier requires satisfies it
on its own:

```
not a veteran            -> reject   (unchanged)
difficulty <  required   -> reject   (unchanged)
difficulty >  required   -> ACCEPT   from any act or chapter
difficulty >= impossible -> ACCEPT   from any act or chapter
otherwise                -> act and chapter enforced exactly as vanilla
```

The clamp on the last rule matters: at the top tier there is nothing above
impossible without limit-break gear, so difficulty 3 is treated as sufficient.
Without it the endgame - the tier people actually get stuck on - would be
unchanged.

Equal-difficulty cats still have to be the specific run they always were, so
this is a relaxation, not a bypass.

## Safety

- The replacement is 70 bytes written into a 72-byte region, so no code shifts.
- Call and branch targets are read out of the bytes found at runtime rather than
  hardcoded, so the patch tolerates the code moving.
- Patches only on **exactly one** signature match, otherwise it logs and does
  nothing. Worst case is the game running unmodded.
- Writes `mewbutch.log` next to the game exe, including the bytes it wrote.
- Nothing is written to your save. This only changes an acceptance test, so the
  failure mode is a donation being refused - no persisted state, no save
  compatibility issues. Remove the DLL and behaviour is exactly vanilla.

`test_patch.cpp` builds the replacement through the same `patch.inc` the mod
uses and prints it, so the shipped bytes can be diffed against an independently
assembled reference.

## Uninstall

Delete `mewbutch.dll`.

## Credits

Built by [Mropat](https://github.com/Mropat) with Claude Opus 5 (Claude Code),
by reverse engineering the retail binary.

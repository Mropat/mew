# mewbutch

Butch accepts veteran cats from an **earlier act** if they came back from a
harder difficulty than he is currently asking for.

Requires [mewjector](https://github.com/githubuser508/mewjector). Drop
`mewbutch.dll` into `Mewgenics/mods/`.

## The problem

The map is three acts of four chapters each, with difficulty as a third axis
over the whole grid. Butch walks act and chapter upward - A1C1, A1C2 ... up to
A3C4, the God run - and once there he stops moving and escalates difficulty
instead: A3C4 normal, then hard, then crazy, then impossible.

Each donated cat is checked against the tier's act, chapter and difficulty
independently, and all three must be met. Since every late tier pins A3C4, a
cat from any other adventure is refused however hard the run was. If you like
playing a spread of adventures rather than farming one, Butch has nothing for
you.

## What this changes

One difficulty step above what he asks for excuses a **lower act**. It never
excuses a lower chapter.

```
not a veteran            -> reject   (unchanged)
chapter < required       -> reject   (unchanged, never excused)
difficulty < required    -> reject   (unchanged)
difficulty > required    -> ACCEPT   from any act
difficulty >= impossible -> ACCEPT   from any act
otherwise                -> act enforced exactly as vanilla
```

So "A3 C4 hard" also takes "A1 C4 crazy" or "A2 C4 crazy" - but not "A1 C1
crazy", and not an equal-difficulty cat from the wrong act.

Two details worth knowing. The clamp on the second-to-last rule matters: nothing
sits above impossible without limit-break gear, so difficulty 3 is treated as
sufficient on its own - otherwise the endgame tier, the one people actually get
stuck on, would be unchanged. And chapter is deliberately checked *before*
difficulty, because the early tiers carry no difficulty requirement at all; if
difficulty were consulted first, playing above normal would walk straight past
the whole A1C1 to A3C4 ladder.

## Safety

- The replacement is 70 bytes written into a 72-byte region, so no code shifts.
- Call and branch targets are read out of the bytes found at runtime rather than
  hardcoded, so the patch tolerates the code moving.
- Patches only on **exactly one** signature match, otherwise it logs and does
  nothing. Worst case is the game running unmodded.
- Writes `mewbutch.log` beside the game exe, including the bytes it wrote.
- Nothing is written to your save. This only changes an acceptance test, so the
  failure mode is a donation being refused - no persisted state, no save
  compatibility issue. Remove the DLL and behaviour is exactly vanilla.

`test_patch.cpp` builds the replacement through the same `patch.inc` the mod
uses and prints it, so the shipped bytes can be diffed against an independently
assembled reference.

## Uninstall

Delete `mewbutch.dll`.

## Credits

Built by [Mropat](https://github.com/Mropat) with Claude Opus 5 (Claude Code),
by reverse engineering the retail binary.

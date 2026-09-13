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
stuck on, would be unchanged. And chapter is checked *before* difficulty,
because the early tiers carry no difficulty requirement at all; if difficulty
were consulted first, playing above normal would walk straight past the whole
A1C1 to A3C4 ladder.

## How it works

Butch's test is case 1 of the NPC condition evaluator, `eval(self, condition,
cat)`. The mod detours that function: the evaluator's 15-byte prologue moves to
a trampoline, and the replacement runs the game's own evaluator first, only
reconsidering a rejection. The rule itself is plain C++ calling the game's own
`required_act` / `required_chapter` / `required_difficulty` getters.

**It can add acceptances but never remove one.** Vanilla runs to completion
first, so anything Butch already took, he still takes. `test_rule.cpp` checks
that exhaustively across all 25,600 combinations of the six numbers involved,
alongside the specific cases above.

Version 1 did the same job as 70 bytes of hand-assembled x86 written over the
original 72, with call and branch targets read back out of the bytes found at
runtime. It worked, but the rule was unreadable and the encoding was hand-
checked. Tag `mewbutch-v1.1.0` has that version if you want to compare.

## Safety

- Four call targets are pinned by RVA plus the first 16 bytes found at each. One
  mismatch aborts the install: no hook is written and the game runs unmodded.
  Regenerate with `tools/make_sig.py mewbutch` after a game update.
- The 15 stolen bytes are three register spills, so nothing position-dependent
  moves and the game's own unwind info still describes the frame.
- Writes `mewbutch.log` beside the game exe. Build with `/DMEWBUTCH_TRACE` to
  also log every cat the mod accepts on Butch's behalf.
- Nothing is written to your save. This only changes an acceptance test, so the
  failure mode is a donation being refused - no persisted state, no save
  compatibility issue. Remove the DLL and behaviour is exactly vanilla.

## Uninstall

Delete `mewbutch.dll`.

## Credits

Built by [Mropat](https://github.com/Mropat) with Claude Opus 5 (Claude Code),
by reverse engineering the retail binary.

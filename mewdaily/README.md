# mewdaily

Baby Jack's shop restocks **every day** instead of once a week.

Requires [mewjector](https://github.com/githubuser508/mewjector). Drop
`mewdaily.dll` into `Mewgenics/mods/`. That's it.

## Why a DLL and not a data mod

The refresh is decided in compiled code, not in GON. A house shop restocks when
**either** the stock size no longer matches the shop level, **or** the weekday
equals its refresh day:

```asm
mov  eax,[rdi+0x274]   ; shop level
add  eax, 2            ; expected item count
cmp  rax, rcx          ; != current stock.size() ?
jne  refresh
call <civil_from_days> ; calendar
cmp  [rax+0xc], esi    ; weekday == refresh day ?
jne  skip              ; <-- mewdaily NOPs these 6 bytes
refresh:
```

Removing that second branch makes the refresh run daily. Shop level is never
touched, so the item count stays at six and **your save stays vanilla** - remove
the mod and everything is exactly as it was.

## Why not GON
The data-only route is a trap, and worth knowing about even if you never use
this mod: you *can* force refreshes by granting `shop_level_up` on a repeating
favour, but shop level is **written into the save and never recomputed**. Past
level 4 the game asks for a `stock_fill_order` entry that does not exist, and
the shop crashes on open - with the mod installed *or not*. That save is bricked.

## How it is structured

`patches.inc` is a registry - one entry per thing the mod does to the game:

```cpp
static const BytePatch PATCHES[] = {
    { "restock the house shop every day, not just on its refresh weekday",
      0x1C4F07 - 41, WEEKDAY_GATE, COUNT(WEEKDAY_GATE),
      41, NOP6, COUNT(NOP6) },
};
```

The RVA is where the region lived in 1.1.b21239, and it is a **hint, not an
address the mod trusts**. The bytes are checked there first; if they do not
match, the region is searched for by pattern, and if that finds it more than
once the mod patches nothing at all. Writing six bytes to a remembered offset
without checking what is at it is the one thing this must never do - after a
game update those bytes land in the middle of whatever moved into that offset,
and the crash shows up somewhere else entirely, long after the write.

Patches are all located before any is written, so a table that only partly
matches leaves the game alone rather than half-patched.

## Safety

- The write window is covered by the verified pattern: every byte overwritten is
  a byte that was checked first, operand wildcards included.
- Same length in, same length out - no code shifts.
- Writes `mod_logs/mewdaily.log`, naming the address and what was
  done, or which region failed to match and why nothing was patched.
- Nothing is written to your save. Shop level is untouched, so the save stays
  vanilla-compatible. Remove the DLL and behaviour is exactly vanilla.

`test_locate.cpp` checks the registry through the same `patches.inc` the mod
uses: that it finds the region where it remembers it, still finds it after the
code moves, refuses when a checked byte changed, refuses when the match is
ambiguous, and that the write window never extends past the verified bytes.
Pass a real `Mewgenics.exe` to also assert the table resolves uniquely against
the shipped binary:

```
clang++ -O2 -o test_locate.exe test_locate.cpp
./test_locate.exe "C:/Program Files (x86)/Steam/steamapps/common/Mewgenics/Mewgenics.exe"
```

## Uninstall

Delete `mewdaily.dll`.

## Credits

Built by [Mropat](https://github.com/Mropat) with Claude Opus 5 (Claude Code),
by reverse engineering the retail binary. Thanks to the authors of mewjector and
Mewtator for the loader this plugs into.

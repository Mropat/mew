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

The data-only route is a trap, and worth knowing about even if you never use
this mod: you *can* force refreshes by granting `shop_level_up` on a repeating
favour, but shop level is **written into the save and never recomputed**. Past
level 4 the game asks for a `stock_fill_order` entry that does not exist, and
the shop crashes on open - with the mod installed *or not*. That save is stuck.

## Safety

- Finds the patch site by **signature scan**, not a hardcoded offset, so a game
  update that shifts code is handled.
- Patches only if there is **exactly one** match, and only if the bytes there
  are still the expected `0F 85`. Zero or several matches: it logs and does
  nothing. The worst case is the game running unmodded.
- Writes `mewdaily.log` next to the game exe so you can see what happened.
- Touches six bytes in memory. `Mewgenics.exe` on disk is never modified.

Full source is [dllmain.cpp](dllmain.cpp) - about 100 lines, and worth a read
before you run someone else's DLL. Verify the download against the SHA-256
printed in the Actions run for the release.

**Antivirus:** an unsigned DLL that calls `VirtualProtect` and writes to another
module's memory can trip heuristics. That is what this mod does by design.

## Uninstall

Delete `mewdaily.dll`.

## Credits

Built by [Mropat](https://github.com/Mropat) with Claude Opus 5 (Claude Code),
by reverse engineering the retail binary. Thanks to the authors of mewjector and
Mewtator for the loader this plugs into.

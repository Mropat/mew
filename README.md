## Vibegenics, proceed with caution
# mew

Mewgenics mods. One folder per mod; each builds to its own DLL.

| mod | what it does |
|---|---|
| [mewdaily](mewdaily) | Baby Jack's shop restocks daily instead of weekly |
| [mewbutch](mewbutch) | Butch takes veteran cats from any fully completed adventure run above his required difficulty |
| [mewlinks](mewlinks) | Click a lover, rival or family tree portrait to select that cat in the house |

Releases are built by GitHub Actions, so every published DLL is traceable to a
commit and a public build log.

## Layout

```
common/    shared detour + logging, used by the mods that hook game functions
tools/     find_gate.py, build_index.py, make_sig.py
<mod>/     one folder per mod, each building to its own DLL
```

Every mod logs to `Mewgenics/mod_logs/<mod>.log`, alongside mewjector's own
chainloader log, rather than dropping files beside the game exe.

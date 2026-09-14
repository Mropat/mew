# mewlinks

**Click a cat's picture, go to that cat.** Two places in the house UI show you a
cat you cannot get to, and this makes both of them clickable:

| you click | you get |
| --- | --- |
| the **lover** or **rival** portrait on the cat info panel | that cat, selected in the house |
| any portrait in the **family tree** | that ancestor, if it is still in the house |

Selecting works the same way the panel's own `<` `>` arrows do, so the cat is
highlighted in the house exactly as if you had picked it yourself.

The family tree also **marks the edge leading up to every cat that is still in
the house**, so you can see at a glance which portraits will take you somewhere
before you click any of them.

Built for **Mewgenics 1.1.b21239**. Drop `mewlinks.dll` into `Mewgenics/mods/`
next to [mewjector](https://github.com/githubuser508/mewjector)'s `version.dll`.

## The cat info panel

The cat info panel is a `glaiel::MenuPanel` subclass. It builds its own buttons
by name out of its movieclip:

```
glaiel::Button* MenuPanel::register_button(std::string name,
                                           std::wstring label,
                                           std::function<void()> cb)
```

It calls that six times — `topipe`, `tobox`, `familytree`, `nametag_button`,
`nextcat_left`, `nextcat_right`. The lover and rival portraits are children of
the very same clip: the `HouseCatStatus` symbol in `ui.swf` lists `lover` and
`hater` right alongside `topipe` and `familytree`. The game just never registers
them — it only shows and hides them, and hangs the hover tooltip off the
separate `lover_tt` / `hater_tt` regions.

So this mod hooks the panel's init, lets it register its six buttons, and then
registers `lover` and `hater` as two more. Nothing about rendering changes; the
portraits were already drawn and already kept up to date.

## The family tree

The tree registers no buttons at all, so there is no named-child route here. But
`register_button` is only a wrapper: the `Button` constructor underneath takes a
clip **pointer**, so any clip can become one. The catch is which object it wants
� a portrait entry's `+0x98` is the clip *component*, and what the constructor
binds is the display object under it at `[component+0x80]`, which links back at
`+0x40`. Handing it the component crashes inside the binder.

Clicking has to **leave the tree before selecting**. Selecting while the tree is
open provably works � `show_cat` returns and the panel holds the new cat � and
is then thrown away when the tree tears down. So the mod runs the tree's own exit
action first. That action's callback reads nothing but its captured hud, so a
two-field stand-in for the `std::function` it normally lives in is enough to
invoke it.

### Marking the reachable edges

The tree strokes its edges black into a vector-graphics batch, and lights the
hovered cat's two edges white over the top. Marked edges use the same mechanism:
same black, at just over the weight the highlight uses, so they read as part of
the existing design rather than as a second highlight competing with the first.

The rule is deliberately **not** the hover rule. Hovering lights both edges
*above* a cat, up to its parents. What is wanted here is the one edge *below*
each living cat, connecting it down into the tree � so the loop runs over
children instead: for every node, if one of its parents is in the house, draw
that node's edge up to that parent. Each living cat is the parent of exactly one
node, so it gets exactly one edge, and the examined cat is nobody's parent, so
the root correctly gets none.

Timing matters more than it looks. The tree only redraws when the hovered cat
changes: it clears the batch, then strokes base and highlight edges. Anything
added has to go on at that exact moment or it is wiped, and at no other moment or
it accumulates every frame. The hud records which cat it last drew for, so
comparing against that identifies precisely the right frames.

`MARK_THICKNESS` in `dllmain.cpp` is the one number worth tuning.

## Reaching the cat you clicked

The click runs the same call the arrows run:

```
HouseCatStatus::show_cat(house_cat, clear_if_null = false)
```

Finding `house_cat` is the only fiddly part, and it is what makes the edge cases
safe rather than lucky.

The panel holds a **HouseCat** — the entity in the house. Its `+0x80` is an id
into the cat registry, and the **Cat** record that resolves to carries the lover
id at `+0xbc8` and the rival id at `+0xbd8`. Those ids name a *record*, not an
entity. So the mod walks the house's HouseCat list — the same list the arrows
iterate — for the entity carrying that id, then applies the arrows' own
eligibility test:

| condition | meaning |
| --- | --- |
| `house_cat[+0x88] != 0` | in the scrollable list at all |
| zoomed out, or `house_cat[+0xe8] == room` | visible from where the camera is |

That set is *by construction* the set of cats the panel can display, so:

- **given away** (Tracy, Jack, Butch, the organ grinder) — no entity carries the
  id, nothing happens
- **dead but the body is still in the room** — it is in the list, so it selects,
  exactly as the arrows would reach it
- **in another room while zoomed in** — filtered out, nothing happens
- **no lover or rival at all** — id is `-1`, nothing happens

Relationships are stored as ids rather than pointers, so a dangling reference
is not representable. The panel's own stale-pointer test (a generation stored
beside the pointer, compared against `[cat-8]`) is checked too, before anything
is dereferenced.

The click is a no-op when it cannot reach the cat. The icons are not greyed out
— the hover tooltip stays the only affordance, same as before.

## Build

```
clang++ -O2 -shared -static -s -o mewlinks.dll dllmain.cpp
```

or with MSVC:

```
cl /nologo /LD /O2 /EHsc /DNDEBUG dllmain.cpp /link /DLL /OUT:mewlinks.dll
```

Either works, because the mod never includes `<string>` or `<functional>`.
`register_button` takes all three arguments **by value**, so the mod has to hand
the game three real MSVC STL objects; `msvcabi.inc` mirrors the layouts read out
of the exe instead of borrowing whatever STL the mod is compiled against:

```
sizeof(std::string)           == 0x20   buffer 16, size +0x10, capacity +0x18
sizeof(std::wstring)          == 0x20
sizeof(std::function<void()>) == 0x40   storage 0x00..0x37, impl pointer at +0x38
the inline callable           == { vptr; captures }
vtable                        == _Copy _Move _Do_call _Target_type _Delete_this _Get
```

`register_button` destroys all three before returning; every object here is
small enough to live inline, so each destructor is a no-op and nothing is freed.

`test_abi.cpp` replays what the game does with them — copy into the Button,
invoke, destroy with the deallocate flag the game computes — and checks the
callback arrives with the right captures:

```
clang++ -O2 -o test_abi.exe test_abi.cpp && ./test_abi.exe
```

## When the game updates

`sites.inc` pins seven call targets by RVA plus the first 16 bytes found there.
A single mismatch aborts the whole install: the hook is never written, the game
runs unmodified, and `mod_logs/mewlinks.log` names the site that moved. Regenerate it
against a new build with `tools/make_sig.py`.

| site | what it is |
| --- | --- |
| `0x0e9ac0` | `HouseCatStatus::init` — hooked; registers the panel's buttons |
| `0x97c2b0` | `MenuPanel::register_button` |
| `0x0ec7b0` | `HouseCatStatus::show_cat` |
| `0x0ecc30` | the house view; `+0x88` is the zoomed room, null when zoomed out |
| `0x96b470` | `ensure_singleton(holder, type_id)` |
| `0x054500` | the object every button stores at `Button+0x88` |
| `0x0d7220` | `CatRegistry::resolve(id)` |

The hook is a plain detour: 13 bytes of prologue (eight pushes) are moved into a
trampoline and replaced with a 12-byte absolute jump plus a `nop`. The stolen
bytes are pushes only, so nothing position-dependent moves, and the game's own
unwind info still describes the frame correctly.

## Notes

- Does not touch saves. It adds two buttons to a UI panel and changes which cat
  that panel is looking at; nothing is written to disk.
- If the portraits start animating on hover, that is the `Button` frame state —
  buttons drive their clip's frames. Registering on `lover_tt` / `hater_tt`
  instead (change `g_clip` in `dllmain.cpp`) puts the hit region on the existing
  invisible tooltip rects instead of the artwork.

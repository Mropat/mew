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

It calls that six times â€” `topipe`, `tobox`, `familytree`, `nametag_button`,
`nextcat_left`, `nextcat_right`. The lover and rival portraits are children of
the very same clip: the `HouseCatStatus` symbol in `ui.swf` lists `lover` and
`hater` right alongside `topipe` and `familytree`. The game just never registers
them â€” it only shows and hides them, and hangs the hover tooltip off the
separate `lover_tt` / `hater_tt` regions.

So this mod hooks the panel's init, lets it register its six buttons, and then
registers `lover` and `hater` as two more. Nothing about rendering changes; the
portraits were already drawn and already kept up to date.

## The family tree

The tree registers no buttons at all, so there is no named-child route here. But
`register_button` is only a wrapper: the `Button` constructor underneath takes a
clip **pointer**, so any clip can become one. The catch is which object it wants
— a portrait entry's `+0x98` is the clip *component*, and what the constructor
binds is the display object under it at `[component+0x80]`, which links back at
`+0x40`. Handing it the component crashes inside the binder.

Clicking has to **leave the tree before selecting**. Selecting while the tree is
open provably works — `show_cat` returns and the panel holds the new cat — and
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
each living cat, connecting it down into the tree — so the loop runs over
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

The panel holds a **HouseCat** â€” the entity in the house. Its `+0x80` is an id
into the cat registry, and the **Cat** record that resolves to carries the lover
id at `+0xbc8` and the rival id at `+0xbd8`. Those ids name a *record*, not an
entity. So the mod walks the house's HouseCat list â€” the same list the arrows
iterate â€” for the entity carrying that id, then applies the arrows' own
eligibility test:

| condition | meaning |
| --- | --- |
| `house_cat[+0x88] != 0` | in the scrollable list at all |
| zoomed out, or `house_cat[+0xe8] == room` | visible from where the camera is |

That set is *by construction* the set of cats the panel can display, so:

- **given away** (Tracy, Jack, Butch, the organ grinder) â€” no entity carries the
  id, nothing happens
- **dead but the body is still in the room** â€” it is in the list, so it selects,
  exactly as the arrows would reach it
- **in another room while zoomed in** â€” filtered out, nothing happens
- **no lover or rival at all** â€” id is `-1`, nothing happens

Relationships are stored as ids rather than pointers, so a dangling reference
is not representable. The panel's own stale-pointer test (a generation stored
beside the pointer, compared against `[cat-8]`) is checked too, before anything
is dereferenced.

The click is a no-op when it cannot reach the cat. The icons are not greyed out
â€” the hover tooltip stays the only affordance, same as before.

## Build

```
build.bat
```

It picks MSVC when available and llvm-mingw otherwise, compiles `version.rc`
alongside, and needs `..\common` on the include path. By hand:

```
clang++ -O2 -shared -static -s -I../common -o mewlinks.dll dllmain.cpp version.res
```

Either compiler works, because the mod never includes `<string>` or
`<functional>`. `register_button` takes all three arguments **by value**, so it
has to hand the game three real MSVC STL objects; `common/msvcabi.inc` mirrors
the layouts read out of the exe instead of borrowing whatever STL it was built
against:

```
sizeof(std::string)           == 0x20   buffer 16, size +0x10, capacity +0x18
sizeof(std::wstring)          == 0x20
sizeof(std::function<void()>) == 0x40   storage 0x00..0x37, impl pointer at +0x38
the inline callable           == { vptr; captures }
vtable                        == _Copy _Move _Do_call _Target_type _Delete_this _Get
```

Everything handed across that boundary is `alignas(16)` with an assertion. The
game reads several of these back with `movaps`, an *aligned* load, and two
doubles do not get 16-byte alignment on their own - a detail that produced three
crashes visible only in MSVC builds.

`test_abi.cpp` replays what the game does with those objects and checks the
build-identity logic. It runs in CI under the same compiler that builds the
released DLL.

## Hooking, and other mods

Hooks go in through mewjector's chain API - `MJ_InstallHook` at priority 50 - so
**other mods can hook the same functions**. `HouseCatStatus::init` in particular
is a place anyone doing cat-UI work will want. Several mods on one address form
a priority-ordered chain, each handed a trampoline to call the next.

Passing `stolenBytes = 0` lets mewjector's length disassembler pick where to cut
the prologue. `common/detour.inc` is a fallback for when the API is absent; it
never overwrites a site mewjector already manages, because that would break
whoever got there first.

## When the game updates

`sites.inc` pins fifteen call targets by RVA plus the first 24 bytes found at
each, and is regenerated with `tools/make_sig.py mewlinks`.

The signature bytes are the authority: they ask *is the code I expected still at
this RVA*, which stays true across a game update that never touched these
functions - so the mod keeps working after an unrelated patch. The one thing
they cannot answer is a site another mod has already hooked, where the bytes are
mewjector's jump. Only then does the recorded build identity (PE `TimeDateStamp`
and `SizeOfImage`, which no hook rewrites) get a vote:

```
site untouched             -> the bytes decide, whatever build this is
site hooked, known build   -> trust whoever verified it before us
site hooked, unknown build -> nobody can vouch for it; refuse
```

A refusal installs nothing at all: the game runs unmodified and
`mod_logs/mewlinks.log` names the site.

| site | what it is |
| --- | --- |
| `0x0e9ac0` | `HouseCatStatus::init` — hooked; registers the panel's buttons |
| `0x17f7b0` | the family tree's per-frame pass — hooked |
| `0x97c2b0` | `MenuPanel::register_button` |
| `0x97d590` | the `Button` constructor, which takes a clip pointer |
| `0x0ec7b0` | `HouseCatStatus::show_cat` |
| `0x0ecc30` | the house view; `+0x88` is the zoomed room, null when zoomed out |
| `0x96b470` | `ensure_singleton(holder, type_id)` |
| `0x0d7220` | `CatRegistry::resolve(id)` |
| `0x18ccd0` | the tree's exit action |
| `0x17ddd0` | strokes one edge into the tree's graphics batch |

## Notes

- **Does not touch saves.** It adds buttons to UI panels and changes which cat
  is selected; nothing is written to disk. Remove the DLL and behaviour is
  exactly vanilla.
- **Coexists with other mods** that hook the same functions, through mewjector's
  hook chain. Without mewjector's API it falls back to hooking alone.
- **Logs one line per launch** to `Mewgenics/mod_logs/mewlinks.log`, truncated
  each run. Build with `/DMEWLINKS_TRACE` for a line per click.
- **Antivirus false positives.** A mod that hooks a game process does the same
  things a scanner looks for - writing to another process's code - and an
  unsigned binary nobody has downloaded before has no reputation to offset that.
  One release was flagged as `Trojan:Win32/Wacatac.B!ml`, submitted to
  Microsoft, and cleared on review. If you hit one, the DLL is built in public
  by GitHub Actions and every release links the run that produced it.

# tools

Offline maintenance scripts. Not shipped with any mod; they exist so a game
update does not mean starting from a blank disassembler.

    pip install capstone

## find_gate.py

Re-derives the house-shop weekday gate that `mewdaily` patches, structurally
rather than by address, and prints a ready-to-paste signature.

    python find_gate.py "C:/.../Mewgenics/Mewgenics.exe"

Anchors on things that survive a recompile: the divide-by-7 idiom identifies
the civil-calendar routine, its callers are filtered to the one that also
references `breakdown` / `TracyHouseShop` / `TracyHouseShopSpecialDay`, and the
gate is the conditional jump after `cmp [rax+0xc], reg`.

When a game update makes the shipped signature stop matching, run this, paste
the new `SIG` / `GATE_OFF` / `GATE_SIZE` into `mewdaily/dllmain.cpp`, and tag a
release. Confirm the new signature matches exactly once - the mod refuses to
patch otherwise, which is the intended failure mode.

## build_index.py

Rebuilds a searchable map of a given build.

    python build_index.py "C:/.../Mewgenics.exe" --out analysis --getfield 0x942da0

| output | contents |
|---|---|
| `source_files.txt` | debug paths left in the binary - the project's module map |
| `classes.txt` | `glaiel::` class names from RTTI |
| `functions.tsv` | every function from `.pdata` plus the strings it references |
| `gon_fields.txt` | GON field names the engine reads, by frequency |

`functions.tsv` is the workhorse: `grep -i shop functions.tsv` narrows 55k
functions to a handful of candidates, which you then disassemble.

`--getfield` is the RVA of `GonObject::operator[](const std::string&)` and is
build-specific; omit it to skip that output. Find it by disassembling any
function that reads a known GON field and taking the call made right after the
field-name string is materialised.

Note these are heuristics - counts shift a little between versions of the
scripts themselves, so treat the numbers as a map, not a census.

# Overflow flies

**Not a released mod.** Raw code and notes, for anyone who wants to pick the
thread up.

## Symptom

A cat with **Incubator+** and **Dark One** passes a turn and rot flies appear
with roughly a billion HP and damage. In one recorded fight a single pass
spawned five of them.

## What is actually happening

Nothing overflows. The number is `1073741823` — `0x3FFFFFFF`, `INT_MAX / 2` —
and it is the engine's *"spawn at full health"* sentinel.

`Character::init` writes **max** HP but never writes **current** HP, leaving the
sentinel in place. Something clamps it later, before that character's next turn.
A character spawned mid-turn can be damaged inside that window, and the damage
code reports damage dealt as the entire sentinel-to-max drop.

Dark One and Incubator together manufacture that window every single turn:

```
Incubator      StatusOnOverHealed       { SpawnScaledRotFly 0 }
Incubator+     ScaledStatusOnOverHealed { SpawnScaledRotFly 1 }

DarkOneStrike  target { aoe_mode all }
               damage_instance { damage 1  effects { Leech 1  IgnoreSelf 1 } }
```

so the AoE reaches the newborn fly in the turn it appeared. From a combat log,
in order:

```
SPAWN  fly                            maxhp -1 -> 1     current HP still 1073741823
hit    fly     amount=1               +4b0  1073741823 -> 1
hit    caster  amount=-1073741822     <- Leech, the full bogus delta
SPAWN  fly                            maxhp -1 -> 1
hit    fly     amount=-1073741821     +4bc  1 -> 1073741822
```

`1073741823 - 1 = 1073741822`, exactly the caster's heal. The caster is at full
health, so every point of it is **overheal** — and `Incubator+` spawns a fly
scaled by the overheal. That fly is itself a mid-turn spawn, so it can feed the
loop again, which is why one pass produces several.

The damage is always 1. Nothing grows. The whole bug is the window.

### It is the window, not the fly

Across one fight, **18 of 18** sentinel collapses were on characters spawned in
that same turn, and **none** on any older one. Flies from earlier turns sit at
their real 1 HP and die correctly, `1 -> 0`, leeching 1 as they should.

The fly also does not die from the bogus hit: the applier computes
`min(current - damage, max)` = `min(1073741822, 1)` = 1, so it survives at full
health and can be hit again on later turns.

Dark One and Incubator are only a reliable generator. **Any same-turn spawn plus
any leech effect should reproduce it.**

## The fix

Fill in current HP at the end of `Character::init`, before the character can be
targeted.

```cpp
#define SPAWN_HP_SENTINEL 0x3FFFFFFF        // 1073741823 == INT_MAX / 2

#define OFF_HP_CURRENT    0x4b0
#define OFF_HP_MAX        0x4bc

static bool needs_spawn_hp(int current, int max_hp) {
    if (current != SPAWN_HP_SENTINEL) return false;   // a real value already
    if (max_hp <= 0) return false;                    // max not established yet
    if (max_hp >= SPAWN_HP_SENTINEL) return false;    // nothing useful to copy
    return true;
}

// hooked on Character::init(std::string, bool, bool)  -  RVA 0x0f70f0
static void hooked_init(void* self, void* name, bool a, bool b) {
    g_orig_init(self, name, a, b);
    if (!self) return;

    int* current = (int*)((char*)self + OFF_HP_CURRENT);
    const int max_hp = *(int*)((char*)self + OFF_HP_MAX);
    if (needs_spawn_hp(*current, max_hp)) *current = max_hp;
}
```

The test is deliberately narrow: only a value that is *exactly* the sentinel is
touched, and only when max HP is a usable positive number. A wounded spawn, a
dead character, a max that is not filled in yet — all left alone. It can close
the window but cannot overwrite a decision the game actually made.

Verified: sentinel collapses went from 18 in a fight to **0**, and flies died
normally.

## Field offsets

Established from a combat log rather than from disassembly:

| offset | meaning | evidence |
| --- | --- | --- |
| `+0x4b0` | current HP | a cat went 48 → 47 on a one-damage hit |
| `+0x4b4` | shield | the caster sat at 4 throughout |
| `+0x4bc` | max HP | every spawn moved it from `-1` to its real maximum |

`Character::init` is at RVA `0x0f70f0`, found via its own assert signature
string `void __cdecl glaiel::Character::init(...)`. The damage applier is
`0x110120` — `(rcx = target, rdx = DamageInstance*)`, 56 call sites, and the
only path to `COMBAT_POPUP_IMMUNE`. Heals travel through it as **negative**
damage, with `0x10` set in the flags byte at `DamageInstance+0x5a`.

## What is not yet established

- **What performs the later clamp, and when.** It happens before the next turn;
  where, nobody has looked.
- **Whether a cat carrying damage between battles is affected.** Every character
  in the recorded fight spawned at full health, so that path was never
  exercised. The reasoning says it is safe — current HP is still the sentinel
  when `init` returns, so the real value must be written afterwards and would
  overwrite the fill — but reasoning is not evidence.
- Whether arena scenery, which takes no damage at all (`20 -> 20` in the logs),
  can ever be caught in the same window.

## Reproducing

`../probe/mewcombat_probe.cpp` logs every damage application, spawn, death and
turn boundary. Two captured logs are in `../probe/`. It hooks at priority 100 so
it observes without disturbing anything else.

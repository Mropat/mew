// no_more_overflow_flies - stop mid-turn spawns handing out ~2^30 healing
//
// A character created during a turn keeps the engine's "spawn at full health"
// sentinel in its current-HP field - INT_MAX/2, 1073741823 - because
// Character::init fills in max HP but not current HP. Something clamps it
// later, before that character's next turn. Damage landing inside that window
// reports damage dealt as the entire sentinel-to-max drop, so any leech effect
// heals for about a billion.
//
// Reproduced with Dark One and Incubator+, which together generate the window
// on every single turn:
//
//   Incubator      spawns a rot fly, max HP 1, current HP left at the sentinel
//   DarkOneStrike  aoe_mode all, damage 1, effects { Leech 1 }
//
// so the AoE reaches the newborn fly in the turn it appeared:
//
//   hit   fly      amount=1              +4b0  1073741823 -> 1
//   hit   caster   amount=-1073741822    <- leech, the full bogus delta
//
// The caster is at full health, so all of it is overheal, and Incubator+ is
// ScaledStatusOnOverHealed { SpawnScaledRotFly 1 } - which spawns a fly scaled
// by the overheal. That fly has ~2^30 HP and damage, and being another newborn
// it can feed the loop again. In one recorded fight, 18 of 18 sentinel
// collapses were on characters spawned that same turn, and none on any older
// one - the window is the whole bug.
//
// The fix is to close it: fill in current HP at the end of init, before the
// character can be targeted. See rule.inc for how narrow the test is.
//
// This is a guess at what the game intends, not a sanctioned fix. It changes
// one integer, only when that integer is still the uninitialised sentinel.
//
// Not released. Raw code for discovery - see BUG.md.

#include <windows.h>
#define MODLOG_NAME "no_more_overflow_flies"
#include "../common/modlog.inc"

#ifdef NOFLIES_TRACE
#define TRACE(...) logf_(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

#include "../common/hookapi.inc"
#include "sites.inc"
#include "rule.inc"

static unsigned char* g_base;
static unsigned char* g_at[SITE_COUNT];

// Established from a combat log rather than from the disassembly: a cat went
// 48 -> 47 at +0x4b0 on a one-damage hit, sat at 4 shield in +0x4b4, and every
// spawn moved +0x4bc from -1 to its real maximum.
#define OFF_HP_CURRENT  0x4b0
#define OFF_HP_MAX      0x4bc

typedef void (*CharInitFn)(void* self, void* name, bool a, bool b);
static CharInitFn g_orig_init;

static unsigned g_fixed;

static void hooked_init(void* self, void* name, bool a, bool b) {
    g_orig_init(self, name, a, b);
    if (!self) return;

    int* current = (int*)((char*)self + OFF_HP_CURRENT);
    const int max_hp = *(int*)((char*)self + OFF_HP_MAX);
    if (!needs_spawn_hp(*current, max_hp)) return;

    *current = max_hp;
    if (++g_fixed <= 8 || (g_fixed % 100) == 0)
        TRACE("filled in spawn HP: %p now %d/%d (%u so far)", self, max_hp, max_hp, g_fixed);
}

// Three register spills and a frame setup. Only a fallback: with Mewjector,
// stolenBytes = 0 and its length disassembler picks the boundary.
#define STOLEN 19

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    g_base = (unsigned char*)GetModuleHandleA(NULL);
    const bool chained = hookapi_init();
    int bad = verify_sites_chained(SITES, SITE_COUNT, SITE_SIGLEN, g_base, g_at,
                                   GAME_TIMESTAMP, GAME_SIZEOFIMAGE);
    if (bad >= 0) {
        logf_("no_more_overflow_flies: %s at +%#x does not match - not installing",
              SITES[bad].name, SITES[bad].rva);
        return TRUE;
    }
    g_orig_init = (CharInitFn)install_hook(SITES[S_CHARINIT].rva, g_at[S_CHARINIT],
                                           STOLEN, (const void*)&hooked_init, "no_more_overflow_flies");
    if (!g_orig_init) { logf_("no_more_overflow_flies: could not install the hook"); return TRUE; }

    logf_("no_more_overflow_flies: spawns now start at full health%s",
          chained ? "" : " (standalone hook - Mewjector API not found)");
    return TRUE;
}

// Installing hooks the neighbourly way.
//
// Mewjector manages the entry points mods hook, and chains several mods on one
// address: hooks on an RVA form a priority-ordered chain, each handed a
// trampoline to call the next. Its own documentation is blunt about the
// alternative - "the only thing that will cause problems: manually patching the
// entry point of an RVA that Mewjector also manages" - which is exactly what
// writing a raw jump does, and why two mods that both want
// HouseCatStatus::init can only work if they go through this.
//
// Passing stolenBytes = 0 lets Mewjector pick the steal length with its own
// length disassembler, so no mod has to count instruction bytes by hand.
//
// Falls back to common/detour.inc when the API is absent, so the mods still
// work if they are loaded by something other than Mewjector - just without the
// ability to share a site.

#include "mewjector.h"
#include "detour.inc"

#ifndef MOD_HOOK_PRIORITY
#define MOD_HOOK_PRIORITY 50        // their bands: 0-9 framework, 10-49 core,
#endif                              // 50-99 secondary, 100+ observers

static MewjectorAPI g_mj;
static bool         g_have_mj;

static bool hookapi_init() {
    g_have_mj = MJ_Resolve(&g_mj) != 0;
    return g_have_mj;
}

// Verify sites, allowing for the fact that another mod may have got here first.
//
// A signature check reads the bytes at an RVA and compares them to the game's
// prologue. That is exactly wrong once Mewjector manages the site: the bytes
// there are its jump, so the check fails and the mod refuses to install for the
// one reason it should not - that a second mod wanted the same function.
//
// Mewjector verified those bytes itself when the first hook went in, so where
// it reports an existing hook, trust that instead of looking.
static int verify_sites_chained(const Site* sites, int count, size_t siglen,
                                unsigned char* base, unsigned char** out) {
    int bad = -1;
    for (int i = 0; i < count; i++) {
        out[i] = base + sites[i].rva;
        if (bad >= 0) continue;
        if (g_have_mj && g_mj.QueryHook(sites[i].rva) > 0) continue;   // already managed
        if (memcmp(out[i], sites[i].sig, siglen) != 0) bad = i;
    }
    return bad;
}

// Returns the function to call to continue the chain - the next hook, or the
// original - or null if the hook could not be installed.
static void* install_hook(unsigned rva, unsigned char* site, size_t fallback_stolen,
                          const void* detour, const char* owner) {
    if (g_have_mj) {
        void* next = 0;
        if (g_mj.InstallHook(rva, 0, (void*)detour, &next, MOD_HOOK_PRIORITY, owner))
            return next;
        // Mewjector is present but refused. Do not fall back to a raw patch:
        // it may already manage this site, and overwriting it breaks whoever
        // got there first.
        return 0;
    }
    return install_detour(site, fallback_stolen, detour);
}

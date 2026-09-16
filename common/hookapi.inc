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

// Is this the build the RVAs were read from?
//
// The signature bytes at each site answer that too, but only while the site is
// untouched - once another mod hooks one, its bytes are Mewjector's jump and
// the question cannot be asked there any more. The PE header can: nothing
// rewrites TimeDateStamp or SizeOfImage, so they identify the build no matter
// how many mods are loaded.
//
// This is the check that must not be skipped. A site check answers "is the code
// I expected still here"; this one answers "is this even the right game", and
// after an update it is the only one still able to.
static bool verify_build(unsigned char* base, unsigned expect_stamp,
                         unsigned expect_image) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return nt->FileHeader.TimeDateStamp == expect_stamp
        && nt->OptionalHeader.SizeOfImage == expect_image;
}

// Verify sites, allowing for the fact that another mod may have got here first.
//
// The signature bytes are the authority, and deliberately so: they answer "is
// the code I expected still at this RVA", which stays true across a game update
// that does not touch the functions we care about. A mod should keep working
// after a patch that had nothing to do with it.
//
// The one case they cannot answer is a site another mod has already hooked -
// the bytes there are Mewjector's jump, not the game's prologue. Only then does
// the build identity get a vote, and only as a fallback:
//
//   site untouched        -> the bytes decide, whatever build this is
//   site hooked, known build   -> trust whoever verified it before us
//   site hooked, unknown build -> nobody can vouch for it; refuse
//
// So a vanilla rebuild that leaves our functions alone still passes, and the
// only thing we refuse is a site we genuinely cannot vouch for.
static int verify_sites_chained(const Site* sites, int count, size_t siglen,
                                unsigned char* base, unsigned char** out,
                                unsigned expect_stamp, unsigned expect_image) {
    const bool known_build = verify_build(base, expect_stamp, expect_image);
    int bad = -1;
    for (int i = 0; i < count; i++) {
        out[i] = base + sites[i].rva;
        if (bad >= 0) continue;
        const bool hooked = g_have_mj && g_mj.QueryHook(sites[i].rva) > 0;
        if (hooked) {
            if (!known_build) bad = i;      // cannot read it, cannot vouch for it
        } else if (memcmp(out[i], sites[i].sig, siglen) != 0) {
            bad = i;
        }
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
#ifdef MOD_REQUIRES_MEWJECTOR
    // No Mewjector, no hook. This mod does not patch entry points itself.
    (void)site; (void)fallback_stolen;
    return 0;
#else
    return install_detour(site, fallback_stolen, detour);
#endif
}

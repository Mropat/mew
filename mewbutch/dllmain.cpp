// mewbutch - let harder adventures satisfy Butch's donation requirements
//
// Acts are the adventures (3 of them, 4 chapters each); difficulty is a third
// axis over the whole grid. Butch walks act and chapter upward to A3 C4, then
// holds there and escalates difficulty instead. Because every tier from that
// point on pins required_chapter to 4 AND required_act to 3, a cat from a
// brutal run of an earlier adventure is refused no matter how hard it was.
//
// The rule this mod applies: one difficulty step above what Butch asks for
// excuses a LOWER ACT, but never a lower chapter. So "A3 C4 hard" also takes
// "A1/A2 C4 crazy".
//
// Butch's test is case 1 of the NPC condition evaluator at EVAL:
//
//     bool eval(self, condition, cat)
//
// whose case-1 body is, in the original:
//
//     not a veteran            -> reject     (cat[+0xbf8] bit 1)
//     act        < required    -> reject
//     chapter    < required    -> reject
//     difficulty < required    -> reject
//     otherwise                -> accept
//
// v2 rewrite: this used to be 70 bytes of hand-assembled x86 written over the
// original 72, with the call and branch targets read back out of the bytes
// found at runtime. It is now a detour - the evaluator's 15-byte prologue moves
// to a trampoline, and the rule is ordinary C++ that calls the game's own
// requirement getters. Same behaviour, none of the hand-encoding. (The old
// version is at tag mewbutch-v1.1.0 if you want to compare.)
//
// The mod runs the game's own evaluator first and only reconsiders a rejection,
// so it can add acceptances but never remove one. Anything vanilla Butch takes,
// he still takes.
//
// Ships as a mewjector mod: drop mewbutch.dll into Mewgenics/mods/.

#define MODLOG_NAME "mewbutch"
#include "../common/modlog.inc"


#ifdef MEWBUTCH_TRACE
#define TRACE(...) logf_(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

// ------------------------------------------------------- pinned call targets --

#include "../common/hookapi.inc"
#include "sites.inc"

static unsigned char* g_base;
static unsigned char* g_at[SITE_COUNT];

#define COND_BUTCH_DONATION  1        // case 1 of the evaluator's jump table
#define COND_STRIDE          0x78     // per-condition slot size on the NPC

// Gates the evaluator applies to every condition before it reaches the case
// body, read off the preamble at EVAL+0x13.
#define OFF_CAT_SPECIAL      0x7ac    // nonzero: this cat only answers condition 6
#define OFF_SLOT_BLOCKED     0x48     // nonzero -> reject
#define OFF_SLOT_ENABLED_A   0x51     // zero    -> reject
#define OFF_SLOT_ENABLED_B   0x50     // zero    -> reject

#define OFF_CAT_VETERAN      0xbf8    // bit 1
#define OFF_CAT_ACT          0xc08    // signed byte
#define OFF_CAT_CHAPTER      0xc09
#define OFF_CAT_DIFFICULTY   0xc0a

typedef bool (*EvalFn)(void* self, int condition, void* cat);
typedef int  (*ReqFn)(void* self);

static EvalFn g_orig_eval;

#include "rule.inc"

// -------------------------------------------------------------- the new rule --

static bool butch_accepts(void* self, const unsigned char* cat) {
    // The evaluator rejected before it ever reached the case body if any of
    // these hold. We are second-guessing a rejection, so we must not
    // second-guess one of those.
    if (cat[OFF_CAT_SPECIAL] != 0) return false;
    const unsigned char* slot =
        (const unsigned char*)self + COND_BUTCH_DONATION * COND_STRIDE;
    if (slot[OFF_SLOT_BLOCKED]   != 0) return false;
    if (slot[OFF_SLOT_ENABLED_A] == 0) return false;
    if (slot[OFF_SLOT_ENABLED_B] == 0) return false;

    if (!(cat[OFF_CAT_VETERAN] & 2)) return false;

    const int act        = (signed char)cat[OFF_CAT_ACT];
    const int chapter    = (signed char)cat[OFF_CAT_CHAPTER];
    const int difficulty = (signed char)cat[OFF_CAT_DIFFICULTY];

    return butch_rule(act, chapter, difficulty,
                      ((ReqFn)g_at[S_REQ_ACT])(self),
                      ((ReqFn)g_at[S_REQ_CHAP])(self),
                      ((ReqFn)g_at[S_REQ_DIFF])(self));
}

static bool hooked_eval(void* self, int condition, void* cat) {
    const bool vanilla = g_orig_eval(self, condition, cat);
    if (vanilla || condition != COND_BUTCH_DONATION || !self || !cat) return vanilla;

    const bool relaxed = butch_accepts(self, (const unsigned char*)cat);
    if (relaxed) {
        TRACE("accepting a cat Butch refused: act %d chapter %d difficulty %d",
              (signed char)((unsigned char*)cat)[OFF_CAT_ACT],
              (signed char)((unsigned char*)cat)[OFF_CAT_CHAPTER],
              (signed char)((unsigned char*)cat)[OFF_CAT_DIFFICULTY]);
    }
    return relaxed;
}

// ------------------------------------------------------------------- install --

// Fallback steal length, used only when Mewjector is absent: three register
// spills, 15 bytes. With it, stolenBytes = 0 and its length disassembler picks.
#define STOLEN 15

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    g_base = (unsigned char*)GetModuleHandleA(NULL);
    const bool chained = hookapi_init();
    int bad = verify_sites_chained(SITES, SITE_COUNT, SITE_SIGLEN, g_base, g_at,
                                   GAME_TIMESTAMP, GAME_SIZEOFIMAGE);
    if (bad >= 0) {
        logf_("mewbutch: %s at +%#x does not match - not installing",
              SITES[bad].name, SITES[bad].rva);
        return TRUE;                                  // leave the game alone
    }
    g_orig_eval = (EvalFn)install_hook(SITES[S_EVAL].rva, g_at[S_EVAL], STOLEN,
                                       (const void*)&hooked_eval, "mewbutch");
    if (!g_orig_eval) { logf_("mewbutch: could not install the hook"); return TRUE; }
    logf_("mewbutch: a harder run now excuses a lower act%s",
          chained ? "" : " (standalone hook - Mewjector API not found)");
    return TRUE;
}

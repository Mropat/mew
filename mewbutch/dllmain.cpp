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

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ------------------------------------------------------------------ logging --

static void logf_(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) return;
    char* slash = strrchr(path, (char)92);
    if (!slash) return;
    lstrcpyA(slash + 1, "mewbutch.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

#ifdef MEWBUTCH_TRACE
#define TRACE(...) logf_(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

// ------------------------------------------------------- pinned call targets --

struct Site {
    const char*   name;
    unsigned      rva;
    unsigned char sig[16];
};

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

// The evaluator's prologue is three register spills - 15 bytes - so a 12-byte
// absolute jump lands on an instruction boundary with three bytes to spare.
#define STOLEN 15

static bool verify_sites() {
    bool ok = true;
    for (int i = 0; i < SITE_COUNT; i++) {
        unsigned char* p = g_base + SITES[i].rva;
        if (memcmp(p, SITES[i].sig, SITE_SIGLEN) != 0) {
            logf_("mewbutch: %s at +%#x does not match - refusing to install",
                  SITES[i].name, SITES[i].rva);
            ok = false;
        }
        g_at[i] = p;
    }
    return ok;
}

static void write_abs_jmp(unsigned char* at, void* dest) {
    at[0] = 0x48; at[1] = 0xB8;                       // mov rax, imm64
    memcpy(at + 2, &dest, 8);
    at[10] = 0xFF; at[11] = 0xE0;                     // jmp rax
}

static bool install() {
    unsigned char* site = g_at[S_EVAL];

    unsigned char* tramp = (unsigned char*)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("mewbutch: VirtualAlloc failed"); return false; }

    memcpy(tramp, site, STOLEN);
    write_abs_jmp(tramp + STOLEN, site + STOLEN);
    g_orig_eval = (EvalFn)tramp;

    DWORD old;
    if (!VirtualProtect(site, STOLEN, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("mewbutch: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    write_abs_jmp(site, (void*)&hooked_eval);
    for (int i = 12; i < STOLEN; i++) site[i] = 0x90;
    VirtualProtect(site, STOLEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, STOLEN);
    return true;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    g_base = (unsigned char*)GetModuleHandleA(NULL);
    logf_("mewbutch: base %p", g_base);
    if (!verify_sites()) return TRUE;                 // leave the game alone
    if (!install())      return TRUE;
    logf_("mewbutch: a harder run now excuses a lower act");
    return TRUE;
}

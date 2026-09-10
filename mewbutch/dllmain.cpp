// mewbutch - let harder adventures satisfy Butch's donation requirements
//
// Butch accepts a donated cat for the next storage upgrade only if the cat is
// a veteran AND its adventure's act, chapter and difficulty each meet or beat
// the tier's requirement:
//
//     test  [rdi+0xbf8], 2          ; cat is a veteran
//     movsx ebx,[rdi+0xc08] ; call get_required_act        ; cmp/jl -> reject
//     movsx ebx,[rdi+0xc09] ; call get_required_chapter    ; cmp/jl -> reject
//     movsx ebx,[rdi+0xc0a] ; call get_required_difficulty ; cmp/jl -> reject
//
// Every tier pins required_chapter to 4, so a cat from any other adventure is
// refused no matter how brutal the difficulty. This rewrites the block so the
// difficulty test runs first and a harder run substitutes for the chapter:
//
//     difficulty <  required  -> reject
//     difficulty >  required  -> accept  (any adventure)
//     difficulty >= 3         -> accept  (clamped: nothing above impossible
//                                         without limit-break gear)
//     otherwise               -> chapter must still match, as before
//
// The replacement is 70 bytes in the original 72, so nothing shifts. Call and
// branch targets are read out of the bytes found at runtime rather than
// hardcoded, so the patch survives the code moving.
//
// Ships as a mewjector mod: drop mewbutch.dll into Mewgenics/mods/.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

static const int SIG[] = {
#include "sig.inc"
};
static const size_t SIG_LEN = sizeof(SIG) / sizeof(SIG[0]);   // 72

// Byte offsets of the operands we read back out of the original block.
static const size_t OFF_JE_FAIL   = 0x0C, NEXT_JE   = 0x0D;   // rel8
static const size_t OFF_CALL_ACT  = 0x18, NEXT_ACT  = 0x1C;   // rel32
static const size_t OFF_CALL_CHAP = 0x2B, NEXT_CHAP = 0x2F;   // rel32
static const size_t OFF_CALL_DIFF = 0x3E, NEXT_DIFF = 0x42;   // rel32
static const size_t OFF_JMP_TRUE  = 0x47, NEXT_JMP  = 0x48;   // rel8

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

static bool text_section(BYTE* base, BYTE** out, size_t* len) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".text", 5) == 0) {
            *out = base + s->VirtualAddress;
            *len = s->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

#include "patch.inc"

static void apply_patch() {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    BYTE* text; size_t len;
    if (!text_section(base, &text, &len)) { logf_("[mewbutch] no .text"); return; }

    std::vector<BYTE*> hits;
    for (size_t i = 0; i + SIG_LEN < len; ++i) {
        size_t j = 0;
        for (; j < SIG_LEN; ++j)
            if (SIG[j] != 0xFFFF && text[i + j] != (BYTE)SIG[j]) break;
        if (j == SIG_LEN) hits.push_back(text + i);
        if (hits.size() > 1) break;
    }
    if (hits.size() != 1) {
        logf_("[mewbutch] %zu signature matches - not patching (need exactly 1)", hits.size());
        return;
    }
    BYTE* blk = hits[0];

    // Recover branch and call targets from the code as found.
    BYTE* fail = blk + NEXT_JE   + (signed char)blk[OFF_JE_FAIL];
    BYTE* acc  = blk + NEXT_JMP  + (signed char)blk[OFF_JMP_TRUE];
    LONG r;
    memcpy(&r, blk + OFF_CALL_ACT,  4); BYTE* get_act  = blk + NEXT_ACT  + r;
    memcpy(&r, blk + OFF_CALL_CHAP, 4); BYTE* get_chap = blk + NEXT_CHAP + r;
    memcpy(&r, blk + OFF_CALL_DIFF, 4); BYTE* get_diff = blk + NEXT_DIFF + r;

    BYTE built[128];
    if (!build_patch(blk, fail, acc, get_act, get_chap, get_diff, built, SIG_LEN)) {
        logf_("[mewbutch] assembled block too large - refusing"); return;
    }
    DWORD old;
    if (!VirtualProtect(blk, SIG_LEN, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("[mewbutch] VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memcpy(blk, built, SIG_LEN);
    VirtualProtect(blk, SIG_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), blk, SIG_LEN);

    char hex[SIG_LEN * 2 + 1];
    for (size_t i = 0; i < SIG_LEN; ++i) sprintf(hex + i * 2, "%02x", blk[i]);
    logf_("[mewbutch] patched Butch donation test at +0x%llX (%zu bytes)",
          (unsigned long long)(blk - base), SIG_LEN);
    logf_("[mewbutch] wrote: %s", hex);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        apply_patch();
    }
    return TRUE;
}

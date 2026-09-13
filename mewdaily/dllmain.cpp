// mewdaily - Baby Jack's shop restocks daily instead of weekly
//
// Mewgenics refreshes a house shop when EITHER the stock vector size no longer
// matches (shop_level + 2), OR the in-game weekday equals the shop's refresh day:
//
//     mov  eax,[rdi+0x274]   ; shop level
//     add  eax, 2            ; expected item count
//     cmp  rax, rcx          ; != current stock.size() ?
//     jne  refresh
//     call <civil_from_days> ; calendar
//     cmp  [rax+0xc], esi    ; weekday == refresh day ?
//     jne  skip              ; <-- we NOP these 6 bytes
//     refresh:
//
// Neutralising that second jne makes the refresh run every day, leaving shop
// level (and therefore the save) completely untouched.
//
// Ships as a mewjector mod: drop mewdaily.dll into Mewgenics/mods/ and the
// chainloader loads it after the loader lock is released. No proxying here -
// mewjector owns version.dll and handles that for every mod.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "patches.inc"

// ------------------------------------------------------------------ logging --

static void logf_(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) return;
    char* slash = strrchr(path, (char)92);
    if (!slash) return;
    lstrcpyA(slash + 1, "mewdaily.log");
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

// ------------------------------------------------------------------ applying --

static void apply_patches() {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    BYTE* text; size_t len;
    if (!text_section(base, &text, &len)) { logf_("mewdaily: no .text section"); return; }

    // Locate everything before writing anything, so a table that only partly
    // matches leaves the game alone instead of half-patched.
    BYTE* at[COUNT(PATCHES)];
    for (size_t i = 0; i < COUNT(PATCHES); ++i) {
        at[i] = locate(PATCHES[i], base, text, len);
        if (at[i] == AMBIGUOUS) {
            logf_("mewdaily: \"%s\" matches more than once - patching nothing",
                  PATCHES[i].description);
            return;
        }
        if (!at[i]) {
            logf_("mewdaily: \"%s\" not found - patching nothing",
                  PATCHES[i].description);
            return;
        }
        if (at[i] != base + PATCHES[i].rva)
            logf_("mewdaily: \"%s\" moved to +%#llx", PATCHES[i].description,
                  (unsigned long long)(at[i] - base));
    }

    for (size_t i = 0; i < COUNT(PATCHES); ++i) {
        const BytePatch& p = PATCHES[i];
        BYTE* dst = at[i] + p.write_at;
        DWORD old;
        if (!VirtualProtect(dst, p.write_len, PAGE_EXECUTE_READWRITE, &old)) {
            logf_("mewdaily: VirtualProtect failed (%lu)", GetLastError());
            return;
        }
        memcpy(dst, p.write, p.write_len);
        VirtualProtect(dst, p.write_len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, p.write_len);
        logf_("mewdaily: +%#llx  %s", (unsigned long long)(dst - base), p.description);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        apply_patches();
    }
    return TRUE;
}

// mewdaily - Mewgenics daily shop refresh
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
#include <vector>

// ------------------------------------------------------------- logging ----
static void logf(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
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

// ----------------------------------------------------------- signature ----
// 0xFFFF marks a wildcard byte.
static const int SIG[] = {
    0x8B,0x87,0x74,0x02,0x00,0x00,        // mov  eax,[rdi+0x274]   shop level
    0x83,0xC0,0x02,                       // add  eax, 2
    0x48,0x98,                            // cdqe
    0x48,0x3B,0xC1,                       // cmp  rax, rcx
    0x75,0xFFFF,                          // jne  refresh
    0x45,0x33,0xC0,                       // xor  r8d, r8d
    0x48,0x8D,0x55,0xFFFF,                // lea  rdx,[rbp-0x69]
    0x48,0x8B,0x1D,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
    0x48,0x8B,0xCB,                       // mov  rcx, rbx
    0xE8,0xFFFF,0xFFFF,0xFFFF,0xFFFF,     // call <calendar>
    0x39,0x70,0x0C,                       // cmp  [rax+0xc], esi   weekday
    0x0F,0x85                             // jne  skip  <-- patch site
};
static const size_t SIG_LEN   = sizeof(SIG) / sizeof(SIG[0]);
static const size_t GATE_OFF  = 41;   // bytes from match start to the 0F 85
static const size_t GATE_SIZE = 6;    // length of the near jne

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

static void apply_patch() {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    BYTE* text; size_t len;
    if (!text_section(base, &text, &len)) { logf("[mewdaily] no .text section"); return; }

    std::vector<BYTE*> hits;
    for (size_t i = 0; i + SIG_LEN < len; ++i) {
        size_t j = 0;
        for (; j < SIG_LEN; ++j)
            if (SIG[j] != 0xFFFF && text[i + j] != (BYTE)SIG[j]) break;
        if (j == SIG_LEN) hits.push_back(text + i);
        if (hits.size() > 1) break;               // ambiguous: stop early
    }

    // Refuse to write unless the match is unique. Zero means the game updated
    // and the pattern must be re-derived; more than one means it is too loose.
    if (hits.size() != 1) {
        logf("[mewdaily] %zu signature matches - not patching (need exactly 1)", hits.size());
        return;
    }

    BYTE* gate = hits[0] + GATE_OFF;
    if (gate[0] != 0x0F || gate[1] != 0x85) {
        logf("[mewdaily] gate bytes unexpected: %02X %02X - not patching", gate[0], gate[1]);
        return;
    }

    DWORD old;
    if (!VirtualProtect(gate, GATE_SIZE, PAGE_EXECUTE_READWRITE, &old)) {
        logf("[mewdaily] VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memset(gate, 0x90, GATE_SIZE);
    VirtualProtect(gate, GATE_SIZE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), gate, GATE_SIZE);
    logf("[mewdaily] patched weekday gate at +0x%llX - shop refreshes daily",
         (unsigned long long)(gate - base));
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        apply_patch();
    }
    return TRUE;
}

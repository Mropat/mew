// Shared detour plumbing for the mods that hook game functions rather than
// overwrite instructions in place.
//
// A mod pins each call target by RVA plus the first SITE_SIGLEN bytes found
// there (see tools/make_sig.py, which generates each mod's sites.inc). One
// mismatch aborts the whole install, so a rebuilt game is left alone rather
// than hooked at a stale address.

#include <windows.h>
#include <cstring>

struct Site {
    const char*   name;
    unsigned      rva;
    unsigned char sig[24];
};

// Returns -1 when every site matches, otherwise the index of the first that
// does not. `out` is filled with the resolved addresses either way.
static int verify_sites(const Site* sites, int count, size_t siglen,
                        unsigned char* base, unsigned char** out) {
    int bad = -1;
    for (int i = 0; i < count; i++) {
        out[i] = base + sites[i].rva;
        if (bad < 0 && memcmp(out[i], sites[i].sig, siglen) != 0) bad = i;
    }
    return bad;
}

// jmp qword [rip+0] ; <8-byte target>
//
// Fourteen bytes, and it clobbers no register. That matters: the obvious
// "mov rax, imm64 ; jmp rax" is two bytes shorter but destroys RAX, and a
// function whose prologue opens with "mov rax, rsp" still needs that value
// after the bytes the trampoline stole. Getting this wrong faults immediately
// and a long way from the cause.
#define JMP_LEN 14

static void write_jmp(unsigned char* at, const void* dest) {
    at[0] = 0xFF; at[1] = 0x25;
    at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &dest, sizeof(dest));
}

// Moves `stolen` bytes of prologue into a trampoline and points the site at
// `detour`. Returns the trampoline - call it to run the original - or null.
//
// `stolen` must land on an instruction boundary, be at least JMP_LEN, and be
// covered by the site's verified signature, so every byte overwritten is a
// byte that was checked first.
static void* install_detour(unsigned char* site, size_t stolen, const void* detour) {
    if (stolen < JMP_LEN) return 0;

    unsigned char* tramp = (unsigned char*)VirtualAlloc(
        NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;

    memcpy(tramp, site, stolen);
    write_jmp(tramp + stolen, site + stolen);

    DWORD old;
    if (!VirtualProtect(site, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    write_jmp(site, detour);
    for (size_t i = JMP_LEN; i < stolen; i++) site[i] = 0x90;
    VirtualProtect(site, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, stolen);
    return tramp;
}

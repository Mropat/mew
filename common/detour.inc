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

// A mod that requires Mewjector never patches anything itself, and compiling
// this in anyway costs it the VirtualAlloc / VirtualProtect / VirtualFree /
// FlushInstructionCache imports - which together are the textbook signature of
// code injection, and exactly what a machine-learning scanner scores. Define
// MOD_REQUIRES_MEWJECTOR to leave it out; struct Site is still available.
#ifndef MOD_REQUIRES_MEWJECTOR

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

    // Allocate writable, then make it executable - never both at once.
    //
    // A page that is simultaneously writable and executable is one of the
    // loudest signals a heuristic scanner looks for, and PAGE_EXECUTE_READWRITE
    // is how it is usually spelled. Writing the trampoline first and flipping
    // to PAGE_EXECUTE_READ afterwards is the same result without ever holding
    // that combination. It is also just better hygiene: nothing needs to write
    // to a trampoline once it has been built.
    unsigned char* tramp = (unsigned char*)VirtualAlloc(
        NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tramp) return 0;

    memcpy(tramp, site, stolen);
    write_jmp(tramp + stolen, site + stolen);

    DWORD prev;
    if (!VirtualProtect(tramp, 128, PAGE_EXECUTE_READ, &prev)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }

    // Likewise for the patch site: the game's code has to be made writable to
    // patch it, but it does not have to be writable *and* executable while we
    // do. PAGE_READWRITE for the write, then its original protection back.
    DWORD old;
    if (!VirtualProtect(site, stolen, PAGE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    write_jmp(site, detour);
    for (size_t i = JMP_LEN; i < stolen; i++) site[i] = 0x90;
    VirtualProtect(site, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, stolen);
    return tramp;
}

#endif   // MOD_REQUIRES_MEWJECTOR

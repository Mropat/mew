// Checks that the patch registry finds what it is supposed to find, and - more
// importantly - refuses to find anything when it should not.
//
// The whole reason mewdaily does not just write to a remembered address is that
// a remembered address is only correct until the game is rebuilt. These tests
// pin that behaviour: pattern beats address, a changed region is not patched,
// and an ambiguous one is not patched either.
//
//   clang++ -O2 -o test_locate.exe test_locate.cpp && ./test_locate.exe
//
// Optionally pass a real Mewgenics.exe to also check the table against it:
//   ./test_locate.exe "C:/.../Mewgenics.exe"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "patches.inc"

static int g_fails;
static void check(bool ok, const char* what) {
    printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fails++;
}

// A fake .text big enough that the registry's remembered RVA falls inside it,
// so `locate` can be exercised exactly as it runs in the game.
struct Fake {
    BYTE*  buf;
    size_t n;
    Fake(size_t want) : n(want) { buf = (BYTE*)malloc(n); }
    ~Fake() { free(buf); }
    BYTE*  text() { return buf; }
    size_t len()  { return n; }
    void fill(BYTE b) { memset(buf, b, n); }
    void plant(size_t at, const int* pat, size_t plen) {
        for (size_t i = 0; i < plen; ++i)
            buf[at + i] = (BYTE)(pat[i] == 0xFFFF ? 0xAB : pat[i]);
    }
};

int main(int argc, char** argv) {
    const BytePatch& p = PATCHES[0];
    Fake f(p.rva + p.expect_len + 0x400);
    if (!f.buf) { puts("out of memory"); return 1; }

    puts("finding the region");
    f.fill(0xCC);
    f.plant(p.rva, p.expect, p.expect_len);           // exactly where remembered
    check(locate(p, f.text(), f.text(), f.len()) == f.text() + p.rva,
          "found at the remembered address");

    f.fill(0xCC);
    f.plant(p.rva - 0x100, p.expect, p.expect_len);   // moved by a rebuild
    check(locate(p, f.text(), f.text(), f.len()) == f.text() + p.rva - 0x100,
          "found by pattern after the code moved");

    puts("\nrefusing to guess");
    f.fill(0xCC);
    check(locate(p, f.text(), f.text(), f.len()) == 0,
          "region gone entirely -> not found, nothing written");

    f.fill(0xCC);
    f.plant(p.rva, p.expect, p.expect_len);
    size_t last_fixed = 0;                            // last byte we actually check
    for (size_t i = 0; i < p.expect_len; ++i)
        if (p.expect[i] != 0xFFFF) last_fixed = i;
    f.buf[p.rva + last_fixed] ^= 0xFF;                // the jne became something else
    check(locate(p, f.text(), f.text(), f.len()) == 0,
          "one checked byte of the region changed -> not found");

    f.fill(0xCC);
    f.plant(p.rva, p.expect, p.expect_len);
    f.plant(p.rva - 0x200, p.expect, p.expect_len);
    check(locate(p, f.text(), f.text(), f.len()) == f.text() + p.rva,
          "two copies but one is where we remembered -> take that one");

    f.fill(0xCC);
    f.plant(0x100, p.expect, p.expect_len);
    f.plant(0x300, p.expect, p.expect_len);
    check(locate(p, f.text(), f.text(), f.len()) == AMBIGUOUS,
          "two copies and neither is remembered -> ambiguous, patch nothing");

    puts("\nwildcards");
    f.fill(0xCC);
    f.plant(p.rva, p.expect, p.expect_len);
    int wildcards = 0;
    for (size_t i = 0; i < p.expect_len; ++i)
        if (p.expect[i] == 0xFFFF) { f.buf[p.rva + i] = 0x5A; wildcards++; }
    check(wildcards > 0, "the pattern has wildcards to test");
    check(locate(p, f.text(), f.text(), f.len()) == f.text() + p.rva,
          "operands may differ without breaking the match");

    puts("\nthe write stays inside the region it matched");
    check(p.write_at + p.write_len <= p.expect_len,
          "write window is covered by the verified bytes");

    // Optional: check the table against a real game binary.
    if (argc > 1) {
        printf("\nagainst %s\n", argv[1]);
        FILE* fh = fopen(argv[1], "rb");
        if (!fh) { puts("  could not open it"); return g_fails != 0; }
        fseek(fh, 0, SEEK_END); long n = ftell(fh); fseek(fh, 0, SEEK_SET);
        BYTE* d = (BYTE*)malloc(n);
        if (fread(d, 1, n, fh) != (size_t)n) { puts("  short read"); return 1; }
        fclose(fh);

        // Walk the section table by hand; the file is not mapped.
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)d;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(d + dos->e_lfanew);
        IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        int hits = 0; DWORD found_rva = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
            if (memcmp(s->Name, ".text", 5) != 0) continue;
            BYTE* t = d + s->PointerToRawData;
            for (size_t k = 0; k + p.expect_len <= s->SizeOfRawData; ++k)
                if (matches(t + k, p.expect, p.expect_len)) {
                    hits++; found_rva = (DWORD)(s->VirtualAddress + k);
                }
        }
        printf("  %d match(es); region at +%#lx, patch site +%#lx\n",
               hits, (unsigned long)found_rva,
               (unsigned long)(found_rva + p.write_at));
        check(hits == 1, "exactly one match in the shipped binary");
        check(found_rva == p.rva, "and it is where the registry remembers it");
        free(d);
    }

    printf("\n%s\n", g_fails ? "FAILED" : "all checks passed");
    return g_fails != 0;
}

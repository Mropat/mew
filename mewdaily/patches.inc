// The registry: what this mod does to the game, and how to find the places it
// does it. Shared by the mod and by test_locate.cpp so the tested table and the
// shipped table cannot diverge.

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// A patch is a region we must recognise before touching, plus the bytes to
// write once we have found it.
//
// `rva` is where the region lived in 1.1.b21239. It is a hint, not an address
// we trust: the bytes are checked there first, and if they do not match, the
// region is searched for by pattern. Writing to a remembered address without
// checking what is at it is the one thing this must never do - after a game
// update those bytes land in the middle of whatever moved into that offset, and
// the failure shows up somewhere else entirely, long after the write.
struct BytePatch {
    const char*          description;
    unsigned             rva;         // where it was in 1.1.b21239
    const int*           expect;      // 0xFFFF marks a wildcard byte
    size_t               expect_len;
    size_t               write_at;    // offset from the start of the region
    const unsigned char* write;
    size_t               write_len;
};

// Wildcards cover operands that legitimately move: branch displacements, the
// rip-relative global load and the call target.
static const int WEEKDAY_GATE[] = {
    0x8B,0x87,0x74,0x02,0x00,0x00,          // mov  eax,[rdi+0x274]   shop level
    0x83,0xC0,0x02,                         // add  eax, 2
    0x48,0x98,                              // cdqe
    0x48,0x3B,0xC1,                         // cmp  rax, rcx
    0x75,0xFFFF,                            // jne  refresh
    0x45,0x33,0xC0,                         // xor  r8d, r8d
    0x48,0x8D,0x55,0xFFFF,                  // lea  rdx,[rbp-0x69]
    0x48,0x8B,0x1D,0xFFFF,0xFFFF,0xFFFF,0xFFFF,
    0x48,0x8B,0xCB,                         // mov  rcx, rbx
    0xE8,0xFFFF,0xFFFF,0xFFFF,0xFFFF,       // call <civil_from_days>
    0x39,0x70,0x0C,                         // cmp  [rax+0xc], esi   weekday
    0x0F,0x85,0xFFFF,0xFFFF,0xFFFF,0xFFFF   // jne  skip             <-- patched
};
// The displacement is wildcarded rather than left out: the six bytes we
// overwrite must all be bytes we checked first.
static const unsigned char NOP6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

// The registry. One line per thing this mod does to the game.
static const BytePatch PATCHES[] = {
    { "restock the house shop every day, not just on its refresh weekday",
      0x1C4F07 - 41, WEEKDAY_GATE, COUNT(WEEKDAY_GATE),
      41, NOP6, COUNT(NOP6) },
};

static bool matches(const BYTE* at, const int* pat, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (pat[i] != 0xFFFF && at[i] != (BYTE)pat[i]) return false;
    return true;
}

#define AMBIGUOUS ((BYTE*)-1)

// The remembered address first, then a scan. A scan that finds the region more
// than once means the pattern is too loose to act on.
static BYTE* locate(const BytePatch& p, BYTE* base, BYTE* text, size_t len) {
    BYTE* hint = base + p.rva;
    if (hint >= text && hint + p.expect_len <= text + len &&
        matches(hint, p.expect, p.expect_len))
        return hint;

    BYTE* found = 0;
    for (size_t i = 0; i + p.expect_len <= len; ++i) {
        if (!matches(text + i, p.expect, p.expect_len)) continue;
        if (found) return AMBIGUOUS;
        found = text + i;
    }
    return found;
}


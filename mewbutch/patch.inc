// Shared by the mod and by test_patch.cpp so the tested bytes and the shipped
// bytes cannot diverge.
struct Asm {
    BYTE  buf[128];
    size_t n;
    BYTE* base;
    Asm(BYTE* at) : n(0), base(at) {}
    void raw(const char* bytes, size_t c) { memcpy(buf + n, bytes, c); n += c; }
    void jcc(BYTE op, BYTE* target) {
        buf[n++] = op;
        buf[n] = (BYTE)(LONG_PTR)(target - (base + n + 1)); ++n;
    }
    void call(BYTE* target) {
        buf[n++] = 0xE8;
        LONG rel = (LONG)(LONG_PTR)(target - (base + n + 4));
        memcpy(buf + n, &rel, 4); n += 4;
    }
};

// Acts are the adventures (3 of them, 4 chapters each); difficulty is a third
// axis over the whole grid. Butch walks act/chapter upward to A3C4, then holds
// there and escalates difficulty.
//
// Rule: one difficulty step above what he asks for excuses a LOWER ACT, but
// never a lower chapter. So "A3 C4 hard" also takes "A1/A2 C4 crazy".
//
//   not a veteran           -> reject
//   chapter < required      -> reject      (always enforced)
//   difficulty < required   -> reject
//   difficulty > required   -> accept, act not checked
//   difficulty >= impossible-> accept, act not checked  (nothing higher exists
//                                       without limit-break gear)
//   otherwise               -> act enforced as vanilla
static size_t build_patch(BYTE* blk, BYTE* fail, BYTE* acc,
                          BYTE* get_act, BYTE* get_chap, BYTE* get_diff,
                          BYTE* out, size_t cap) {
    Asm a(blk);
    a.raw("\x48\x8D\xBF\x00\x0C\x00\x00", 7);  // lea   rdi,[rdi+0xc00]
    a.raw("\xF6\x47\xF8\x02", 4);              // test  byte [rdi-8], 2   veteran
    a.jcc(0x74, fail);
    a.raw("\x0F\xBE\x5F\x09", 4);              // movsx ebx, byte [rdi+9]    chapter
    a.raw("\x48\x8B\xCE", 3); a.call(get_chap);
    a.raw("\x3B\xD8", 2); a.jcc(0x7C, fail);   // chapter is never excused
    a.raw("\x0F\xBE\x5F\x0A", 4);              // movsx ebx, byte [rdi+0xa]  difficulty
    a.raw("\x48\x8B\xCE", 3); a.call(get_diff);
    a.raw("\x3B\xD8", 2);
    a.jcc(0x7C, fail);                         // jl  reject
    a.jcc(0x7F, acc);                          // jg  accept - lower act excused
    a.raw("\x83\xFB\x03", 3);                  // cmp ebx, 3
    a.jcc(0x7D, acc);                          // jge accept - clamp at impossible
    a.raw("\x0F\xBE\x5F\x08", 4);              // movsx ebx, byte [rdi+8]    act
    a.raw("\x48\x8B\xCE", 3); a.call(get_act);
    a.raw("\x3B\xD8", 2); a.jcc(0x7C, fail);
    a.jcc(0xEB, acc);
    if (a.n > cap) return 0;
    while (a.n < cap) a.buf[a.n++] = 0x90;
    memcpy(out, a.buf, cap);
    return cap;
}

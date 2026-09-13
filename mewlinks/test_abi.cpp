// Exercises the hand-rolled MSVC layouts in msvcabi.inc the way the game does.
//
// register_button does not just invoke the callback. It copies the
// std::function into the Button, may move it, invokes it later, and finally
// destroys the caller's copy with a deallocate flag it computes itself:
//
//     rcx = fn[0x38]                 ; the impl pointer
//     cmp rcx, fn                    ; is the impl inline, or on the heap?
//     setne dl                       ; deallocate only if it is on the heap
//     call [[rcx]+0x20]              ; _Delete_this(impl, dl)
//
// This replays that sequence against the real vtable and checks the callback
// arrives with the arguments it was built with.
//
//   clang++ -O2 -o test_abi.exe test_abi.cpp && ./test_abi.exe

#include <cstddef>
#include <cstdio>
#include <cstring>

static void* g_seen_comp;
static unsigned long long g_seen_which;
static int g_calls;

#include "msvcabi.inc"

// The stub msvcabi.inc forward-declares; stands in for the real click handler.
static void link_click(void* comp, unsigned long long which) {
    g_seen_comp  = comp;
    g_seen_which = which;
    g_calls++;
}

static int g_fails;
static void check(bool ok, const char* what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fails++;
}

int main() {
    check(sizeof(MsvcString) == 0x20,   "sizeof(std::string) == 0x20");
    check(sizeof(MsvcWString) == 0x20,  "sizeof(std::wstring) == 0x20");
    check(sizeof(MsvcFunction) == 0x40, "sizeof(std::function) == 0x40");
    check(offsetof(MsvcFunction, impl) == 0x38, "_Getimpl() pointer at +0x38");
    check(offsetof(MsvcString, size) == 0x10 &&
          offsetof(MsvcString, cap)  == 0x18, "string size/capacity at +0x10/+0x18");

    // ---- std::string, as register_button will read it -----------------------
    MsvcString s;
    check(str_init(&s, "lover"), "str_init accepts a short clip name");
    check(s.size == 5 && s.cap == 15 && memcmp(s.buf, "lover", 6) == 0,
          "string holds 'lover' inline, capacity 15");
    check(!str_init(&s, "0123456789abcdef"), "str_init refuses a name that needs a heap buffer");

    MsvcWString w;
    wstr_init_empty(&w);
    check(w.size == 0 && w.cap == 7 && w.buf[0] == 0, "wstring is empty with capacity 7");

    // ---- std::function ------------------------------------------------------
    void* const comp = (void*)0xDEADBEEF;
    MsvcFunction fn;
    fn_init(&fn, comp, 1);
    check((void*)fn.impl == (void*)fn.storage, "impl is stored inline, as the game's lambdas are");

    // The Button copy-constructs its own std::function from ours. MSVC does
    // that as: dest._Set(src._Getimpl()->_Copy(&dest._Mystorage)).
    MsvcFunction held;
    memset(&held, 0, sizeof(held));
    held.impl = fn.impl->vt->copy(fn.impl, held.storage);
    check((void*)held.impl == (void*)held.storage, "_Copy returns the destination storage");
    check(held.impl->comp == comp && held.impl->which == 1, "_Copy carries the captures across");

    // The caller's copy is then destroyed. Inline impl means dealloc is false.
    bool dealloc = ((void*)fn.impl != (void*)&fn);
    check(!dealloc, "destroying our copy does not try to free it");
    fn.impl->vt->delete_this(fn.impl, dealloc);

    // Clicking the button invokes the copy the Button kept.
    held.impl->vt->do_call(held.impl);
    check(g_calls == 1 && g_seen_comp == comp && g_seen_which == 1,
          "_Do_call reaches link_click with the right captures");

    // A second button, built for the other icon, must stay independent.
    MsvcFunction fn0;
    fn_init(&fn0, comp, 0);
    fn0.impl->vt->do_call(fn0.impl);
    check(g_calls == 2 && g_seen_which == 0, "the lover and rival callbacks do not alias");

    printf("\n%s\n", g_fails ? "FAILED" : "all checks passed");
    return g_fails != 0;
}

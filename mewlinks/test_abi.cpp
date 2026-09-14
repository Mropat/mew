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
static unsigned long long g_seen_kind;
static int g_calls;

#include "../common/hookapi.inc"
#include "sites.inc"
#include "../common/msvcabi.inc"

// msvcabi.inc forward-declares this; here it stands in for the real handler.
static void on_button_click(void* a, unsigned long long b, unsigned long long kind) {
    g_seen_comp  = a;
    g_seen_which = b;
    g_seen_kind  = kind;
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
    void* const comp = (void*)(size_t)0xDEADBEEF;   // size_t: MSVC warns C4312 otherwise
    MsvcFunction fn;
    fn_init(&fn, comp, 1, 0);
    check((void*)fn.impl == (void*)fn.storage, "impl is stored inline, as the game's lambdas are");

    // The Button copy-constructs its own std::function from ours. MSVC does
    // that as: dest._Set(src._Getimpl()->_Copy(&dest._Mystorage)).
    MsvcFunction held;
    memset(&held, 0, sizeof(held));
    held.impl = fn.impl->vt->copy(fn.impl, held.storage);
    check((void*)held.impl == (void*)held.storage, "_Copy returns the destination storage");
    check(held.impl->a == comp && held.impl->b == 1 && held.impl->kind == 0,
          "_Copy carries the captures across");

    // The caller's copy is then destroyed. Inline impl means dealloc is false.
    bool dealloc = ((void*)fn.impl != (void*)&fn);
    check(!dealloc, "destroying our copy does not try to free it");
    fn.impl->vt->delete_this(fn.impl, dealloc);

    // Clicking the button invokes the copy the Button kept.
    held.impl->vt->do_call(held.impl);
    check(g_calls == 1 && g_seen_comp == comp && g_seen_which == 1 && g_seen_kind == 0,
          "_Do_call reaches the handler with the right captures");

    // A button of a different kind - a family tree portrait - must not alias it.
    MsvcFunction fn0;
    fn_init(&fn0, 0, 7, 1);
    fn0.impl->vt->do_call(fn0.impl);
    check(g_calls == 2 && g_seen_which == 7 && g_seen_kind == 1,
          "panel-icon and tree-portrait callbacks do not alias");

    // ---- the build check --------------------------------------------------
    // Once other mods have hooked every site, this is the only thing still able
    // to tell us the game was rebuilt - so it has to reject as well as accept.
    puts("\nbuild identity");
    {
        static unsigned char img[0x400];
        memset(img, 0, sizeof(img));
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(img + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.TimeDateStamp   = GAME_TIMESTAMP;
        nt->OptionalHeader.SizeOfImage = GAME_SIZEOFIMAGE;

        check(verify_build(img, GAME_TIMESTAMP, GAME_SIZEOFIMAGE),
              "accepts the build the RVAs came from");

        nt->FileHeader.TimeDateStamp = GAME_TIMESTAMP + 1;
        check(!verify_build(img, GAME_TIMESTAMP, GAME_SIZEOFIMAGE),
              "rejects a rebuilt game (timestamp moved)");
        nt->FileHeader.TimeDateStamp = GAME_TIMESTAMP;

        nt->OptionalHeader.SizeOfImage = GAME_SIZEOFIMAGE + 0x1000;
        check(!verify_build(img, GAME_TIMESTAMP, GAME_SIZEOFIMAGE),
              "rejects a differently sized image");
        nt->OptionalHeader.SizeOfImage = GAME_SIZEOFIMAGE;

        nt->Signature = 0;
        check(!verify_build(img, GAME_TIMESTAMP, GAME_SIZEOFIMAGE),
              "rejects a non-PE image");
        nt->Signature = IMAGE_NT_SIGNATURE;

        dos->e_magic = 0;
        check(!verify_build(img, GAME_TIMESTAMP, GAME_SIZEOFIMAGE), "rejects garbage");
        dos->e_magic = IMAGE_DOS_SIGNATURE;
    }

    // ---- what the build check is and is not allowed to gate ----------------
    // A vanilla rebuild that leaves our functions alone must still install: the
    // signature bytes can answer for themselves there, so a changed timestamp
    // is none of their business. Only a site we cannot read falls back to it.
    puts("\nan unrelated game update");
    {
        static unsigned char img[0x2000];
        memset(img, 0, sizeof(img));
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(img + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.TimeDateStamp   = GAME_TIMESTAMP + 99;   // rebuilt
        nt->OptionalHeader.SizeOfImage = GAME_SIZEOFIMAGE + 99;

        Site one = { "ONE", 0x1000, { 0 } };
        memcpy(one.sig, SITES[0].sig, SITE_SIGLEN);
        memcpy(img + 0x1000, SITES[0].sig, SITE_SIGLEN);        // code unmoved

        unsigned char* out[1];
        check(verify_sites_chained(&one, 1, SITE_SIGLEN, img, out,
                                   GAME_TIMESTAMP, GAME_SIZEOFIMAGE) == -1,
              "new build, our code unchanged -> still installs");

        img[0x1000] ^= 0xFF;                                    // code moved
        check(verify_sites_chained(&one, 1, SITE_SIGLEN, img, out,
                                   GAME_TIMESTAMP, GAME_SIZEOFIMAGE) == 0,
              "new build, our code changed -> refuses");
    }

    printf("\n%s\n", g_fails ? "FAILED" : "all checks passed");
    return g_fails != 0;
}

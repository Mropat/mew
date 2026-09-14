// Hand-rolled MSVC STL object layouts.
//
// register_button takes std::string, std::wstring and std::function<void()>
// *by value*, so the mod has to hand the game three real STL objects. Building
// them with a local <string>/<functional> would tie the mod to whichever STL
// the mod author happened to compile against; these structs instead mirror the
// layouts read straight out of Mewgenics.exe, so the mod is independent of the
// toolchain that builds it.
//
// Verified against build 1.1.b21239:
//   sizeof(std::string)  == 0x20   buffer 16, size at +0x10, capacity at +0x18
//   sizeof(std::wstring) == 0x20   buffer  8 wchars, same trailing fields
//   sizeof(std::function<void()>) == 0x40
//        storage 0x00..0x37, _Getimpl() pointer at +0x38
//   the callable stored in that storage is { vptr; captures... }
//
// register_button destroys all three arguments before it returns. Every object
// below is small enough to live in its own inline buffer, so each destructor is
// a no-op and nothing here is ever freed.

struct MsvcString {                 // 0x20 bytes
    char   buf[16];
    size_t size;
    size_t cap;
};

// Only ever called with clip names, which are far shorter than the 15 bytes
// that fit inline. Anything longer would need a heap buffer and a real
// destructor, so it is rejected instead.
static bool str_init(MsvcString* s, const char* lit) {
    size_t n = 0;
    while (lit[n]) n++;
    if (n > 15) return false;
    for (size_t i = 0; i < sizeof(*s); i++) ((char*)s)[i] = 0;
    for (size_t i = 0; i < n; i++) s->buf[i] = lit[i];
    s->size = n;
    s->cap  = 15;
    return true;
}

struct MsvcWString {                // 0x20 bytes
    unsigned short buf[8];
    size_t size;
    size_t cap;
};

// The game passes an empty label for every button it registers; so do we.
static void wstr_init_empty(MsvcWString* s) {
    for (size_t i = 0; i < sizeof(*s); i++) ((char*)s)[i] = 0;
    s->cap = 7;
}

// ---------------------------------------------------------------- function --

struct FnImpl;

// Slot order read back from the game's own callable vtables (0xedbff8 etc.):
//   0 _Copy   1 _Move   2 _Do_call   3 _Target_type   4 _Delete_this   5 _Get
struct FnVtbl {
    FnImpl*     (*copy)(FnImpl*, void* dest);
    FnImpl*     (*move)(FnImpl*, void* dest);
    void        (*do_call)(FnImpl*);
    const void* (*target_type)(FnImpl*);
    void        (*delete_this)(FnImpl*, bool dealloc);
    void*       (*get)(FnImpl*);
};

// The captures. One callable shape serves every button the mod registers;
// `kind` says which, and the meaning of `a` and `b` follows from it.
struct FnImpl {
    const FnVtbl*      vt;
    void*              a;
    unsigned long long b;
    unsigned long long kind;
};

struct MsvcFunction {               // 0x40 bytes
    unsigned char storage[0x38];
    FnImpl*       impl;
};

// Defined by the mod: what a click does.
static void on_button_click(void* a, unsigned long long b, unsigned long long kind);

static FnImpl* fn_copy(FnImpl* self, void* dest) {
    FnImpl* d = (FnImpl*)dest;
    *d = *self;
    return d;
}
static void fn_do_call(FnImpl* self) { on_button_click(self->a, self->b, self->kind); }
static const void* fn_target_type(FnImpl*)    { return 0; }
static void fn_delete_this(FnImpl*, bool)     { }    // always inline; never owns
static void* fn_get(FnImpl* self)             { return &self->a; }

static const FnVtbl FN_VTBL = {
    fn_copy, fn_copy, fn_do_call, fn_target_type, fn_delete_this, fn_get
};

static void fn_init(MsvcFunction* f, void* a, unsigned long long b,
                    unsigned long long kind) {
    for (size_t i = 0; i < sizeof(*f); i++) ((char*)f)[i] = 0;
    FnImpl* impl = (FnImpl*)f->storage;      // stored inline, like the game's
    impl->vt   = &FN_VTBL;
    impl->a    = a;
    impl->b    = b;
    impl->kind = kind;
    f->impl    = impl;
}

// The three layouts above are only correct if they come out this size; a
// compiler that padded them differently would corrupt the game's stack.
static_assert(sizeof(MsvcString)   == 0x20, "std::string must be 0x20 bytes");
static_assert(sizeof(MsvcWString)  == 0x20, "std::wstring must be 0x20 bytes");
static_assert(sizeof(MsvcFunction) == 0x40, "std::function must be 0x40 bytes");
static_assert(sizeof(FnVtbl)       == 6 * sizeof(void*), "vtable must have 6 slots");
static_assert(sizeof(FnImpl)       <= 0x38, "callable must fit the inline storage");

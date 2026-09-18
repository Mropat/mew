// A settings row of our own, in Audio Settings, under the last volume slider.
//
// The game builds that screen in code, not from data, and every row is a
// statically placed clip in swfs/ui.swf that MenuPanel::register_selector finds
// by name - so there is no list to append to and no spare row to adopt. What
// there is instead is an engine that can make another copy of any clip already
// on screen: a MovieClip remembers the tag list it was built from at +0xd0, and
// a text box remembers its DefineEditText at +0x98. Two calls apiece and the
// last volume row's three pieces have independent twins, which get a name, a
// position one row further down, and then the same register_selector the game
// uses for its own rows.
//
// The panel has the room. Audio Settings runs its five rows from y -232 to
// y -63.5 on a dead-regular 42.15 pitch, and the screen is as tall as the Game
// Settings one, which goes on to +269.
//
// Nothing here is reached unless all 17 sites matched, and every step is
// checked: a missing piece means no row, never a half-built one.

// ------------------------------------------------------------ engine layout --
// glaiel::swf::DisplayObject, from its constructor at 0x9b08b0.
#define OFF_DO_PARENT 0x38       // addChild refuses a child that already has one
#define OFF_DO_NAME   0x48       // const char*, "" by default, compared by name
#define OFF_DO_XFORM  0x60       // six floats, the last two of them translation
#define OFF_DO_TX     0x70
#define OFF_DO_TY     0x74
#define XFORM_BYTES   0x18

#define OFF_CLIP_TAGS 0xd0       // MovieClip      -> the tag list it plays

// A container's children, from addChild: capacity, count, then four slots
// inline or a pointer to the rest.
#define OFF_KIDS_CAP   0xa8
#define OFF_KIDS_COUNT 0xac
#define OFF_KIDS_DATA  0xb0
#define OFF_TEXT_DEF  0x98       // DynamicTextBox -> its DefineEditText
// A MenuPanel does not answer find_ui_child itself: register_selector looks its
// own key up through [panel+0x38], which is the component holding the rows.
#define OFF_PANEL_ROOT 0x38

// The row we copy, and the key of the one we register.
#define SRC_ROW      "housemeowvol"
#define PREV_ROW     "meowvol"
#define ROW_KEY      "bunga_int_limit"
#define ROW_LABEL    "Bunga Stupid Requirement"

typedef void* (*FindChildFn)(void* panel, void* name);
typedef void  (*AddChildFn)(void* parent, void* child, int clear_flag);
typedef void* (*PoolAllocFn)(void* pool);
typedef void* (*ClipCtorFn)(void* obj, void* tags);
typedef void* (*SetTextFn)(void* box, void* text, unsigned char plain, unsigned char commit);
typedef void* (*GameAllocFn)(unsigned long long n);
typedef void* (*RegSelFn)(void* panel, void* key, void* current,
                          void* options, int enabled, void* fn);
typedef void* (*NewInstanceFn)(void* definition);

static FindChildFn g_find_child;
static AddChildFn  g_add_child;
static PoolAllocFn g_pool_alloc;
static ClipCtorFn  g_clip_ctor;
static SetTextFn   g_set_text;
static GameAllocFn g_game_alloc;
static RegSelFn    g_next_regsel;

// Filled in at install time rather than written as an initialised array, so the
// slots can be named next to the functions that go in them.
static void* g_fn_vtbl[5];

// ------------------------------------------------------------ small strings --
// Both string types fit their contents inline below a threshold, which every
// literal here is under - so none of them owns memory and none of them cares
// whose allocator freed what.
struct alignas(16) GameWString {
    wchar_t            buf[8];
    unsigned long long size;
    unsigned long long cap;
};
static_assert(sizeof(GameWString) == 32, "std::wstring is 0x20 bytes");

struct SelOption { GameString value; GameWString label; };
static_assert(sizeof(SelOption) == 64, "SelectorWidget::Option is 0x40 bytes");

struct OptVec { const void* begin; const void* end; const void* cap; };

static void sso_narrow(GameString* s, const char* lit) {
    const unsigned long long n = strlen(lit);
    memset(s, 0, sizeof(*s));
    memcpy(s->buf, lit, (size_t)n);
    s->size = n;
    s->cap  = 15;
}

static void sso_wide(GameWString* s, const wchar_t* lit) {
    const unsigned long long n = wcslen(lit);
    memset(s, 0, sizeof(*s));
    memcpy(s->buf, lit, (size_t)n * sizeof(wchar_t));
    s->size = n;
    s->cap  = 7;
}

// find_ui_child takes its name by value and destroys it, so it gets one built
// through the game's own allocator - the names here are longer than a
// std::string holds inline.
static void* child_of(void* panel, const char* name) {
    void* root = panel ? *(void**)((char*)panel + OFF_PANEL_ROOT) : 0;
    GameString s;
    if (!g_find_child || !root) return 0;
    make_game_string(&s, name, strlen(name));
    return g_find_child(root, &s);
}

// -------------------------------------------------------------- the callback --
// MSVC's std::function is 64 bytes with the callable's pointer at +0x38, and
// counts as "local" - never freed, moved rather than stolen - when that pointer
// is the storage itself. register_selector's own teardown is what says so:
//
//     mov rcx,[rbx+0x38] / cmp rcx,rbx / call [r8+0x20]
//
// which is _Tidy(): the impl pointer, the local test, and _Delete_this in slot
// 4. Slot 2 is _Do_call, and the game's own combat_speed lambda at 0x29c3f0
// takes (this, std::string&&) and does nothing but write the setting.
struct alignas(16) GameFunction {
    const void* vptr;
    void*       pad[6];
    void*       self;
};
static_assert(sizeof(GameFunction) == 64, "std::function is 0x40 bytes");

static char g_fn_dummy_type[16];
static GameFunction g_fn;

static void* fn_copy(void* self, void* where) {
    (void)self;
    *(const void**)where = g_fn_vtbl;        // stateless: the vptr is the whole of it
    return where;
}
static void* fn_move(void* self, void* where) { return fn_copy(self, where); }
static const void* fn_target(void* self) { (void)self; return g_fn_dummy_type; }
static void fn_delete(void* self, bool deallocate) { (void)self; (void)deallocate; }

// The chosen option's value string, as register_selector was given it.
static void fn_call(void* self, void* value) {
    const char* v = str_of(value);
    long n = 0;
    (void)self;

    if (v) {
        while (*v == ' ') v++;
        for (; *v >= '0' && *v <= '9'; v++) n = n * 10 + (*v - '0');
    }
    if (n < INT_LIMIT_DEFAULT) n = INT_LIMIT_DEFAULT;
    if (n > INT_LIMIT_MAX)     n = INT_LIMIT_MAX;

    InterlockedExchange(&g_int_limit, n);
    say("settings row: int limit -> %ld", n);

    if (g_ini[0]) {
        wchar_t text[2];
        text[0] = (wchar_t)(L'0' + n);
        text[1] = 0;
        if (!WritePrivateProfileStringW(L"LegendOfBunga", L"IntLimit", text, g_ini)) {
            // Keep what was picked for this session rather than let the next
            // level load read the stale file back over it.
            InterlockedExchange(&g_ini_unwritable, 1);
            say("settings row: could not write %S - keeping it in memory only", g_ini);
        }
    }
}

// --------------------------------------------------------------- the clones --
// The pool a fresh MovieClip comes from is named by the lea at +0x11 of
// DefineSprite's instantiate slot, which is inside the 24 bytes this mod pins -
// so a matching signature vouches for that address the same way it vouches for
// the code.
static void* movieclip_pool(void) {
    const unsigned char* lea = g_at[S_SPRITE_NEW] + 0x11;
    return (void*)(lea + 7 + *(const int*)(lea + 3));
}

static void* clone_clip(void* src) {
    void* tags = *(void**)((char*)src + OFF_CLIP_TAGS);
    void* o;
    if (!tags) return 0;
    o = g_pool_alloc(movieclip_pool());
    return o ? g_clip_ctor(o, tags) : 0;
}

// A text box knows its own definition, and a definition knows how to make one -
// so this one needs no pool and no constructor of its own.
static void* clone_text(void* src) {
    void* def = *(void**)((char*)src + OFF_TEXT_DEF);
    void* const* vt;
    if (!def) return 0;
    vt = *(void* const**)def;
    return ((NewInstanceFn)vt[3])(def);
}

// Which of the two translation floats runs down the screen, and how far apart
// the rows sit on it - both measured from two rows that are already there, so
// neither the axis nor the unit has to be assumed.
static unsigned g_axis = OFF_DO_TY;

static float row_pitch(const void* last, const void* prev) {
    const float dx = *(const float*)((const char*)last + OFF_DO_TX)
                   - *(const float*)((const char*)prev + OFF_DO_TX);
    const float dy = *(const float*)((const char*)last + OFF_DO_TY)
                   - *(const float*)((const char*)prev + OFF_DO_TY);
    const float ax = dx < 0 ? -dx : dx;
    const float ay = dy < 0 ? -dy : dy;
    g_axis = ay >= ax ? OFF_DO_TY : OFF_DO_TX;
    return ay >= ax ? dy : dx;
}

// Same place as the piece it was copied from, one row on, under a name of its
// own. The literal outlives the clip, which is what the name field wants.
static void place(void* obj, void* src, float pitch, const char* name) {
    memcpy((char*)obj + OFF_DO_XFORM, (const char*)src + OFF_DO_XFORM, XFORM_BYTES);
    *(float*)((char*)obj + g_axis) += pitch;
    *(const char**)((char*)obj + OFF_DO_NAME) = name;
}

// The band behind every other row is a clip of its own and carries no name, so
// it is found by shape instead: of everything in the panel it is the only thing
// scaled wider than 1, the row pieces all sitting at 1.0 or 0.6. The one
// furthest down is the last one, and the next belongs two rows on - which is
// also why ours is added before the rest of the row and not after. A child goes
// on the end of the list, and the list is the draw order.
#define STRIPE_MIN_SCALE 1.5f

static void* last_stripe(void* parent) {
    const int cap = *(const int*)((const char*)parent + OFF_KIDS_CAP);
    const int n   = *(const int*)((const char*)parent + OFF_KIDS_COUNT);
    void* const* kids = (void* const*)((const char*)parent + OFF_KIDS_DATA);
    void* best = 0;
    int i;

    if (cap > 4) kids = *(void* const* const*)kids;
    for (i = 0; i < n; i++) {
        void* k = kids[i];
        float scale;
        if (!k) continue;
        scale = *(const float*)((const char*)k + OFF_DO_XFORM);
        if (scale < 0) scale = -scale;
        if (scale < STRIPE_MIN_SCALE) continue;
        if (!best || *(const float*)((const char*)k + g_axis)
                   > *(const float*)((const char*)best + g_axis))
            best = k;
    }
    return best;
}

static bool adopt(void* src, void* clone) {
    void* parent = *(void**)((char*)src + OFF_DO_PARENT);
    if (!parent || !clone) return false;
    if (*(void**)((char*)clone + OFF_DO_PARENT)) return false;   // addChild asserts
    g_add_child(parent, clone, 1);
    return true;
}

// ------------------------------------------------------------------ the row --
static bool build_options(OptVec* out) {
    static const wchar_t* const LABELS[] = { L"<= 0", L"<= 1", L"<= 2", L"<= 3", L"<= 4" };
    SelOption* opts;
    int i;

    // Passed by value and destroyed by the callee, so the array has to come
    // from the heap that will free it.
    opts = (SelOption*)g_game_alloc(sizeof(SelOption) * (INT_LIMIT_MAX + 1));
    if (!opts) return false;

    for (i = 0; i <= INT_LIMIT_MAX; i++) {
        char digit[2];
        digit[0] = (char)('0' + i);
        digit[1] = 0;
        sso_narrow(&opts[i].value, digit);
        sso_wide(&opts[i].label, LABELS[i]);
    }
    out->begin = opts;
    out->end   = opts + INT_LIMIT_MAX + 1;
    out->cap   = out->end;
    return true;
}

static void add_our_row(void* panel) {
    void* src_val   = child_of(panel, SRC_ROW);
    void* src_left  = child_of(panel, SRC_ROW "_left");
    void* src_right = child_of(panel, SRC_ROW "_right");
    void* src_prev  = child_of(panel, PREV_ROW);
    void *val, *left, *right;
    float pitch;
    GameString key, current, text;
    OptVec options;
    char digit[2];

    say("settings row: panel=%p val=%p left=%p right=%p prev=%p",
        panel, src_val, src_left, src_right, src_prev);
    if (!src_val || !src_left || !src_right || !src_prev) {
        say("settings row: %s is not where it should be - no row", SRC_ROW);
        return;
    }
    if (child_of(panel, ROW_KEY)) {
        say("settings row: this panel already has one");
        return;
    }

    pitch = row_pitch(src_val, src_prev);

    // Underneath first, so everything else in the row draws over it.
    {
        void* parent = *(void**)((char*)src_val + OFF_DO_PARENT);
        void* src_stripe = parent ? last_stripe(parent) : 0;
        void* stripe = src_stripe ? clone_clip(src_stripe) : 0;
        if (stripe) {
            place(stripe, src_stripe, pitch * 2.0f, ROW_KEY "_stripe");
            adopt(src_stripe, stripe);
        } else {
            say("settings row: no stripe to copy (src=%p)", src_stripe);
        }
    }

    val   = clone_text(src_val);
    left  = clone_clip(src_left);
    right = clone_clip(src_right);
    if (!val || !left || !right) {
        say("settings row: could not copy the row (val=%p left=%p right=%p)",
            val, left, right);
        return;
    }

    place(val,   src_val,   pitch, ROW_KEY);
    place(left,  src_left,  pitch, ROW_KEY "_left");
    place(right, src_right, pitch, ROW_KEY "_right");

    if (!adopt(src_val, val) || !adopt(src_left, left) || !adopt(src_right, right)) {
        say("settings row: could not parent the copies");
        return;
    }

    // The label is its own text box, one row down from the one it was copied
    // from, carrying our own words instead of a localisation key.
    {
        void* src_label = child_of(panel, "SETTINGS_HOUSEMEOWVOLUME");
        void* label = src_label ? clone_text(src_label) : 0;
        if (label) {
            place(label, src_label, pitch, ROW_KEY "_label");
            if (adopt(src_label, label) && g_set_text) {
                make_game_string(&text, ROW_LABEL, sizeof(ROW_LABEL) - 1);
                g_set_text(label, &text, 1, 1);
            }
        } else {
            say("settings row: no label");
        }
    }

    if (!build_options(&options)) {
        say("settings row: no options");
        return;
    }

    digit[0] = (char)('0' + g_int_limit);
    digit[1] = 0;
    make_game_string(&key, ROW_KEY, sizeof(ROW_KEY) - 1);
    make_game_string(&current, digit, 1);

    g_fn.vptr = g_fn_vtbl;
    g_fn.self = &g_fn;                        // impl == storage, so: local, never freed

    g_next_regsel(panel, &key, &current, &options, 0, &g_fn);
    say("settings row: added below %s, %.1f on axis +0x%x, at INT <= %ld",
        SRC_ROW, (double)pitch, g_axis, g_int_limit);
}

// The game's own last audio row is the cue: the panel is in reach, its clips
// are up, and ours belongs directly underneath it.
//
// The key has to be read before the call, not after. register_selector takes it
// by value and destroys it on the way out, which leaves the caller's string
// empty - so asking afterwards is asking a corpse, and every key looks like "".
static void* hooked_register_selector(void* panel, void* key, void* current,
                                      void* options, int enabled, void* fn) {
    const char* k = str_of(key);
    const bool ours = k && strcmp(k, SRC_ROW) == 0;
    void* r;

    say("register_selector: panel=%p key=%s", panel, k ? k : "(null)");
    r = g_next_regsel(panel, key, current, options, enabled, fn);

    if (ours) {
        config_load();                        // whatever the file says is the row's value
        add_our_row(panel);
    }
    return r;
}


static void settingsrow_init(void) {
    g_find_child  = (FindChildFn)g_at[S_FIND_CHILD];
    g_add_child   = (AddChildFn)g_at[S_ADD_CHILD];
    g_pool_alloc  = (PoolAllocFn)g_at[S_POOL_ALLOC];
    g_clip_ctor   = (ClipCtorFn)g_at[S_CLIP_CTOR];
    g_set_text    = (SetTextFn)g_at[S_SET_TEXT];
    g_game_alloc  = (GameAllocFn)g_at[S_GAME_ALLOC];

    g_fn_vtbl[0] = (void*)&fn_copy;      // _Copy
    g_fn_vtbl[1] = (void*)&fn_move;      // _Move
    g_fn_vtbl[2] = (void*)&fn_call;      // _Do_call
    g_fn_vtbl[3] = (void*)&fn_target;    // _Target_type
    g_fn_vtbl[4] = (void*)&fn_delete;    // _Delete_this

    g_next_regsel = (RegSelFn)install_hook(SITES[S_REGISTER_SEL].rva, g_at[S_REGISTER_SEL], 24,
                                          (const void*)&hooked_register_selector, "legendofbunga");
}


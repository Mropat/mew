// mewlinks - make the lover and rival icons on the cat info panel clickable
//
// The cat info panel (a glaiel::MenuPanel subclass, HouseCatStatus) already
// registers six named buttons out of its movieclip: topipe, tobox, familytree,
// nametag_button, nextcat_left and nextcat_right. It does that through
//
//     glaiel::Button* MenuPanel::register_button(std::string name,
//                                                std::wstring label,
//                                                std::function<void()> cb)
//
// which looks `name` up as a child of the panel's clip and wires it for input.
// The lover and rival portraits are children of that same clip - the SWF symbol
// lists `lover`, `hater`, `lover_tt` and `hater_tt` right beside `topipe` and
// `familytree` - but the game only ever shows and hides the portraits, and
// hangs a hover tooltip off the `_tt` regions. None of the four is registered,
// so none of them is clickable.
//
// This mod hooks the panel's init, lets it register its own six buttons, then
// registers two more on the `lover_tt` and `hater_tt` hover regions - the ones
// already sitting over the portraits to catch the tooltip. Clicking one moves
// the panel to that cat exactly the way the nextcat arrows do:
//
//     HouseCatStatus::show_cat(house_cat, clear_if_null=false)
//
// Reaching the target is the only interesting part. The panel holds a HouseCat
// (the in-house entity), whose +0x80 is an id into the cat registry; the Cat
// record it resolves to carries the lover id at +0xbc8 and the rival id at
// +0xbd8. Those ids name a record, not an entity, so the mod walks the house's
// HouseCat list - the same list the arrows iterate - looking for the entity
// carrying that id. Being anywhere in the house is enough:
//
//     house_cat[+0x88] != 0    ; present in the list at all
//
// The arrows additionally require house_cat[+0xe8] to be the room the camera is
// zoomed into. This does not - if the cat is elsewhere in the house it leaves
// the room first, by clearing the view's room the way the game's own house
// popups do, and then selects.
//
// A cat that has been given away has no entity at all, so the click does
// nothing. Ids rather than pointers mean a dangling reference is not
// representable.
//
// Ships as a mewjector mod: drop mewlinks.dll into Mewgenics/mods/.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ------------------------------------------------------------------ logging --

static void logf_(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) return;
    char* slash = strrchr(path, (char)92);
    if (!slash) return;
    lstrcpyA(slash + 1, "mewlinks.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

#ifdef MEWLINKS_TRACE
#define TRACE(...) logf_(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

// ------------------------------------------------------- pinned call targets --

struct Site {
    const char*   name;
    unsigned      rva;
    unsigned char sig[16];
};

#include "sites.inc"

static unsigned char* g_base;
static unsigned char* g_at[SITE_COUNT];

// Object layouts, all read out of the same build the signatures pin.
#define OFF_COMP_ENTITY   0x18      // -> entity; [entity+8] is the singleton holder
#define OFF_COMP_PANEL    0x60      // -> MenuPanel
#define OFF_COMP_LOCK     0x73      // nonzero while the panel refuses input
#define OFF_COMP_CAT      0x78      // -> currently shown HouseCat
#define OFF_COMP_GEN      0x80      // its generation, checked against [cat-8]

#define OFF_HC_CATID      0x80      // HouseCat -> registry id of its Cat record
#define OFF_HC_LISTABLE   0x88      // byte: appears in the scrollable list
#define OFF_HC_ROOM       0xe8      // -> the room it is in

#define OFF_CAT_LOVER     0xbc8     // Cat -> id of the cat it loves
#define OFF_CAT_RIVAL     0xbd8     // Cat -> id of the cat it hates

#define OFF_VIEW_ROOM     0x88      // house view -> zoomed room, null when out
#define OFF_BTN_SOUND     0x88      // Button field every sibling button fills in
#define SOUND_FIELD       0x2398    // ...with soundctx + this

#define TYPE_HOUSECATS    0x448     // singleton id of the HouseCat list
#define OFF_HOLDER_TABLE  0x20      // holder -> singleton table, stride 16
#define OFF_VEC_COUNT     0x0c
#define OFF_VEC_DATA      0x10

#define RVA_GLOBALS       0x13dac30 // -> globals; [globals+0x598] is the registry
#define OFF_REGISTRY      0x598

typedef void* (*RegisterButtonFn)(void* panel, void* name, void* label, void* fn);
typedef void  (*ShowCatFn)(void* comp, void* house_cat, bool clear_if_null);
typedef void* (*HouseViewFn)(void* comp);
typedef void  (*SingletonFn)(void* holder, int type_id);
typedef void* (*SoundCtxFn)(void* comp);
typedef void* (*ResolveCatFn)(void* registry, unsigned long long id);
typedef void  (*InitFn)(void* comp);

static InitFn g_orig_init;

#include "msvcabi.inc"

// ------------------------------------------------------------ the click path --

// Not `lover` and `hater`, which is the obvious-looking choice: the invisible
// `_tt` regions are what catch the hover for the tooltip, so they sit above the
// portraits and swallow the click before it can reach them. Registering the
// portrait clips builds a Button that never once fires. Registering the tooltip
// regions instead puts the hit area exactly where the existing affordance
// already is, and leaves the artwork untouched.
struct Link {
    const char*        clip;
    unsigned long long rival;      // 0 = lover, 1 = rival
};

static const Link LINKS[] = {
    { "lover_tt", 0 },
    { "hater_tt", 1 },
};
#define LINK_COUNT (sizeof(LINKS) / sizeof(LINKS[0]))

// The Cat record behind a HouseCat, or null if the id is unset or unknown.
static void* cat_record(void* house_cat) {
    unsigned long long id = *(unsigned long long*)((char*)house_cat + OFF_HC_CATID);
    if (id == ~0ull) return 0;
    unsigned char* globals = *(unsigned char**)(g_base + RVA_GLOBALS);
    if (!globals) return 0;
    void* registry = *(void**)(globals + OFF_REGISTRY);
    if (!registry) return 0;
    return ((ResolveCatFn)g_at[S_RESOLVE_CAT])(registry, id);
}

// The house's HouseCat list, reached the way the nextcat arrows reach it.
static bool house_cats(void* comp, void*** out_data, unsigned* out_count) {
    void* entity = *(void**)((char*)comp + OFF_COMP_ENTITY);
    if (!entity) return false;
    unsigned char* holder = *(unsigned char**)((char*)entity + 8);
    if (!holder) return false;
    ((SingletonFn)g_at[S_SINGLETON])(holder, TYPE_HOUSECATS);
    unsigned char* table = *(unsigned char**)(holder + OFF_HOLDER_TABLE);
    if (!table) return false;
    unsigned char* vec = *(unsigned char**)(table + TYPE_HOUSECATS * 16);
    if (!vec) return false;
    *out_count = *(unsigned*)(vec + OFF_VEC_COUNT);
    *out_data  = *(void***)(vec + OFF_VEC_DATA);
    return *out_data != 0;
}

static void link_click(void* comp, unsigned long long idx) {
    const unsigned long long which = LINKS[idx].rival;
    TRACE("click: %s comp=%p", LINKS[idx].clip, comp);
    if (!comp) return;
    if (*(unsigned char*)((char*)comp + OFF_COMP_LOCK)) { TRACE("  bail: panel locked"); return; }

    unsigned char* cur = *(unsigned char**)((char*)comp + OFF_COMP_CAT);
    if (!cur) { TRACE("  bail: no current cat"); return; }
    // The panel keeps a generation alongside the pointer; a mismatch means the
    // entity behind it is gone. This is the game's own staleness test.
    if (*(unsigned long long*)(cur - 8) !=
        *(unsigned long long*)((char*)comp + OFF_COMP_GEN)) { TRACE("  bail: stale gen"); return; }

    TRACE("  cur=%p catid=%#llx", cur, *(unsigned long long*)(cur + OFF_HC_CATID));
    void* record = cat_record(cur);
    if (!record) { TRACE("  bail: no cat record"); return; }

    unsigned long long want = *(unsigned long long*)
        ((char*)record + (which ? OFF_CAT_RIVAL : OFF_CAT_LOVER));
    TRACE("  record=%p want=%#llx", record, want);
    if (want == ~0ull) { TRACE("  bail: no such relationship"); return; }

    void** data; unsigned count;
    if (!house_cats(comp, &data, &count)) { TRACE("  bail: no housecat list"); return; }
    TRACE("  list count=%u", count);

    unsigned char* target = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char* hc = (unsigned char*)data[i];
        if (!hc) continue;
        if (*(unsigned long long*)(hc + OFF_HC_CATID) != want) continue;
        target = hc;
        break;
    }
    if (!target) { TRACE("  bail: id not in house list"); return; }
    if (target == cur) { TRACE("  bail: target is already shown"); return; }

    // Being in the house is enough; being in *this room* is not required.
    if (*(unsigned char*)(target + OFF_HC_LISTABLE) == 0) { TRACE("  bail: not listable"); return; }

    // The arrows stop at the edge of the room they are in. This does not: if the
    // cat is somewhere else in the house, leave the room first. Clearing the
    // view's room is how the game itself gets out of one - the house popups
    // (pass-day, low-on-food, Jack's introduction) all do exactly this before
    // they show something house-wide.
    unsigned char* view = (unsigned char*)((HouseViewFn)g_at[S_HOUSE_VIEW])(comp);
    if (view) {
        void* room = *(void**)(view + OFF_VIEW_ROOM);
        if (room && *(void**)(target + OFF_HC_ROOM) != room) {
            TRACE("  zooming out: target is in another room");
            *(void**)(view + OFF_VIEW_ROOM) = 0;
        }
    }

    TRACE("  -> show_cat(%p)", target);
    ((ShowCatFn)g_at[S_SHOW_CAT])(comp, target, false);
}

// ---------------------------------------------------------- button creation --


static void add_link(void* comp, void* panel, unsigned long long idx) {
    MsvcString   name;
    MsvcWString  label;
    MsvcFunction fn;
    if (!str_init(&name, LINKS[idx].clip)) return;
    wstr_init_empty(&label);
    fn_init(&fn, comp, idx);

    // register_button takes all three by value and destroys them itself.
    void* btn = ((RegisterButtonFn)g_at[S_REGISTER_BTN])(panel, &name, &label, &fn);
    TRACE("  register_button(%s) -> %p", LINKS[idx].clip, btn);
    if (!btn) return;
    TRACE("    btn+0x0c=%#x btn+0x2f0=%#x", *(unsigned*)((char*)btn + 0x0c),
          *(unsigned*)((char*)btn + 0x2f0));

    // Every sibling button gets this field filled in right after registering;
    // match them rather than leaving the zero the constructor wrote.
    void* sctx = ((SoundCtxFn)g_at[S_SOUND_CTX])(comp);
    if (sctx) *(void**)((char*)btn + OFF_BTN_SOUND) = (char*)sctx + SOUND_FIELD;
}

static void hooked_init(void* comp) {
    g_orig_init(comp);                       // let the panel build itself first
    TRACE("init: comp=%p", comp);
    if (!comp) return;
    void* panel = *(void**)((char*)comp + OFF_COMP_PANEL);
    if (!panel) { TRACE("  bail: no panel"); return; }
    for (unsigned long long i = 0; i < LINK_COUNT; i++) add_link(comp, panel, i);
}

// ------------------------------------------------------------------- install --

// The init prologue is eight pushes - 13 bytes - so a 12-byte absolute jump
// lands on an instruction boundary with one byte to spare.
#define STOLEN 13

static bool verify_sites() {
    bool ok = true;
    for (int i = 0; i < SITE_COUNT; i++) {
        unsigned char* p = g_base + SITES[i].rva;
        if (memcmp(p, SITES[i].sig, SITE_SIGLEN) != 0) {
            logf_("  %s at +%#x does not match - refusing to install",
                  SITES[i].name, SITES[i].rva);
            ok = false;
        }
        g_at[i] = p;
    }
    return ok;
}

static void write_abs_jmp(unsigned char* at, void* dest) {
    at[0] = 0x48; at[1] = 0xB8;                       // mov rax, imm64
    memcpy(at + 2, &dest, 8);
    at[10] = 0xFF; at[11] = 0xE0;                     // jmp rax
}

static bool install() {
    unsigned char* site = g_at[S_INIT];

    unsigned char* tramp = (unsigned char*)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("  VirtualAlloc failed"); return false; }

    memcpy(tramp, site, STOLEN);
    write_abs_jmp(tramp + STOLEN, site + STOLEN);
    g_orig_init = (InitFn)tramp;

    DWORD old;
    if (!VirtualProtect(site, STOLEN, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("  VirtualProtect failed");
        return false;
    }
    write_abs_jmp(site, (void*)&hooked_init);
    site[12] = 0x90;                                  // pad the spare byte
    VirtualProtect(site, STOLEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, STOLEN);
    return true;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    g_base = (unsigned char*)GetModuleHandleA(NULL);
    logf_("mewlinks: base %p", g_base);
    if (!verify_sites()) return TRUE;                 // leave the game alone
    if (!install())      return TRUE;
    logf_("mewlinks: %u link regions armed", (unsigned)LINK_COUNT);
    return TRUE;
}

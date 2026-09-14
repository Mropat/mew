// mewlinks - click a cat's picture, go to that cat
//
// Two places in the house UI show you a cat you cannot get to:
//
//   * the lover and rival portraits on the cat info panel
//   * every portrait in the family tree
//
// Both become clickable, and clicking one selects that cat in the house - the
// same thing the panel's own < > arrows do, which also highlights it.
//
// ---------------------------------------------------------------- the panel --
//
// The info panel is a glaiel::MenuPanel subclass and already builds its six
// buttons by name out of its movieclip through
//
//     Button* MenuPanel::register_button(std::string, std::wstring,
//                                        std::function<void()>)
//
// The relationship icons are children of that same clip but are never
// registered, so the mod hooks the panel's init and registers two more - on
// `lover_tt` and `hater_tt`, the invisible hover regions, not on `lover` and
// `hater`. The `_tt` rects have to sit above the artwork to catch the tooltip,
// so they swallow the click on its way down: a button on `lover` never fires
// once, silently, while the tooltip keeps working.
//
// ----------------------------------------------------------- the family tree --
//
// The tree registers no buttons at all, so there is no named-child route. But
// register_button is only a wrapper: the Button constructor underneath takes a
// clip *pointer*, so any clip can become one. What it wants is the display
// object, not the clip component that owns it - portrait entry +0x98 is the
// component, [component+0x80] is the object, and [object+0x40] links back.
//
// A click there has to leave the tree before selecting. Selecting while the
// tree is open provably works - show_cat returns and the panel holds the new
// cat - and is then discarded when the tree tears down. So the mod first runs
// the tree's own exit action, whose callback reads nothing but its captured
// hud, and only then selects.
//
// ------------------------------------------------------------- reaching them --
//
// Every route ends the same way. A portrait or a relationship icon names a cat
// by registry id, so the mod searches the house's HouseCat list - the list the
// arrows iterate - for the entity carrying that id. Being in the house is
// enough; if the cat is in another room the mod leaves the room first, the way
// the game's own house popups do. A cat that has been given away has no entity
// at all and the click does nothing.
//
// Ships as a mewjector mod: drop mewlinks.dll into Mewgenics/mods/.

#include <windows.h>
#define MODLOG_NAME "mewlinks"
#include "../common/modlog.inc"

#ifdef MEWLINKS_TRACE
#define TRACE(...) logf_(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

#include "../common/hookapi.inc"
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
#define OFF_HOLDER_BUSY   0x4b0     // nonzero: the scene will not take components

#define OFF_HUD_GFX       0x40      // FamilyTreeHud -> its vector-graphics batch
#define OFF_HUD_HOVER     0xe8      // the cat it last drew highlight edges for
#define OFF_HUD_NODES     0x148     // FamilyTreeHud -> std::list of portraits
#define OFF_ENTRY_CAT     0x10      // portrait entry -> its cat id
#define OFF_ENTRY_CLIP    0x98      // -> its clip component
#define OFF_CLIP_OBJECT   0x80      // -> the display object a Button wants
#define OFF_OBJ_OWNER     0x40      // -> back to the clip component

#define RVA_GLOBALS       0x13dac30 // -> globals; [globals+0x598] is the registry
#define OFF_REGISTRY      0x598

typedef void* (*RegisterButtonFn)(void* panel, void* name, void* label, void* fn);
typedef void  (*ShowCatFn)(void* comp, void* house_cat, bool clear_if_null);
typedef void* (*HouseViewFn)(void* comp);
typedef void  (*SingletonFn)(void* holder, int type_id);
typedef void* (*SoundCtxFn)(void* comp);
typedef void* (*ResolveCatFn)(void* registry, unsigned long long id);
typedef void  (*InitFn)(void* comp);
typedef void  (*TreePassFn)(void* hud);
typedef void* (*ButtonFn)(void* holder, void* x, void** obj, void* fn, void* label);
typedef void* (*HolderArgFn)(void* holder);
typedef void  (*TreeExitFn)(void* callable);
typedef void  (*StrokeFn)(void* gfx, const void* style);
typedef void  (*DrawCurveFn)(void* hud, const void* p3, const void* p2,
                             const void* p1, const void* p0);
typedef bool  (*MapHasFn)(void* map, const unsigned long long* key);
typedef void**(*MapFindFn)(void* map, void* scratch, const unsigned long long* key);

static InitFn     g_orig_init;
static TreePassFn g_orig_tree_pass;
static void*      g_panel;          // the HouseCatStatus component, once built
static void*      g_hud;            // the open family tree, for exiting it
// Not a cat id any tree will ever hover: -1 already means "nothing hovered".
#define NEVER_DRAWN (~0ull - 1)
static unsigned long long g_drawn_for = NEVER_DRAWN;  // hover state we last drew on

#include "../common/msvcabi.inc"

#define KIND_PANEL_ICON 0           // a = the panel, b = 0 lover / 1 rival
#define KIND_TREE_CAT   1           // b = the portrait's cat id

// ------------------------------------------------------------ finding a cat --

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

// The house entity for a cat id, or null if that cat is not in the house.
static unsigned char* find_in_house(void* panel, unsigned long long id) {
    if (id == ~0ull) return 0;
    void** data; unsigned count;
    if (!house_cats(panel, &data, &count)) return 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char* hc = (unsigned char*)data[i];
        if (!hc) continue;
        if (*(unsigned long long*)(hc + OFF_HC_CATID) != id) continue;
        // The arrows' own test for what is selectable at all.
        return *(unsigned char*)(hc + OFF_HC_LISTABLE) ? hc : 0;
    }
    return 0;
}

// Show a cat, leaving the current room first if it is somewhere else. Clearing
// the view's room is how the game itself gets out of one - the house popups all
// do exactly this before showing something house-wide.
static void select_in_house(void* panel, unsigned char* target) {
    unsigned char* view = (unsigned char*)((HouseViewFn)g_at[S_HOUSE_VIEW])(panel);
    if (view) {
        void* room = *(void**)(view + OFF_VIEW_ROOM);
        if (room && *(void**)(target + OFF_HC_ROOM) != room)
            *(void**)(view + OFF_VIEW_ROOM) = 0;
    }
    ((ShowCatFn)g_at[S_SHOW_CAT])(panel, target, false);
}

// ----------------------------------------------------------------- on click --

static void panel_icon_click(void* comp, unsigned long long rival) {
    if (!comp) return;
    if (*(unsigned char*)((char*)comp + OFF_COMP_LOCK)) { TRACE("  panel locked"); return; }

    unsigned char* cur = *(unsigned char**)((char*)comp + OFF_COMP_CAT);
    if (!cur) { TRACE("  no current cat"); return; }
    // The panel keeps a generation alongside the pointer; a mismatch means the
    // entity behind it is gone. This is the game's own staleness test.
    if (*(unsigned long long*)(cur - 8) !=
        *(unsigned long long*)((char*)comp + OFF_COMP_GEN)) { TRACE("  stale"); return; }

    void* record = cat_record(cur);
    if (!record) { TRACE("  no cat record"); return; }

    unsigned long long want = *(unsigned long long*)
        ((char*)record + (rival ? OFF_CAT_RIVAL : OFF_CAT_LOVER));
    unsigned char* target = find_in_house(comp, want);
    if (!target || target == cur) { TRACE("  %#llx not reachable", want); return; }

    TRACE("  -> %#llx", want);
    select_in_house(comp, target);
}

static void tree_portrait_click(unsigned long long id) {
    void* panel = g_panel;
    if (!panel) { TRACE("  no panel yet"); return; }
    if (*(unsigned char*)((char*)panel + OFF_COMP_LOCK)) { TRACE("  panel locked"); return; }

    unsigned char* target = find_in_house(panel, id);
    if (!target) { TRACE("  %#llx not in the house", id); return; }

    // Selecting while the tree is up works and is then thrown away when the tree
    // tears down, so leave first. The exit action reads only its captured hud,
    // which is why a two-field stand-in for the std::function it lives in works.
    if (g_hud) {
        struct { void* vptr; void* hud; } callable = { 0, g_hud };
        ((TreeExitFn)g_at[S_TREE_EXIT])(&callable);
    }
    TRACE("  -> %#llx", id);
    select_in_house(panel, target);
}

static void on_button_click(void* a, unsigned long long b, unsigned long long kind) {
    TRACE("click: kind=%llu a=%p b=%#llx", kind, a, b);
    if (kind == KIND_PANEL_ICON) panel_icon_click(a, b);
    else                         tree_portrait_click(b);
}

// -------------------------------------------------- the panel's two buttons --

struct Link {
    const char*        clip;
    unsigned long long rival;
};

static const Link LINKS[] = {
    { "lover_tt", 0 },
    { "hater_tt", 1 },
};
#define LINK_COUNT (sizeof(LINKS) / sizeof(LINKS[0]))

static void add_link(void* comp, void* panel, unsigned long long idx) {
    MsvcString   name;
    MsvcWString  label;
    MsvcFunction fn;
    if (!str_init(&name, LINKS[idx].clip)) return;
    wstr_init_empty(&label);
    fn_init(&fn, comp, LINKS[idx].rival, KIND_PANEL_ICON);

    // register_button takes all three by value and destroys them itself.
    void* btn = ((RegisterButtonFn)g_at[S_REGISTER_BTN])(panel, &name, &label, &fn);
    if (!btn) return;

    // Every sibling button gets this field filled in right after registering;
    // match them rather than leaving the zero the constructor wrote.
    void* sctx = ((SoundCtxFn)g_at[S_SOUND_CTX])(comp);
    if (sctx) *(void**)((char*)btn + OFF_BTN_SOUND) = (char*)sctx + SOUND_FIELD;
}

static void hooked_init(void* comp) {
    g_orig_init(comp);                       // let the panel build itself first
    if (!comp) return;
    g_panel = comp;                          // the tree has no way to find this
    void* panel = *(void**)((char*)comp + OFF_COMP_PANEL);
    if (!panel) return;
    for (unsigned long long i = 0; i < LINK_COUNT; i++) add_link(comp, panel, i);
}

// ----------------------------------------------------- the tree's N buttons --

#define MAX_PORTRAITS 512
static void*    g_done[MAX_PORTRAITS];
static int      g_done_n;
static void*    g_last_first;               // first list node: identifies a build
static unsigned g_last_count;

static bool already_buttoned(void* obj) {
    for (int i = 0; i < g_done_n; i++) if (g_done[i] == obj) return true;
    return false;
}

static void add_tree_buttons(void* hud) {
    g_hud = hud;
    void** head = *(void***)((char*)hud + OFF_HUD_NODES);
    if (!head) return;

    // The hud object is reused between trees, so it cannot identify a build.
    // The portrait list is rebuilt every time, so its first node and length can.
    void* first = *head;
    unsigned n = 0;
    for (void** q = (void**)first; q != head && n < MAX_PORTRAITS; q = (void**)*q) n++;
    if (first != g_last_first || n != g_last_count) {
        g_last_first = first; g_last_count = n; g_done_n = 0;
        // A rebuild clears the graphics batch too, so the marked edges are gone
        // and have to be redrawn - even if the hovered cat is unchanged. It
        // usually is: two trees opened back to back both start with nothing
        // hovered, so keying the redraw on the hover alone silently skips the
        // second one until the mouse happens to move over a portrait.
        g_drawn_for = NEVER_DRAWN;
        TRACE("new family tree: %u portraits", n);
    }

    void* entity = *(void**)((char*)hud + OFF_COMP_ENTITY);
    if (!entity) return;
    unsigned char* holder = *(unsigned char**)((char*)entity + 8);
    if (!holder || holder[OFF_HOLDER_BUSY] != 0) return;

    for (void** p = (void**)*head; p != head && g_done_n < MAX_PORTRAITS; p = (void**)*p) {
        char* entry = (char*)p;
        void* clip = *(void**)(entry + OFF_ENTRY_CLIP);
        if (!clip) continue;
        void* obj = *(void**)((char*)clip + OFF_CLIP_OBJECT);
        if (!obj || already_buttoned(obj)) continue;

        // The accept test the game's own child lookup applies before using one.
        typedef int (*TypeFn)(void*);
        int t = ((TypeFn*)(*(void***)obj))[0](obj);
        if ((t != 1 && t != 2) || !*(void**)((char*)obj + OFF_OBJ_OWNER)) continue;

        MsvcWString label; wstr_init_empty(&label);
        MsvcFunction fn;
        fn_init(&fn, 0, *(unsigned long long*)(entry + OFF_ENTRY_CAT), KIND_TREE_CAT);

        void* x = ((HolderArgFn)g_at[S_HOLDER_ARG])(holder);
        if (((ButtonFn)g_at[S_BUTTON])(holder, x, &obj, &fn, &label))
            g_done[g_done_n++] = obj;
    }
}

// 0x9ffc40 reads this with movaps at +0x00, +0x10 and +0x20, so it must be
// 16-byte aligned and exactly 48 bytes. The game never has to think about that
// because it builds one on an aligned stack slot; a plain static does not get
// the alignment for free, and an unaligned movaps faults on the spot.
struct alignas(16) Stroke {
    float         colour[4];
    double        thickness;
    int           a, b;
    double        extra;
    unsigned char flag;
    unsigned char pad[7];
};
static_assert(sizeof(Stroke) == 48, "stroke style must be 48 bytes");
static_assert(alignof(Stroke) == 16, "stroke style must be 16-byte aligned");

// The tree strokes edges black at two weights, 0.1 and 0.05, and lights the
// hovered one white at 0.075. A marked edge is the same black, at just over the
// weight the game's own highlight uses - no new colour in the palette, and no
// more emphasis than the game already gives a line it wants you to notice.
//
// MARK_THICKNESS is the one number worth tuning. 0.075 matches the highlight
// exactly; 0.2 was legible from across the room and far too loud for it.
#define MARK_THICKNESS 0.085

static const Stroke REACHABLE = {
    { 0.0f, 0.0f, 0.0f, 1.0f }, MARK_THICKNESS, 1, 1, 2.0, 0, { 0, 0, 0, 0, 0, 0, 0 }
};

// The game reads these back with movaps - an *aligned* 16-byte load - so they
// need 16-byte alignment, which two doubles do not get on their own: the type's
// natural alignment is 8, and a compiler is free to place it there. clang put
// them on 16 by luck; MSVC did not, and the released build faulted on the first
// family tree it drew.
//
// This is the third bug of exactly this shape. Anything handed to the game that
// it might load with movaps gets alignas(16) and an assertion, whether or not
// the crash has been seen yet.
struct alignas(16) Pt { double x, y; };
static_assert(alignof(Pt) == 16, "curve points must be 16-byte aligned");

#define OFF_HUD_MAP      0x140      // FamilyTreeHud -> map of cat id to node
#define OFF_NODE_X       0x48       // layout node position
#define OFF_NODE_Y       0x50
#define OFF_NODE_DROP    0x68       // how far its connector drops before turning
#define OFF_NODE_PARENT0 0x20       // its two parents, by cat id
#define OFF_NODE_PARENT1 0x28
#define EDGE_INSET       0.2        // gap left at each portrait

// One connector, reproducing the shape the game's own highlight draws: straight
// down out of the child, across, then straight up into the parent.
static void draw_edge(void* hud, const unsigned char* node, const unsigned char* parent) {
    const double nx = *(const double*)(node + OFF_NODE_X);
    const double ny = *(const double*)(node + OFF_NODE_Y);
    const double px = *(const double*)(parent + OFF_NODE_X);
    const double py = *(const double*)(parent + OFF_NODE_Y);
    const double drop = *(const double*)(node + OFF_NODE_DROP);

    Pt p0 = { nx, ny + EDGE_INSET };
    Pt p1 = { nx, ny + drop };
    Pt p2 = { px, ny + drop };
    Pt p3 = { px, py - EDGE_INSET };
    ((DrawCurveFn)g_at[S_DRAW_CURVE])(hud, &p3, &p2, &p1, &p0);
}

// Mark the single edge that leads up to each cat still in the house.
//
// Not the same rule as the hover highlight, which lights both edges *above* a
// cat - up to its two parents. What we want is the one edge *below* each living
// cat, the one connecting it down into the tree. So the loop runs over children
// rather than over living cats: for every node, if one of its parents is in the
// house, draw that node's edge up to that parent. Each living cat is the parent
// of exactly one node, so it gets exactly one edge; the examined cat is nobody's
// parent, so the root correctly gets none.
//
// The tree only redraws when the hovered cat changes - it clears the batch, then
// strokes base and highlight edges. Anything we add has to go on at that moment
// or it is wiped, and at no other moment or it accumulates. The hud records what
// it last drew for, so comparing against that catches exactly those frames.
static void mark_reachable_edges(void* hud) {
    unsigned long long hover = *(unsigned long long*)((char*)hud + OFF_HUD_HOVER);
    if (hover == g_drawn_for) return;
    g_drawn_for = hover;

    void* gfx = *(void**)((char*)hud + OFF_HUD_GFX);
    void* panel = g_panel;
    void** head = *(void***)((char*)hud + OFF_HUD_NODES);
    if (!gfx || !panel || !head) return;

    void* map = (char*)hud + OFF_HUD_MAP;
    alignas(16) unsigned char scratch[64];

    ((StrokeFn)g_at[S_STROKE])(gfx, &REACHABLE);

    int lit = 0;
    for (void** p = (void**)*head; p != head && lit < MAX_PORTRAITS; p = (void**)*p) {
        unsigned long long id = *(unsigned long long*)((char*)p + OFF_ENTRY_CAT);
        if (id == hover) continue;              // the game just drew this one white
        if (!((MapHasFn)g_at[S_MAP_HAS])(map, &id)) continue;
        unsigned char* node =
            *(unsigned char**)((MapFindFn)g_at[S_MAP_FIND])(map, scratch, &id);
        if (!node) continue;

        const unsigned off[2] = { OFF_NODE_PARENT0, OFF_NODE_PARENT1 };
        for (int k = 0; k < 2; k++) {
            unsigned long long pid = *(unsigned long long*)(node + off[k]);
            if (pid == ~0ull) continue;
            if (!((MapHasFn)g_at[S_MAP_HAS])(map, &pid)) continue;
            if (!find_in_house(panel, pid)) continue;     // only cats you can reach
            unsigned char* pnode =
                *(unsigned char**)((MapFindFn)g_at[S_MAP_FIND])(map, scratch, &pid);
            if (!pnode) continue;
            draw_edge(hud, node, pnode);
            lit++;
        }
    }
    TRACE("edges: marked %d reachable cats (hover=%#llx)", lit, hover);
}

static void hooked_tree_pass(void* hud) {
    g_orig_tree_pass(hud);
    if (!hud) return;
    add_tree_buttons(hud);
    mark_reachable_edges(hud);
}

// ------------------------------------------------------------------- install --

// Fallback steal lengths, used only when Mewjector is absent. With it, they are
// its problem: stolenBytes = 0 and its length disassembler picks the boundary.
//
//   init: eight pushes plus the frame-pointer lea, 21 bytes
//   tree: mov rax,rsp plus eight pushes, 15 bytes - and that first instruction
//         is why a trampoline jump must not touch RAX, see common/detour.inc
#define STOLEN_INIT 21
#define STOLEN_TREE 15

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    g_base = (unsigned char*)GetModuleHandleA(NULL);
    int bad = verify_sites(SITES, SITE_COUNT, SITE_SIGLEN, g_base, g_at);
    if (bad >= 0) {
        logf_("mewlinks: %s at +%#x does not match - not installing",
              SITES[bad].name, SITES[bad].rva);
        return TRUE;                                  // leave the game alone
    }
    const bool chained = hookapi_init();
    g_orig_init = (InitFn)install_hook(SITES[S_INIT].rva, g_at[S_INIT], STOLEN_INIT,
                                       (const void*)&hooked_init, "mewlinks");
    g_orig_tree_pass = (TreePassFn)install_hook(SITES[S_TREE_PASS].rva, g_at[S_TREE_PASS],
                                                STOLEN_TREE, (const void*)&hooked_tree_pass,
                                                "mewlinks");
    if (!g_orig_init || !g_orig_tree_pass) {
        logf_("mewlinks: could not install hooks");
        return TRUE;
    }
    logf_("mewlinks: relationship icons and family tree portraits are clickable%s",
          chained ? "" : " (standalone hooks - Mewjector API not found)");
    return TRUE;
}

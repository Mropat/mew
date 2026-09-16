// The Legend of Bunga
//
// A cat with 0 INT makes the Lord Bunga fight play the radio version of its
// song, "Mom I Really Hate You" - the community legend, made true. A cat that
// drops to 0 mid-turn, by Stoopzerk or a concussion, hears it change while it
// is still standing there.
//
// The mod adds two layers of its own carrying the radio version, trimmed and
// looped into a stem of the Bunga song so every cat stays on one grid, and
// crossfades to them on the turns of cats that qualify. Nothing is borrowed:
// every zone layer keeps its job.
//
// README.md covers how the game's music system works, how the radio version
// is made to fit it, and the measurements behind the constants here.
//
// Two things to know before changing any of the timing. Decoded-sample counts
// advance a whole block - one second - at a time, so they can decide WHETHER
// something happens but never shape HOW; anything audible runs off the frame
// clock. And where an exact position is needed it is computed, not sampled.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#define MOD_HOOK_PRIORITY 50
#define MOD_REQUIRES_MEWJECTOR   // no raw patching, so no injection-shaped imports
#include "hookapi.inc"
#include "sites.inc"

#define RADIO_TRACK "audio/music/radio/songs/mom_i_really_hate_you.ogg"




// Who hears it: player cats at or below the threshold, which is 0 - the
// legend's own condition and nothing else. Enemies are excluded by
// is_player_cat; they take turns and have stats too.
//
// Read live, every frame of the acting cat's turn, so Stoopzerk or a
// concussion is heard the moment it lands.
#define RADIO_INT_THRESHOLD 0        // INT at or below this: always
#define OFF_IS_PLAYER_CAT 0x489

// Character stat block, from the buff applier at 0x7d610, which does
// "add dword ptr [rdx+0x5bc], ecx" immediately after loading "strength":
//   +0x5bc str  +0x5c0 dex  +0x5c4 con  +0x5c8 int  +0x5cc spd  +0x5d0 cha
// The same function reads current HP at +0x4b0, which is where the combat
// probe independently put it, so the block is anchored to something known.
#define OFF_INT 0x5c8

static unsigned char* g_base;
static unsigned char* g_at[SITE_COUNT];

typedef void (*QueueFn)(void* self, void* path, int onfinish);
typedef void (*TurnFn)(void* self, int kind);
static QueueFn g_next;
static TurnFn  g_next_turn;

#include "trace.inc"

// --------------------------------------------------------------------
// talking to the game's C++ ABI
// --------------------------------------------------------------------
// MSVC std::string: 16-byte SSO buffer, size at +0x10, capacity at +0x18.
// Capacity over 15 means +0x00 holds a heap pointer instead of the characters.
static const char* str_of(const void* s) {
    if (!s) return 0;
    const unsigned long long cap = *(const unsigned long long*)((const char*)s + 0x18);
    return cap > 15 ? *(const char* const*)s : (const char*)s;
}

// A std::string for the game, allocated by the game.
//
// Passing one backed by our own memory corrupts its heap: something in
// QueueSongChunk frees the buffer, and this DLL's CRT heap is not the game's.
// append() grows through the right allocator. Fresh object every call - if the
// game takes ownership, reusing one would append into a freed pointer.
struct alignas(16) GameString {
    char               buf[16];
    unsigned long long size;
    unsigned long long cap;
};
static_assert(sizeof(GameString) == 32, "std::string is 0x20 bytes");

typedef void* (*AppendFn)(void* self, const char* s, unsigned long long n);
static AppendFn g_append;

static void make_game_string(GameString* out, const char* lit, unsigned long long n) {
    for (unsigned i = 0; i < sizeof(*out); i++) ((char*)out)[i] = 0;
    out->cap = 15;                       // the empty short-string state
    g_append(out, lit, n);
}

static bool is_radio_cat(void* self) {
    if (!self) return false;
    if (!*(const unsigned char*)((const char*)self + OFF_IS_PLAYER_CAT)) return false;
    return *(const int*)((const char*)self + OFF_INT) <= RADIO_INT_THRESHOLD;
}

// The joke belongs to one fight, so everything is gated on being in the ice
// age - Lord Bunga's zone, and the one the radio version is counterpart to.
static volatile long g_in_iceage;

// Our track is the second chunk of our layer: [intro, radio]. Recorded as
// it is queued rather than assumed - a layer without an intro has it first.
static volatile long g_chunk_radio = 1;

#define BODY_SAMPLES   8568000         // the instrumental's exact length


// SoundStream fields, as read out of the pull and QueueSongChunk.
#define OFF_CHUNK_VEC    0x170   // vector of chunks, 0x10 stride, decoder first
#define OFF_CHUNK_INDEX  0x188   // which chunk is live
#define OFF_CHUNK_COUNT  0x18c   // how many chunks are queued
#define OFF_BLOCK_FRAMES 0x198   // frames decoded per pull; set from the sample rate
#define OFF_PRODUCED     0x1a0   // running count of frames this stream has produced

#define LAYER_STRIDE   0x30
#define OFF_GAIN_CUR   0x20      // current gain: ramped toward the target
#define OFF_GAIN_TGT   0x28      // target gain: 1.0 audible, 0.0 silent
#define LAYER_BATTLE   0         // queue order: battle, map, event, boss
#define LAYER_BOSS     3

// Which fight layer the current cat should hear, or -1 for "do not interfere".
static volatile int g_want = -1;

// The cat currently acting, or null. Its INT is re-read every frame so a cat
// that makes itself stupid mid-turn hears the change at once. Only ever read
// between BeginTurn and EndTurn, and dropped the moment the cat dies - that
// window is what makes holding the pointer safe.
static void* volatile g_acting;

static char           g_intro_path[128];  // the zone's intro sting, seen as it is queued
static void* volatile g_stream_battle;
static void* volatile g_stream_boss;

// How long a switch takes, in milliseconds. The game's own transitions glide
// over roughly a second, so this is in the same register.
#define FADE_MS 900.0

// Our fade envelope: 0 = the game's music, 1 = the radio version. Assigned,
// not nudged - nudging fought the game's ramp and gave an uneven fade - and
// timed off the clock so it lasts the same at any frame rate.
static double        g_mix;
static unsigned long g_mix_tick;

volatile long g_block;      // frames a single decode yields, 0 until seen
volatile long g_pulls;      // blocks our stems have produced
volatile long g_pulls_any;  // blocks ANY stream produced through this hook

// Trim the radio version into a stem of the Bunga theme: drop its two-bar
// count-in and its four-bar tail, and loop what remains on the INSTRUMENTAL's
// length. The numbers and how they were measured are in the README.
//
// There is no seek, so trimming means decoding and discarding - a few blocks
// per call, since 2.9s of vorbis in one go would miss an audio deadline.
#define SKIP_SAMPLES   127808          // the two-bar count-in
#define SKIP_PER_PULL  8               // blocks discarded per call, to spread the work

// OnFinish, at [chunk+8]: 0 advance, 1 loop in place, 2 advance-or-loop. Our
// layers are [intro, radio], matching the shape the game gives its own.

// Two copies of the radio version, taking turns, so the loop seam can be
// hidden inside the radio track itself - the Bunga theme has a male vocal, so
// ducking under it swaps singers rather than covering a splice.
//
// A stem's position is measured from the START OF THE BODY, so it is negative
// during the count-in and crosses zero on the downbeat.
typedef struct {
    void*         stream;
    long          skip_left;   // samples still to discard
    long          skip_req;    // what to skip after the next rewind
    long long     skipped;     // discarded since that rewind
    long long     pos;         // body position: negative during the count-in
    volatile long realign;     // rewind and re-skip on the next pull
} Stem;

static Stem          g_stem[2];
static volatile long g_live;           // which stem is carrying the body
static volatile long g_armed;          // the other one is primed for the seam
static double        g_xf;             // handover: 0 = live, 1 = the other
static unsigned long g_xf_tick;        // when the handover began, 0 when idle

// A bar, in milliseconds: four beats at 164.10 BPM.
#define XF_MS          1463


// The game's layers decode a second at a time, so comparing their produced
// counts with ours is only good to about a second. Corrections finer than that
// are noise, and this only ever skips FORWARD - so it fires once, for a real
// start lag, and never again.
#define CONVERGE_FLOOR 44100
static volatile long g_converged;

static void converge(void* self, Stem* st) {
    if (!g_stream_boss || st->skip_left > 0 || g_block <= 0) return;
    if (g_mix > 0.0 || g_converged) return;
    const long long theirs = *(const long long*)((const unsigned char*)g_stream_boss + OFF_PRODUCED);
    const long long mine   = *(const long long*)((const unsigned char*)self + OFF_PRODUCED);
    const long long drift  = (mine - theirs) - st->skipped;
    if (drift > -(long long)CONVERGE_FLOOR) return;
    const long blocks = (long)((-drift) / CONVERGE_FLOOR);
    st->skip_left = blocks * CONVERGE_FLOOR;
    g_converged = 1;
    say("converging once: %.3fs behind, discarding %ld more second(s)",
        (double)(-drift) / 44100.0, blocks);
}

static int stem_of(void* stream) {
    if (stream && stream == g_stem[0].stream) return 0;
    if (stream && stream == g_stem[1].stream) return 1;
    return -1;
}


// --------------------------------------------------------------------
// per-frame: gains, the switch, and the loop handover
// --------------------------------------------------------------------
// Set the gains every frame, after the game's own update.
//
// Writing the layer TARGET loses: something re-asserts targets almost every
// frame, and on the one occasion it does not, the forced value surfaces later
// over the reward screen. Current is what the mixer uses, and this runs after
// the ramp that would otherwise pull it back.
//
// Layers live in a std::vector<Layer>, 0x30 stride, reached through the group:
// begin at +0x00, end at +0x08. Read fresh every frame - a growing vector
// moves its elements, so a cached Layer pointer goes stale.
//
// Self-limiting: it only ever redirects a fight the game is already scoring.
static void force_layers(unsigned char* group, int which) {
    (void)which;   // only used by the trace build
    unsigned char* begin = *(unsigned char**)(group + 0x00);
    unsigned char* end   = *(unsigned char**)(group + 0x08);
    if (!begin || !end || end <= begin) return;
    const size_t count = (size_t)(end - begin) / LAYER_STRIDE;
    if (count < 2 || count > 8) return;

    int mine[2] = { -1, -1 }, boss = -1;
    for (size_t i = 0; i < count; i++) {
        void* ls = *(void**)(begin + i * LAYER_STRIDE);
        if (ls && ls == g_stem[0].stream) mine[0] = (int)i;
        if (ls && ls == g_stem[1].stream) mine[1] = (int)i;
        if (ls && ls == g_stream_boss)    boss    = (int)i;
    }
    if (mine[0] < 0) return;             // not the set we added to

    // Only the boss layer. The legend is about the Lord Bunga fight, not about
    // ice age skirmishes, so an ordinary battle in the zone is left alone.
    double* t_bos = boss >= 0 ? (double*)(begin + boss * LAYER_STRIDE + OFF_GAIN_TGT) : 0;
    const bool fighting = g_in_iceage && t_bos && *t_bos > 0.0;

    const unsigned long now = GetTickCount();

#ifdef LEGENDOFBUNGA_TRACE
    static unsigned long s_last;
    if ((now - s_last) > 3000) {
        s_last = now;
        say("layers[%d]: live=%d (layer %d) other=%d boss=%d fight=%d mix=%.2f "
            "xf=%.2f pos=%.1fs/%.1fs pulls=%ld/%ld want=%d",
            which, g_live, mine[g_live], mine[g_live ^ 1], boss, (int)fighting,
            g_mix, g_xf,
            (double)g_stem[g_live].pos / 44100.0,
            (double)g_stem[g_live ^ 1].pos / 44100.0,
            g_pulls, g_pulls_any, g_want);
    }
#endif

    const double dt = g_mix_tick ? (double)(now - g_mix_tick) : 0.0;
    g_mix_tick = now;
    // Ask the acting cat again, every frame. This is what makes a mid-turn
    // change audible at once; between turns there is nobody to ask and the
    // last verdict stands, so the music does not flap on enemy turns.
    void* acting = g_acting;
    if (acting) g_want = is_radio_cat(acting) ? LAYER_BOSS : LAYER_BATTLE;

    const double want = (fighting && g_want == LAYER_BOSS) ? 1.0 : 0.0;

    if (dt > 0.0 && dt < 500.0) {        // ignore a hitch or a loading pause
        const double step = dt / FADE_MS;
        if (g_mix < want) { g_mix += step; if (g_mix > want) g_mix = want; }
        else if (g_mix > want) { g_mix -= step; if (g_mix < want) g_mix = want; }
    }
    // Hand over across the seam: the live copy runs on into its tail while the
    // other starts the body again, and the two crossfade over a bar.
    const int live = g_live, other = live ^ 1;
    if (mine[other] >= 0) {
        const long long past = g_stem[live].pos - BODY_SAMPLES;

        // Position decides WHEN the handover starts; the frame clock shapes it. Driven
        // off position, a bar-long fade got one or two updates - a step, then a jump.
        if (past >= 0 && g_xf_tick == 0) {
            g_xf_tick = now ? now : 1;
            say("handover: blending the tail into the fresh start over %dms", XF_MS);
        }
        if (g_xf_tick) {
            g_xf = (double)(now - g_xf_tick) / (double)XF_MS;
            if (g_xf >= 1.0) {                  // handover complete: swap roles
                InterlockedExchange(&g_live, other);
                InterlockedExchange(&g_armed, 0);
                g_xf = 0.0;
                g_xf_tick = 0;
                say("handed the body over to copy %d", other);
            }
        } else {
            g_xf = 0.0;
        }
    }

    // The handover runs whether or not anyone can hear it. It used to sit below
    // this line, so a seam that passed during a normal cat's turn left the
    // roles unswapped and priming disarmed: the live copy ran into its tail
    // and took its own 203.051s loop, 8.765s off the Bunga grid, and the next
    // cat to qualify raised a copy that was no longer on the beat.
    if (g_mix <= 0.0) return;            // silent: the game's own gains stand

    // Never silence the game's music in favour of a layer that is not actually
    // producing audio. Entering the Bunga room once did exactly that - our
    // stem was added and faded up while decoding nothing, and the fight played
    // with ambience only. If the stem is not alive, this does nothing at all.
    if (g_pulls <= 0) {
        *(double*)(begin + mine[0] * LAYER_STRIDE + OFF_GAIN_CUR) = 0.0;
        if (mine[1] >= 0)
            *(double*)(begin + mine[1] * LAYER_STRIDE + OFF_GAIN_CUR) = 0.0;
        return;
    }

    // Assign, do not nudge. Only the fight layer the game actually has up is
    // damped - raising the other one would put two layers on at once.
    *(double*)(begin + mine[live] * LAYER_STRIDE + OFF_GAIN_CUR) = g_mix * (1.0 - g_xf);
    if (mine[other] >= 0)
        *(double*)(begin + mine[other] * LAYER_STRIDE + OFF_GAIN_CUR) = g_mix * g_xf;
    if (t_bos && *t_bos > 0.0)
        *(double*)(begin + boss * LAYER_STRIDE + OFF_GAIN_CUR) = 1.0 - g_mix;

}

#define OFF_BLOCK_FRAMES 0x198


// --------------------------------------------------------------------
// the two radio stems, and keeping them on the grid
// --------------------------------------------------------------------
static int stream_chunk(void* stream) {
    return stream ? *(const int*)((const unsigned char*)stream + OFF_CHUNK_INDEX) : -1;
}

typedef void (*PullFn)(void* self, void* out);
typedef void (*RewindFn)(void* decoder);
static PullFn   g_next_pull;
static RewindFn g_rewind;

// --------------------------------------------------------------------
// the decode path: trimming and looping our stems
// --------------------------------------------------------------------
// Note for anyone chaining here: this calls the trampoline more than once per
// invocation - up to SKIP_PER_PULL extra times while discarding the count-in.
// That is legitimate (a hook may call through as often as it likes) but a
// higher-priority observer counting calls will see more than the game made.
static void hooked_pull(void* self, void* out) {
    InterlockedIncrement(&g_pulls_any);
    const int si = stem_of(self);
    if (si < 0 || stream_chunk(self) != g_chunk_radio) { g_next_pull(self, out); return; }
    Stem* st = &g_stem[si];

    if (InterlockedExchange(&st->realign, 0)) {
        // Chunk layout read out of the pull itself: the chunk vector is at
        // stream+0x170 with a 0x10 stride, the live index at stream+0x188,
        // and the decoder is the first field of the chunk.
        unsigned char* vec = *(unsigned char**)((unsigned char*)self + OFF_CHUNK_VEC);
        const int idx = *(const int*)((unsigned char*)self + OFF_CHUNK_INDEX);
        if (vec && idx >= 0 && idx < 64) {
            void* decoder = *(void**)(vec + (size_t)idx * 0x10);
            if (decoder && g_rewind) {
                g_rewind(decoder);
                st->skip_left = st->skip_req;
                st->skipped   = 0;
            }
        }
    }

    // Discard, a few blocks at a time - decoding 2.9s inside one call is a
    // good way to miss an audio deadline. Each call overwrites the same output
    // block, so nothing discarded here reaches the mixer.
    if (st->skip_left > 0) {
        for (int i = 0; i < SKIP_PER_PULL && st->skip_left > 0; i++) {
            g_next_pull(self, out);
            const int got = *(const int*)((const unsigned char*)out + 8);
            if (got <= 0) { st->skip_left = 0; break; }
            if (g_block <= 0) g_block = got;
            st->skip_left -= got;
            st->skipped   += got;

            // Ask for exactly the remainder, so the skip ends on the sample we
            // want rather than on the next block boundary.
            if (st->skip_left > 0 && st->skip_left < g_block)
                *(int*)((unsigned char*)self + OFF_BLOCK_FRAMES) = (int)st->skip_left;
        }
        if (g_block > 0)
            *(int*)((unsigned char*)self + OFF_BLOCK_FRAMES) = (int)g_block;

        if (st->skip_left <= 0) {
            st->skip_left = 0;
            st->pos = st->skipped - SKIP_SAMPLES;   // negative while counting in
        }
    }

    g_next_pull(self, out);

    if (si == g_live) converge(self, st);

    if (st->skip_left <= 0) {
        const int got = *(const int*)((const unsigned char*)out + 8);
        if (got > 0) {
            InterlockedIncrement(&g_pulls);
            st->pos += got;

            // If there is no second copy, fall back to splicing this one at
            // the loop point - the behaviour before the ping-pong existed. The
            // seam is audible, but the stem stays on the Bunga grid, which is
            // the thing that must not be lost. Without this a missing second
            // layer would let the live one run into its own 203.051s loop and
            // drift 8.765s out, silently.
            if (si == g_live && !g_stem[si ^ 1].stream && st->pos >= BODY_SAMPLES) {
                st->skip_req = SKIP_SAMPLES;
                st->pos      = 0;
                InterlockedExchange(&st->realign, 1);
                say("no second copy - splicing copy %d at the loop point", si);
            }

            // Prime the other copy so it reaches the downbeat exactly as this
            // one runs out of body: it needs SKIP_SAMPLES of count-in, and
            // there are (BODY - pos) samples left, so it starts that much of
            // the count-in already behind it. Computed from the position
            // rather than triggered on a deadline, so how often we get to look
            // does not matter.
            if (si == g_live && !g_armed && g_stem[si ^ 1].stream) {
                const long long remaining = BODY_SAMPLES - st->pos;

                // Lands exactly on the seam, never a bar either side. Delaying it blended
                // better but stretched the radio's loop past the Bunga song's, and the shared
                // grid is the whole point.
                if (remaining <= SKIP_SAMPLES && remaining > 0) {
                    Stem* nx = &g_stem[si ^ 1];
                    nx->skip_req = (long)(SKIP_SAMPLES - remaining);
                    InterlockedExchange(&nx->realign, 1);
                    InterlockedExchange(&g_armed, 1);
                    say("priming the other copy: %lld samples of body left, "
                        "skipping %ld of its count-in",
                        remaining, nx->skip_req);
                }
            }
        }
    }
}


// Stop trusting the acting pointer. EndTurn is the ordinary case; Die is the
// one that matters, since a cat can be killed in the middle of its own turn.
typedef void (*EndTurnFn)(void* self);
typedef void (*DieFn)(void* self, bool a, void* b, bool c);
static EndTurnFn g_next_endturn;
static DieFn     g_next_die;

// --------------------------------------------------------------------
// combat hooks: whose turn it is
// --------------------------------------------------------------------
static void hooked_endturn(void* self) {
    if (self && self == g_acting) g_acting = 0;
    g_next_endturn(self);
}

static void hooked_die(void* self, bool a, void* b, bool c) {
    if (self && self == g_acting) g_acting = 0;
    g_next_die(self, a, b, c);
}

typedef void (*UpdateFn)(void* self);
static UpdateFn g_next_update;

static void hooked_update(void* self) {
    g_next_update(self);
    // Nothing is added here: the layer goes in through the AddLayer hook,
    // while the builder is still assembling the set.
    if (self) {
        force_layers((unsigned char*)self + 0x50, 0);
        force_layers((unsigned char*)self + 0x78, 1);
    }
}

static bool ends_with(const char* s, const char* suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

// --------------------------------------------------------------------
// music hooks: what is being queued, and where
// --------------------------------------------------------------------
static void hooked_queue(void* self, void* path, int onfinish) {
    const char* p = str_of(path);
    if (p && ends_with(p, "_intro.ogg") && strlen(p) < sizeof(g_intro_path)) {
        lstrcpynA(g_intro_path, p, sizeof(g_intro_path));   // the zone's own sting
    }
    if (p && strcmp(p, RADIO_TRACK) == 0) {       // one of our two copies
        const int si = g_stem[0].stream ? 1 : 0;
        g_stem[si].stream   = self;
        g_stem[si].skip_req = SKIP_SAMPLES;        // trim the count-in
        g_stem[si].skipped  = 0;
        g_stem[si].pos      = 0;
        InterlockedExchange(&g_stem[si].realign, 1);
        // Our chunk's index is however many are already queued on this stream
        // - 1 when it follows an intro, 0 without one.
        g_chunk_radio = *(const int*)((const unsigned char*)self + OFF_CHUNK_COUNT);
        say("radio copy %d is streaming: %p", si, self);
    }
    // Which zone's music is loading. Every layer path is <zone>/<name>_kind.ogg,
    // so the set that is being queued says where we are.
    if (p && (ends_with(p, "_map.ogg") || ends_with(p, "_battle.ogg")
           || ends_with(p, "_boss.ogg") || ends_with(p, "_event.ogg")))
        InterlockedExchange(&g_in_iceage, strstr(p, "iceage/") ? 1 : 0);

    if (p && ends_with(p, "_battle.ogg")) g_stream_battle = self;
    if (p && ends_with(p, "_boss.ogg"))   g_stream_boss   = self;

    if (p) TRACE("pass %s", p);
    g_next(self, path, onfinish);
}

// Records the acting cat, and logs the verdict once per turn.
static void hooked_turn(void* self, int kind) {
    if (self) {
        const int intel = *(const int*)((const char*)self + OFF_INT);
        const bool player = *(const unsigned char*)((const char*)self + OFF_IS_PLAYER_CAT) != 0;
        if (player) {
            const bool radio = is_radio_cat(self);
            g_acting = self;
            g_want = radio ? LAYER_BOSS : LAYER_BATTLE;
            say("turn: player cat=%p INT=%d -> %s", self, intel,
                radio ? "RADIO" : "vanilla");
        } else {
            say("turn:   NPC  cat=%p INT=%d -> -", self, intel);
        }
    }
    g_next_turn(self, kind);
}


// Our layers, added from inside the builder's own add loop so its start pass
// brings them up with the other four. AddLayer alone leaves a layer that
// nothing decodes for; the builder does two more things per layer afterwards.
//
// The add loop is bounded by the TRACKLIST and re-reads it each iteration, so
// growing the layer vector underneath it is safe.
typedef void (*AddLayerFn)(void* group, void* core, int index,
                           const void* paths, const void* finishes);
// Added while the builder is still building. A layer added after its start
// pass has run joins however many frames later the update loop happens to run,
// and that lateness varies - which made the alignment a lottery.
static AddLayerFn    g_next_add;
static volatile long g_adding_ours;
#define LAST_GAME_LAYER 3            // map, battle, event, boss



// std::vector is three pointers: begin, end, capacity-end. The callee only
// reads them, and copies what it needs out, so these can point at our own
// storage. The string itself is built through the game's allocator, because
// anything downstream that copies or frees it must find a buffer it owns.
struct Vec3 { const void* begin; const void* end; const void* cap; };

// --------------------------------------------------------------------
// building our layers inside the game's own builder
// --------------------------------------------------------------------
// Our layer is built as [intro, radio], mirroring the game's own [intro,
// track]. Every layer of a set begins with the zone's intro sting - all four
// consume an identical-length chunk before their body starts, which is exactly
// why they stay sample-aligned. A layer without one starts its body about six
// seconds early and is out of step with everything else for the rest of the
// fight.
static GameString  g_layer_paths[2];
static int         g_layer_finish[2] = { 2, 2 };   // advance from intro, then loop
static void hooked_add_layer(void* group, void* core, int index,
                             const void* paths, const void* finishes) {
    g_next_add(group, core, index, paths, finishes);

    if (index != LAST_GAME_LAYER || !g_in_iceage) return;
    if (InterlockedExchange(&g_adding_ours, 1)) return;      // never re-enter

    const bool with_intro = g_intro_path[0] != 0;
    int n = 0;
    if (with_intro)
        make_game_string(&g_layer_paths[n++], g_intro_path, strlen(g_intro_path));
    make_game_string(&g_layer_paths[n++], RADIO_TRACK, sizeof(RADIO_TRACK) - 1);

    const Vec3 our_paths    = { &g_layer_paths[0], &g_layer_paths[n], &g_layer_paths[n] };
    const Vec3 our_finishes = { &g_layer_finish[0], &g_layer_finish[n], &g_layer_finish[n] };

    g_want = -1; g_mix = 0.0; g_pulls = 0; g_block = 0;
    // Forget the previous set's streams before adding this one's. A set is
    // rebuilt on entering the boss room, and the slots used to fill on a
    // first-come basis - so copy 0 stayed pointed at a dead stream from the
    // hallway set while the new set's only layer took copy 1.
    g_stem[0].stream = 0;
    g_stem[1].stream = 0;
    g_live = 0; g_armed = 0; g_xf = 0.0; g_converged = 0;

    // Two copies, so one can cover the other's loop seam.
    say("adding two radio layers as %d and %d, inside the builder (intro=%s)",
        LAST_GAME_LAYER + 1, LAST_GAME_LAYER + 2, with_intro ? g_intro_path : "none");
    g_next_add(group, core, LAST_GAME_LAYER + 1, &our_paths, &our_finishes);
    g_next_add(group, core, LAST_GAME_LAYER + 2, &our_paths, &our_finishes);

    InterlockedExchange(&g_adding_ours, 0);
}

// --------------------------------------------------------------------
// install
// --------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);
    g_base = (unsigned char*)GetModuleHandleA(NULL);

    trace_open();

    // Mewjector is required. Without it common/hookapi.inc patches each site with
    // a fixed 24-byte steal never checked against an instruction boundary;
    // Mewjector passes 0 and its length disassembler gets it right. It is also the
    // only way these sites can be shared with another mod.
    if (!hookapi_init()) {
        say("legendofbunga: Mewjector not found - not installing "
            "(this mod needs its hook chaining, and its steal lengths)");
        return TRUE;
    }

    const int bad = verify_sites_chained(SITES, SITE_COUNT, SITE_SIGLEN, g_base, g_at,
                                         GAME_TIMESTAMP, GAME_SIZEOFIMAGE);
    if (bad >= 0) {
        // The one thing a user needs to know, so it goes where they will look.
        if (g_mj.Log)
            g_mj.Log("legendofbunga", "not installing: %s (rva 0x%x) does not match this build",
                     SITES[bad].name, SITES[bad].rva);
        say("legendofbunga: site %s (rva 0x%x) does not match this build - not installing",
            SITES[bad].name, SITES[bad].rva);
        return TRUE;
    }

    // Called, not hooked.
    g_append = (AppendFn)g_at[S_APPEND];
    g_rewind = (RewindFn)g_at[S_DECODER_REWIND];

    g_next = (QueueFn)install_hook(SITES[S_QUEUECHUNK].rva, g_at[S_QUEUECHUNK], 24,
                                   (const void*)&hooked_queue, "legendofbunga");
    g_next_add = (AddLayerFn)install_hook(SITES[S_ADD_LAYER].rva, g_at[S_ADD_LAYER], 24,
                                          (const void*)&hooked_add_layer, "legendofbunga");
    g_next_pull = (PullFn)install_hook(SITES[S_STREAM_PULL].rva, g_at[S_STREAM_PULL], 24,
                                       (const void*)&hooked_pull, "legendofbunga");
    g_next_update = (UpdateFn)install_hook(SITES[S_MLMP_UPDATE].rva, g_at[S_MLMP_UPDATE], 24,
                                           (const void*)&hooked_update, "legendofbunga");
    g_next_endturn = (EndTurnFn)install_hook(SITES[S_ENDTURN].rva, g_at[S_ENDTURN], 24,
                                             (const void*)&hooked_endturn, "legendofbunga");
    g_next_die     = (DieFn)install_hook(SITES[S_DIE].rva, g_at[S_DIE], 24,
                                         (const void*)&hooked_die, "legendofbunga");
    g_next_turn = (TurnFn)install_hook(SITES[S_BEGINTURN].rva, g_at[S_BEGINTURN], 24,
                                       (const void*)&hooked_turn, "legendofbunga");
    if (g_mj.Log)
        g_mj.Log("legendofbunga", "the Lord Bunga radio version: %s",
                 g_next ? "installed" : "FAILED to install");
    say("legendofbunga: queue=%s turn=%s  update=%s  (player cats at INT <= %d)",
        g_next ? "hooked" : "FAILED", g_next_turn ? "hooked" : "FAILED",
        g_next_update ? "hooked" : "FAILED", RADIO_INT_THRESHOLD);
    return TRUE;
}

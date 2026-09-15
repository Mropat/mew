// mewbunga - the Lord Bunga radio-version joke.
//
// Community legend says a cat with 0 INT makes the Lord Bunga fight play the
// radio version of its song, "Mom I Really Hate You". It does not, but the
// track is real: audio/music/radio.gon maps every zone's instrumental to a
// vocal radio counterpart and lists
//
//     mom_i_really_hate_you  //ice age
//
// while data/maps/iceage.gon puts Lord Bunga on the ice age boss node.
//
// The two are the same arrangement at the same tempo - both 164.10 BPM - and
// their onset envelopes lock at a constant +2.902s across the whole track,
// which is 7.94 beats: the radio version has a two-bar count-in. They are not
// the same master recording, so they never correlate as waveforms, but they
// play together perfectly once that offset is taken out. That is what the
// layered version of this mod will need.
//
// THIS BUILD IS THE FIRST STEP AND IS DELIBERATELY BLUNT. It swaps the track
// in EVERY fight, with no INT check, so the mechanism can be tested without
// walking to the ice age boss every time. What it proves: that rewriting the
// path at SoundStream::QueueSongChunk actually controls what the game plays.
//
// Not the finished joke. The finished one queues the radio song into an idle
// layer slot and crossfades per character at BeginTurn, so the cat whose turn
// it is decides what you hear.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#define MOD_HOOK_PRIORITY 50
#include "hookapi.inc"
#include "sites.inc"

#define RADIO_TRACK "audio/music/radio/songs/mom_i_really_hate_you.ogg"
#define BUNGA_TRACK "audio/music/iceage/iceage_boss.ogg"

// TEST BUILD SWITCH - not how the mod should ship.
//
// With this on, every other music layer in the adventure becomes the Lord
// Bunga track, so the pairing you hear anywhere is the real one: the ice age
// boss instrumental against its own vocal radio version. Off, the radio
// version is crossfaded against whatever zone you happen to be in, which
// sounds like two different songs because it is.
//
// It also makes the loop behaviour audible. The instrumental is 194.286s and
// the radio version 203.051s, both looping independently, so they drift 8.765s
// further apart every time round.
#define MEWBUNGA_ALL_BUNGA 0

// Which layer carries the radio version.
//
// 1 = a fifth layer of our own, added through the game's AddLayer. Cleanest in
//     principle - it steals nothing - but currently INERT: the layer and its
//     stream are created and the chunk is queued, yet the mixer never pulls it
//     (pulls=0 while other streams pull fine). SoundStream's constructor
//     leaves [stream+0] null and the gain loop skips any layer whose stream
//     has it null, so something still has to start playback, and that call has
//     not been found yet.
//
// 0 = borrow the event layer, which works. It costs the ice age's event
//     encounter music, and nothing else: every part of this mod is gated on
//     being in Lord Bunga's zone, so no other zone is touched at all.
#define MEWBUNGA_OWN_LAYER 1

// The radio version carries a two-bar count-in, so at any moment it is playing
// content 2.902s earlier in the arrangement than the instrumental - which is
// why a switch sounds a couple of bars out.
//
// Holding the instrumental layers back by that amount was tried and reverted.
// It aligned them, but it assumed the stream would still be alive when the
// held chunk came due - and crossing into the shop tears the music set down
// and builds a new one. The chunks were released into streams that no longer
// mattered, and the next fight had no music at all. The same shape of mistake
// could have been a use-after-free rather than silence.
//
// The alignment still needs doing, but through the decoder's own position
// rather than by second-guessing when the game wants a chunk queued.

// Who gets the radio version.
//
// The legend's rule - 0 INT - is the one the mod is named for, but a 0 INT cat
// is not something you can produce on demand, so it cannot be the only trigger
// or the mod is untestable and almost never fires in a real run. Every other
// cat gets a fixed draw instead: one cat in RADIO_CHANCE_IN_N hears it.
//
// The draw is deterministic per cat, not per turn. A cat that is a radio cat
// stays one for the whole fight, because a coin flipped every turn would make
// the music flap back and forth and read as a bug rather than a joke.
#define RADIO_INT_THRESHOLD 0        // INT at or below this: always
#define RADIO_CHANCE_PERCENT 0       // everyone else: this many percent
                                     //
                                     // 0 while the legend's own condition is
                                     // being tested, so the only cat that can
                                     // trigger it is one at or below the INT
                                     // threshold - nothing else can muddy the
                                     // result. Put it back to 50 afterwards,
                                     // or the joke almost never fires in a
                                     // real run.

// is_player_cat, a byte written by Character::init - it is the gon property
// that is "true" in data/characters/player_cat.gon and "false" in enemies.gon
// and finalboss.gon. Found at the store "mov byte ptr [r14+0x489], al" right
// after init parses the literal "is_player_cat" at 0xf9fc5.
//
// Enemies and arena scenery take turns too, and they have stats like anything
// else - a LordBunga has intelligence 5 - so without this the music would flip
// on the boss's turn as readily as on yours.
#define OFF_IS_PLAYER_CAT 0x489

// Character stat block, from the buff applier at 0x7d610, which does
// "add dword ptr [rdx+0x5bc], ecx" immediately after loading "strength":
//   +0x5bc str  +0x5c0 dex  +0x5c4 con  +0x5c8 int  +0x5cc spd  +0x5d0 cha
// The same function reads current HP at +0x4b0, which is where the combat
// probe independently put it, so the block is anchored to something known.
#define OFF_INT 0x5c8

static unsigned char* g_base;
static unsigned char* g_at[SITE_COUNT];
static FILE*          g_log;
static unsigned       g_swaps;

typedef void (*QueueFn)(void* self, void* path, int onfinish);
typedef void (*TurnFn)(void* self, int kind);
static QueueFn g_next;
static TurnFn  g_next_turn;

static void say(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc(10, g_log); fflush(g_log);
}

// MSVC std::string: 16-byte SSO buffer, size at +0x10, capacity at +0x18.
// Capacity over 15 means +0x00 holds a heap pointer instead of the characters.
static const char* str_of(const void* s) {
    if (!s) return 0;
    const unsigned long long cap = *(const unsigned long long*)((const char*)s + 0x18);
    return cap > 15 ? *(const char* const*)s : (const char*)s;
}

// A std::string we hand to the game, built with the game's own allocator.
//
// The first attempt pointed a hand-built string at a static literal and passed
// that. It crashed with 0xC0000374 - heap corruption - inside ntdll with
// QueueSongChunk on the stack: something in there frees the string's buffer,
// and a static literal is not the game's to free. Nor would our own malloc
// have been, since this DLL's CRT heap is not the game's.
//
// std::string::append grows through the game's own allocator, which makes the
// result safe for it to free, move or keep. A fresh object every call, never
// reused: if the game does take ownership, reusing one would mean appending
// into a freed pointer the second time round.
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

// splitmix64, so the draw is spread evenly over pointers that differ only in
// their low bits - characters in one battle are allocated close together, and
// a plain modulo of the address would put whole runs of them on the same side.
static unsigned long long mix64(unsigned long long x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Randomised once per launch, so the draw is genuinely unpredictable rather
// than a fixed function of an address, and differs between playthroughs.
static unsigned long long g_seed;

static void seed_draw(void) {
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    g_seed = mix64((unsigned long long)qpc.QuadPart)
           ^ mix64((unsigned long long)GetTickCount64())
           ^ mix64((unsigned long long)GetCurrentProcessId());
}

// Mixing the session seed with the character's address rather than drawing
// from a running RNG is deliberate: the answer has to be the same every time
// it is asked for a given cat. A fresh draw per turn would make the music flap
// back and forth mid-fight and read as a bug rather than a joke. Characters
// are new objects each battle, so the same cat draws again next fight, and
// differently next launch.
static bool is_radio_cat(void* self) {
    if (!self) return false;
    if (!*(const unsigned char*)((const char*)self + OFF_IS_PLAYER_CAT)) return false;
    const int intel = *(const int*)((const char*)self + OFF_INT);
    if (intel <= RADIO_INT_THRESHOLD) return true;
    return (mix64((unsigned long long)self ^ g_seed) % 100) < RADIO_CHANCE_PERCENT;
}

// Where the layer set lives.
//
// Layers are held in a std::vector<Layer> with a 0x30 stride, reached through
// the component's layer group: begin at group+0x00, end at group+0x08. Read
// fresh every frame rather than cached - a vector that grows moves its
// elements, and a cached Layer pointer becomes freed memory.
// The zone whose music is currently loaded. The joke belongs to one fight, so
// everything is gated on being in the ice age - Lord Bunga's zone, and the one
// the radio version is the counterpart of.
static volatile long g_in_iceage;

// Which chunk of the event layer is live: the zone's own event music, or ours.
//
// Not constants. Entering the Bunga room rebuilds the layer set and queues
// onto the same stream, so chunks accumulate - the list was three long after
// one rebuild - and a hardcoded index 1 stopped pointing at our track. Both
// indices are recorded as the chunks are appended.
static volatile long g_chunk_event = 0;
static volatile long g_chunk_radio = 1;
static void select_chunk(void* stream, int which);

#define LAYER_STRIDE   0x30
#define OFF_GAIN_CUR   0x20      // current gain: ramped toward the target
#define OFF_GAIN_TGT   0x28      // target gain: 1.0 audible, 0.0 silent
#define LAYER_BATTLE   0         // queue order: battle, map, event, boss
#define LAYER_BOSS     3

// Which fight layer the current cat should hear, or -1 for "do not interfere".
static volatile int g_want = -1;

// The cat currently taking its turn, or null when there is nobody to ask.
//
// Its INT is re-read every frame rather than once at BeginTurn, so a cat that
// makes itself stupid DURING its own turn hears the music change immediately
// instead of on its next turn.
//
// The pointer is only ever read between BeginTurn and EndTurn, and is dropped
// the moment the cat dies. That window is the whole reason this is safe: a
// character freed mid-fight would otherwise be read every frame forever.
static void* volatile g_acting;

// Force the choice every frame, after the game has updated.
//
// Writing the gains once at BeginTurn was not enough: the music switched for
// about a second and then slid back. The ramp at 0xa1a7e0 only moves current
// toward target, so it was not the culprit - something re-set the TARGET, and
// finding what was more work than simply having the last word. Setting the
// target after the game's own update each frame does that, and setting only
// the target (never current) leaves the crossfade to the game, so the switch
// fades the way its own transitions do.
//
// The override is deliberately self-limiting. It only ever redirects a choice
// the game has ALREADY made between the two fight layers: if the game wants
// neither - in the hallway, on the map, during an event - both targets are
// zero, and this leaves them alone. So it can change which fight music plays
// but can never start music the game did not ask for.
// The streams, so layers can be identified by what they play rather than by a
// guessed index. The queue order happens to be battle, map, event, boss - but
// that is an observation about two zones, not a promise, and a set with a
// missing slot would shift it. Matching on the stream pointer cannot drift.
static char           g_intro_path[128];  // the zone's intro sting, seen as it is queued
static void* volatile g_stream_radio;    // the layer that carries the radio version
static void* volatile g_stream_battle;
static void* volatile g_stream_boss;

// How long a switch takes, in milliseconds. The game's own transitions glide
// over roughly a second, so this is in the same register.
#define FADE_MS 900.0

// Our own fade envelope: 0 = the game's music, 1 = the radio version.
//
// Nudging the gain by a fixed amount per frame fought the game's ramp pulling
// the same value the other way, and gave a fade-in that was quick and uneven
// while the fade-out - performed by the game once we stopped writing - sounded
// right. Assigning an envelope we compute ourselves settles it: the shape and
// duration are ours, and the game's ramp cannot argue. Timed off the clock, so
// it lasts the same wall-clock time at any frame rate.
static double        g_mix;
static unsigned long g_mix_tick;

// Defined with the stem state below; declared here because the gain override
// refuses to act unless the stem is demonstrably producing audio.
extern volatile long g_pulls;
extern volatile long g_pulls_any;
extern long          g_skip_left;
extern long long     g_pos;   // defined with the stem state below

static void force_layers(unsigned char* group, int which) {
    unsigned char* begin = *(unsigned char**)(group + 0x00);
    unsigned char* end   = *(unsigned char**)(group + 0x08);
    if (!begin || !end || end <= begin) return;
    const size_t count = (size_t)(end - begin) / LAYER_STRIDE;
    if (count < 2 || count > 8) return;

    int radio = -1, boss = -1;
    for (size_t i = 0; i < count; i++) {
        void* st = *(void**)(begin + i * LAYER_STRIDE);
        if (st && st == g_stream_radio)  radio  = (int)i;
        if (st && st == g_stream_boss)   boss   = (int)i;
    }
    if (radio < 0) return;               // not the set we swapped into

    // Only the boss layer. The legend is about the Lord Bunga fight, not about
    // ice age skirmishes, so an ordinary battle in the zone is left alone.
    double* t_bos = boss >= 0 ? (double*)(begin + boss * LAYER_STRIDE + OFF_GAIN_TGT) : 0;
    const bool fighting = g_in_iceage && t_bos && *t_bos > 0.0;

    const unsigned long now = GetTickCount();

    // Reported before the early returns, not after them. The previous version
    // sat at the end of the function and so could only print in the one case
    // that needed no explaining.
    static unsigned long s_last;
    if ((now - s_last) > 3000) {
        s_last = now;
        // Compare our layer's stream against the game's own. A stream is born
        // with +8 pointing at the AudioCore and +0 null; the voice at +0 is
        // created when its first chunk is queued. If ours has a null voice
        // where the game's four have one, that is why nothing pulls it.
        for (size_t i = 0; i < count; i++) {
            const unsigned char* ls = *(const unsigned char* const*)(begin + i * LAYER_STRIDE);
            say("  layer %zu stream=%p voice=%p core=%p chunks=%d idx=%d%s",
                i, (const void*)ls,
                ls ? *(void* const*)ls : 0,
                ls ? *(void* const*)(ls + 8) : 0,
                ls ? *(const int*)(ls + 0x18c) : -1,
                ls ? *(const int*)(ls + 0x188) : -1,
                (const void*)ls == g_stream_radio ? "   <- ours" : "");
        }
        const unsigned char* st = (const unsigned char*)g_stream_radio;
        say("layers[%d]: radio=%d boss=%d fight=%d mix=%.2f pos=%.1fs pulls=%ld/%ld skip=%ld want=%d chunk=%d/%d",
            which, radio, boss, (int)fighting, g_mix,
            (double)g_pos / 44100.0, g_pulls, g_pulls_any, g_skip_left, g_want,
            st ? *(const int*)(st + 0x188) : -1,
            st ? *(const int*)(st + 0x18c) : -1);
    }

    const double dt = g_mix_tick ? (double)(now - g_mix_tick) : 0.0;
    g_mix_tick = now;
    // Ask the acting cat again, every frame. This is what makes a mid-turn
    // change audible at once; between turns there is nobody to ask and the
    // last verdict stands, so the music does not flap on enemy turns.
    void* acting = g_acting;
    if (acting) g_want = is_radio_cat(acting) ? LAYER_BOSS : LAYER_BATTLE;

    const double want = (fighting && g_want == LAYER_BOSS) ? 1.0 : 0.0;

    // When a layer is shared, put it on the right chunk while it is silent -
    // ours on the way in, the borrowed track back again once released. With a
    // layer of our own there is nothing to share and nothing to select.
    if (g_mix <= 0.0 && g_chunk_event >= 0)
        select_chunk(g_stream_radio, want > 0.0 ? g_chunk_radio : g_chunk_event);

    if (dt > 0.0 && dt < 500.0) {        // ignore a hitch or a loading pause
        const double step = dt / FADE_MS;
        if (g_mix < want) { g_mix += step; if (g_mix > want) g_mix = want; }
        else if (g_mix > want) { g_mix -= step; if (g_mix < want) g_mix = want; }
    }
    if (g_mix <= 0.0) return;            // released: the game's own gains stand

    // Never silence the game's music in favour of a layer that is not actually
    // producing audio. Entering the Bunga room once did exactly that - our
    // stem was added and faded up while decoding nothing, and the fight played
    // with ambience only. If the stem is not alive, this does nothing at all.
    if (MEWBUNGA_OWN_LAYER && g_pulls <= 0) {
        *(double*)(begin + radio * LAYER_STRIDE + OFF_GAIN_CUR) = 0.0;
        return;
    }

    // Assign, do not nudge. Only the fight layer the game actually has up is
    // damped - raising the other one would put two layers on at once.
    *(double*)(begin + radio * LAYER_STRIDE + OFF_GAIN_CUR) = g_mix;
    if (t_bos && *t_bos > 0.0)
        *(double*)(begin + boss * LAYER_STRIDE + OFF_GAIN_CUR) = 1.0 - g_mix;

}

// Trim the radio version into a stem of the Bunga theme.
//
// Measured from the two files: the instrumental is exactly 8,568,000 samples
// (194.2857s) and the radio version 8,954,535. The radio carries a two-bar
// count-in of 127,808 samples - 7.93 beats at their shared 164.10 BPM,
// measured by correlating onset envelopes at 64-sample resolution - and a
// four-bar tail of 258,727 samples afterwards.
//
// Discard both and what remains is the same body, the same length as the
// instrumental. Loop it on the INSTRUMENTAL's length rather than its own and
// the two stay locked together indefinitely, so a switch lands on the same bar
// and the same beat.
//
// This replaces restarting the track at each switch. Restarting was consistent
// but it always entered at the top of the song, no matter where the fight's
// music had got to. Looping it as a stem means it is simply always in the
// right place, and nothing needs to happen at the moment of the switch at all.
#define SKIP_SAMPLES   127808          // the two-bar count-in
#define BODY_SAMPLES   8568000         // the instrumental's exact length
#define SKIP_PER_PULL  8               // blocks discarded per call, to spread the work

// A layer holds a LIST of chunks, and which one plays is just an int.
//
//   [stream+0x188]  active chunk index      [stream+0x18c]  chunk count
//   [chunk+8]       OnFinish: 0 advance, 1 loop in place, 2 advance-or-loop
//
// So the event layer does not have to be sacrificed. It carries two chunks -
// the zone's real event music at index 0, the radio version at index 1 - both
// set to loop in place, and the index says which is playing. Event encounters
// get their own music; only Bunga's fight switches to index 1.
//
// Both chunks are given OnFinish 1 rather than the 2 the game would use. With
// a single chunk those are identical (advance-or-loop on the last chunk IS
// loop), but with two chunks a 2 would run the event music into the radio
// version the moment it reached its end.
#define ONFINISH_LOOP  1

volatile long        g_block;          // samples a single decode block yields
volatile long        g_pulls;          // blocks our stem has actually produced
volatile long        g_pulls_any;      // blocks ANY stream has produced through this hook
long                 g_skip_left;      // samples still to discard
long long            g_pos;            // samples of body played since alignment
static volatile long g_realign;        // rewind and re-trim at the next pull
static long long     g_carry;          // overshoot past the loop point, preserved

#define OFF_CHUNK_VEC   0x170
#define OFF_CHUNK_INDEX 0x188

static int stream_chunk(void* stream) {
    return stream ? *(const int*)((const unsigned char*)stream + OFF_CHUNK_INDEX) : -1;
}

// Only ever called while the layer is inaudible, so a switch cannot be heard.
static void select_chunk(void* stream, int which) {
    if (!stream || stream_chunk(stream) == which) return;
    InterlockedExchange((volatile long*)((unsigned char*)stream + OFF_CHUNK_INDEX), which);
    if (which == g_chunk_radio) {
        g_pos = 0; g_carry = 0; g_skip_left = 0;
        InterlockedExchange(&g_realign, 1);   // trim the count-in before it plays
    }
}

typedef void (*PullFn)(void* self, void* out);
typedef void (*RewindFn)(void* decoder);
static PullFn   g_next_pull;
static RewindFn g_rewind;

static void hooked_pull(void* self, void* out) {
    const bool ours = self && self == g_stream_radio
                   && stream_chunk(self) == g_chunk_radio;
    InterlockedIncrement(&g_pulls_any);   // is this hook on the decode path at all?

    if (ours && InterlockedExchange(&g_realign, 0)) {
        // Chunk layout read out of the pull itself: the chunk vector is at
        // stream+0x170 with a 0x10 stride, the live index at stream+0x188, and
        // the decoder is the first field of the chunk.
        unsigned char* vec = *(unsigned char**)((unsigned char*)self + 0x170);
        const int idx = *(const int*)((unsigned char*)self + 0x188);
        if (vec && idx >= 0 && idx < 64) {
            void* decoder = *(void**)(vec + (size_t)idx * 0x10);
            if (decoder && g_rewind) {
                g_rewind(decoder);
                // Carry the overshoot into the skip so the loop point stays
                // exact instead of slipping by up to a block every lap.
                g_skip_left = (long)(SKIP_SAMPLES + g_carry);
                g_pos = 0; g_carry = 0;

                // Then close the gap to the fight music.
                //
                // Our layer joins a frame or two after the game builds the
                // set, so it runs that far behind for the rest of the fight -
                // audible as a slight looseness rather than as an offset. Each
                // stream counts the samples it has produced at +0x1a0, so the
                // difference between the boss layer's count and ours IS that
                // lateness, in samples. Skipping it catches us up.
                //
                // Forward only: if we are somehow ahead, there is nothing to
                // do but let it be, and the bound keeps a wild reading - a
                // stall, a stream that has only just started - from throwing
                // the alignment away entirely.
                if (g_stream_boss) {
                    const long long theirs = *(const long long*)((const unsigned char*)g_stream_boss + 0x1a0);
                    const long long mine   = *(const long long*)((const unsigned char*)self + 0x1a0);
                    const long long behind = theirs - mine;
                    if (behind > 0 && behind < 2 * 44100) g_skip_left += (long)behind;
                }
            }
        }
    }

    // Discard, a few blocks at a time. There is no seek to call, so skipping
    // means decoding and throwing away - and 2.9s of vorbis decode inside one
    // call is a good way to miss an audio deadline. Each call overwrites the
    // same output block, so nothing discarded here reaches the mixer.
    if (ours && g_skip_left > 0) {
        for (int i = 0; i < SKIP_PER_PULL && g_skip_left > 0; i++) {
            g_next_pull(self, out);
            const int got = *(const int*)((const unsigned char*)out + 8);
            if (got <= 0) { g_skip_left = 0; break; }   // chunk ended: stop

            // Blocks are indivisible, so the skip can only ever land on a
            // block boundary - and always discarding until the target is
            // passed lands on the boundary AFTER it, every time, leaving us
            // that much ahead of the fight music.
            //
            // Once the block size is known, aim at the NEAREST boundary
            // instead. That halves the worst case and, more importantly, lets
            // the error fall either side of the mark rather than always ahead.
            if (g_block <= 0 && got > 0) {
                g_block = got;
                const long total = g_skip_left + got;          // what we set out to discard
                const long blocks = (total + got / 2) / got;   // nearest, not next
                g_skip_left = (blocks > 0 ? blocks * got : got) - got;
                say("block=%ld samples; aiming %ld blocks (%.0fms error)",
                    (long)got, blocks,
                    ((double)(blocks * got - total) / 44.1));
            }
            g_skip_left -= got;
        }
        // Blocks are whole, so the last one always overshoots. Those samples
        // are already part of the body, so count them - otherwise the loop
        // period runs long by up to a block every lap.
        if (g_skip_left < 0) { g_pos = -g_skip_left; g_skip_left = 0; }
    }

    g_next_pull(self, out);

    if (ours && g_skip_left <= 0) {
        const int got = *(const int*)((const unsigned char*)out + 8);
        if (got > 0) {
            InterlockedIncrement(&g_pulls);
            g_pos += got;
            if (g_pos >= BODY_SAMPLES) {            // end of the body: loop it
                g_carry = g_pos - BODY_SAMPLES;
                InterlockedExchange(&g_realign, 1);
            }
        }
    }
}

static void add_our_layer(unsigned char* group, int which);

// Stop trusting the acting pointer. EndTurn is the ordinary case; Die is the
// one that matters, since a cat can be killed in the middle of its own turn.
typedef void (*EndTurnFn)(void* self);
typedef void (*DieFn)(void* self, bool a, void* b, bool c);
static EndTurnFn g_next_endturn;
static DieFn     g_next_die;

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
    // Tried every frame rather than on a flag. A single global "a set was
    // built" flag was consumed by whichever MultiLayerMusicPlayer updated
    // first - the ambience or title player, not necessarily the one holding
    // the zone's music - so the add never reached the right group. Checking
    // whether our stream is already present makes retrying free and correct.
#if MEWBUNGA_OWN_LAYER
    if (self && g_in_iceage) {
        add_our_layer((unsigned char*)self + 0x50, 0);
        add_our_layer((unsigned char*)self + 0x78, 1);
    }
#endif
    if (self) {
        force_layers((unsigned char*)self + 0x50, 0);
        force_layers((unsigned char*)self + 0x78, 1);
    }
}

static bool ends_with(const char* s, const char* suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

// The radio version goes in the EVENT layer.
//
// All four layers of a zone - map, battle, event, boss - stream in parallel
// from the moment the level loads, and the game crossfades between them. One
// of them can be borrowed to carry the radio version at no cost, as long as
// it is one no fight ever uses.
//
// The boss slot was the obvious choice and the wrong one: this is a mod about
// a boss fight, and in the real Lord Bunga fight the game wants that slot, so
// borrowing it leaves no vanilla boss music to switch back to. Event is idle
// in ordinary fights AND in boss fights, so it works for both.
//
// The cost, and it is a real one: music for event encounters becomes the radio
// version, since that layer now holds it whenever the game chooses event on
// its own. Every slot has some such cost - map is the hallway music, which
// would be worse - and event is the one heard least during the joke.
static bool is_fight_track(const char* p) {
#if MEWBUNGA_OWN_LAYER
    (void)p;
    return false;                        // we bring our own layer
#else
    return ends_with(p, "_event.ogg");   // borrow the one no fight uses
#endif
}

// The two tracks are the same arrangement at the same tempo - both 164.10 BPM,
// with their onset envelopes locking at a constant +2.902s across the whole
// track, which is 7.94 beats: the radio version carries a two-bar count-in.
// They are not the same recording, so they never correlate as waveforms, but
// they sit together properly once that offset is accounted for. Nothing here
// accounts for it yet: both layers start when the level loads, so the radio
// version runs two bars behind.
static void queue_as(void* self, const char* track, unsigned long long len,
                     void* path, int onfinish) {
    GameString replacement;
    make_game_string(&replacement, track, len);
    say("swap %s -> %s", str_of(path), track);
    g_swaps++;
    g_next(self, &replacement, onfinish);
}

static void hooked_queue(void* self, void* path, int onfinish) {
    const char* p = str_of(path);
    if (p && ends_with(p, "_intro.ogg") && strlen(p) < sizeof(g_intro_path)) {
        lstrcpynA(g_intro_path, p, sizeof(g_intro_path));   // the zone's own sting
    }
    if (p && strcmp(p, RADIO_TRACK) == 0) {       // our own layer, starting up
        g_stream_radio = self;
        // Our chunk's index is however many are already queued on this stream
        // - 1 when it follows an intro, 0 without one.
        g_chunk_radio = *(const int*)((const unsigned char*)self + 0x18c);
        g_chunk_event = -1;                       // nothing shares this layer
        g_pos = 0; g_carry = 0; g_skip_left = 0;
        InterlockedExchange(&g_realign, 1);        // trim the count-in first
        say("our layer is streaming: %p", self);
    }
    // Which zone's music is loading. Every layer path is <zone>/<name>_kind.ogg,
    // so the set that is being queued says where we are.
    if (p && (ends_with(p, "_map.ogg") || ends_with(p, "_battle.ogg")
           || ends_with(p, "_boss.ogg") || ends_with(p, "_event.ogg")))
        InterlockedExchange(&g_in_iceage, strstr(p, "iceage/") ? 1 : 0);

    if (p && ends_with(p, "_battle.ogg")) g_stream_battle = self;
    if (p && ends_with(p, "_boss.ogg"))   g_stream_boss   = self;

    // Only in Lord Bunga's zone. The set queues its battle and map tracks
    // before its event track, so the zone is already known by the time this is
    // reached. Without this the substitution was global: every zone's event
    // layer carried the radio version, and an event anywhere in the run played
    // it instead of that zone's own music.
    if (p && g_in_iceage && is_fight_track(p)) {
        // Keep the real event music as chunk 0, and append the radio version
        // as chunk 1 on the same layer.
        g_stream_radio = self;
        g_pos = 0; g_carry = 0; g_skip_left = 0;
        g_next(self, path, ONFINISH_LOOP);
        g_chunk_event = *(const int*)((const unsigned char*)self + 0x18c) - 1;

        GameString second;
        make_game_string(&second, RADIO_TRACK, sizeof(RADIO_TRACK) - 1);
        g_next(self, &second, ONFINISH_LOOP);
        g_chunk_radio = *(const int*)((const unsigned char*)self + 0x18c) - 1;

        // Leave the layer on OUR chunk, so it is already looping in step by the
        // time anything wants to hear it.
        InterlockedExchange((volatile long*)((unsigned char*)self + OFF_CHUNK_INDEX),
                            g_chunk_radio);
        g_pos = 0; g_carry = 0; g_skip_left = 0;
        InterlockedExchange(&g_realign, 1);
        say("event layer: %s = chunk %ld, %s = chunk %ld (of %d)",
            p, g_chunk_event, RADIO_TRACK, g_chunk_radio,
            *(const int*)((const unsigned char*)self + 0x18c));
        return;
    }
    if (false) {
        g_stream_radio = self;
        g_pos = 0; g_carry = 0; g_skip_left = 0;
        InterlockedExchange(&g_realign, 1);   // trim the count-in before it is heard
        queue_as(self, RADIO_TRACK, sizeof(RADIO_TRACK) - 1, path, onfinish);
        return;
    }
#if MEWBUNGA_ALL_BUNGA
    if (p && (ends_with(p, "_map.ogg") || ends_with(p, "_battle.ogg")
                                       || ends_with(p, "_boss.ogg"))) {
        queue_as(self, BUNGA_TRACK, sizeof(BUNGA_TRACK) - 1, path, onfinish);
        return;
    }
#endif
    if (p) say("pass %s", p);
    g_next(self, path, onfinish);
}

// Nothing switches yet - the layer machinery is still being mapped - so for now
// this only reports what it WOULD do, once per cat per turn. That is enough to
// see the rule behave in a real fight: the same cats should keep their verdict
// all the way through, and roughly one in RADIO_CHANCE_IN_N of them should be
// radio cats.
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


// A layer of our own.
//
// Earlier builds borrowed the event layer, because a zone only ever uses four
// and event is the one no fight touches. It worked, but event encounters then
// played the radio version, which is not the joke - and every other slot is
// worse: boss is the one the actual Bunga fight needs, map is the hallway.
//
// The group can simply be told to hold five. AddLayer grows the vector and
// initialises the element, which is exactly what the game does for its own
// four, so the fifth is an ordinary layer that happens to be ours. The game's
// selector never chooses it, which is the point: its gain is nobody's business
// but this mod's.
typedef void (*AddLayerFn)(void* group, void* core, int index,
                           const void* paths, const void* finishes);
typedef void (*StreamStartFn)(void* stream, bool play);
typedef void (*StreamSpawnFn)(void* stream);
typedef void (*VoicePlayFn)(void* voice, int zero, int play);
static StreamStartFn g_stream_start;
static StreamSpawnFn g_stream_spawn;
static VoicePlayFn   g_voice_play;
static AddLayerFn g_add_layer;

typedef void (*LayerInitFn)(void* self, void* core, void* paths, void* finishes);
static LayerInitFn g_next_layer_init;
static void* volatile g_core;            // AudioCore, captured from InitStream

static void hooked_layer_init(void* self, void* core, void* paths, void* finishes) {
    if (core) g_core = core;             // AddLayer needs it; only InitStream is handed one
    g_next_layer_init(self, core, paths, finishes);
}

// std::vector is three pointers: begin, end, capacity-end. The callee only
// reads them, and copies what it needs out, so these can point at our own
// storage. The string itself is built through the game's allocator, because
// anything downstream that copies or frees it must find a buffer it owns.
struct Vec3 { const void* begin; const void* end; const void* cap; };

// Our layer is built as [intro, radio], mirroring the game's own [intro,
// track]. Every layer of a set begins with the zone's intro sting - all four
// consume an identical-length chunk before their body starts, which is exactly
// why they stay sample-aligned. A layer without one starts its body about six
// seconds early and is out of step with everything else for the rest of the
// fight.
static GameString  g_layer_paths[2];
static int         g_layer_finish[2] = { 2, 2 };   // advance from intro, then loop
// g_intro_path is declared earlier, next to the other queue-time state.

static void add_our_layer(unsigned char* group, int which) {
    // Say why, at most once a second, so a silent bail names itself instead of
    // costing another round trip.
    // One throttle PER GROUP. A single shared static meant the empty group's
    // message hid the real one's, which is how the last diagnostic run said
    // nothing useful.
    static unsigned long s_last[2];
    const unsigned long now = GetTickCount();
    const int slot = (which >= 0 && which < 2) ? which : 0;
    const bool tell = (now - s_last[slot]) > 2000;
    #define BAIL(...) do { if (tell) { s_last[slot] = now; say(__VA_ARGS__); } return; } while (0)

    unsigned char* begin = *(unsigned char**)(group + 0x00);
    unsigned char* end   = *(unsigned char**)(group + 0x08);
    if (!begin || !end || end < begin) BAIL("add: empty group (begin=%p end=%p)", begin, end);
    const size_t count = (size_t)(end - begin) / LAYER_STRIDE;
    if (count < 2 || count > 8) BAIL("add: %zu layers, not a music set", count);

    // A music set is four layers - map, battle, event, boss - and we are only
    // here while the ice age's music is loaded. Matching on remembered stream
    // pointers was fragile: they go stale the moment a set is rebuilt, which
    // happens on entering the boss room.
    for (size_t i = 0; i < count; i++)
        if (*(void**)(begin + i * LAYER_STRIDE) == g_stream_radio) return;  // already ours
    if (count != 4) BAIL("add[%d]: %zu layers, not a music set", which, count);
    if (!g_add_layer) BAIL("add: AddLayer address missing");

    // The AudioCore argument is passed through and never used: InitStream
    // stores it to shadow space at 0xa1a1df and reads it back nowhere, and
    // AddLayer only forwards its own rdx. The game itself passes null here, so
    // requiring one before adding a layer was blocking the whole feature over
    // an argument that does not matter.

    const bool with_intro = g_intro_path[0] != 0;
    int n = 0;
    if (with_intro)
        make_game_string(&g_layer_paths[n++], g_intro_path, strlen(g_intro_path));
    make_game_string(&g_layer_paths[n++], RADIO_TRACK, sizeof(RADIO_TRACK) - 1);

    const Vec3 paths    = { &g_layer_paths[0], &g_layer_paths[n], &g_layer_paths[n] };
    const Vec3 finishes = { &g_layer_finish[0], &g_layer_finish[n], &g_layer_finish[n] };
    // A new set is a new fight. Carrying the last one's verdict over meant the
    // override engaged the moment the boss music started, before any cat had
    // taken a turn.
    g_want = -1; g_mix = 0.0; g_pulls = 0;
    say("adding layer %zu for %s (group=%p)", count, RADIO_TRACK, group);
    g_add_layer(group, g_core, (int)count, &paths, &finishes);

    // Start it. The game starts every layer of a set in one pass when the set
    // is built - a pass that has already happened by the time we add ours, so
    // without this the layer is complete in every visible respect (stream,
    // voice, chunk, core) and simply never plays. That is exactly what the
    // diagnostics showed: a voice like all the others, and pulls=0 forever.
    // Finish the layer the way the builder does at 0xa1abba: spawn the
    // stream's decode task, then play its voice. Starting it with 0xb51670
    // alone was not enough - that only resumes a stream that already has a
    // task decoding for it, which is why ours had a voice, a chunk, and no
    // pulls at all.
    unsigned char* nb = *(unsigned char**)(group + 0x00);   // re-read: AddLayer may realloc
    unsigned char* ne = *(unsigned char**)(group + 0x08);
    if (nb && ne > nb) {
        void* st = *(void**)(ne - LAYER_STRIDE);
        void* voice = st ? *(void**)st : 0;
        if (st && voice && g_stream_spawn && g_voice_play) {
            g_stream_spawn(st);
            g_voice_play(voice, 0, 1);
            say("started our stream %p (voice %p)", st, voice);
        } else {
            say("could not start our stream %p (voice %p)", st, voice);
        }
    }
    #undef BAIL
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);
    g_base = (unsigned char*)GetModuleHandleA(NULL);

    char path[MAX_PATH];
    if (GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH)) {
        char* slash = strrchr(path, (char)92);
        if (slash) {
            lstrcpyA(slash + 1, "mod_logs");
            CreateDirectoryA(path, NULL);
            lstrcatA(path, "\\mewbunga.log");
            g_log = fopen(path, "w");
        }
    }

    seed_draw();

    hookapi_init();
    const int bad = verify_sites_chained(SITES, SITE_COUNT, SITE_SIGLEN, g_base, g_at,
                                         GAME_TIMESTAMP, GAME_SIZEOFIMAGE);
    if (bad >= 0) {
        say("mewbunga: site %s (rva 0x%x) does not match this build - not installing",
            SITES[bad].name, SITES[bad].rva);
        return TRUE;
    }

    g_append = (AppendFn)g_at[S_APPEND];

    g_next = (QueueFn)install_hook(SITES[S_QUEUECHUNK].rva, g_at[S_QUEUECHUNK], 24,
                                   (const void*)&hooked_queue, "mewbunga");
    g_rewind     = (RewindFn)g_at[S_DECODER_REWIND];
    g_add_layer    = (AddLayerFn)g_at[S_ADD_LAYER];
    g_stream_start = (StreamStartFn)g_at[S_STREAM_START];
    g_stream_spawn = (StreamSpawnFn)g_at[S_STREAM_SPAWN];
    g_voice_play   = (VoicePlayFn)g_at[S_VOICE_PLAY];
    // Only needed to capture the AudioCore for AddLayer, which the borrowed
    // layer does not use - but the hook is harmless and keeps the two paths
    // symmetrical.
    g_next_layer_init = (LayerInitFn)install_hook(SITES[S_LAYER_INIT].rva, g_at[S_LAYER_INIT],
                                       24, (const void*)&hooked_layer_init, "mewbunga");
    g_next_pull = (PullFn)install_hook(SITES[S_STREAM_PULL].rva, g_at[S_STREAM_PULL], 24,
                                       (const void*)&hooked_pull, "mewbunga");
    g_next_update = (UpdateFn)install_hook(SITES[S_MLMP_UPDATE].rva, g_at[S_MLMP_UPDATE], 24,
                                           (const void*)&hooked_update, "mewbunga");
    g_next_endturn = (EndTurnFn)install_hook(SITES[S_ENDTURN].rva, g_at[S_ENDTURN], 24,
                                             (const void*)&hooked_endturn, "mewbunga");
    g_next_die     = (DieFn)install_hook(SITES[S_DIE].rva, g_at[S_DIE], 24,
                                         (const void*)&hooked_die, "mewbunga");
    g_next_turn = (TurnFn)install_hook(SITES[S_BEGINTURN].rva, g_at[S_BEGINTURN], 24,
                                       (const void*)&hooked_turn, "mewbunga");
    say("mewbunga: queue=%s turn=%s  update=%s  (player cats: INT<=%d always, else %d%%)",
        g_next ? "hooked" : "FAILED", g_next_turn ? "hooked" : "FAILED",
        g_next_update ? "hooked" : "FAILED", RADIO_INT_THRESHOLD, RADIO_CHANCE_PERCENT);
    return TRUE;
}

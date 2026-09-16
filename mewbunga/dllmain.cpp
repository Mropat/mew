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

typedef void (*QueueFn)(void* self, void* path, int onfinish);
typedef void (*TurnFn)(void* self, int kind);
static QueueFn g_next;
static TurnFn  g_next_turn;

// Verbose per-frame diagnostics. They earned their keep while the alignment
// was being worked out; they are noise now. Build with -DMEWBUNGA_TRACE to get
// them back.
#ifdef MEWBUNGA_TRACE
#define TRACE(...) say(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

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

// Our track is the second chunk of our layer: [intro, radio]. Recorded as
// it is queued rather than assumed - a layer without an intro has it first.
static volatile long g_chunk_radio = 1;

#define BODY_SAMPLES   8568000         // the instrumental's exact length


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
extern volatile long g_block;   // frames per decode, 0 until the first is seen
extern volatile long g_pulls;
extern volatile long g_pulls_any;
extern long          g_skip_left;
extern long long     g_pos;   // defined with the stem state below

static void force_layers(unsigned char* group, int which) {
    (void)which;   // only used by the trace build
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

#ifdef MEWBUNGA_TRACE
    static unsigned long s_last;
    if ((now - s_last) > 3000) {
        s_last = now;
        const unsigned char* st = (const unsigned char*)g_stream_radio;
        say("layers[%d]: radio=%d boss=%d fight=%d mix=%.2f pos=%.1fs pulls=%ld/%ld "
            "skip=%ld want=%d chunk=%d/%d",
            which, radio, boss, (int)fighting, g_mix,
            (double)g_pos / 44100.0, g_pulls, g_pulls_any, g_skip_left, g_want,
            st ? *(const int*)(st + 0x188) : -1,
            st ? *(const int*)(st + 0x18c) : -1);
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
    if (g_mix <= 0.0) return;            // released: the game's own gains stand

    // Never silence the game's music in favour of a layer that is not actually
    // producing audio. Entering the Bunga room once did exactly that - our
    // stem was added and faded up while decoding nothing, and the fight played
    // with ambience only. If the stem is not alive, this does nothing at all.
    if (g_pulls <= 0) {
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
#define SKIP_PER_PULL  8               // blocks discarded per call, to spread the work

// A layer holds a LIST of chunks, and which one plays is just an int.
//
//   [stream+0x188]  active chunk index      [stream+0x18c]  chunk count
//   [chunk+8]       OnFinish: 0 advance, 1 loop in place, 2 advance-or-loop
//
// Our layer carries two chunks, [intro, radio], the same shape the game gives
// its own. OnFinish 1 loops a chunk in place; 2 advances, looping only on the
// last. The intro gets 2 so it advances into the track.
//
#define ONFINISH_LOOP  1

volatile long        g_block;          // samples a single decode block yields
static long          g_lag;            // how late our layer started, in samples
static long long     g_skipped;        // samples discarded since the last realign
static volatile long g_converged;      // the one start-lag correction has been made

// The game's layers decode a second at a time, so comparing their produced
// counts against ours is only good to about a second. Corrections smaller than
// that are noise, not lag.
#define CONVERGE_FLOOR 44100

// Keep converging instead of measuring once.
//
// Every stream counts what it has produced at +0x1a0, and they all produce in
// lockstep - the game's four agree to the sample. So once we have discarded S
// samples and started `lag` late, (ours - boss) settles at exactly S - lag,
// and stays there. The shortfall IS the lag, readable at any moment rather
// than at one unlucky one.
//
// Measuring it once at the realign could not work: the counters move in whole
// one-second steps, so a single reading depends on where in the block cycle it
// was taken - which is why the same code reported "3.000s late" and "0.000s
// late" in consecutive sets, and why the result was a second out either way.
//
// Correcting only ever discards more, never less, so it approaches from behind
// and cannot overshoot. It runs only while the layer is inaudible, so the jump
// is never heard.
static void converge(void* self) {
    if (!g_stream_boss || g_skip_left > 0 || g_block <= 0) return;
    if (g_mix > 0.0 || g_converged) return;        // audible, or already done
    const long long theirs = *(const long long*)((const unsigned char*)g_stream_boss + 0x1a0);
    const long long mine   = *(const long long*)((const unsigned char*)self + 0x1a0);
    const long long drift  = (mine - theirs) - g_skipped;   // negative = we are behind

    // The threshold is set by how noisy the comparison is, NOT by our block
    // size. The game's streams decode a second at a time and ours decodes a
    // tenth, so the two counters are never sampled at the same point in their
    // cycles and the difference carries up to a second of slop. Tying the
    // threshold to our block instead made this fire ten times as readily, and
    // since it only ever skips FORWARD it walked us into the middle of the
    // song one correction at a time.
    //
    // Firing once is also the point: a genuine start lag is fixed by a single
    // correction. Anything that keeps needing them is measurement noise.
    if (drift > -(long long)CONVERGE_FLOOR) return;
    const long blocks = (long)((-drift) / CONVERGE_FLOOR);
    g_skip_left = blocks * CONVERGE_FLOOR;
    g_converged = 1;
    say("converging once: %.3fs behind, discarding %ld more second(s)",
        (double)(-drift) / 44100.0, blocks);
}

// The last hundred milliseconds.
//
// Discarding can only land on a block boundary, and a block is a whole second:
// the count-in is 127,808 samples, three blocks is 132,300, so the trim always
// overshoots by 4,492 - about 102ms ahead of the fight music, every time.
//
// That last stretch is taken off the other end instead. The stream's voice can
// be paused, so it is held back by exactly the overshoot and released a frame
// later: granularity of about 16ms rather than 1000ms. Pausing happens on the
// audio thread the moment the skip finishes, and releasing from the per-frame
// update, which is what makes the timing fine enough to matter.

volatile long        g_pulls;          // blocks our stem has actually produced
volatile long        g_pulls_any;      // blocks ANY stream has produced through this hook
long                 g_skip_left;      // samples still to discard
long long            g_pos;            // samples of body played since alignment
static volatile long g_realign;        // rewind and re-trim at the next pull
static long long     g_carry;          // overshoot past the loop point, preserved

// How much the stream decodes per pull, in frames. QueueSongChunk sets it from
// the file's sample rate, which is why a block is exactly one second - it is a
// field on our own stream, not a property of the decoder or the mixer.
//
// Lowering it for the last part of a skip lets the skip end on any sample we
// like: ask for exactly the frames still wanted, get exactly those, restore
// the rate afterwards. That removes the block quantisation entirely - the
// ~102ms residue, and the ragged splice at the loop point with it.
#define OFF_BLOCK_FRAMES 0x198

#define OFF_CHUNK_VEC   0x170
#define OFF_CHUNK_INDEX 0x188

static int stream_chunk(void* stream) {
    return stream ? *(const int*)((const unsigned char*)stream + OFF_CHUNK_INDEX) : -1;
}

typedef void (*PullFn)(void* self, void* out);
typedef void (*RewindFn)(void* decoder);
static PullFn   g_next_pull;
static RewindFn g_rewind;

// Note for anyone chaining here: this calls the trampoline more than once per
// invocation - up to SKIP_PER_PULL extra times while discarding the count-in.
// That is legitimate (a hook may call through as often as it likes) but a
// higher-priority observer counting calls will see more than the game made.
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

                // How late our layer started, in samples.
                //
                // Each stream counts what it has produced at +0x1a0, and the
                // game's own four agree to the sample - so this is a precise
                // instrument, not the noisy one I took it for. Our layer joins
                // a block late even when added inside the builder, and the
                // skip has to make that up: aligning means skipping the
                // count-in PLUS however late we were.
                //
                // Bounded, because a reading taken before our stream has
                // produced anything would be the boss layer's entire history.
                long lag = 0;
                if (g_stream_boss) {
                    const long long theirs = *(const long long*)((const unsigned char*)g_stream_boss + 0x1a0);
                    const long long mine   = *(const long long*)((const unsigned char*)self + 0x1a0);
                    const long long d = theirs - mine;
                    if (d > 0 && d <= 4 * 44100) lag = (long)d;
                }
                g_lag = lag;

                // Skip a FIXED number of blocks - the same count every time.
                //
                // This used to add a correction for how late our layer joined,
                // measured as the difference between the boss layer's produced
                // -sample counter and ours. Two things make that measurement
                // unusable. It counts samples DECODED, which runs ahead of
                // playback by whatever is buffered, so it is not a position.
                // And it moves in whole blocks - one second each - so it is
                // quantised far coarser than the error it was meant to fix.
                //
                // Rounding a varying total to whole blocks then lands on a
                // different block from run to run, which is why the alignment
                // was excellent one night and half a second out the next. It
                // was never deterministic; we simply got a good roll.
                //
                // Skipping a constant instead gives a constant result: always
                // the same block count, always the same ~100ms, every launch.
                g_skip_left = (long)SKIP_SAMPLES + lag;
                g_skipped = 0;
                g_converged = 0;
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
                say("block=%ld frames; we started %.3fs late; skipping %ld exactly",
                    (long)got, (double)g_lag / 44100.0, g_skip_left + got);
            }
            g_skip_left -= got;
            g_skipped += got;

            // Ask for exactly the remainder, so the skip ends on the sample we
            // want rather than on the next second boundary.
            if (g_skip_left > 0 && g_skip_left < g_block)
                *(int*)((unsigned char*)self + OFF_BLOCK_FRAMES) = (int)g_skip_left;
        }
        // Back to the stream's own block size for ordinary playback.
        if (g_block > 0)
            *(int*)((unsigned char*)self + OFF_BLOCK_FRAMES) = (int)g_block;

        if (g_skip_left <= 0) {
            // Where the body actually starts, given we landed on a block
            // boundary rather than exactly on the count-in. Carrying the
            // previous lap's overshoot keeps the loop period exact.
            g_pos = g_carry;          // the skip is exact now; no overshoot to carry
            g_carry = 0;
            g_skip_left = 0;

            // The last ~102ms is left alone.
            //
            // Two attempts to shave it both failed: pausing the voice from the
            // pull crashed (that is the audio thread, mid-decode on the very
            // stream being stopped), and pausing it from the game thread left
            // the stream dead - 0xb51670 does more than toggle a voice, and
            // resuming does not restart the decode task it tears down.
            //
            // A tenth of a second, constant and in the same direction every
            // launch, is a better place to stop than a third attempt.
        }
    }

    g_next_pull(self, out);

    if (ours) converge(self);

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

#if MEWBUNGA_ALL_BUNGA
    if (p && (ends_with(p, "_map.ogg") || ends_with(p, "_battle.ogg")
                                       || ends_with(p, "_boss.ogg"))) {
        queue_as(self, BUNGA_TRACK, sizeof(BUNGA_TRACK) - 1, path, onfinish);
        return;
    }
#endif
    if (p) TRACE("pass %s", p);
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
// Added while the builder is still building, not afterwards.
//
// The borrowed event layer was always perfectly in sync because the GAME made
// it: same set, same intro chunk, started by the builder's own per-layer pass.
// A layer added after that pass has finished joins however many frames later
// the update loop happens to run, and that lateness varies from launch to
// launch - which is what made the alignment a lottery.
//
// Hooking AddLayer puts ours in during the builder's own add loop, right after
// it adds its last layer. The start pass that follows walks the vector from
// begin to end, so it picks ours up with the others and starts it at the same
// instant. Nothing to measure and nothing to correct.
//
// The add loop is bounded by the TRACKLIST, not the layer vector, and re-reads
// it each iteration, so growing the layer vector underneath it is safe.
static AddLayerFn    g_next_add;
static volatile long g_adding_ours;
#define LAST_GAME_LAYER 3            // map, battle, event, boss



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
    say("adding our layer as %d, inside the builder (intro=%s)",
        LAST_GAME_LAYER + 1, with_intro ? g_intro_path : "none");
    g_next_add(group, core, LAST_GAME_LAYER + 1, &our_paths, &our_finishes);

    InterlockedExchange(&g_adding_ours, 0);
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

    // Mewjector is required, not optional.
    //
    // Without it, common/hookapi.inc falls back to patching each site itself
    // with a fixed 24-byte steal - and 24 has never been checked against an
    // instruction boundary on any of these seven sites. Mewjector passes 0 and
    // its length disassembler gets it right; on its own, this mod would be
    // NOP-padding through the middle of an instruction.
    //
    // It is also the only way the music hooks can share a site with another
    // mod, which several of them plausibly want.
    if (!hookapi_init()) {
        say("mewbunga: Mewjector not found - not installing "
            "(this mod needs its hook chaining, and its steal lengths)");
        return TRUE;
    }

    const int bad = verify_sites_chained(SITES, SITE_COUNT, SITE_SIGLEN, g_base, g_at,
                                         GAME_TIMESTAMP, GAME_SIZEOFIMAGE);
    if (bad >= 0) {
        say("mewbunga: site %s (rva 0x%x) does not match this build - not installing",
            SITES[bad].name, SITES[bad].rva);
        return TRUE;
    }

    // Called, not hooked.
    g_append = (AppendFn)g_at[S_APPEND];
    g_rewind = (RewindFn)g_at[S_DECODER_REWIND];

    g_next = (QueueFn)install_hook(SITES[S_QUEUECHUNK].rva, g_at[S_QUEUECHUNK], 24,
                                   (const void*)&hooked_queue, "mewbunga");
    g_next_add = (AddLayerFn)install_hook(SITES[S_ADD_LAYER].rva, g_at[S_ADD_LAYER], 24,
                                          (const void*)&hooked_add_layer, "mewbunga");
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
    if (g_mj.Log)
        g_mj.Log("mewbunga", "the Lord Bunga radio version: %s",
                 g_next ? "installed" : "FAILED to install");
    say("mewbunga: queue=%s turn=%s  update=%s  (player cats: INT<=%d always, else %d%%)",
        g_next ? "hooked" : "FAILED", g_next_turn ? "hooked" : "FAILED",
        g_next_update ? "hooked" : "FAILED", RADIO_INT_THRESHOLD, RADIO_CHANCE_PERCENT);
    return TRUE;
}

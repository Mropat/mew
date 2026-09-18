# The Legend of Bunga

Released: [legendofbunga-v1.0.2](https://github.com/Mropat/mew/releases/tag/legendofbunga-v1.0.2).
Needs [Mewjector](https://github.com/githubuser508/mewjector); drop the DLL in
`Mewgenics/mods`.

There is a community legend that a cat with 0 INT makes the Lord Bunga fight
play the radio version of its song, "Mom I Really Hate You". It does not. This
mod makes it true - and a cat that drops to 0 mid-turn, by Stoopzerk or a
concussion, hears it change while it is still standing there.

Built as `legendofbunga.dll`, keeping the naming of the other mods in this repo.

The legend is more grounded than it sounds. `audio/music/radio.gon` maps every
zone's instrumental to a vocal radio counterpart, and lists

```
mom_i_really_hate_you  //ice age
```

while `data/maps/iceage.gon` puts Lord Bunga on the ice age boss node. So the
fight plays `iceage/iceage_boss.ogg`, and the radio version of that exact track
is a real asset the game already ships. Somebody noticed the pairing and
invented a trigger for it.

## How the music system works

Worth writing down, because almost none of it is obvious and all of it was
learned the hard way.

A zone's four tracks - `map`, `battle`, `event`, `boss` - are **not**
alternatives. All four stream in parallel from the moment the level loads, and
the game crossfades between them. That is why walking from a hallway into a
fight is seamless: nothing loads, a gain moves. The four are stems of one
master, cut to identical lengths - every ice age layer is exactly 194.2857s,
and they cross-correlate at precisely +0.000s.

The final boss abuses this, and Tyler says so himself in `music_info.gon`:

```
finalboss { //these aren't tied to battle, map etc I'm just sorting them by ID
            //so its easy to fade into the next part during the fight
```

Each layer is a `Layer` in a `std::vector<Layer>`, stride `0x30`, reached from
the component's layer group. `+0x00` is its `SoundStream`, `+0x20` the current
gain and `+0x28` the target. A per-frame ramp steps current toward target and
multiplies it by the master to set the stream's volume.

## What this mod does

Everything is gated on being in the ice age - Lord Bunga's zone - and on the
game having the **boss** layer up, so an ordinary skirmish in the zone is left
alone and no other zone is touched at all.

Inside that fight, the radio version plays on **a fifth layer of its own**,
crossfaded to on the turns of cats that qualify. Nothing is borrowed: the
zone's own four layers are untouched, so event encounters keep their music.

The radio version is not a stem - it is a separate performance, 203.0507s to
the instrumental's 194.2857s, and the two never correlate as waveforms. But
they share a tempo (164.10 BPM) and an arrangement, and their onset envelopes
lock at a constant offset. So it can be *made* into a stem:

| | samples | |
| --- | --- | --- |
| count-in | 127,808 | discarded (2.898s, 7.93 beats) |
| body | 8,568,000 | looped on **the instrumental's** length |
| tail | 258,727 | dropped (5.867s) |

Trimmed and looped that way, it stays locked to the fight music indefinitely,
and a switch lands on the same bar and the same beat. There is no seek to call,
so the trimming is done by decoding and discarding inside the stream's own
pull - a few blocks per call, since 2.9s of vorbis decode in one go would miss
an audio deadline.

The skip is sample-exact, which took a while to earn. A stream decodes in
blocks of `[stream+0x198]` frames, a field `QueueSongChunk` fills in from the
file's sample rate - so a block is one whole second, and discarding whole
blocks can only ever land on a second boundary. Every alignment problem this
mod had came from that: a ~100ms residue at best, a full second out at worst,
and a ragged splice at the loop point.

But `0x198` is just a field on our own stream. Asking for exactly the frames
still wanted on the final call of a skip ends it on the chosen sample, after
which the rate goes back for ordinary playback. No rounding, no residue.

Our layer also starts a whole number of blocks after the game's four, and that
lag is not measurable at any single instant - the counters move in one-second
steps, so one reading reports 0s and the next 3s. It is measurable
*continuously*: with `S` samples discarded and a start lag of `lag`, the gap
between our produced-sample count and the boss layer's settles at exactly
`S - lag`. The shortfall is the lag, so the mod converges on it while the layer
is silent rather than guessing once.

## Adding a layer

A zone has four layers because the parser reads four key names. `AddLayer`
(`0xa1a580`) will happily build a fifth - it grows the vector and tail-calls
`InitStream` - but a layer built that way is complete and permanently silent:
stream, voice, chunk, audio core, and nothing ever decodes for it.

What is missing is the rest of what the set builder does per layer, at
`0xa1abba`:

| | |
| --- | --- |
| `0xb51520` | spawn the stream's decode task (a thread per stream) |
| `0xd80b60` | play its voice |

`0xb51670` looks like the start call and is not - it only resumes a stream that
already has a task decoding for it.

The layer is built as `[intro, radio]`, mirroring the game's own `[intro,
track]`. Every layer of a set consumes the zone's intro sting before its body
begins, which is exactly why the four stay sample-aligned; a layer without one
starts its body about six seconds early and is out of step all fight.

## Timing, and two traps in it

Anything in this mod that has to be exact runs into the same wall: a stream
decodes a whole block at a time, and a block is one second - `[stream+0x198]`,
which `QueueSongChunk` fills in from the file's sample rate.

**Decoded-sample counts can decide whether, never how.** They advance a second
at a time, so they are fine for "has the body ended yet" and useless for
shaping a fade. Everything audible - the switch, the loop crossfade - runs off
the frame clock instead. A crossfade driven off stem position got one or two
updates across a bar, heard as a step down and a jump.

**Where an exact position is needed, it is computed rather than sampled.**
`0x198` is a field on our own stream, so the last call of a skip asks for
precisely the frames still wanted and the skip ends on the chosen sample. The
loop handover is timed the same way: when the live copy has `remaining` samples
of body left, the other is rewound and skipped by `count-in - remaining`, so it
arrives on the downbeat no matter how often we get to look.

Cross-stream comparisons are a second-grained *measurement*, not a fine one:
the game's layers decode a second at a time and ours may not, so the difference
between two produced-sample counters carries that much slop. A correction built
on it must fire once, with a threshold of a second - a forward-only correction
on a noisy signal walks the song out of alignment one step at a time.

## When it switches

The acting cat's INT is re-read every frame, not once when its turn starts, so
a cat that makes itself stupid *during* its own turn - Stoopzerk, a concussion
- hears the music change immediately rather than on its next turn.

That means holding a `Character*` across frames, which is only safe because the
window is bounded: it is taken at `BeginTurn`, dropped at `EndTurn`, and
dropped the moment the cat dies. The last case is the one that matters, since a
cat can be killed in the middle of its own turn. Between turns there is nobody
to ask and the previous verdict stands, so the music does not flap while the
enemy acts.

## Who hears it

A player cat with INT at or below the limit gets it. `is_player_cat` (a byte at
`[Character + 0x489]`, written by `Character::init`) keeps enemies out; they
take turns and have stats too, and a LordBunga has `intelligence 5` like
anything else.

INT is read from `+0x5c8`, which is the live, buffed value - so a concussion or
a Stoopzerk counts, and counts immediately.

An earlier build gave every other cat a fixed random draw, so the joke would
fire in an ordinary run without hunting for a 0 INT cat. It has been removed:
at 0 INT the mod means something, and a coin flip made it mean nothing.

### The limit

`legendofbunga.ini`, written beside the DLL on first run:

```ini
[LegendOfBunga]
IntLimit=0
```

0 is the legend's own condition, the default, and the floor. 1 to 4 widen it,
for anyone who would rather hear the joke than wait for a 0-INT cat. 4 is the
ceiling and the row offers nothing past it: the joke is that the stupid cat
hears it, and a cat that is not stupid should not.

It is re-read whenever a level builds its music, so an edit lands on the next
fight rather than the next launch. The file is created with `CREATE_NEW`, so a
version already on disk is never overwritten.

## The settings row

The limit is also a row in **Audio Settings**, under the last volume slider,
and getting one there took some finding out.

The screen is not data-driven - there is no settings gon anywhere in the pak.
Four functions build the four panels in code (`0x28eee0` game, `0x291610`
video, `0x294860` audio, `0x2938e0` the shared tooltip/status-size pair), and
each row is a clip placed statically in `swfs/ui.swf` that

```cpp
SelectorWidget* MenuPanel::register_selector(std::string key, std::string current,
                                             std::vector<SelectorWidget::Option>,
                                             bool, std::function<void(std::string)>)
```

looks up by name - `key`, `key_left` and `key_right` - through `find_ui_child`
(`0x5a100`), which returns null rather than creating anything. All 18 of those
clips are spoken for, one per `register_selector` call site. There is no list to
append to and no spare row to adopt.

What there is instead is an engine that can make another copy of anything
already on screen. A `glaiel::swf::MovieClip` keeps the tag list it was built
from at `+0xd0`, and a `DynamicTextBox` keeps its `DefineEditText` at `+0x98` -
both of them the argument their constructor was called with, which is also what
`DefineSprite`'s and `DefineEditText`'s instantiate slots pass. So a second copy
of a row's three pieces is two calls apiece, and the copies are independent:
their own text, their own position, their own name.

From there it is the game's own path. `DisplayObject` keeps its parent at
`+0x38`, its name at `+0x48` as a plain `const char*`, and its transform at
`+0x60` as six floats. The clones are parented next to the pieces they came
from - `addChild` (`0x999b90`) refuses a child that already has a parent, which
a fresh one does not - given a name, and moved one row on.

How far "one row" is, and which of the two translation floats it is on, are
both measured at runtime from the two rows above: the audio screen runs on a
dead-regular 42.15 pitch and the rows differ on one axis only, so neither the
unit nor the axis has to be guessed. There is room for about seven more rows
below the last volume slider - Audio Settings stops at y -63.5 while Game
Settings goes on to +269.

The `std::function` is hand-built. MSVC's is 64 bytes with the callable's
pointer at `+0x38`, counting as "local" - moved rather than stolen, and never
freed - when that pointer is the storage itself. `register_selector`'s own
teardown is what says so:

```
mov rcx,[rbx+0x38]   ; _Getimpl()
cmp rcx,rbx          ; _Local()?
call [r8+0x20]       ; _Delete_this, vtable slot 4
```

which fixes the whole layout: `_Copy`, `_Move`, `_Do_call`, `_Target_type`,
`_Delete_this`. The game's own `combat_speed` lambda at `0x29c3f0` confirms the
call shape - it takes `(this, std::string&&)` and does nothing but write the
setting.

Every step is checked and the mod is happy without any of it: a missing piece
means no row, never a half-built one.

### Why not settings.txt

The game's own settings are not in the save database - they are
`AppData/Roaming/Glaiel Games/Mewgenics/<id>/settings.txt`, a flat `key value`
file, and the writer at `0x9d2f70` walks a `std::map<std::string, std::string>`
in order and dumps it whole. A key the game has never heard of would survive a
round trip, so the row's value could live there.

It lives in the ini instead, so there is one place to look and one place to
edit. The row writes it there when you change it.

## Known

- **The loop handover has only been heard with the radio up.** It is meant to
  run whether or not anyone is listening - a seam passing during an ordinary
  cat's turn has to swap the copies over anyway, or the live one drifts off the
  grid - and it is written to, but that case has not been observed. A trace
  build logs `handed the body over to copy N` when it happens.
- **`SKIP_SAMPLES` is 127,808; the measurement says 127,780.** Correlating the
  two onset envelopes at 64-sample resolution gives a mean of 2.8975s with a
  spread of 1.4ms across the whole track. The 28-sample difference is 0.6ms and
  has been left alone rather than perturb something that currently sounds
  right.
- **`force_layers` ignores a group with more than eight layers.** A music set
  is six now that this mod adds two. Another mod adding three or more would
  quietly switch this one off.

# mewbunga

**Work in progress. Not released, not ready to ship.**

There is a community legend that a cat with 0 INT makes the Lord Bunga fight
play the radio version of its song, "Mom I Really Hate You". It does not. This
mod makes it true.

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

Two corrections keep it honest. Our layer joins a frame or two after the set is
built, and each stream counts the samples it has produced at `+0x1a0`, so the
difference between the boss layer's count and ours is exactly that lateness -
folded into the skip. And a decode block is a whole second, so the skip can
only land on a second boundary: aiming at the nearest one rather than the next
leaves about 100ms either way instead of always running ahead.

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

`is_player_cat` is a byte at `[Character + 0x489]`, so enemies are excluded -
they take turns too, and a LordBunga has `intelligence 5` like anything else.
INT lives at `+0x5c8` and is the live, buffed value.

A cat with INT at or below the threshold gets it. `RADIO_CHANCE_PERCENT` is 0,
so that is the only way to trigger it - the legend's own condition and nothing
else. Raising it gives every other cat a draw, which makes the joke fire in an
ordinary run at the cost of no longer meaning anything.

The draw mixes a per-launch seed with the
character's address rather than running an RNG per turn, so a cat keeps its
verdict for the whole fight - a coin flipped every turn makes the music flap
and reads as a bug. Characters are new objects each battle, so the same cat
draws again next fight, and differently next launch.

## Known, and why it is not shippable yet

- **Ice age event encounters play the radio version**, because that is the
  layer it borrows. Only that zone: the substitution is gated on it, as is
  everything else. Every slot costs something: `boss` is the one the actual
  Bunga fight needs and `map` is the hallway music, so `event` is the least
  bad. Only the ice age is affected, since nothing outside that zone is touched.
- **A layer of our own would cost nothing, and does not work yet.**
  `MEWBUNGA_OWN_LAYER 1` adds a fifth layer through the game's own `AddLayer`
  at `0xa1a580`, which builds the layer and queues the chunk correctly - and
  the mixer never pulls it. `SoundStream`'s constructor leaves `[stream+0]`
  null, and the gain loop skips any layer whose stream has it null, so
  something else must start playback and that call has not been found. The code
  is kept for whoever picks it up.
- The count-in offset is measured, not derived: 127,808 samples is 7.93 beats
  where an exact two bars would be 8.00, leaving about 23ms unaccounted for.
- Nothing has been checked against a real Lord Bunga fight yet.

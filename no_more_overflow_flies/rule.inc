// Should this freshly-initialised character have its current HP filled in?
//
// Character::init writes max HP but leaves current HP holding the engine's
// "spawn at full health" sentinel, INT_MAX/2. Something clamps it later, before
// the character's next turn - but a character spawned mid-turn can be damaged
// inside that window, and the damage code then reports damage dealt as the
// whole sentinel-to-max drop. Any leech effect heals for ~2^30.
//
// The test is deliberately narrow. Only a value that is *exactly* the sentinel
// is touched, and only when max HP is a sensible positive number. Anything the
// game has genuinely decided - a wounded spawn, a character whose max is filled
// in later - is left alone, so the mod can only ever close the window and never
// overwrite a real decision.
//
// Shared by the mod and by test_rule.cpp so the tested rule and the shipped
// rule cannot diverge.

#define SPAWN_HP_SENTINEL 0x3FFFFFFF        // 1073741823 == INT_MAX / 2

static bool needs_spawn_hp(int current, int max_hp) {
    if (current != SPAWN_HP_SENTINEL) return false;   // a real value already
    if (max_hp <= 0) return false;                    // max not established yet
    if (max_hp >= SPAWN_HP_SENTINEL) return false;    // already absurd; not ours
    return true;
}

// Exercises the one decision mewspawnhp makes.
//
// The risk in this mod is not that it fails to fix the bug - it is that it
// overwrites a health value the game had legitimately decided. So most of these
// check that it keeps its hands off.
//
//   clang++ -O2 -o test_rule.exe test_rule.cpp && ./test_rule.exe

#include <cstdio>
#include "rule.inc"

static int g_fails;
static void want(bool expect, int current, int max_hp, const char* what) {
    bool got = needs_spawn_hp(current, max_hp);
    printf("  current=%-11d max=%-11d %-5s %-44s %s\n",
           current, max_hp, got ? "fill" : "-", what, got == expect ? "ok" : "FAIL");
    if (got != expect) g_fails++;
}

int main() {
    puts("the window this exists to close");
    want(true,  SPAWN_HP_SENTINEL, 1,   "a rot fly: sentinel, max 1");
    want(true,  SPAWN_HP_SENTINEL, 20,  "an arena object: sentinel, max 20");
    want(true,  SPAWN_HP_SENTINEL, 105, "a cat: sentinel, max 105");

    puts("\nvalues the game has already decided - do not touch");
    want(false, 1,   1,   "already filled in at full");
    want(false, 1,   20,  "a wounded spawn");
    want(false, 20,  20,  "healthy, at max");
    want(false, 0,   20,  "dead");
    want(false, -1,  20,  "negative, whatever that means");
    want(false, 48,  48,  "the caster, untouched");

    puts("\nmax HP not usable yet");
    want(false, SPAWN_HP_SENTINEL, -1, "max still -1, as it is entering init");
    want(false, SPAWN_HP_SENTINEL, 0,  "max zero");

    puts("\nalready absurd - not ours to fix");
    want(false, SPAWN_HP_SENTINEL, SPAWN_HP_SENTINEL,     "max is itself the sentinel");
    want(false, SPAWN_HP_SENTINEL, SPAWN_HP_SENTINEL + 1, "max beyond the sentinel");
    // An already-scaled fly has an absurd max, but it is a real number the game
    // decided. Filling it in is still right: it closes that spawn's window too,
    // and leaving the sentinel there would just mean a smaller bogus delta.
    want(true,  SPAWN_HP_SENTINEL, 1073741822,            "a scaled fly: huge but real max");

    puts("\nonly ever the exact sentinel");
    want(false, SPAWN_HP_SENTINEL - 1, 20, "one below the sentinel");
    want(false, SPAWN_HP_SENTINEL + 1, 20, "one above the sentinel");

    printf("\n%s\n", g_fails ? "FAILED" : "all checks passed");
    return g_fails != 0;
}

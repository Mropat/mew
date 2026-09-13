// Exercises Butch's donation rule.
//
// The v1 test checked that the byte emitter produced the expected machine code.
// There is no machine code to check any more, so this checks the thing that
// actually matters: who Butch takes and who he refuses.
//
//   clang++ -O2 -o test_rule.exe test_rule.cpp && ./test_rule.exe

#include <cstdio>

#include "rule.inc"

static int g_fails;

static void want(bool expect, int act, int chap, int diff,
                 int ra, int rc, int rd, const char* what) {
    bool got = butch_rule(act, chap, diff, ra, rc, rd);
    printf("  A%d C%d D%d vs A%d C%d D%d  %-4s  %-46s %s\n",
           act, chap, diff, ra, rc, rd,
           got ? "take" : "-", what, got == expect ? "ok" : "FAIL");
    if (got != expect) g_fails++;
}

// What the original did, to check the mod never takes an acceptance away.
static bool vanilla(int act, int chap, int diff, int ra, int rc, int rd) {
    return act >= ra && chap >= rc && diff >= rd;
}

int main() {
    puts("vanilla acceptances must survive");
    want(true,  3, 4, 2,  3, 4, 2, "exactly what he asked for");
    want(true,  3, 4, 3,  3, 4, 2, "harder than asked, top act");
    want(true,  2, 3, 1,  1, 2, 0, "comfortably past an early tier");

    puts("\nthe point of the mod: a harder run excuses a lower act");
    want(true,  1, 4, 3,  3, 4, 2, "A1 C4 crazy when he wants A3 C4 hard");
    want(true,  2, 4, 3,  3, 4, 2, "A2 C4 crazy when he wants A3 C4 hard");
    want(false, 1, 4, 2,  3, 4, 2, "A1 C4 hard - same difficulty, no excuse");

    puts("\nchapter is never excused");
    want(false, 1, 3, 3,  3, 4, 2, "A1 C3 crazy - run not finished");
    want(false, 3, 3, 3,  3, 4, 2, "A3 C3 crazy - run not finished");
    want(false, 3, 1, 3,  3, 4, 0, "A3 C1 crazy - barely started");

    puts("\ndifficulty is never excused downward");
    want(false, 3, 4, 1,  3, 4, 2, "top act but too easy");
    want(false, 3, 4, 0,  3, 4, 3, "top act, much too easy");

    puts("\nclamped at the hardest a normal run reaches");
    want(true,  1, 4, 3,  3, 4, 3, "he wants crazy and there is nothing above it");
    want(false, 1, 4, 2,  3, 4, 3, "still short of crazy");

    puts("\nnever stricter than vanilla, over the whole grid");
    int checked = 0, regressions = 0;
    for (int act = 0; act <= 3; act++)
    for (int chap = 0; chap <= 4; chap++)
    for (int diff = 0; diff <= 7; diff++)
    for (int ra = 0; ra <= 3; ra++)
    for (int rc = 0; rc <= 4; rc++)
    for (int rd = 0; rd <= 7; rd++) {
        checked++;
        if (vanilla(act, chap, diff, ra, rc, rd) &&
            !butch_rule(act, chap, diff, ra, rc, rd)) regressions++;
    }
    printf("  %d combinations, %d that vanilla took and the mod refuses   %s\n",
           checked, regressions, regressions == 0 ? "ok" : "FAIL");
    if (regressions) g_fails++;

    printf("\n%s\n", g_fails ? "FAILED" : "all checks passed");
    return g_fails != 0;
}

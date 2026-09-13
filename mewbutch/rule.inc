// Butch's donation rule, as a pure function of the six numbers involved.
// Shared by the mod and by test_rule.cpp so the tested rule and the shipped
// rule cannot diverge.
//
// Vanilla is act >= required && chapter >= required && difficulty >= required.
// Every tier past A3 C4 pins required_act to 3 and required_chapter to 4 and
// only raises the difficulty, so a brutal run of an earlier adventure is
// refused however hard it was.
//
// The change: one difficulty step above what Butch asks for excuses a LOWER
// ACT. Chapter is never excused, and difficulty is never excused downward.
//
// difficulties.gon has tiers 0..7, but 4 and up need limit-break gear, so 3 is
// the hardest a normal run reaches. Once Butch is asking for 3 there is no
// "one step above" left to offer, so the act stops being enforced there too.
#define DIFFICULTY_MAX_NORMAL 3

static bool butch_rule(int act, int chapter, int difficulty,
                       int req_act, int req_chapter, int req_difficulty) {
    if (chapter < req_chapter) return false;          // never excused
    if (difficulty < req_difficulty) return false;    // never excused
    if (difficulty > req_difficulty) return true;     // harder run, any act
    if (difficulty >= DIFFICULTY_MAX_NORMAL) return true;
    return act >= req_act;
}

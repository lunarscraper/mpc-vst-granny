// Granny: checks of the BEAT generator's slice choice (src/engine/beat.hpp).   tests/build_x86.sh builds build/x86/test_beat
#include <cstdio>
#include "engine/beat.hpp"
using namespace omni;

static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main() {
    const int n = 8;
    float w[n];
    BeatRng rng;
    // ANCHOR 1: the steps on beats 1 and 3 (slices 0 and 4) keep their slice at any COMPLEXITY
    for (int k = 0; k < n; k++) w[k] = beat_weight(k * 0.5, 1.0f);
    bool kept = true;
    for (int i = 0; i < 20000; i++) {
        int nat = i % n, s = beat_pick(rng, n, nat, -1, w, 1.0f, 0.0f);
        if ((nat == 0 || nat == 4) && s != nat) kept = false;
    }
    check(kept, "anchor 100: beats 1 and 3 are never swapped");
    // COMPLEXITY 0, ROLL 0: straight playback
    for (int k = 0; k < n; k++) w[k] = beat_weight(k * 0.5, 0.0f);
    bool straight = true;
    for (int i = 0; i < 1000; i++) if (beat_pick(rng, n, i % n, (i + n - 1) % n, w, 0.0f, 0.0f) != i % n) straight = false;
    check(straight, "complexity 0, roll 0: every step plays its own slice");
    // COMPLEXITY 1, ANCHOR 0: about uniform
    int hist[n] = {};
    for (int i = 0; i < 80000; i++) hist[beat_pick(rng, n, 3, -1, w, 1.0f, 0.0f)]++;
    bool uniform = true;
    for (int k = 0; k < n; k++) if (hist[k] < 8500 || hist[k] > 11500) uniform = false;
    check(uniform, "complexity 100, anchor 0: slices about evenly spread");
    // ROLL 1, ANCHOR 1, on slice 0: mostly stays there
    for (int k = 0; k < n; k++) w[k] = beat_weight(k * 0.5, 1.0f);
    int stay = 0;
    for (int i = 0; i < 10000; i++) if (beat_pick(rng, n, 2, 0, w, 0.5f, 1.0f) == 0) stay++;
    check(stay > 9000, "roll 100 on an anchored slice: it sticks (escape hatch aside)");
    // RETRIG: 100 % fires every beat, 0 never, 50 % per bar about half the bars
    float all[4] = {1, 0, 0, 0}, none[4] = {0, 0, 0, 0}, half[4] = {0, 0, 0.5f, 0};
    bool every = true, never = true;
    for (int i = 0; i < 1000; i++) { if (beat_roll_retrig(rng, all, 4) != 2) every = false; if (beat_roll_retrig(rng, none, 4)) never = false; }
    check(every && never, "retrig 100 % every beat at its rate, 0 % never");
    int bars = 0;
    for (int b = 0; b < 20000; b++) { bool any = false; for (int q = 0; q < 4; q++) if (beat_roll_retrig(rng, half, 4) == 4) any = true; bars += any; }
    check(bars > 9400 && bars < 10600, "retrig 50 %: about half the bars stutter");
    // the curve on 16 steps: the off-eighth steps weigh 1, beats 1 and 3 weigh 0
    check(beat_weight(0.25, 1.0f) == 1.0f && beat_weight(2.0, 1.0f) == 0.0f && beat_weight(3.5, 1.0f) > 1.1f, "anchor curve on a finer grid");
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}

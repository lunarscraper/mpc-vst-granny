// Granny: the BEAT generator's slice choice (pure logic, no audio: host-testable, see tests/test_beat.cpp).
//
// The idea and the knob set follow mestela's schwung-breakbeat (Breakbeat Generator for Schwung, described in its
// README): a break cut into equal steps, every step either plays the slice that belongs there or a different one,
// steered by COMPLEXITY (how often a random slice replaces the natural one), ANCHOR (keeps the bar's structural
// positions in place), ROLL (sticks to the slice just played: repeats, +-1 walks, rare jumps) and FILL (opens the
// last bar of a phrase up). This is a new implementation written from that description, not that module's code;
// the anchor curve's values are the ones its README documents.
#pragma once
#include <cmath>
#include <cstdint>

namespace omni {

constexpr int BEAT_MAX_STEPS = 32;

// The anchor weight curve at ANCHOR 100 on a bar's eighths (README of schwung-breakbeat): 0 = never swapped (beats 1
// and 3), above 1 = swapped more often than COMPLEXITY alone says (the bar's last eighth: fill territory).
constexpr float BEAT_ANCHOR_CURVE[8] = {0.0f, 0.5f, 1.0f, 0.7f, 0.0f, 0.5f, 1.0f, 1.2f};

// The weight of a step that starts pos_beats (quarter notes) into its bar; positions between eighths (16ths, 32nds)
// weigh 1. ANCHOR 0 weighs every step 1, ANCHOR 1 gives the curve.
inline float beat_weight(double pos_beats, float anchor) {
    double e = pos_beats * 2.0;                     // in eighths
    double r = std::floor(e + 1e-6);
    float curve = 1.0f;
    if (std::fabs(e - r) < 1e-6) {
        int i = int(r) % 8;
        if (i < 0) i += 8;
        curve = BEAT_ANCHOR_CURVE[i];
    }
    return (1.0f - anchor) + anchor * curve;
}

struct BeatRng {
    uint32_t s = 0x2545F491u;
    void seed(uint32_t v) { s = v ? v : 0x2545F491u; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float unit() { return float(next() >> 8) * (1.0f / 16777216.0f); }   // 0..1 (1 excluded)
    int below(int n) { return n > 0 ? int(next() % uint32_t(n)) : 0; }
};

// The next slice of n. natural = the slice that belongs to this step, cur = the one playing (-1 = none yet),
// w[k] = the anchor weight of slice k's own position. complexity, roll 0..1.
inline int beat_pick(BeatRng &rng, int n, int natural, int cur, const float *w, float complexity, float roll) {
    if (n <= 1) return 0;
    if (cur < 0 || cur >= n || rng.unit() < 1.0f - roll) {
        // move: an independent decision; the anchored positions keep their own slice
        float p_swap = complexity * w[natural];
        if (rng.unit() < p_swap) return rng.below(n);
        return natural;
    }
    // stay: correlated with the slice just played
    if (rng.unit() < 0.05f) return (cur + 2 + rng.below(3)) % n;       // the escape hatch: 2..4 ahead
    if (rng.unit() < 1.0f - w[cur]) return cur;                        // repeat (anchored slices stick)
    return (cur + (rng.unit() < 0.5f ? n - 1 : 1)) % n;                // walk to a neighbour
}

// RETRIG: per-bar odds (0..1) to per-beat odds, so a knob value means the share of bars that get a stutter
// (1 = every beat). beats = beats per bar.
inline float beat_per_beat(float per_bar, int beats) {
    if (per_bar <= 0) return 0;
    if (per_bar >= 1) return 1;
    return 1.0f - std::pow(1.0f - per_bar, 1.0f / float(beats));
}

// The RETRIG rates (repeats per beat) for the four knobs: 2x eighths, 3x eighth triplets, 4x sixteenths, 8x 32nds.
constexpr int BEAT_RT_RATES[4] = {2, 3, 4, 8};

// Chooses this beat's stutter: 0 = none, else repeats per beat. Rolls every rate, one of those that hit wins.
inline int beat_roll_retrig(BeatRng &rng, const float *per_bar, int beats) {
    int hit[4], nh = 0;
    for (int k = 0; k < 4; k++)
        if (per_bar[k] > 0 && rng.unit() < beat_per_beat(per_bar[k], beats)) hit[nh++] = BEAT_RT_RATES[k];
    return nh ? hit[rng.below(nh)] : 0;
}

// FILL on the last bar of a phrase: COMPLEXITY up, ROLL and ANCHOR down, by FILL's amount.
inline void beat_fill_mod(float fill, float &complexity, float &anchor, float &roll) {
    complexity += fill * (1.0f - complexity);
    anchor *= 1.0f - fill;
    roll *= 1.0f - fill;
}

}  // namespace omni

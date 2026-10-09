// Granny: sample analysis for slicing. Run once per decoded sample on the loader thread: where the sounds are
// (non-silent segments), where the hits are (onsets with a strength), and the tempo (a beat length). The breaks a
// key plays are worked out from that at note time, so the BREAKS BY and HIT SENS settings need no new analysis.
#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include "../core/model.hpp"

namespace omni {

struct Hit {
    int64_t at = 0;        // frame
    float strength = 0;    // onset strength (log energy rise), about 0.3 .. 4
};

struct Analysis {
    std::vector<Hit> hits;                                // by time
    std::vector<std::pair<int64_t, int64_t>> segments;    // sounding stretches [start, end) between silences
    double beat = 0;                                      // frames per beat (estimated), 0 = unknown
    int64_t first = 0, last = 0;                          // first sound, end of the last one
    int rate = 44100;                                     // the sample's rate
    double bpm(int rate) const { return beat > 0 ? 60.0 * rate / beat : 0; }
};
using AnaPtr = std::shared_ptr<const Analysis>;

AnaPtr analyze(const Pcm &pcm);

// BREAKS BY: 0 auto (silences when there are several sounding stretches, else 4 bars), 1 silences, 2/3/4 = 1/2/4 bars
constexpr int BREAK_BY_COUNT = 5;
// HIT SENS 0..1 -> the strength a hit needs
inline float hit_threshold(float sens01) { return 2.6f - 2.2f * (sens01 < 0 ? 0 : sens01 > 1 ? 1 : sens01); }

// The breaks of [zs, ze) (a zone's range): count, and the k-th one's range. All cheap enough for the audio thread.
int break_count(const Analysis &a, int by, int64_t zs, int64_t ze);
bool break_range(const Analysis &a, int by, int k, int64_t zs, int64_t ze, int64_t &s, int64_t &e);
// The hits of [zs, ze) at or above thr: count, and the k-th one's range (to the next hit or the end of its segment)
int hit_count(const Analysis &a, float thr, int64_t zs, int64_t ze);
bool hit_range(const Analysis &a, float thr, int k, int64_t zs, int64_t ze, int64_t &s, int64_t &e);
// SLICES: [zs, ze) cut into n slices. by 0 = at the n-1 strongest hits (plus the first sound), 1 = an even grid from
// the first sound to the end, each cut moved onto the strongest hit near it. Writes the slice starts (time order) and their
// ends; returns how many slices there are (fewer than n when the sample has fewer hits). No allocation (audio thread).
constexpr int MAX_SLICES = 64;
int make_slices(const Analysis &a, int by, int n, int64_t zs, int64_t ze, int64_t *starts, int64_t *ends);
// the break (index) holding frame f, -1 = none
int break_at(const Analysis &a, int by, int64_t f, int64_t zs, int64_t ze);

}  // namespace omni

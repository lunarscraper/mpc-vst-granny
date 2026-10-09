// Granny: sample analysis for slicing (see slicer.hpp).
//
// Everything runs on 256-frame hops: an RMS envelope (silences, and so the sounding segments), a high-passed energy
// (the first difference of the signal, which drum hits raise sharply) whose rise over the previous hops is the onset
// strength, and an autocorrelation of that onset curve for the tempo (refined over one and four bars, so long chunks
// stay on the beat).
#include "slicer.hpp"
#include <algorithm>
#include <cmath>

namespace omni {

namespace {
constexpr int HOP = 256;

struct Bars { bool bars; int count; int64_t t0, end; double len; };

// the zone's sounding segments (clipped to [zs, ze))
template <class F> void each_segment(const Analysis &a, int64_t zs, int64_t ze, F f) {
    for (auto &sg : a.segments) {
        int64_t s = std::max(sg.first, zs), e = std::min(sg.second, ze);
        if (e - s > 0) f(s, e);
    }
}

int resolve_by(const Analysis &a, int by, int64_t zs, int64_t ze) {
    if (by == 0) {
        int n = 0;
        each_segment(a, zs, ze, [&](int64_t, int64_t) { n++; });
        by = n >= 2 ? 1 : 4;
    }
    if (by >= 2 && a.beat <= 0) by = 1;   // no tempo: silences
    return by;
}

Bars bar_grid(const Analysis &a, int by, int64_t zs, int64_t ze) {
    Bars b{false, 0, 0, 0, 0};
    if (by < 2) return b;
    int64_t t0 = -1, end = 0;
    each_segment(a, zs, ze, [&](int64_t s, int64_t e) { if (t0 < 0) t0 = s; end = e; });
    if (t0 < 0) return b;
    int bars = by == 2 ? 1 : by == 3 ? 2 : 4;
    b.bars = true;
    b.len = bars * 4 * a.beat;
    b.t0 = t0;
    b.end = end;
    b.count = std::max(1, int(std::ceil(double(end - t0) / b.len - 0.25)));   // a last chunk of a quarter or more counts
    return b;
}

// move a chunk boundary onto a hit when one is close (within 60 ms)
int64_t snap(const Analysis &a, int64_t f, int rate) {
    int64_t reach = int64_t(0.06 * rate), best = f, bd = reach + 1;
    auto it = std::lower_bound(a.hits.begin(), a.hits.end(), f - reach, [](const Hit &h, int64_t v) { return h.at < v; });
    for (; it != a.hits.end() && it->at <= f + reach; ++it) {
        int64_t d = std::llabs(it->at - f);
        if (d < bd) { bd = d; best = it->at; }
    }
    return best;
}
}  // namespace

AnaPtr analyze(const Pcm &p) {
    auto a = std::make_shared<Analysis>();
    const int64_t N = p.frames();
    const int ch = std::max(1, p.channels), rate = p.rate > 0 ? p.rate : 44100;
    a->rate = rate;
    const int64_t nh = N / HOP;
    if (nh < 8) { a->segments.push_back({0, N}); a->last = N; return a; }
    std::vector<float> rms(static_cast<size_t>(nh), 0.f), hf(static_cast<size_t>(nh), 0.f);
    const int16_t *d = p.data.data();
    float prev = 0;
    for (int64_t h = 0; h < nh; h++) {
        double s2 = 0, d2 = 0;
        for (int64_t i = h * HOP; i < (h + 1) * HOP; i++) {
            float x = ch > 1 ? 0.5f * (float(d[i * ch]) + float(d[i * ch + 1])) : float(d[i * ch]);
            s2 += double(x) * x;
            d2 += double(x - prev) * (x - prev);
            prev = x;
        }
        rms[size_t(h)] = float(std::sqrt(s2 / HOP));
        hf[size_t(h)] = float(d2 / HOP);
    }
    float peak = *std::max_element(rms.begin(), rms.end()), hpeak = *std::max_element(hf.begin(), hf.end());
    if (peak <= 0) { a->segments.push_back({0, N}); a->last = N; return a; }
    const float floor_rms = peak * 0.0032f;   // -50 dB: no hits below it
    const float silence = peak * 0.0056f;     // -45 dB: silence between sounds

    // onset strength: the high-passed energy against the mean of the three hops before
    const float eps = std::max(1e-3f, hpeak * 1e-6f);
    std::vector<float> on(size_t(nh), 0.f);
    for (int64_t h = 0; h < nh; h++) {
        float m = 0;   // before the first hop: silence
        int k = 0;
        for (int64_t j = std::max<int64_t>(0, h - 3); j < h; j++, k++) m += hf[size_t(j)];
        m /= float(std::max(1, k));
        float v = std::log((hf[size_t(h)] + eps) / (m + eps));
        on[size_t(h)] = rms[size_t(h)] > floor_rms && v > 0 ? v : 0.f;
    }
    // peaks, at least 50 ms apart (the stronger one stays)
    const int64_t mind = std::max<int64_t>(2, int64_t(0.05 * rate / HOP));
    std::vector<std::pair<int64_t, float>> cand;
    for (int64_t h = 0; h + 1 < nh; h++) {
        float v = on[size_t(h)];
        if (v < 0.3f || (h > 0 && v < on[size_t(h - 1)]) || v <= on[size_t(h + 1)]) continue;
        if (!cand.empty() && h - cand.back().first < mind) {
            if (v > cand.back().second) cand.back() = {h, v};
            continue;
        }
        cand.push_back({h, v});
    }
    // each hit at its attack: in 32-frame steps of high-passed energy around the hop, the first step that rises a
    // third of the way from the level before (a decaying tail of the last hit) to the peak, a few frames early
    for (auto &c : cand) {
        const int SUB = 32;
        int64_t a0 = std::max<int64_t>(0, c.first * HOP - HOP), a1 = std::min(N - 1, (c.first + 1) * HOP);
        int nsub = int((a1 - a0) / SUB);
        float e[2 * HOP / SUB + 2] = {};
        for (int j = 0; j < nsub; j++) {
            double s2 = 0;
            for (int64_t i = a0 + j * SUB; i < a0 + (j + 1) * SUB; i++) {
                float x0 = float(d[i * ch]), x1 = float(d[(i + 1) * ch]);
                s2 += double(x1 - x0) * (x1 - x0);
            }
            e[j] = float(s2);
        }
        float base = 1e30f, top = 0;
        for (int j = 0; j < std::min(nsub, 3); j++) base = std::min(base, e[j]);
        for (int j = 0; j < nsub; j++) top = std::max(top, e[j]);
        if (c.first == 0) base = 0;
        int64_t at = c.first * HOP;
        for (int j = 0; j < nsub; j++) if (e[j] > base + (top - base) * 0.33f) { at = a0 + int64_t(j) * SUB; break; }
        a->hits.push_back({std::max<int64_t>(0, at - 8), c.second});
    }

    // sounding segments: silences of 150 ms or more split them (pauses inside a break are shorter); shorter than
    // 100 ms is dropped
    const int64_t gap = std::max<int64_t>(2, int64_t(0.15 * rate / HOP)), minlen = int64_t(0.1 * rate);
    int64_t h = 0;
    while (h < nh) {
        while (h < nh && rms[size_t(h)] < silence) h++;
        if (h >= nh) break;
        int64_t s = h, last = h;
        int64_t quiet = 0;
        for (; h < nh; h++) {
            if (rms[size_t(h)] >= silence) { last = h; quiet = 0; }
            else if (++quiet >= gap) break;
        }
        int64_t fs = s * HOP, fe = std::min(N, (last + 1) * HOP + int64_t(0.01 * rate));
        // start on the hit that opens it, if there is one
        for (auto &ht : a->hits) if (ht.at >= fs - HOP && ht.at <= fs + 2 * HOP) { fs = std::min(fs, ht.at); break; }
        if (fe - fs >= minlen) a->segments.push_back({fs, fe});
    }
    if (a->segments.empty()) a->segments.push_back({0, N});
    a->first = a->segments.front().first;
    a->last = a->segments.back().second;

    // tempo: autocorrelation of a full-band onset curve (kicks and snares count more than hats there), 70..190 BPM
    if (a->hits.size() >= 4) {
        std::vector<float> lo(static_cast<size_t>(nh), 0.f);
        for (int64_t h = 1; h < nh; h++) {
            float v = std::log((rms[size_t(h)] + peak * 1e-4f) / (rms[size_t(h - 1)] + peak * 1e-4f));
            lo[size_t(h)] = v > 0 ? v * (rms[size_t(h)] / peak) : 0.f;
        }
        {   // smoothed over +-2 hops, so multiples of a beat that falls between hops still line up
            std::vector<float> sm(lo.size(), 0.f);
            const float K[5] = {1, 2, 3, 2, 1};
            for (int64_t h = 0; h < nh; h++)
                for (int k = -2; k <= 2; k++)
                    if (h + k >= 0 && h + k < nh) sm[size_t(h)] += K[k + 2] * lo[size_t(h + k)];
            lo.swap(sm);
        }
        auto r = [&](int64_t L) {
            double s = 0;
            for (int64_t i = 0; i + L < nh; i++) s += double(lo[size_t(i)]) * lo[size_t(i + L)];
            return s / double(std::max<int64_t>(1, nh - L));
        };
        int64_t lmin = int64_t(60.0 * rate / (190.0 * HOP)), lmax = int64_t(60.0 * rate / (70.0 * HOP)) + 1;
        double best = 0;
        int64_t bl = 0;
        for (int64_t L = lmin; L <= lmax && L < nh / 2; L++) {
            // the beat that also repeats over two beats and a bar wins (a break repeats its bar, a syncopation doesn't)
            double bpm = 60.0 * rate / (double(L) * HOP), w = std::exp(-0.5 * std::pow(std::log2(bpm / 140.0) / 0.8, 2));
            auto rm = [&](int64_t c, int64_t m) { double b = 0; for (int64_t k = c - m; k <= c + m; k++) if (k < nh) b = std::max(b, r(k)); return b; };
            double v = r(L) + rm(2 * L, 1) + 2 * rm(4 * L, 2);
            v *= w;
            if (v > best) { best = v; bl = L; }
        }
        if (bl > 0) {
            auto refine = [&](double L, int mult) {   // the peak near mult x L, parabolic, back to one beat
                int64_t c = int64_t(std::lround(L * mult));
                if (c + 4 >= nh / 2) return L;
                int64_t bk = c;
                double bv = -1;
                for (int64_t k = c - 3; k <= c + 3; k++) { double v = r(k); if (v > bv) { bv = v; bk = k; } }
                double y0 = r(bk - 1), y1 = r(bk), y2 = r(bk + 1), den = y0 - 2 * y1 + y2;
                double off = den != 0 ? 0.5 * (y0 - y2) / den : 0;
                if (off < -1 || off > 1) off = 0;
                return (double(bk) + off) / mult;
            };
            double L = refine(double(bl), 1);
            L = refine(L, 4);
            L = refine(L, 16);
            a->beat = L * HOP;
        }
    }
    return a;
}

int break_count(const Analysis &a, int by, int64_t zs, int64_t ze) {
    by = resolve_by(a, by, zs, ze);
    if (by >= 2) return bar_grid(a, by, zs, ze).count;
    int n = 0;
    each_segment(a, zs, ze, [&](int64_t, int64_t) { n++; });
    return std::max(1, n);
}

bool break_range(const Analysis &a, int by, int k, int64_t zs, int64_t ze, int64_t &s, int64_t &e) {
    if (k < 0) return false;
    by = resolve_by(a, by, zs, ze);
    if (by >= 2) {
        Bars b = bar_grid(a, by, zs, ze);
        if (k >= b.count) return false;
        s = k == 0 ? b.t0 : snap(a, b.t0 + int64_t(b.len * k), a.rate);
        e = k + 1 >= b.count ? b.end : snap(a, b.t0 + int64_t(b.len * (k + 1)), a.rate);
        s = std::max(s, zs);
        e = std::min(e, ze);
        return e - s > 16;
    }
    int n = 0;
    bool found = false;
    each_segment(a, zs, ze, [&](int64_t ss, int64_t ee) { if (n++ == k) { s = ss; e = ee; found = true; } });
    if (!found && k == 0) { s = zs; e = ze; found = ze - zs > 16; }   // no segments in the zone: the whole of it
    return found;
}

int hit_count(const Analysis &a, float thr, int64_t zs, int64_t ze) {
    int n = 0;
    for (auto &h : a.hits) if (h.at >= zs && h.at < ze && h.strength >= thr) n++;
    return n;
}

bool hit_range(const Analysis &a, float thr, int k, int64_t zs, int64_t ze, int64_t &s, int64_t &e) {
    if (k < 0) return false;
    int n = 0;
    size_t i = 0;
    for (; i < a.hits.size(); i++) {
        const Hit &h = a.hits[i];
        if (h.at < zs || h.at >= ze || h.strength < thr) continue;
        if (n++ == k) break;
    }
    if (i >= a.hits.size()) return false;
    s = a.hits[i].at;
    e = ze;
    for (size_t j = i + 1; j < a.hits.size(); j++)
        if (a.hits[j].strength >= thr && a.hits[j].at < ze) { e = a.hits[j].at; break; }
    for (auto &sg : a.segments) if (s >= sg.first && s < sg.second) { e = std::min(e, sg.second); break; }
    return e - s > 16;
}

int make_slices(const Analysis &a, int by, int n, int64_t zs, int64_t ze, int64_t *starts, int64_t *ends) {
    n = std::max(1, std::min(MAX_SLICES, n));
    int64_t t0 = -1, end = zs;
    each_segment(a, zs, ze, [&](int64_t s, int64_t e) { if (t0 < 0) t0 = s; end = e; });
    if (t0 < 0) { t0 = zs; end = ze; }
    const int64_t mind = int64_t(0.03 * a.rate);   // no slice shorter than 30 ms
    int count = 1;
    starts[0] = t0;
    if (by == 1) {
        // even cuts, each moved onto the strongest nearby hit (within a quarter slice, at most 150 ms; nearer counts more)
        const double len = double(end - t0) / n;
        const int64_t reach = std::max<int64_t>(1, std::min(int64_t(len * 0.25), int64_t(0.15 * a.rate)));
        for (int k = 1; k < n; k++) {
            int64_t g = t0 + int64_t(len * k), f = g;
            float best = 0;
            auto it = std::lower_bound(a.hits.begin(), a.hits.end(), g - reach, [](const Hit &h, int64_t v) { return h.at < v; });
            for (; it != a.hits.end() && it->at <= g + reach; ++it) {
                float sc = it->strength * (1.0f - 0.5f * float(std::llabs(it->at - g)) / float(reach));
                if (sc > best) { best = sc; f = it->at; }
            }
            if (f - starts[count - 1] >= mind && end - f >= mind) starts[count++] = f;
        }
    } else {
        // the n-1 strongest hits after the first sound: keep a list sorted by strength (insertion, at most 63)
        int64_t at[MAX_SLICES];
        float st[MAX_SLICES];
        int m = 0;
        for (auto &h : a.hits) {
            if (h.at < t0 + mind || h.at >= end - mind || h.at >= ze) continue;
            if (m == n - 1 && (m == 0 || h.strength <= st[m - 1])) continue;
            int i = m < n - 1 ? m++ : m - 1;   // a full list drops its weakest
            while (i > 0 && st[i - 1] < h.strength) { st[i] = st[i - 1]; at[i] = at[i - 1]; i--; }
            st[i] = h.strength;
            at[i] = h.at;
        }
        std::sort(at, at + m);
        for (int i = 0; i < m; i++) if (at[i] - starts[count - 1] >= mind) starts[count++] = at[i];
    }
    for (int k = 0; k < count; k++) {
        int64_t e = k + 1 < count ? starts[k + 1] : end;
        for (auto &sg : a.segments) if (starts[k] >= sg.first && starts[k] < sg.second) { e = std::min(e, std::max(sg.second, starts[k] + 1)); break; }
        ends[k] = std::min(e, ze);
    }
    return count;
}

int break_at(const Analysis &a, int by, int64_t f, int64_t zs, int64_t ze) {
    int n = break_count(a, by, zs, ze);
    for (int k = 0; k < n; k++) {
        int64_t s, e;
        if (break_range(a, by, k, zs, ze, s, e) && f >= s && f < e) return k;
    }
    return -1;
}

}  // namespace omni

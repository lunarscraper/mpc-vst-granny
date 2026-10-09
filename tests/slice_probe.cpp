// Granny: print what the slicer finds in an audio file (WAV/AIFF/FLAC/Ogg through the WAV reader).
//   slice_probe <file> [hit_sens 0..100]
#include <cstdio>
#include <cstdlib>
#include "core/vfs.hpp"
#include "engine/slicer.hpp"
#include "formats/format.hpp"
using namespace omni;

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: slice_probe <file> [hit_sens]\n"); return 2; }
    register_all();
    Vfs vfs;
    Location loc = vfs.resolve(argv[1]);
    const FormatReader *r = find_reader(*loc.volume, loc.inner);
    if (!r) { std::printf("no reader\n"); return 1; }
    Instrument inst = r->load(loc.volume, loc.inner, r->list(loc.volume, loc.inner)[0].index);
    PcmPtr pcm = inst.zones.at(0).sample->decode();
    AnaPtr a = analyze(*pcm);
    float thr = hit_threshold((argc > 2 ? std::atof(argv[2]) : 50) / 100.0f);
    int64_t zs = 0, ze = pcm->frames();
    double sr = pcm->rate;
    std::printf("%.2f s, %d Hz | tempo %.1f BPM | %zu segments | %d of %zu hits at sens\n", ze / sr, pcm->rate, a->bpm(pcm->rate),
                a->segments.size(), hit_count(*a, thr, zs, ze), a->hits.size());
    const char *by_name[] = {"auto", "silence", "1 bar", "2 bars", "4 bars"};
    for (int by = 0; by < BREAK_BY_COUNT; by++) {
        int n = break_count(*a, by, zs, ze);
        std::printf("breaks by %-7s: %d |", by_name[by], n);
        for (int k = 0; k < n && k < 12; k++) { int64_t s, e; if (break_range(*a, by, k, zs, ze, s, e)) std::printf(" %.3f-%.3f", s / sr, e / sr); }
        std::printf("\n");
    }
    std::printf("hits:");
    int n = hit_count(*a, thr, zs, ze);
    for (int k = 0; k < n && k < 40; k++) { int64_t s, e; if (hit_range(*a, thr, k, zs, ze, s, e)) std::printf(" %.3f", s / sr); }
    std::printf("\n");
    return 0;
}

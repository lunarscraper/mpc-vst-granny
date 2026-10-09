// Granny: offline render through mpc_engine(), for listening to the granular engine and GRAIN FX and for a rough
// CPU figure on the desktop.
//   grain_render <instrument> <out.wav> [key=value ...] [notes=60,67] [secs=6] [hold=4] [bpm=120] [play=1]
// key=value pairs are plugin parameters in their own units (e.g. engine=1 g_scan=50 fx_mix=40 fx_mode=2).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static std::string get(const mpc_engine_t *e, void *inst, const char *key) {
    char buf[4096];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}

static void put32(FILE *f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: grain_render <instrument> <out.wav> [key=value ...]\n"); return 2; }
    const mpc_engine_t *e = mpc_engine();
    void *inst = e->create("/tmp/granny-test");
    std::string state = std::string("granny 1\npath=") + argv[1] + "\npreset=0\n";
    e->set_param(inst, "state", state.c_str());
    std::vector<int> notes = {60};
    double secs = 6, hold = 4, bpm = 120;
    bool play = false;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "notes") {
            notes.clear();
            for (size_t p = 0; p < v.size();) { size_t c = v.find(',', p); notes.push_back(std::atoi(v.substr(p, c - p).c_str())); if (c == std::string::npos) break; p = c + 1; }
        } else if (k == "secs") secs = std::atof(v.c_str());
        else if (k == "hold") hold = std::atof(v.c_str());
        else if (k == "bpm") bpm = std::atof(v.c_str());
        else if (k == "play") play = std::atoi(v.c_str()) != 0;
        else e->set_param(inst, k.c_str(), v.c_str());
    }
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", bpm);
    e->set_param(inst, "lfo_bpm", b);
    if (play) e->set_param(inst, "transport", "1");
    std::vector<int16_t> buf(256);
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        e->render(inst, buf.data(), 128);
        std::string info = get(e, inst, "prog_info"), status = get(e, inst, "status");
        if (info.find("zones") != std::string::npos && status.compare(0, 7, "Loading") != 0 && status.compare(0, 7, "Reading") != 0) break;
        if (status.compare(0, 5, "Error") == 0) { std::printf("FAIL %s\n", status.c_str()); return 1; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) { std::printf("FAIL timeout\n"); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("%s | %s\n", get(e, inst, "prog_name").c_str(), get(e, inst, "prog_info").c_str());
    int blocks = int(secs * 44100 / 128), hold_blocks = int(hold * 44100 / 128);
    std::vector<int16_t> out;
    out.reserve(size_t(blocks) * 256);
    for (int n : notes) { uint8_t on[3] = {0x90, uint8_t(n), 100}; e->midi(inst, on, 3); }
    double worst = 0, total = 0;
    int max_grains = 0;
    std::string status_mid;
    for (int k = 0; k < blocks; k++) {
        if (k == hold_blocks) for (int n : notes) { uint8_t off[3] = {0x80, uint8_t(n), 0}; e->midi(inst, off, 3); }
        auto a = std::chrono::steady_clock::now();
        e->render(inst, buf.data(), 128);
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
        worst = std::max(worst, us);
        total += us;
        out.insert(out.end(), buf.begin(), buf.end());
        if (k == hold_blocks / 2) status_mid = get(e, inst, "status") + " | " + get(e, inst, "slice_info") + " | " + get(e, inst, "slice_active");
    }
    double sum = 0;
    int peak = 0;
    size_t clipped = 0;
    for (int16_t s : out) { sum += double(s) * s; peak = std::max(peak, std::abs(int(s))); if (std::abs(int(s)) >= 32700) clipped++; }
    double rms = std::sqrt(sum / double(out.size())) / 32768.0;
    std::printf("rms %.4f peak %d clipped %zu | status mid-note: %s | render avg %.1f us, worst %.1f us per 128 frames (2902 us = real time)\n",
                rms, peak, clipped, status_mid.c_str(), total / blocks, worst);
    (void)max_grains;
    FILE *f = std::fopen(argv[2], "wb");
    if (!f) return 1;
    uint32_t bytes = uint32_t(out.size() * 2);
    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, 44100); put32(f, 44100 * 4); put16(f, 4); put16(f, 16);
    std::fwrite("data", 1, 4, f); put32(f, bytes);
    std::fwrite(out.data(), 2, out.size(), f);
    std::fclose(f);
    e->destroy(inst);
    return peak == 0 ? 1 : 0;
}

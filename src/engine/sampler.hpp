// Granny (based on Omni Sampler): the playing engine. A Program (instrument + decoded audio) is built on the loader thread and handed
// to the audio thread through set_program(); the audio thread fades its voices out, swaps, and hands the old program
// back through take_retired() so it is freed off the audio thread.
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "../core/model.hpp"
#include "dsp.hpp"
#include "slicer.hpp"

namespace omni {

struct Program {
    uint32_t serial = 0;               // the loader's load counter, to match LoadedInfo
    Instrument inst;
    std::vector<PcmPtr> pcm;           // per zone (shared between zones of one sample)
    std::vector<AnaPtr> ana;           // per zone: the sample's slicing analysis (may be null)
    std::vector<uint32_t> rr;          // per zone round-robin counters (audio thread only)
    std::vector<std::vector<int>> by_key;   // zone indices per MIDI key
    int64_t bytes = 0;
    void index();                      // fill by_key and rr
};

// The modulation matrix: sources and destinations (the skin's MOD page; option indices in params.json).
enum ModSrc { MS_OFF, MS_LFO1, MS_LFO2, MS_WHEEL, MS_AT, MS_BEND, MS_VEL, MS_KEY, MS_RAND, MS_ENV, MS_COUNT };
enum ModDst { MD_OFF, MD_PITCH, MD_CUTOFF, MD_RES, MD_VOL, MD_PAN, MD_START, MD_DRIVE, MD_REVERB, MD_LFO1_RATE, MD_LFO2_RATE,
              MD_GPOS, MD_GSIZE, MD_GDENS, MD_GSPRAY, MD_GPITCH, MD_FXMIX, MD_COUNT };
constexpr int MOD_SLOTS = 8;
// full-scale (amount 100%, source 1) ranges of the destinations
constexpr float MOD_PITCH_SEMIS = 12, MOD_CUTOFF_OCT = 4, MOD_RES = 0.5f, MOD_RATE_OCT = 3;
// grain destinations: position +-50% of the sample, size and density +-3 octaves, spray +-100%, grain pitch +-24 st
constexpr float MOD_GPOS = 0.5f, MOD_GSIZE_OCT = 3, MOD_GDENS_OCT = 3, MOD_GPITCH_SEMIS = 24;   // volume: gain 1 + x (silent at -100%, +6 dB at +100%)

// Global controls, written by the UI thread, read per block by the audio thread.
struct Settings {
    // LFOs: wave 0 sine 1 triangle 2 saw up 3 saw down 4 square 5 sample & hold; sync 0 = free (rate_hz), else a
    // tempo division (Sampler::SYNC_BEATS); retrig 0 free-running, 1 restarts at every note
    std::atomic<int> lfo_wave[2]{}, lfo_sync[2]{}, lfo_retrig[2]{};
    std::atomic<float> lfo_rate[2]{};
    std::atomic<int> mod_src[MOD_SLOTS]{}, mod_dst[MOD_SLOTS]{};
    std::atomic<float> mod_amt[MOD_SLOTS]{};   // -1..1
    // instrument slots A-D: key range, gain (linear), transpose (semitones), mute; layer_mode 0 = layer (every
    // slot in its range), 1 = keyswitch (ks_base..ks_base+3 pick the one slot that plays; they make no sound)
    std::atomic<int> slot_lo[4]{}, slot_hi[4]{}, slot_mute[4]{};
    std::atomic<float> slot_gain[4]{}, slot_tune[4]{};
    std::atomic<int> layer_mode{0}, ks_base{24};
    std::atomic<float> volume_db{0}, pan{0}, transpose{0}, tune_cents{0};
    std::atomic<float> cutoff_hz{20000}, resonance{0}, filter_type{0}, filter_vel{0}, filter_env{0};
    std::atomic<float> attack_add{0}, decay_scale{1}, sustain_scale{1}, release_add{0};
    std::atomic<float> vel_sens{1}, bend_range{0};   // bend 0 = the instrument's own
    std::atomic<int> polyphony{48}, voice_mode{0}, interpolation{2};   // mode 0 poly 1 mono 2 legato; interp 0 none 1 linear 2 cubic
    std::atomic<float> glide_s{0};
    std::atomic<float> reverb_mix{0}, reverb_size{0.6f}, reverb_damp{0.4f};
    std::atomic<float> drive{0};
    std::atomic<int> zone_filter_on{1}, zone_env_on{1};

    // Granular engine (engine 1): every note plays a cloud of grains from its zone's sample instead of one playhead.
    //   position 0..1 of the zone's sample, scan -2..2 (playhead speed through it, 1 = original speed, 0 = frozen),
    //   size_s grain length, density_hz grains per second; size_sync / rate_sync 0 = free, else a GRAIN_DIV index;
    //   contour -1..1 (window: -1 percussive ramp down, 0 smooth, +1 swell), spray 0..1 with spray_mode 0 random per
    //   grain, 1 warp (a smooth random drift); stereo 0..1 random grain pan; pitch semitones; pattern 0..1 (a fixed
    //   pseudo-random pitch pattern of octaves and fifths); detune 0..1 (+-1 semitone at most, random per grain);
    //   reverse 0..1 (share of grains playing backwards); grain_limit = grains for all voices together
    std::atomic<int> engine{0};
    std::atomic<float> g_position{0.25f}, g_scan{0}, g_size_s{0.12f}, g_density_hz{24}, g_contour{0}, g_spray{0.1f};
    std::atomic<int> g_size_sync{0}, g_rate_sync{0}, g_spray_mode{0}, g_limit{96};
    std::atomic<float> g_stereo{0.3f}, g_pitch{0}, g_pattern{0}, g_detune{0}, g_reverse{0};
    // GRAIN FX: a granular delay on the voices' sum (before drive and reverb), after the S-4's buffer granular idea.
    //   fx_mix 0..1 (0 = off), fx_mode 0 delay sync (delay = fx_rate), 1 delay free (fx_time_s), 2 stretch (the read
    //   head runs at fx_speed against the write head), fx_rate a GRAIN_DIV index (the grain clock), fx_size_s,
    //   fx_pitch semitones, fx_spray 0..1 (position and stereo), fx_feedback 0..0.95, fx_pattern 0..1, fx_contour -1..1,
    //   fx_lock: the buffer keeps what it holds (freeze)
    std::atomic<float> fx_mix{0}, fx_time_s{0.375f}, fx_speed{0.5f}, fx_size_s{0.15f}, fx_pitch{0}, fx_spray{0.2f};
    std::atomic<float> fx_feedback{0.3f}, fx_pattern{0}, fx_contour{0};
    std::atomic<int> fx_mode{0}, fx_rate{6}, fx_lock{0};
    // SLICE: what a key plays. key_mode 0 pitch (keys transpose), 1 positions (key_count keys from key_base spread
    // over the region), 2 breaks (key_base = break 1, ...), 3 hits (key_base = hit 1, ...). In 1-3 keys play at the
    // zone's root. region 0 = the whole zone, n = break n (pitch and positions modes); break_by as slicer.hpp;
    // hit_thr the strength a hit needs; scan_fit 0 off, else the bars a break or region takes when SCAN plays through
    // it (SCAN_FIT_BARS)
    std::atomic<int> key_mode{0}, key_base{36}, key_count{16}, break_by{0}, region{0}, scan_fit{0};
    // key mode 4 (slices): the sample cut into slice_count slices (slice_by 0 at the strongest hits, 1 on a grid)
    std::atomic<int> slice_count{16}, slice_by{1};
    std::atomic<float> hit_thr{1.5f};
};

constexpr double SCAN_FIT_BARS[] = {0, 0.5, 1, 2, 4, 8};
constexpr int SCAN_FIT_COUNT = int(sizeof SCAN_FIT_BARS / sizeof SCAN_FIT_BARS[0]);

// Tempo divisions of the grain clocks and synced sizes, in quarter notes (index 0 = free).
constexpr double GRAIN_DIV_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 0.0625, 1.0 / 3, 1.0 / 6, 0.75, 0.375};
constexpr int GRAIN_DIV_COUNT = int(sizeof GRAIN_DIV_BEATS / sizeof GRAIN_DIV_BEATS[0]);
constexpr int GRAINS_PER_VOICE = 16;
constexpr int FX_GRAINS = 32;
constexpr int FX_BUFFER = 1 << 18;   // frames (5.9 s at 44.1 kHz), stereo float

struct Grain {
    bool on = false;
    double pos = 0, step = 0;        // read position (source frames) and increment (negative: backwards)
    int len = 0, t = 0;              // length and age in output samples
    float inv_p = 0, inv_q = 0, peak = 0.5f;   // window: rise over [0, peak), fall over [peak, 1)
    float gl = 1, gr = 1;            // pan * level
};

// The grain window: a raised-cosine rise in a table (0..1), read forwards for the attack and backwards for the fall.
constexpr int WIN_N = 512;
float grain_window(const Grain &g);

constexpr int MAX_VOICES = 64;

class Sampler {
public:
    Sampler();
    ~Sampler();
    Settings settings;

    // Loader thread: hand over a new program (takes ownership). Returns false while a previous hand-over is pending.
    bool set_program(Program *p);
    // Loader / UI thread: a program the audio thread no longer uses, or null.
    Program *take_retired();
    bool switching() const { return pending_.load() != nullptr; }

    // Audio thread
    void midi(const uint8_t *msg, int len);
    void render(int16_t *out, int frames);
    void transport(double ppq, double tempo, int playing, int ppq_valid);   // before render, host block start
    // from the wrapper (any thread): the host tempo and play state; a play start or a jump back resets the song position
    void host_tempo(double bpm) { if (bpm > 1) host_bpm_.store(float(bpm)); }
    void host_transport(bool playing) { host_play_.store(playing ? 1 : 0); host_play_rev_.fetch_add(1); }
    std::atomic<int> active_grains{0};
    // SLICE page: a tap on the waveform (bin 0..47) plays from there for a moment (any thread)
    void touch(int bin) { touch_req_.store(bin + 1); }
    // the region the last note played (frames in its zone's sample), for the displays; kind 0 whole/region,
    // 1 position, 2 break, 3 hit; index = break/hit/position number
    std::atomic<int> last_region_zone{-1}, last_region_kind{0}, last_region_index{0};
    std::atomic<int64_t> last_region_s{0}, last_region_e{0};
    std::atomic<uint32_t> region_events{0};
    std::atomic<float> lfo_out[2]{};          // the LFOs' last values (-1..1), for the skin
    std::atomic<int> active_slot{0};          // keyswitch mode: the slot that plays (set by keyswitch notes)

    // Diagnostics (any thread)
    std::atomic<int> active_voices{0};
    std::atomic<float> peak{0};
    std::atomic<int> notes_played{0};
    std::atomic<int> last_note{-1};
    // the zone the last note-on started (first one when several), for the skin's "active layer" readout
    std::atomic<int> last_zone{-1}, last_vel{0};
    std::atomic<uint32_t> last_zone_prog{0}, layer_events{0};
    std::atomic<uint8_t> key_down[128]{};   // keys held (any channel): the skin's pad lights
    std::atomic<float> slowest_attack{0};   // the program's longest delay + attack + hold (s): how long a pad tap holds

private:
    struct Voice {
        bool on = false;
        const Zone *zone = nullptr;
        const Pcm *pcm = nullptr;
        int zi = 0, note = 0, vel = 0, chan = 0;
        uint32_t age = 0;
        double pos = 0, step_base = 0;
        int dir = 1;
        bool released = false, held_by_pedal = false, looping = false, fading = false, reverse_ = false;
        int64_t start = 0, stop = 0, loop_start = 0, loop_end = 0;   // loop_end exclusive
        LoopType loop_type = LoopType::Forward;
        bool loop_until_release = false, one_shot = false;
        double loop_tune = 0;                             // semitones while inside the loop
        float gain = 1, pan_l = 1, pan_r = 1, xfade = 1;
        Env amp, fenv, penv;
        LfoState alfo, plfo, flfo;
        VoiceFilter zf[2], mf[2];    // zone filter, master filter (left, right)
        float glide_from = 0, glide = 0, glide_inc = 0;   // semitones offset gliding to 0
        float last_l = 0, last_r = 0;
        float rnd = 0;                                    // per-note random source, -1..1
        float mod_cut_mul = 1, mod_res = 0, mod_gain = 1, mod_pan = 0;   // matrix results for this sub-block
        // granular voices
        bool granular = false;
        double scan = 0;                                  // the scan offset, a share of the sample (wraps)
        double spawn_in = 0;                              // output samples to the next grain
        uint32_t grain_no = 0;                            // grains started (the pattern's step)
        float warp = 0, warp_from = 0, warp_to = 0, warp_t = 1;   // spray WARP: a smooth random walk -1..1
        float m_gpos = 0, m_gsize = 1, m_gdens = 1, m_gspray = 0, m_gpitch = 0;   // matrix results
        int ngrains = 0;
        Grain grains[GRAINS_PER_VOICE];
        int pitch_note = 60;                              // the note that sets the pitch (the root in slice modes)
        float gfix = 0;                                   // slice modes: the position the key chose (share of the region)
        double fit_bars = 0;                              // SCAN FIT: bars for one pass through the region (0 = SCAN)
        bool touch = false;                               // started by a tap on the waveform
    };

    std::atomic<Program *> pending_{nullptr}, retired_{nullptr};
    Program *prog_ = nullptr;
    Voice voices_[MAX_VOICES];
    uint32_t age_counter_ = 0;
    uint32_t rand_ = 12345;
    float bend_ = 0, modwheel_ = 0, cc7_ = 1, cc10_ = 0, cc11_ = 1;
    bool sustain_ = false;
    float at_ = 0;                                         // channel / poly aftertouch 0..1
    // modulation (audio thread): transport, LFO state, the global sources and this block's matrix snapshot
    double ppq_ = 0, tempo_ = 120;
    bool playing_ = false, ppq_valid_ = false;
    double lfo_phase_[2] = {};
    float lfo_sh_[2] = {}, lfo_sh_prev_[2] = {};
    std::atomic<float> host_bpm_{0};
    std::atomic<int> host_play_{0}, host_play_rev_{0};
    int seen_play_rev_ = 0;
    int grains_total_ = 0;                                 // grains sounding (last block), for the grain limit
    float g_fxmix_add_ = 0;
    uint32_t grand_ = 0x9E3779B9u;                          // grain randomness (audio thread)
    float grand() { grand_ = grand_ * 1664525u + 1013904223u; return float(int32_t(grand_)) / 2147483648.0f; }   // -1..1
    double beats_to_samples(double beats) const { return beats * 60.0 / tempo_ * SR; }
    // GRAIN FX state
    std::vector<float> fxbuf_;                              // interleaved stereo ring
    int fx_w_ = 0;                                          // write index
    double fx_read_ = 0;                                    // stretch mode: the read head (frames behind fx_w_)
    double fx_spawn_in_ = 0, fx_last_q_ = -1, block_ppq0_ = 0;
    uint32_t fx_grain_no_ = 0;
    float fx_warp_ = 0;
    Grain fxg_[FX_GRAINS];
    float fx_level_ = 0;                                    // fades the effect in and out (no clicks on MIX 0)
    void render_fx(float *l, float *r, int frames);
    float gsrc_[MS_COUNT] = {};
    int slot_src_[MOD_SLOTS] = {}, slot_dst_[MOD_SLOTS] = {};
    float slot_amt_[MOD_SLOTS] = {};
    float g_drive_add_ = 0, g_rev_add_ = 0;
    bool cut_mod_ = false;
    void update_mod(int frames);
    float src_val(const Voice &v, int s) const;
    bool slot_plays(const Zone &z, int note) const;
    bool held_[16][128] = {};
    int held_count_ = 0;
    int last_mono_note_ = -1;
    float last_pitch_semi_ = 0;
    int swap_fade_ = 0;
    Reverb reverb_;
    std::vector<float> mixl_, mixr_;

    void note_on(int chan, int note, int vel);
    void note_off(int chan, int note);
    // a slice region chosen by the key (or a tap): frames [s, e) of the zone's sample and where in it to start
    struct Slice { bool on = false; int64_t s = 0, e = 0; float pos = 0; int kind = 0, index = 0; };
    bool key_slice(int zi, int note, Slice &sl) const;   // false: this key plays nothing in the current key mode
    void start_voice(int zi, int chan, int note, int vel, bool release_trigger, float glide_semis, const Slice *slice = nullptr);
    std::atomic<int> touch_req_{0};
    // the slices of the last zone asked for (audio thread): rebuilt when the zone, count or method changes
    struct SliceCache { const Analysis *a = nullptr; uint32_t serial = 0; int64_t zs = 0, ze = 0; int n = 0, by = -1, count = 0;
                        int64_t s[MAX_SLICES], e[MAX_SLICES]; };
    mutable SliceCache slice_cache_;
    int touch_gate_ = 0;
    Voice *alloc_voice();
    void all_off(bool hard);
    void render_voice(Voice &v, float *l, float *r, int n);
    void render_grains(Voice &v, float *l, float *r, int n);
    void spawn_grain(Voice &v, double step, float spray, float size_mul, float pitch_semis);
    void block_mod(Voice &v, int n, double &step, float &cutoff_mul);
    void swap_program();
};

}  // namespace omni

// Granny (based on Omni Sampler): voices, note handling, grains and rendering (see sampler.hpp).
#include "sampler.hpp"
#include <algorithm>
#include <cmath>

namespace omni {

static inline double clampd_(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void Program::index() {
    by_key.assign(128, {});
    rr.assign(inst.zones.size(), 0);
    for (size_t i = 0; i < inst.zones.size(); i++) {
        const Zone &z = inst.zones[i];
        if (!pcm[i] || pcm[i]->frames() == 0) continue;
        for (int k = z.key_lo; k <= z.key_hi && k < 128; k++) by_key[size_t(k)].push_back(int(i));
    }
}

Sampler::Sampler() {
    for (int i = 0; i < 4; i++) { settings.slot_hi[i].store(127); settings.slot_gain[i].store(1); }
    mixl_.assign(4096, 0);
    mixr_.assign(4096, 0);
    fxbuf_.assign(size_t(FX_BUFFER) * 2, 0.0f);
}

// ---------------------------------------------------------------------------------------------------------------
// grain helpers

namespace {
// raised-cosine rise sin^2(pi/2 x), x = 0..1 in WIN_N steps
struct WinTable {
    float v[WIN_N + 2];
    WinTable() { for (int i = 0; i <= WIN_N + 1; i++) { double x = std::min(1.0, double(i) / WIN_N); v[i] = float(std::sin(x * 1.5707963267948966) * std::sin(x * 1.5707963267948966)); } }
};
const WinTable WIN;

// The grain pattern (the S-4's PATTERN idea): 16 steps of octave / fifth jumps. A step plays its jump when its rank is
// below the PATTERN amount, so turning PATTERN up adds jumps one by one, always the same ones.
const float PATTERN_SEMIS[16] = {0, 12, 0, 7, -12, 0, 19, 12, 0, -5, 24, 7, 0, 12, -12, 5};
const uint8_t PATTERN_RANK[16] = {15, 3, 11, 6, 9, 13, 1, 8, 14, 4, 0, 10, 12, 2, 7, 5};
inline float pattern_semis(uint32_t k, float amount) {
    int i = int(k & 15);
    return (float(PATTERN_RANK[i]) + 0.5f) / 16.0f < amount ? PATTERN_SEMIS[i] : 0.0f;
}

inline double frac(double x) { return x - std::floor(x); }

// set a grain's window: contour -1 (peak at the start: a ramp down) .. 0 (centred) .. +1 (peak at the end: a swell)
inline void grain_shape(Grain &g, int len, float contour) {
    g.len = std::max(16, len);
    float peak = 0.5f + 0.47f * std::max(-1.0f, std::min(1.0f, contour));
    int pl = std::max(1, std::min(g.len - 1, int(peak * float(g.len))));
    g.inv_p = float(WIN_N) / float(pl);
    g.inv_q = float(WIN_N) / float(g.len - pl);
    g.peak = float(pl);
}

inline float window_at(const Grain &g) {
    float t = float(g.t);
    if (t < g.peak) return WIN.v[int(t * g.inv_p)];
    return WIN.v[int((float(g.len) - t) * g.inv_q)];
}

// the ring's input: untouched up to full scale, then bent softly towards 2 (keeps feedback from running away)
inline float soft(float x) {
    float a = std::fabs(x);
    if (a <= 1) return x;
    float y = 1 + (a - 1) / a;
    return x > 0 ? y : -y;
}
}  // namespace

float grain_window(const Grain &g) { return window_at(g); }

Sampler::~Sampler() {
    delete prog_;
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
}

bool Sampler::set_program(Program *p) {
    Program *expected = nullptr;
    return pending_.compare_exchange_strong(expected, p);
}

Program *Sampler::take_retired() { return retired_.exchange(nullptr); }

// ---------------------------------------------------------------------------------------------------------------
// MIDI

void Sampler::midi(const uint8_t *m, int len) {
    if (len < 1) return;
    // events come on the audio thread just before render: a program that arrived meanwhile takes this note
    if (pending_.load() && !swap_fade_) swap_program();
    int status = m[0] & 0xF0, chan = m[0] & 0x0F;
    int d1 = len > 1 ? m[1] & 0x7F : 0, d2 = len > 2 ? m[2] & 0x7F : 0;
    switch (status) {
    case 0x90: if (d2) note_on(chan, d1, d2); else note_off(chan, d1); break;
    case 0x80: note_off(chan, d1); break;
    case 0xD0: at_ = d1 / 127.0f; break;
    case 0xA0: at_ = d2 / 127.0f; break;
    case 0xE0: bend_ = float((d2 << 7 | d1) - 8192) / 8192.0f; break;
    case 0xB0:
        switch (d1) {
        case 1: modwheel_ = d2 / 127.0f; break;
        case 7: cc7_ = d2 / 127.0f; break;
        case 10: cc10_ = (d2 - 64) / 64.0f; break;
        case 11: cc11_ = d2 / 127.0f; break;
        case 64:
            sustain_ = d2 >= 64;
            if (!sustain_)
                for (auto &v : voices_)
                    if (v.on && v.held_by_pedal) { v.held_by_pedal = false; if (!v.one_shot) { v.released = true; v.amp.release(); v.fenv.release(); v.penv.release(); } }
            break;
        case 120: all_off(true); break;
        case 121: bend_ = 0; modwheel_ = 0; cc7_ = 1; cc10_ = 0; cc11_ = 1; break;
        case 123: case 124: case 125: case 126: case 127:
            for (int c = 0; c < 16; c++) for (int n = 0; n < 128; n++) if (held_[c][n]) note_off(c, n);
            all_off(false);
            break;
        }
        break;
    default: break;
    }
}

Sampler::Voice *Sampler::alloc_voice() {
    int limit = settings.polyphony.load();
    if (prog_ && prog_->inst.polyphony > 0 && prog_->inst.polyphony < limit) limit = prog_->inst.polyphony;
    if (limit < 1) limit = 1;
    if (limit > MAX_VOICES - 8) limit = MAX_VOICES - 8;   // keep slots for the fade-out tails of stolen voices
    int sounding = 0;
    Voice *oldest = nullptr, *oldest_released = nullptr;
    for (auto &v : voices_) {
        if (!v.on || v.fading) continue;
        sounding++;
        if (!oldest || v.age < oldest->age) oldest = &v;
        if (v.released && (!oldest_released || v.age < oldest_released->age)) oldest_released = &v;
    }
    if (sounding >= limit) {
        Voice *victim = oldest_released ? oldest_released : oldest;
        if (victim) { victim->fading = true; victim->amp.fast_release(0.004f); }
    }
    for (auto &v : voices_) if (!v.on) return &v;
    // no free slot at all: take the oldest fading or oldest voice (hard cut)
    Voice *best = nullptr;
    for (auto &v : voices_) if (!best || (v.fading && !best->fading) || (v.fading == best->fading && v.age < best->age)) best = &v;
    return best;
}

void Sampler::all_off(bool hard) {
    for (auto &v : voices_) {
        if (!v.on) continue;
        if (hard) v.on = false;
        else { v.released = true; v.fading = true; v.amp.fast_release(0.01f); }
    }
    sustain_ = false;
}

void Sampler::note_on(int chan, int note, int vel) {
    for (int i = 0; i < 2; i++)
        if (settings.lfo_retrig[i].load() == 1) { lfo_phase_[i] = 0; gsrc_[MS_LFO1 + i] = 0; }
    last_note.store(note);
    notes_played.fetch_add(1);
    bool others_held = held_count_ > 0;
    if (!held_[chan][note]) { held_[chan][note] = true; held_count_++; key_down[note].fetch_add(1); }
    if (settings.layer_mode.load() == 1) {   // keyswitch notes choose the slot and make no sound
        int ks = settings.ks_base.load();
        if (note >= ks && note < ks + 4) { active_slot.store(note - ks); return; }
    }
    if (!prog_ || swap_fade_) return;

    int mode = settings.voice_mode.load();
    float glide_semis = 0;
    if (mode != 0) {
        if (last_mono_note_ >= 0 && settings.glide_s.load() > 0.0005f && (mode == 2 ? others_held : true))
            glide_semis = float(last_mono_note_ - note);
        for (auto &v : voices_) if (v.on && !v.released) { v.released = true; v.fading = true; v.amp.fast_release(mode == 2 && others_held ? 0.03f : 0.008f); }
    }
    last_mono_note_ = note;

    rand_ = rand_ * 1664525u + 1013904223u;
    double rnd = double(rand_ >> 8) / double(1u << 24);
    const auto &list = prog_->by_key[size_t(note)];
    bool shown = false;
    for (int zi : list) {
        const Zone &z = prog_->inst.zones[size_t(zi)];
        if (vel < std::max(1, z.vel_lo) && !(z.vel_lo == 0 && vel >= 1 && z.vel_hi >= vel)) continue;
        if (vel > z.vel_hi) continue;
        if (z.midi_channel >= 0 && z.midi_channel != chan) continue;
        if (!slot_plays(z, note)) continue;
        Trigger tr = z.trigger;
        if (tr == Trigger::Release) continue;
        if (tr == Trigger::First && others_held) continue;
        if (tr == Trigger::Legato && !others_held) continue;
        if (z.play_logic == PlayLogic::RoundRobin) {
            uint32_t c = prog_->rr[size_t(zi)]++;
            int len = std::max(1, z.seq_length);
            if (int(c % uint32_t(len)) != z.seq_position - 1) continue;
        } else if (z.play_logic == PlayLogic::Random) {
            if (rnd < z.rand_lo || rnd >= z.rand_hi) continue;
        }
        Slice sl;
        if (!key_slice(zi, note, sl)) continue;
        start_voice(zi, chan, note, vel, false, glide_semis, sl.on ? &sl : nullptr);
        if (!shown) {
            shown = true;
            if (last_zone.load() != zi || last_vel.load() != vel || last_zone_prog.load() != prog_->serial) {
                last_zone.store(zi);
                last_vel.store(vel);
                last_zone_prog.store(prog_->serial);
                layer_events.fetch_add(1);
            }
        }
    }
}

void Sampler::note_off(int chan, int note) {
    if (held_[chan][note]) { held_[chan][note] = false; held_count_--; key_down[note].fetch_sub(1); }
    int vel = 0;
    for (auto &v : voices_) {
        if (!v.on || v.note != note || v.chan != chan || v.released) continue;
        vel = std::max(vel, v.vel);
        if (v.one_shot) continue;
        if (sustain_) { v.held_by_pedal = true; continue; }
        v.released = true;
        v.amp.release(); v.fenv.release(); v.penv.release();
    }
    if (!prog_ || swap_fade_) return;
    // release triggers
    if (vel == 0) vel = 64;
    for (int zi : prog_->by_key[size_t(note)]) {
        const Zone &z = prog_->inst.zones[size_t(zi)];
        if (z.trigger != Trigger::Release || vel < z.vel_lo || vel > z.vel_hi) continue;
        if (z.midi_channel >= 0 && z.midi_channel != chan) continue;
        if (!slot_plays(z, note)) continue;
        start_voice(zi, chan, note, vel, true, 0);
    }
    if (settings.voice_mode.load() != 0 && held_count_ > 0) {
        // mono: fall back to the most recent still held note (simple last-note priority)
        for (int n = 127; n >= 0; n--) if (held_[chan][n]) { int keep = n; held_[chan][keep] = false; held_count_--; note_on(chan, keep, 100); break; }
    }
}

bool Sampler::slot_plays(const Zone &z, int note) const {
    int s = z.slot < 0 || z.slot > 3 ? 0 : z.slot;
    if (settings.slot_mute[s].load()) return false;
    if (settings.layer_mode.load() == 1) return s == active_slot.load();
    return note >= settings.slot_lo[s].load() && note <= settings.slot_hi[s].load();
}

// The SLICE key modes: which part of the zone's sample a key plays. Pitch mode with REGION 0 plays the whole zone.
bool Sampler::key_slice(int zi, int note, Slice &sl) const {
    sl = Slice();
    const Zone &z = prog_->inst.zones[size_t(zi)];
    const Pcm *pcm = prog_->pcm[size_t(zi)].get();
    const Analysis *a = zi < int(prog_->ana.size()) ? prog_->ana[size_t(zi)].get() : nullptr;
    int mode = settings.key_mode.load(), region = settings.region.load(), by = settings.break_by.load();
    if (!pcm || !a) return mode == 0;
    int64_t frames = pcm->frames();
    int64_t ze = z.stop > 0 ? std::min<int64_t>(z.stop, frames) : frames, zs = std::max<int64_t>(0, std::min<int64_t>(z.start, ze));
    int64_t rs = zs, re = ze;
    if (mode <= 1 && region > 0 && !break_range(*a, by, region - 1, zs, ze, rs, re)) { rs = zs; re = ze; }
    int k = note - settings.key_base.load();
    switch (mode) {
    case 1: {
        int n = std::max(1, settings.key_count.load());
        if (k < 0 || k >= n) return false;
        sl.on = true; sl.s = rs; sl.e = re; sl.pos = float(k) / float(n); sl.kind = 1; sl.index = k;
        return true;
    }
    case 2:
        if (k < 0 || !break_range(*a, by, k, zs, ze, sl.s, sl.e)) return false;
        sl.on = true; sl.kind = 2; sl.index = k;
        return true;
    case 3:
        if (k < 0 || !hit_range(*a, settings.hit_thr.load(), k, zs, ze, sl.s, sl.e)) return false;
        sl.on = true; sl.kind = 3; sl.index = k;
        return true;
    case 4: {
        SliceCache &c = slice_cache_;
        int n = settings.slice_count.load(), sb = settings.slice_by.load();
        if (c.a != a || c.serial != prog_->serial || c.zs != zs || c.ze != ze || c.n != n || c.by != sb) {
            c.a = a; c.serial = prog_->serial; c.zs = zs; c.ze = ze; c.n = n; c.by = sb;
            c.count = make_slices(*a, sb, n, zs, ze, c.s, c.e);
        }
        if (k < 0 || k >= c.count) return false;
        sl.on = true; sl.s = c.s[k]; sl.e = c.e[k]; sl.kind = 4; sl.index = k;
        return true;
    }
    default:
        if (region > 0 && (rs != zs || re != ze)) { sl.on = true; sl.s = rs; sl.e = re; sl.kind = 0; sl.index = region - 1; sl.pos = -1; }
        return true;
    }
}

void Sampler::start_voice(int zi, int chan, int note, int vel, bool release_trigger, float glide_semis, const Slice *slice) {
    const Zone &z = prog_->inst.zones[size_t(zi)];
    const Pcm *pcm = prog_->pcm[size_t(zi)].get();
    if (!pcm || pcm->frames() == 0) return;

    // choke groups
    if (z.exclusive_group > 0)
        for (auto &o : voices_) {
            if (!o.on || o.fading) continue;
            int by = o.zone->off_by < 0 ? o.zone->exclusive_group : o.zone->off_by;
            if (by == z.exclusive_group && !(o.note == note && o.age == age_counter_)) { o.fading = true; o.amp.fast_release(0.004f); }
        }

    Voice *vp = alloc_voice();
    if (!vp) return;
    Voice &v = *vp;
    v = Voice();
    v.on = true;
    v.zone = &z;
    v.pcm = pcm;
    v.zi = zi; v.note = note; v.vel = vel; v.chan = chan;
    v.age = ++age_counter_;
    int64_t frames = pcm->frames();
    v.start = std::min<int64_t>(std::max<int64_t>(0, z.start), frames - 1);
    v.stop = z.stop > 0 ? std::min<int64_t>(z.stop, frames) : frames;
    if (v.stop <= v.start) v.stop = frames;
    v.pitch_note = note;
    if (slice && slice->e - slice->s > 16) {
        v.start = std::max<int64_t>(0, std::min(slice->s, frames - 1));
        v.stop = std::max(v.start + 1, std::min(slice->e, frames));
        if (slice->kind != 0) v.pitch_note = z.root;   // positions, breaks, hits: as recorded
        last_region_kind.store(slice->kind);
        last_region_index.store(slice->index);
    } else {
        last_region_kind.store(0);
        last_region_index.store(-1);
    }
    if (!release_trigger) {
        last_region_zone.store(zi);
        last_region_s.store(v.start);
        last_region_e.store(v.stop);
        region_events.fetch_add(1);
    }
    v.granular = settings.engine.load() == 1 && !release_trigger;
    v.one_shot = z.one_shot && !release_trigger && !v.granular;   // a grain cloud has no end of its own: notes release it
    const bool sliced = slice && slice->kind != 0;
    if (sliced && !v.granular && slice->pos > 0) v.start = std::min(v.stop - 1, v.start + int64_t(double(slice->pos) * double(v.stop - v.start)));
    if (!z.loops.empty() && !z.reverse && !v.one_shot && !v.granular && !sliced) {
        const Loop &l = z.loops[0];
        int64_t ls = std::max<int64_t>(v.start, l.start), le = std::min<int64_t>(l.end + 1, v.stop);
        if (le - ls >= 2) {
            v.looping = true;
            v.loop_start = ls; v.loop_end = le;
            v.loop_type = l.type;
            v.loop_until_release = l.until_release;
            v.loop_tune = l.tune;
        }
    }
    rand_ = rand_ * 1664525u + 1013904223u;
    v.rnd = float(double(rand_ >> 8) / double(1u << 23) - 1.0);
    {   // matrix: sample start offset (a share of the playable length, positive amounts only)
        float acc = 0;
        for (int k = 0; k < MOD_SLOTS; k++)
            if (slot_dst_[k] == MD_START && slot_amt_[k] != 0) acc += slot_amt_[k] * src_val(v, slot_src_[k]);
        if (acc > 0 && !z.reverse && !v.granular) v.start = std::min(v.stop - 1, v.start + int64_t(std::min(1.0f, acc) * 0.9f * float(v.stop - v.start)));
    }
    if (v.granular) {
        v.gfix = slice && slice->pos > 0 ? slice->pos : 0;
        int fit = settings.scan_fit.load();
        v.fit_bars = fit > 0 && fit < SCAN_FIT_COUNT ? SCAN_FIT_BARS[fit] : 0;
        v.scan = 0;
        v.spawn_in = 0;   // the first grain starts with the note
        v.grain_no = 0;
        v.warp = v.warp_from = v.warp_to = 0;
        v.warp_t = 1;
        v.ngrains = 0;
    }
    v.reverse_ = z.reverse;
    v.dir = z.reverse ? -1 : 1;
    v.pos = z.reverse ? double(v.stop - 1) : double(v.start);

    // level
    float vel_sens = settings.vel_sens.load();
    double vel01 = vel / 127.0;
    double curve = std::pow(vel01, 2.0 * std::pow(3.0, z.amp_vel_curve));
    double depth = clampd_(z.amp_vel_depth * vel_sens, 0, 1);
    double velgain = (1 - depth) + depth * curve;
    double db = z.gain_db + prog_->inst.gain_db + z.amp_key_tracking * (note - z.root);
    double g = velgain * std::pow(10.0, db / 20.0);
    // crossfades (equal power)
    if (z.key_xfade_lo > 0 && note < z.key_lo + z.key_xfade_lo) g *= std::sqrt(double(note - z.key_lo + 1) / (z.key_xfade_lo + 1));
    if (z.key_xfade_hi > 0 && note > z.key_hi - z.key_xfade_hi) g *= std::sqrt(double(z.key_hi - note + 1) / (z.key_xfade_hi + 1));
    if (z.vel_xfade_lo > 0 && vel < z.vel_lo + z.vel_xfade_lo) g *= std::sqrt(double(vel - z.vel_lo + 1) / (z.vel_xfade_lo + 1));
    if (z.vel_xfade_hi > 0 && vel > z.vel_hi - z.vel_xfade_hi) g *= std::sqrt(double(z.vel_hi - vel + 1) / (z.vel_xfade_hi + 1));
    v.gain = float(g) * settings.slot_gain[z.slot < 0 || z.slot > 3 ? 0 : z.slot].load();
    float pan = float(clampd_(z.pan, -1, 1));
    v.pan_l = std::cos((pan + 1) * PI_F / 4);
    v.pan_r = std::sin((pan + 1) * PI_F / 4);

    // envelopes
    Envelope ae = z.amp_env;
    if (!ae.set) { ae = Envelope(); ae.release = 0.02; }
    if (ae.release < 0.004) ae.release = 0.004;
    float tvel = float(1.0 - ae.time_vel_tracking * (vel01 - 0.5));   // + tracking shortens towards high velocities
    float tkey = float(std::pow(2.0, -ae.time_key_tracking * (note - 60) / 24.0));
    v.amp.begin(ae, tvel * tkey, tkey, settings.attack_add.load(), settings.release_add.load(), settings.sustain_scale.load());
    v.amp.decay_s *= settings.decay_scale.load();
    if (v.amp.stage == Env::Attack) v.amp.enter(Env::Attack);   // re-read the scaled times
    if (release_trigger || v.one_shot) { v.amp.sustain = 1; }
    if (v.granular && v.amp.sustain < 0.001f && z.amp_env.set && z.amp_env.decay <= 0) v.amp.sustain = 1;
    if (z.filter.env.set) v.fenv.begin(z.filter.env, 1, 1, 0, 0, 1);
    if (z.pitch_env.set) v.penv.begin(z.pitch_env, 1, 1, 0, 0, 1);
    if (z.amp_lfo.set) v.alfo.begin(z.amp_lfo, v.age);
    if (z.pitch_lfo.set) v.plfo.begin(z.pitch_lfo, v.age + 7);
    else { Lfo vib; vib.wave = LfoWave::Sine; vib.rate_hz = 5.5; v.plfo.begin(vib, v.age + 7); }
    if (z.filter.lfo.set) v.flfo.begin(z.filter.lfo, v.age + 13);
    int mt = int(settings.filter_type.load());
    for (int c = 0; c < 2; c++) {
        v.zf[c].setup(z.filter.type, z.filter.poles);
        v.mf[c].setup(mt == 1 ? FilterType::HighPass : mt == 2 ? FilterType::BandPass : FilterType::LowPass, 2);
    }
    if (glide_semis != 0) {
        v.glide = glide_semis;
        v.glide_inc = -glide_semis / (settings.glide_s.load() * SR);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// rendering

void Sampler::block_mod(Voice &v, int n, double &step, float &cutoff_mul) {
    const Zone &z = *v.zone;
    float pe = z.pitch_env.set ? v.penv.level : 0;
    float pl = v.plfo.advance(n);
    float al = z.amp_lfo.set ? v.alfo.advance(n) : 0;
    float fl = z.filter.lfo.set ? v.flfo.advance(n) : 0;
    (void)al;
    float bend_cents = bend_ >= 0 ? bend_ * float(z.bend_up) : -bend_ * float(z.bend_down);
    float br = settings.bend_range.load();
    if (br > 0) bend_cents = bend_ * br * 100.0f;
    double semis = (v.pitch_note - z.root) * z.key_tracking + z.tune + settings.transpose.load() + settings.tune_cents.load() / 100.0 +
                   settings.slot_tune[z.slot < 0 || z.slot > 3 ? 0 : z.slot].load() +
                   bend_cents / 100.0 + pe * z.pitch_env_depth * (MAX_ENVELOPE_DEPTH / 100.0) +
                   pl * (z.pitch_lfo.set ? z.pitch_lfo_depth * (MAX_ENVELOPE_DEPTH / 100.0) : 0) + pl * modwheel_ * 0.5 + v.glide;
    if (v.glide != 0) {
        v.glide += v.glide_inc * float(n);
        if ((v.glide_inc < 0 && v.glide < 0) || (v.glide_inc > 0 && v.glide > 0)) v.glide = 0;
    }
    // the matrix's per-voice destinations
    float m_pitch = 0, m_cut = 0, m_res = 0, m_vol = 0, m_pan = 0, m_gpos = 0, m_gsize = 0, m_gdens = 0, m_gspray = 0, m_gpitch = 0;
    for (int k = 0; k < MOD_SLOTS; k++) {
        if (slot_amt_[k] == 0 || slot_src_[k] == MS_OFF) continue;
        float x = slot_amt_[k] * src_val(v, slot_src_[k]);
        switch (slot_dst_[k]) {
        case MD_PITCH: m_pitch += x; break;
        case MD_CUTOFF: m_cut += x; break;
        case MD_RES: m_res += x; break;
        case MD_VOL: m_vol += x; break;
        case MD_PAN: m_pan += x; break;
        case MD_GPOS: m_gpos += x; break;
        case MD_GSIZE: m_gsize += x; break;
        case MD_GDENS: m_gdens += x; break;
        case MD_GSPRAY: m_gspray += x; break;
        case MD_GPITCH: m_gpitch += x; break;
        default: break;
        }
    }
    if (v.granular) {
        v.m_gpos = m_gpos * MOD_GPOS;
        v.m_gsize = m_gsize != 0 ? std::exp2(m_gsize * MOD_GSIZE_OCT) : 1.0f;
        v.m_gdens = m_gdens != 0 ? std::exp2(m_gdens * MOD_GDENS_OCT) : 1.0f;
        v.m_gspray = m_gspray;
        v.m_gpitch = m_gpitch * MOD_GPITCH_SEMIS;
    }
    semis += m_pitch * MOD_PITCH_SEMIS;
    if (v.loop_tune != 0 && v.looping && !v.granular && v.pos >= double(v.loop_start) && !(v.released && v.loop_until_release)) semis += v.loop_tune;
    v.mod_cut_mul = m_cut != 0 ? std::exp2(m_cut * MOD_CUTOFF_OCT) : 1.0f;
    v.mod_res = m_res * MOD_RES;
    v.mod_gain = std::max(0.0f, std::min(2.0f, 1.0f + m_vol));
    v.mod_pan = std::max(-1.0f, std::min(1.0f, m_pan));
    step = std::pow(2.0, semis / 12.0) * double(v.pcm->rate) / double(SR);
    float cents = 0;
    if (z.filter.type != FilterType::None) {
        cents += float(z.filter.env_depth * MAX_ENVELOPE_DEPTH) * (z.filter.env.set ? v.fenv.level : 0);
        cents += float(z.filter.vel_depth * 9600.0) * (v.vel / 127.0f);
        cents += float(z.filter.key_tracking * 100.0) * float(v.note - z.root);
        cents += float(z.filter.lfo_depth * MAX_ENVELOPE_DEPTH) * fl;
        cents += float(z.filter.modwheel_depth * MAX_ENVELOPE_DEPTH) * modwheel_;
    }
    cutoff_mul = std::pow(2.0f, cents / 1200.0f) * v.mod_cut_mul;
}

float Sampler::src_val(const Voice &v, int s) const {
    switch (s) {
    case MS_VEL: return v.vel / 127.0f;
    case MS_KEY: return std::max(-1.0f, std::min(1.0f, (v.note - 60) / 64.0f));
    case MS_RAND: return v.rnd;
    case MS_ENV: return v.amp.level;
    default: return s > MS_OFF && s < MS_COUNT ? gsrc_[s] : 0.0f;
    }
}

void Sampler::transport(double ppq, double tempo, int playing, int ppq_valid) {
    if (tempo > 1) tempo_ = tempo;
    playing_ = playing != 0;
    ppq_valid_ = ppq_valid != 0;
    if (ppq_valid_) ppq_ = ppq;
}

// beats (quarter notes) per LFO cycle for each sync setting; 0 = free
static const double SYNC_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
constexpr int SYNC_COUNT = int(sizeof SYNC_BEATS / sizeof SYNC_BEATS[0]);

void Sampler::update_mod(int frames) {
    {   // the host's tempo and play state (wrapper HAS_LFO_BPM / HAS_TRANSPORT): play starts the song position at 0
        float hb = host_bpm_.load();
        if (hb > 1) tempo_ = hb;
        int rev = host_play_rev_.load();
        if (rev != seen_play_rev_) {
            seen_play_rev_ = rev;
            playing_ = host_play_.load() != 0;
            ppq_valid_ = playing_;
            ppq_ = 0;
            fx_last_q_ = -1;
        }
    }
    block_ppq0_ = ppq_;
    cut_mod_ = false;
    g_drive_add_ = g_rev_add_ = g_fxmix_add_ = 0;
    for (int k = 0; k < MOD_SLOTS; k++) {
        slot_src_[k] = settings.mod_src[k].load();
        slot_dst_[k] = settings.mod_dst[k].load();
        slot_amt_[k] = settings.mod_amt[k].load();
        if (slot_src_[k] <= MS_OFF || slot_src_[k] >= MS_COUNT || slot_dst_[k] <= MD_OFF || slot_dst_[k] >= MD_COUNT) slot_amt_[k] = 0;
        if (slot_amt_[k] != 0 && (slot_dst_[k] == MD_CUTOFF || slot_dst_[k] == MD_RES)) cut_mod_ = true;
    }
    gsrc_[MS_WHEEL] = modwheel_;
    gsrc_[MS_AT] = at_;
    gsrc_[MS_BEND] = bend_;
    auto global = [](int s) { return s == MS_LFO1 || s == MS_LFO2 || s == MS_WHEEL || s == MS_AT || s == MS_BEND; };
    for (int i = 0; i < 2; i++) {
        float rmod = 0;
        for (int k = 0; k < MOD_SLOTS; k++)
            if (slot_dst_[k] == MD_LFO1_RATE + i && slot_amt_[k] != 0 && global(slot_src_[k])) rmod += slot_amt_[k] * gsrc_[slot_src_[k]];
        int sync = settings.lfo_sync[i].load();
        double beats = sync > 0 && sync < SYNC_COUNT ? SYNC_BEATS[sync] : 0;
        double hz = beats > 0 ? tempo_ / 60.0 / beats : std::max(0.001f, settings.lfo_rate[i].load());
        hz *= std::exp2(double(rmod) * MOD_RATE_OCT);
        double prev = lfo_phase_[i];
        if (beats > 0 && playing_ && ppq_valid_ && rmod == 0 && settings.lfo_retrig[i].load() == 0) {
            double q = ppq_ / beats;                        // locked to the song position (no fmod: glibc 2.38)
            lfo_phase_[i] = q - std::floor(q);
        } else {
            lfo_phase_[i] += hz * frames / SR;
            lfo_phase_[i] -= std::floor(lfo_phase_[i]);
        }
        if (lfo_phase_[i] < prev || lfo_sh_[i] == 0) {   // a new cycle: the next sample & hold value
            lfo_sh_prev_[i] = lfo_sh_[i];
            rand_ = rand_ * 1664525u + 1013904223u;
            lfo_sh_[i] = float(double(rand_ >> 8) / double(1u << 23) - 1.0);
            if (lfo_sh_[i] == 0) lfo_sh_[i] = 1e-6f;
        }
        float p = float(lfo_phase_[i]), val;
        switch (settings.lfo_wave[i].load()) {
        case 1: val = p < 0.5f ? 4 * p - 1 : 3 - 4 * p; break;
        case 2: val = 2 * p - 1; break;
        case 3: val = 1 - 2 * p; break;
        case 4: val = p < 0.5f ? 1.0f : -1.0f; break;
        case 5: val = lfo_sh_[i]; break;
        case 6: {   // drift: glides from the last random value to the next over one cycle
            float e = 0.5f - 0.5f * std::cos(PI_F * p);
            val = lfo_sh_prev_[i] + (lfo_sh_[i] - lfo_sh_prev_[i]) * e;
            break;
        }
        default: val = std::sin(2 * PI_F * p); break;
        }
        gsrc_[MS_LFO1 + i] = val;
        lfo_out[i].store(val);
    }
    for (int k = 0; k < MOD_SLOTS; k++) {
        if (slot_amt_[k] == 0 || !global(slot_src_[k])) continue;
        float x = slot_amt_[k] * gsrc_[slot_src_[k]];
        if (slot_dst_[k] == MD_DRIVE) g_drive_add_ += x;
        else if (slot_dst_[k] == MD_REVERB) g_rev_add_ += x;
        else if (slot_dst_[k] == MD_FXMIX) g_fxmix_add_ += x;
    }
    if (playing_ && ppq_valid_) ppq_ += double(frames) / SR * tempo_ / 60.0;
}

static inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

void Sampler::render_voice(Voice &v, float *outl, float *outr, int n) {
    if (v.granular) { render_grains(v, outl, outr, n); return; }
    const Zone &z = *v.zone;
    double step;
    float cutoff_mul;
    block_mod(v, n, step, cutoff_mul);
    const int ch = v.pcm->channels;
    const int nch = ch > 1 ? 2 : 1;
    if (v.zf[0].on) for (int c = 0; c < nch; c++) v.zf[c].set(float(z.filter.cutoff) * cutoff_mul, std::max(0.0f, std::min(1.0f, float(z.filter.resonance) + v.mod_res)));
    float mc = settings.cutoff_hz.load(), mr = settings.resonance.load();
    float fv = settings.filter_vel.load();
    if (fv > 0) mc *= std::pow(2.0f, -4.0f * fv * (1.0f - v.vel / 127.0f));
    mc *= v.mod_cut_mul;
    mr = std::max(0.0f, std::min(1.0f, mr + v.mod_res));
    bool master_on = !(int(settings.filter_type.load()) == 0 && mc >= 19500 && mr < 0.01f) || cut_mod_;
    if (master_on) for (int c = 0; c < nch; c++) v.mf[c].set(mc, mr);
    float amp_lfo_gain = 1;
    if (z.amp_lfo.set && z.amp_lfo_depth != 0)
        amp_lfo_gain = std::pow(10.0f, float(z.amp_lfo_depth * MAX_VOLUME_DEPTH) * v.alfo.value / 20.0f);
    const int interp = settings.interpolation.load();
    const int16_t *d = v.pcm->data.data();
    const float bal_l = v.mod_pan > 0 ? 1 - v.mod_pan : 1, bal_r = v.mod_pan < 0 ? 1 + v.mod_pan : 1;
    const float gl = v.gain * v.mod_gain * bal_l * v.pan_l * amp_lfo_gain * (1.0f / 32768.0f),
                gr = v.gain * v.mod_gain * bal_r * v.pan_r * amp_lfo_gain * (1.0f / 32768.0f);
    const bool zf_on = v.zf[0].on, fenv_on = z.filter.env.set, penv_on = z.pitch_env.set;
    for (int i = 0; i < n; i++) {
        // Fast path: a forward run that can reach no loop point, end or start this block. It works in 32-bit
        // indices: on ARMv7 every double <-> int64 conversion of the generic path below is a library call.
        if (v.dir > 0) {
            bool loop_active = v.looping && !(v.released && v.loop_until_release);
            int64_t lim = loop_active ? std::min(v.loop_end - 1, v.stop) : v.stop;   // first index the run may not read
            double hi = double(lim - 3), lo = double(v.start + 1);
            double pos = v.pos;
            if (pos >= lo && pos <= hi) {
                int k = int((hi - pos) / step) + 1;
                if (k > n - i) k = n - i;
                for (int e = i + k; i < e; i++) {
                    if (fenv_on) v.fenv.tick();
                    if (penv_on) v.penv.tick();
                    int i0 = int(pos);
                    float t = float(pos - double(i0));
                    float sl, sr;
                    if (interp == 2) {
                        const int16_t *p = d + (i0 - 1) * ch;
                        sl = hermite(p[0], p[ch], p[2 * ch], p[3 * ch], t);
                        sr = ch > 1 ? hermite(p[1], p[ch + 1], p[2 * ch + 1], p[3 * ch + 1], t) : sl;
                    } else if (interp == 1) {
                        const int16_t *p = d + i0 * ch;
                        sl = float(p[0]) + float(p[ch] - p[0]) * t;
                        sr = ch > 1 ? float(p[1]) + float(p[ch + 1] - p[1]) * t : sl;
                    } else {
                        const int16_t *p = d + i0 * ch;
                        sl = p[0];
                        sr = ch > 1 ? float(p[1]) : sl;
                    }
                    pos += step;
                    float a = v.amp.tick();
                    if (v.amp.done()) { v.on = false; v.pos = pos; return; }
                    if (zf_on) { sl = v.zf[0].tick(sl); sr = ch > 1 ? v.zf[1].tick(sr) : sl; }
                    if (master_on) { sl = v.mf[0].tick(sl); sr = ch > 1 ? v.mf[1].tick(sr) : sl; }
                    outl[i] += sl * a * gl;
                    outr[i] += sr * a * gr;
                }
                v.pos = pos;
                if (i >= n) break;
            }
        }
        if (fenv_on) v.fenv.tick();
        if (penv_on) v.penv.tick();
        // loop / end handling (the generic path: one sample near a loop point, the ends, or playing backwards)
        if (v.looping && !(v.released && v.loop_until_release)) {
            if (v.loop_type == LoopType::Forward) {
                if (v.dir > 0 && v.pos >= double(v.loop_end)) v.pos -= double(v.loop_end - v.loop_start);
            } else if (v.loop_type == LoopType::Backward) {
                if (v.dir > 0 && v.pos >= double(v.loop_end)) { v.pos = double(v.loop_end) - (v.pos - double(v.loop_end)) - 1; v.dir = -1; }
                else if (v.dir < 0 && v.pos < double(v.loop_start)) v.pos += double(v.loop_end - v.loop_start);
            } else {
                if (v.dir > 0 && v.pos >= double(v.loop_end - 1)) { v.pos = 2.0 * double(v.loop_end - 1) - v.pos; v.dir = -1; }
                else if (v.dir < 0 && v.pos < double(v.loop_start)) { v.pos = 2.0 * double(v.loop_start) - v.pos; v.dir = 1; }
            }
        } else if (v.looping && v.dir < 0 && !v.reverse_) {
            v.dir = 1;   // released out of a backward loop: play on forwards to the end
        }
        if ((v.dir > 0 && v.pos >= double(v.stop)) || (v.dir < 0 && v.pos < double(v.start))) { v.on = false; break; }

        int64_t i0 = int64_t(v.pos);
        float t = float(v.pos - double(i0));
        float sl, sr;
        auto fetch = [&](int64_t k, int c) -> float {
            if (v.looping && !(v.released && v.loop_until_release) && v.loop_type == LoopType::Forward && v.dir > 0 && k >= v.loop_end)
                k = v.loop_start + (k - v.loop_end) % (v.loop_end - v.loop_start);
            if (k < v.start || k >= v.stop) return 0.0f;
            return float(d[size_t(k) * ch + c]);
        };
        if (interp == 0) {
            sl = float(d[size_t(i0) * ch]);
            sr = ch > 1 ? float(d[size_t(i0) * ch + 1]) : sl;
        } else if (interp == 1) {
            float a = fetch(i0, 0), b = fetch(i0 + 1, 0);
            sl = a + (b - a) * t;
            if (ch > 1) { float a2 = fetch(i0, 1), b2 = fetch(i0 + 1, 1); sr = a2 + (b2 - a2) * t; } else sr = sl;
        } else {
            if (i0 - 1 >= v.start && i0 + 2 < (v.looping ? std::min(v.loop_end, v.stop) : v.stop)) {
                const int16_t *p = d + size_t(i0 - 1) * ch;
                sl = hermite(p[0], p[ch], p[2 * ch], p[3 * ch], t);
                sr = ch > 1 ? hermite(p[1], p[ch + 1], p[2 * ch + 1], p[3 * ch + 1], t) : sl;
            } else {
                sl = hermite(fetch(i0 - 1, 0), fetch(i0, 0), fetch(i0 + 1, 0), fetch(i0 + 2, 0), t);
                sr = ch > 1 ? hermite(fetch(i0 - 1, 1), fetch(i0, 1), fetch(i0 + 1, 1), fetch(i0 + 2, 1), t) : sl;
            }
        }
        v.pos += step * v.dir;

        float a = v.amp.tick();
        if (v.amp.done()) { v.on = false; break; }
        if (v.zf[0].on) { sl = v.zf[0].tick(sl); sr = ch > 1 ? v.zf[1].tick(sr) : sl; }
        if (master_on) { sl = v.mf[0].tick(sl); sr = ch > 1 ? v.mf[1].tick(sr) : sl; }
        outl[i] += sl * a * gl;
        outr[i] += sr * a * gr;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// granular voices: the note's zone sample is the material; grains start every 1/density (or a tempo division) at
// POSITION (+ the SCAN offset, + SPRAY), each with its own pitch, direction, pan and window. The grains' sum then
// runs through the voice's own envelope and filters like a sampled note.

void Sampler::render_grains(Voice &v, float *outl, float *outr, int n) {
    const Zone &z = *v.zone;
    double step;
    float cutoff_mul;
    block_mod(v, n, step, cutoff_mul);
    const Settings &S = settings;
    const int ch = v.pcm->channels;
    if (v.zf[0].on) for (int c = 0; c < 2; c++) v.zf[c].set(float(z.filter.cutoff) * cutoff_mul, std::max(0.0f, std::min(1.0f, float(z.filter.resonance) + v.mod_res)));
    float mc = S.cutoff_hz.load(), mr = S.resonance.load();
    float fv = S.filter_vel.load();
    if (fv > 0) mc *= std::pow(2.0f, -4.0f * fv * (1.0f - v.vel / 127.0f));
    mc *= v.mod_cut_mul;
    mr = std::max(0.0f, std::min(1.0f, mr + v.mod_res));
    bool master_on = !(int(S.filter_type.load()) == 0 && mc >= 19500 && mr < 0.01f) || cut_mod_;
    if (master_on) for (int c = 0; c < 2; c++) v.mf[c].set(mc, mr);
    float amp_lfo_gain = 1;
    if (z.amp_lfo.set && z.amp_lfo_depth != 0)
        amp_lfo_gain = std::pow(10.0f, float(z.amp_lfo_depth * MAX_VOLUME_DEPTH) * v.alfo.value / 20.0f);

    // the material: the zone's sample from its start to its end (loops do not apply)
    const int64_t L64 = v.stop - v.start;
    if (L64 < 32) { v.on = false; return; }
    const int L = int(std::min<int64_t>(L64, 0x7fffff00));
    const double Ld = double(L);
    const int16_t *d = v.pcm->data.data() + size_t(v.start) * size_t(ch);
    const double src_rate = double(v.pcm->rate) / double(SR);

    // SCAN: the playhead moves through the sample at scan x original speed (0: stays at POSITION)
    // (SCAN FIT: one pass through the region takes that many bars at MPC's tempo, in SCAN's direction)
    if (v.fit_bars > 0) v.scan = frac(v.scan + (S.g_scan.load() < 0 ? -1.0 : 1.0) * double(n) / beats_to_samples(v.fit_bars * 4));
    else v.scan = frac(v.scan + double(S.g_scan.load()) * double(n) * src_rate / Ld);
    // WARP: a smooth random walk to a new target every ~0.3 s
    v.warp_t += float(n) / (0.3f * SR);
    if (v.warp_t >= 1) { v.warp_t -= std::floor(v.warp_t); v.warp_from = v.warp_to; v.warp_to = grand(); }
    {
        float t = v.warp_t, e = t * t * (3 - 2 * t);
        v.warp = v.warp_from + (v.warp_to - v.warp_from) * e;
    }

    // grain clock and size
    int rs = S.g_rate_sync.load(), ss = S.g_size_sync.load();
    double interval = rs > 0 && rs < GRAIN_DIV_COUNT ? beats_to_samples(GRAIN_DIV_BEATS[rs]) / double(v.m_gdens)
                                                    : double(SR) / std::max(0.2, double(S.g_density_hz.load()) * double(v.m_gdens));
    interval = std::max(16.0, interval);
    double size_s = ss > 0 && ss < GRAIN_DIV_COUNT ? GRAIN_DIV_BEATS[ss] * 60.0 / tempo_ : double(S.g_size_s.load());
    size_s *= double(v.m_gsize);
    int len = int(std::max(0.004, std::min(4.0, size_s)) * SR);
    // level: overlapping grains add up, so scale by 1/sqrt(overlap) (a raised-cosine window averages 0.5)
    float overlap = float(len) / float(interval);
    float level = (1.0f / 32768.0f) * std::min(1.0f, 1.2f / std::sqrt(std::max(1.0f, overlap)));
    const float spray = std::max(0.0f, std::min(1.0f, S.g_spray.load() + v.m_gspray));
    const float stereo = S.g_stereo.load(), contour = S.g_contour.load(), pattern = S.g_pattern.load();
    const float detune = S.g_detune.load(), reverse = S.g_reverse.load(), base_semis = S.g_pitch.load() + v.m_gpitch;
    const int spray_mode = S.g_spray_mode.load(), limit = S.g_limit.load();
    const double centre = frac(double(v.gfix) + double(S.g_position.load()) + double(v.m_gpos) + v.scan);

    while (v.spawn_in < double(n)) {
        int offset = std::max(0, int(v.spawn_in));
        v.spawn_in += interval;
        if (v.amp.stage == Env::Release && v.amp.level < 0.0005f) continue;
        if (v.ngrains >= GRAINS_PER_VOICE || grains_total_ >= limit) continue;   // over the budget: skip this grain
        Grain *g = nullptr;
        for (auto &c : v.grains) if (!c.on) { g = &c; break; }
        if (!g) continue;
        float off = spray_mode == 1 ? v.warp : grand();
        double p = frac(centre + double(off * spray * spray) * 0.5);
        float semis = base_semis + pattern_semis(v.grain_no, pattern) + detune * grand();
        double gstep = step * std::exp2(double(semis) / 12.0);
        if (reverse > 0 && grand() * 0.5f + 0.5f < reverse) gstep = -gstep;
        g->on = true;
        g->pos = std::min(p * Ld, Ld - 1.0);
        g->step = gstep;
        g->t = -offset;
        grain_shape(*g, len, contour);
        float pan = stereo * grand();
        g->gl = level * std::cos((pan + 1) * PI_F / 4) * 1.41421356f;
        g->gr = level * std::sin((pan + 1) * PI_F / 4) * 1.41421356f;
        v.ngrains++;
        grains_total_++;
        v.grain_no++;
    }
    v.spawn_in -= double(n);

    // the grains' sum (stereo)
    float bl[64], br[64];
    std::fill(bl, bl + n, 0.f);
    std::fill(br, br + n, 0.f);
    for (auto &g : v.grains) {
        if (!g.on) continue;
        int i = 0;
        if (g.t < 0) { i = -g.t; g.t = 0; if (i >= n) { g.t = -(i - n); continue; } }
        double pos = g.pos;
        const double st = g.step;
        for (; i < n; i++) {
            if (g.t >= g.len) { g.on = false; v.ngrains--; break; }
            int i0 = int(pos);
            float t = float(pos - double(i0));
            int i1 = i0 + 1 >= L ? 0 : i0 + 1;
            float w = window_at(g);
            float sl, sr;
            if (ch > 1) {
                const int16_t *a = d + size_t(i0) * size_t(ch), *b = d + size_t(i1) * size_t(ch);
                sl = float(a[0]) + float(b[0] - a[0]) * t;
                sr = float(a[1]) + float(b[1] - a[1]) * t;
            } else {
                float a = d[i0], b = d[i1];
                sl = sr = a + (b - a) * t;
            }
            bl[i] += sl * w * g.gl;
            br[i] += sr * w * g.gr;
            pos += st;
            if (pos >= Ld) pos -= Ld;
            else if (pos < 0) pos += Ld;
            if (pos >= Ld) pos = 0;   // -tiny + Ld can round up to Ld
            g.t++;
        }
        g.pos = pos;
    }

    // the voice: envelope, filters, level and pan
    const bool zf_on = v.zf[0].on, fenv_on = z.filter.env.set, penv_on = z.pitch_env.set;
    const float bal_l = v.mod_pan > 0 ? 1 - v.mod_pan : 1, bal_r = v.mod_pan < 0 ? 1 + v.mod_pan : 1;
    const float gl = v.gain * v.mod_gain * bal_l * v.pan_l * amp_lfo_gain,   // the same level as a sampled note
                gr = v.gain * v.mod_gain * bal_r * v.pan_r * amp_lfo_gain;
    for (int i = 0; i < n; i++) {
        if (fenv_on) v.fenv.tick();
        if (penv_on) v.penv.tick();
        float a = v.amp.tick();
        if (v.amp.done()) {
            v.on = false;
            for (auto &g : v.grains) g.on = false;
            v.ngrains = 0;
            return;
        }
        float sl = bl[i], sr = br[i];
        if (zf_on) { sl = v.zf[0].tick(sl); sr = v.zf[1].tick(sr); }
        if (master_on) { sl = v.mf[0].tick(sl); sr = v.mf[1].tick(sr); }
        outl[i] += sl * a * gl;
        outr[i] += sr * a * gr;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// GRAIN FX: the voices' sum is written into a ring; grains read it back from behind the write head (a granular delay,
// tempo-synced or free) or from a read head running at its own speed (stretch). LOCK stops writing (and the write head),
// so the grains keep playing what the ring holds. Feedback writes the grains back in.

void Sampler::render_fx(float *l, float *r, int frames) {
    const Settings &S = settings;
    float target = std::max(0.0f, std::min(1.0f, S.fx_mix.load() + g_fxmix_add_));
    bool any = false;
    for (auto &g : fxg_) if (g.on) { any = true; break; }
    const bool lock = S.fx_lock.load() != 0;
    const int MASK = FX_BUFFER - 1;
    if (target < 0.0005f && fx_level_ < 0.0005f && !any) {
        fx_level_ = 0;
        if (!lock)   // keep the ring filled, so the effect has material the moment it is turned up
            for (int i = 0; i < frames; i++) { fxbuf_[size_t(fx_w_) * 2] = l[i]; fxbuf_[size_t(fx_w_) * 2 + 1] = r[i]; fx_w_ = (fx_w_ + 1) & MASK; }
        return;
    }
    const int mode = S.fx_mode.load(), rate = S.fx_rate.load();
    const double size_s = std::max(0.01, std::min(2.0, double(S.fx_size_s.load())));
    const int len = int(size_s * SR);
    const float pitch = S.fx_pitch.load(), spray = S.fx_spray.load(), pattern = S.fx_pattern.load(), contour = S.fx_contour.load();
    const float fb = std::max(0.0f, std::min(0.95f, S.fx_feedback.load()));
    // the grain clock
    double beats = rate > 0 && rate < GRAIN_DIV_COUNT ? GRAIN_DIV_BEATS[rate] : 0;
    double interval = beats > 0 ? beats_to_samples(beats) : double(len) * 0.5;
    interval = std::max(32.0, interval);
    // where grains read: a delay behind the write head
    double delay;
    if (mode == 0) delay = beats > 0 ? beats_to_samples(beats) : double(S.fx_time_s.load()) * SR;
    else if (mode == 1) delay = double(S.fx_time_s.load()) * SR;
    else {
        // stretch: the read head runs at fx_speed while the write head runs at 1 (stopped when locked)
        const double lo = double(len) * 2 + 512, hi = double(FX_BUFFER) * 0.75;
        fx_read_ += (lock ? 0.0 : double(frames)) - double(S.fx_speed.load()) * double(frames);
        if (fx_read_ > hi) fx_read_ = lo + (fx_read_ - hi);
        if (fx_read_ < lo) fx_read_ = hi - (lo - fx_read_);
        if (fx_read_ > hi || fx_read_ < lo) fx_read_ = lo;
        delay = fx_read_;
    }
    const double step = std::exp2(double(pitch) / 12.0);
    const double min_delay = 256.0 + double(frames) + std::max(0.0, double(len) * (step * (pattern > 0 ? 4.0 : 1.0) - 1.0));
    float overlap = float(len) / float(interval);
    float level = std::min(1.0f, 1.2f / std::sqrt(std::max(1.0f, overlap)));

    // grains starting in this block: on the song's grid while MPC plays, else a free-running clock
    int starts[16], nstart = 0;
    if (beats > 0 && playing_ && ppq_valid_) {
        double q0 = block_ppq0_ / beats, q1 = (block_ppq0_ + double(frames) / SR * tempo_ / 60.0) / beats;
        double k = std::ceil(q0);
        if (fx_last_q_ >= k) k = fx_last_q_ + 1;
        for (; k < q1 && nstart < 16; k += 1) {
            starts[nstart++] = std::max(0, std::min(frames - 1, int((k - q0) / (q1 - q0) * frames)));
            fx_last_q_ = k;
        }
    } else {
        fx_last_q_ = -1;
        while (fx_spawn_in_ < double(frames) && nstart < 16) { starts[nstart++] = std::max(0, int(fx_spawn_in_)); fx_spawn_in_ += interval; }
        fx_spawn_in_ -= double(frames);
        if (fx_spawn_in_ < 0) fx_spawn_in_ = 0;
    }
    // WARP-like drift of the read position for a moving texture
    fx_warp_ += (grand() - fx_warp_) * 0.02f;
    for (int k = 0; k < nstart; k++) {
        if (target < 0.0005f) break;
        Grain *g = nullptr;
        for (auto &c : fxg_) if (!c.on) { g = &c; break; }
        if (!g) break;
        double D = delay + double(grand() * spray * spray) * SR * 0.5 + double(fx_warp_ * spray) * SR * 0.05;
        // pitched up, a grain must not overtake the write head (min_delay); the ring's length wins over that
        D = std::max(512.0, std::min(double(FX_BUFFER) - double(len) - 1024.0, std::max(min_delay, D)));
        float semis = pattern_semis(fx_grain_no_++ + 5, pattern);
        g->on = true;
        g->step = step * std::exp2(double(semis) / 12.0);
        double wpos = double(fx_w_) + (lock ? 0.0 : double(starts[k]));
        g->pos = wpos - D + double(FX_BUFFER);
        while (g->pos >= double(FX_BUFFER)) g->pos -= double(FX_BUFFER);
        while (g->pos < 0) g->pos += double(FX_BUFFER);
        g->t = -starts[k];
        grain_shape(*g, len, contour);
        float pan = spray * grand();
        g->gl = level * std::cos((pan + 1) * PI_F / 4) * 1.41421356f;
        g->gr = level * std::sin((pan + 1) * PI_F / 4) * 1.41421356f;
    }
    // wet signal: grains read the ring (written up to the block start)
    float wl[128], wr[128];
    int done = 0;
    while (done < frames) {
        int n = std::min(128, frames - done);
        std::fill(wl, wl + n, 0.f);
        std::fill(wr, wr + n, 0.f);
        for (auto &g : fxg_) {
            if (!g.on) continue;
            int i = 0;
            if (g.t < 0) { i = -g.t; g.t = 0; if (i >= n) { g.t = -(i - n); continue; } }
            double pos = g.pos;
            for (; i < n; i++) {
                if (g.t >= g.len) { g.on = false; break; }
                int i0 = int(pos);
                float t = float(pos - double(i0));
                int i1 = (i0 + 1) & MASK;
                float w = window_at(g);
                const float *a = &fxbuf_[size_t(i0) * 2], *b = &fxbuf_[size_t(i1) * 2];
                wl[i] += (a[0] + (b[0] - a[0]) * t) * w * g.gl;
                wr[i] += (a[1] + (b[1] - a[1]) * t) * w * g.gr;
                pos += g.step;
                if (pos >= double(FX_BUFFER)) pos -= double(FX_BUFFER);
                if (pos >= double(FX_BUFFER)) pos = 0;
                g.t++;
            }
            g.pos = pos;
        }
        float lv0 = fx_level_, lv1 = fx_level_ + (target - fx_level_) * std::min(1.0f, float(n) / 2048.0f);
        if (std::fabs(lv1 - target) < 0.0005f) lv1 = target;
        for (int i = 0; i < n; i++) {
            float m = lv0 + (lv1 - lv0) * float(i) / float(n);
            float dl = l[done + i], dr = r[done + i];
            if (!lock) {
                fxbuf_[size_t(fx_w_) * 2] = soft(dl + wl[i] * fb);
                fxbuf_[size_t(fx_w_) * 2 + 1] = soft(dr + wr[i] * fb);
                fx_w_ = (fx_w_ + 1) & MASK;
            }
            l[done + i] = dl * (1 - m) + wl[i] * m;
            r[done + i] = dr * (1 - m) + wr[i] * m;
        }
        fx_level_ = lv1;
        done += n;
    }
}

void Sampler::swap_program() {
    Program *next = pending_.load();
    if (!next) return;
    if (!swap_fade_) {
        bool any = false;
        for (auto &v : voices_) if (v.on) { any = true; v.fading = true; v.amp.fast_release(0.006f); }
        swap_fade_ = any ? 1 : 2;
        if (any) return;
    }
    for (auto &v : voices_) if (v.on) return;   // wait for the fades
    if (retired_.load()) return;                // the loader has not collected the last one yet
    retired_.store(prog_);
    prog_ = pending_.exchange(nullptr);
    if (prog_) {
        prog_->index();
        double a = 0;
        for (auto &z : prog_->inst.zones)
            if (z.amp_env.set) a = std::max(a, z.amp_env.delay + z.amp_env.attack + z.amp_env.hold);
        slowest_attack.store(float(a));
    }
    swap_fade_ = 0;
}

// Flush denormals to zero while rendering (filter and reverb tails decay into them, and they are slow on ARMv7's
// VFP). The host's FPU mode is restored afterwards: this is MPC's audio thread.
namespace {
struct FlushDenormals {
#if defined(__arm__) && defined(__ARM_PCS_VFP)
    uint32_t saved;
    FlushDenormals() {
        __asm__ volatile("vmrs %0, fpscr" : "=r"(saved));
        uint32_t v = saved | (1u << 24);   // FZ
        __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
    }
    ~FlushDenormals() { __asm__ volatile("vmsr fpscr, %0" : : "r"(saved)); }
#elif defined(__SSE__) || defined(__x86_64__)
    unsigned saved;
    FlushDenormals() { saved = __builtin_ia32_stmxcsr(); __builtin_ia32_ldmxcsr(saved | 0x8040); }   // FTZ | DAZ
    ~FlushDenormals() { __builtin_ia32_ldmxcsr(saved); }
#endif
};
}  // namespace

void Sampler::render(int16_t *out, int frames) {
    FlushDenormals ftz;
    if (pending_.load()) swap_program();
    update_mod(frames);
    float *l = mixl_.data(), *r = mixr_.data();
    std::fill(l, l + frames, 0.f);
    std::fill(r, r + frames, 0.f);
    // a tap on the SLICE page's waveform: play from that spot (in its break) for 0.7 s
    if (int t = touch_req_.exchange(0)) {
        if (prog_ && !swap_fade_ && !prog_->inst.zones.empty()) {
            for (auto &v : voices_) if (v.on && v.touch && !v.released) { v.released = true; v.amp.release(); v.fenv.release(); v.penv.release(); }
            int zi = last_zone.load();
            if (last_zone_prog.load() != prog_->serial || zi < 0 || zi >= int(prog_->inst.zones.size()) || !prog_->pcm[size_t(zi)]) {
                zi = -1;
                for (size_t i = 0; i < prog_->pcm.size(); i++) if (prog_->pcm[i]) { zi = int(i); break; }
            }
            if (zi >= 0) {
                const Zone &z = prog_->inst.zones[size_t(zi)];
                int64_t frames = prog_->pcm[size_t(zi)]->frames();
                int64_t ze = z.stop > 0 ? std::min<int64_t>(z.stop, frames) : frames, zs = std::max<int64_t>(0, std::min<int64_t>(z.start, ze));
                int64_t f = zs + int64_t((double(t - 1) + 0.5) / 48.0 * double(ze - zs));
                Slice sl;
                sl.on = true; sl.s = zs; sl.e = ze; sl.kind = 1; sl.index = t - 1;
                const Analysis *a = zi < int(prog_->ana.size()) ? prog_->ana[size_t(zi)].get() : nullptr;
                if (a) {
                    int k = break_at(*a, settings.break_by.load(), f, zs, ze);
                    if (k >= 0) break_range(*a, settings.break_by.load(), k, zs, ze, sl.s, sl.e);
                }
                sl.pos = float(double(f - sl.s) / double(std::max<int64_t>(1, sl.e - sl.s)));
                start_voice(zi, 16, z.root, 100, false, 0, &sl);   // channel 16: no MIDI note-off reaches it
                for (auto &v : voices_)
                    if (v.on && v.age == age_counter_) {
                        v.touch = true;
                        if (v.granular) v.gfix = sl.pos - settings.g_position.load();   // grains start right where the tap was
                    }
                touch_gate_ = int(0.7f * SR);
            }
        }
    }
    if (touch_gate_ > 0 && (touch_gate_ -= frames) <= 0)
        for (auto &v : voices_) if (v.on && v.touch && !v.released) { v.released = true; v.amp.release(); v.fenv.release(); v.penv.release(); }
    int active = 0;
    grains_total_ = 0;
    for (auto &v : voices_) if (v.on && v.granular) grains_total_ += v.ngrains;
    if (prog_)
        for (auto &v : voices_) {
            if (!v.on) continue;
            for (int o = 0; o < frames && v.on; o += 64) render_voice(v, l + o, r + o, std::min(64, frames - o));
            if (v.on) active++;
        }
    active_voices.store(active);
    active_grains.store(grains_total_);
    render_fx(l, r, frames);
    float vol = std::pow(10.0f, settings.volume_db.load() / 20.0f) * cc7_ * cc7_ * cc11_;
    float pan = std::max(-1.0f, std::min(1.0f, settings.pan.load() + cc10_));
    float pl = pan > 0 ? 1 - pan : 1, pr = pan < 0 ? 1 + pan : 1;
    float mix = std::max(0.0f, std::min(1.0f, settings.reverb_mix.load() + g_rev_add_)),
          drive = std::max(0.0f, std::min(1.0f, settings.drive.load() + g_drive_add_));
    if (mix > 0.001f) reverb_.set(settings.reverb_size.load(), settings.reverb_damp.load());
    float pk = 0;
    for (int i = 0; i < frames; i++) {
        float a = l[i], b = r[i];
        if (mix > 0.001f) {
            float wl, wr;
            reverb_.tick(a, b, wl, wr);
            a = a * (1 - mix * 0.5f) + wl * mix;
            b = b * (1 - mix * 0.5f) + wr * mix;
        }
        a *= vol * pl; b *= vol * pr;
        if (drive > 0.001f) {
            float k = 1 + drive * 8;
            a = std::tanh(a * k) / std::tanh(k) ;
            b = std::tanh(b * k) / std::tanh(k);
        }
        // soft limit above -1 dBFS instead of hard clipping
        auto lim = [](float x) { float ax = std::fabs(x); return ax < 0.89f ? x : (x > 0 ? 1 : -1) * (0.89f + 0.11f * std::tanh((ax - 0.89f) / 0.11f)); };
        a = lim(a); b = lim(b);
        pk = std::max(pk, std::max(std::fabs(a), std::fabs(b)));
        out[2 * i] = int16_t(std::lrint(a * 32767.0f));
        out[2 * i + 1] = int16_t(std::lrint(b * 32767.0f));
    }
    peak.store(std::max(pk, peak.load() * 0.9f));
}

}  // namespace omni

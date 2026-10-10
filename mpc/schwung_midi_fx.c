/* Schwung MIDI FX module (midi_fx_api_v1) -> mpc_engine_t, for sequencers, arpeggiators and chord generators that
 * play OTHER MPC/Force tracks. (steve/tools/midifx, extended for the insert-effect build of Groove Bank.)
 *
 * - MPC OS ignores a VST's own MIDI output, so everything the module emits goes out an ALSA sequencer port named after
 *   the plugin ("<name> MIDI Out"; a second instance is "<name> 2"). MPC subscribes to new ports by itself (verified
 *   on a stock Live II 2026-10-01); pick that port as another track's MIDI input.
 * - The host transport (engine.h's mpc_engine_transport hook) becomes 24-PPQN MIDI clock plus Start/Stop fed to the
 *   module's process_midi, and answers the module's get_bpm / get_clock_status / get_beat_position.
 * - Notes played on the plugin's own track go to the module (arpeggiators and chord generators use them).
 *
 * Insert-effect build (vst.json "effect": true -> PLUG_EFFECT):
 * - The track's audio passes through unchanged (process()); the plugin adds no sound and no latency.
 * - The Force/MPC does not hand MIDI notes to an insert effect (tested on a Force with Oneiroi FX, 2026-10), so the
 *   plugin also opens an ALSA input port, "<name> MIDI In". Send a MIDI track (pads, keyboard or a clip) to that port
 *   (its Output Port; works on a Force, 2026-10-09);
 *   its notes are the chord the groove plays. MIDI that does arrive through the VST (another host) is treated alike.
 * - "in_ch" filters both inputs: 0 = omni, 1..16 = that channel only.
 * - CC 20..30 on that channel move the controls (GENRE, GROOVE, VARIANT, SWING, GATE, STRUM, ACCENT, LATCH, ROOT,
 *   CHORD, OCTAVE); the
 *   screen follows through "display_rev" (HAS_DISPLAY_REV). Other CCs, bend and aftertouch pass through to the output.
 * - "genre" jumps to the first groove of a genre (the original's genre knob on Move, K8), read back from the groove.
 * - Chord source ("src", v2). Confirmed on a Force 2026-10-09 (Oneiroi FX, same port code): a MIDI track whose Output
 *   Port is the plugin's "MIDI In" plays the insert plugin.
 *     0 INTERNAL  the chord is set on the plugin (ROOT, CHORD, OCTAVE); no routing at all. Input notes are ignored.
 *     1 MIDI IN   notes from the plugin's own "MIDI In" port, from the VST input and from external USB MIDI devices.
 *   Either way the plugin connects external USB MIDI devices to its MIDI In itself (for INTERNAL: their CCs) (kernel clients other than System, Midi Through and the Force's
 *                 own "Akai" ports; rescanned every 3 s, so a keyboard plugged in later is found too).
 *
 * libasound is loaded at run time from the device (no ALSA headers needed to build); the event struct below is the
 * kernel's stable snd_seq_event_t layout (28 bytes on 32-bit ARM). */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "params.h"
#include "host/plugin_api_v1.h"
#include "host/midi_fx_api_v1.h"

#ifndef MODULE_DIR
#define MODULE_DIR ""
#endif
#define SR 44100
#define MAX_OUT MIDI_FX_MAX_OUT_MSGS
#define MAX_GENRES 32
#define CC_FIRST 20                     /* CC 20.. -> cc_keys[] */
#define MAX_IN_EVENTS 128               /* ALSA events read per block at most */
#define RESCAN_FRAMES (3 * SR)          /* MIDI IN source: look for new USB MIDI devices this often */
#define MAX_LINKS 32

extern midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host);

/* ---- ALSA sequencer, loaded with dlopen ------------------------------------------------------------------------ */
typedef struct { unsigned char client, port; } seq_addr_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    unsigned int time[2];
    seq_addr_t source, dest;
    unsigned char data[12];
} seq_ev_t;
enum { EV_NOTEON = 6, EV_NOTEOFF = 7, EV_KEYPRESS = 8, EV_CONTROLLER = 10, EV_PGMCHANGE = 11, EV_CHANPRESS = 12,
       EV_PITCHBEND = 13 };
static struct {
    int ok;
    int (*open)(void **, const char *, int, int);
    int (*set_name)(void *, const char *);
    int (*port)(void *, const char *, unsigned, unsigned);
    int (*nonblock)(void *, int);
    int (*out_direct)(void *, seq_ev_t *);
    int (*input)(void *, seq_ev_t **);
    int (*close)(void *);
    int (*me_new)(size_t, void **);
    long (*me_encode)(void *, const unsigned char *, long, seq_ev_t *);
    void (*me_reset)(void *);
    void (*me_free)(void *);
    /* finding and connecting external devices (optional: without them only the own port listens) */
    int (*client_id)(void *);
    int (*ci_malloc)(void **); void (*ci_free)(void *); void (*ci_set_client)(void *, int);
    int (*ci_get_client)(const void *); int (*ci_get_type)(const void *); const char *(*ci_get_name)(void *);
    int (*next_client)(void *, void *);
    int (*pi_malloc)(void **); void (*pi_free)(void *); void (*pi_set_client)(void *, int); void (*pi_set_port)(void *, int);
    int (*pi_get_port)(const void *); unsigned (*pi_get_cap)(const void *);
    int (*next_port)(void *, void *);
    int (*connect_from)(void *, int, int, int);
    int scan_ok;
} A;

static void alsa_load(void) {
    if (A.ok) return;
    void *h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h) { A.ok = -1; return; }
    A.open = dlsym(h, "snd_seq_open");
    A.set_name = dlsym(h, "snd_seq_set_client_name");
    A.port = dlsym(h, "snd_seq_create_simple_port");
    A.nonblock = dlsym(h, "snd_seq_nonblock");
    A.out_direct = dlsym(h, "snd_seq_event_output_direct");
    A.input = dlsym(h, "snd_seq_event_input");
    A.close = dlsym(h, "snd_seq_close");
    A.me_new = dlsym(h, "snd_midi_event_new");
    A.me_encode = dlsym(h, "snd_midi_event_encode");
    A.me_reset = dlsym(h, "snd_midi_event_reset_encode");
    A.me_free = dlsym(h, "snd_midi_event_free");
    A.client_id = dlsym(h, "snd_seq_client_id");
    A.ci_malloc = dlsym(h, "snd_seq_client_info_malloc");
    A.ci_free = dlsym(h, "snd_seq_client_info_free");
    A.ci_set_client = dlsym(h, "snd_seq_client_info_set_client");
    A.ci_get_client = dlsym(h, "snd_seq_client_info_get_client");
    A.ci_get_type = dlsym(h, "snd_seq_client_info_get_type");
    A.ci_get_name = dlsym(h, "snd_seq_client_info_get_name");
    A.next_client = dlsym(h, "snd_seq_query_next_client");
    A.pi_malloc = dlsym(h, "snd_seq_port_info_malloc");
    A.pi_free = dlsym(h, "snd_seq_port_info_free");
    A.pi_set_client = dlsym(h, "snd_seq_port_info_set_client");
    A.pi_set_port = dlsym(h, "snd_seq_port_info_set_port");
    A.pi_get_port = dlsym(h, "snd_seq_port_info_get_port");
    A.pi_get_cap = dlsym(h, "snd_seq_port_info_get_capability");
    A.next_port = dlsym(h, "snd_seq_query_next_port");
    A.connect_from = dlsym(h, "snd_seq_connect_from");
    A.scan_ok = A.client_id && A.ci_malloc && A.ci_free && A.ci_set_client && A.ci_get_client && A.ci_get_type &&
                A.ci_get_name && A.next_client && A.pi_malloc && A.pi_free && A.pi_set_client && A.pi_set_port &&
                A.pi_get_port && A.pi_get_cap && A.next_port && A.connect_from;
    A.ok = (A.open && A.set_name && A.port && A.out_direct && A.close && A.me_new && A.me_encode && A.me_reset && A.me_free) ? 1 : -1;
}

/* ---- instance ---------------------------------------------------------------------------------------------------- */
typedef struct {
    void *mod;
    void *seq, *enc;
    int port, in_port;       /* ALSA ports: MIDI Out, MIDI In (-1: none) */
    double bpm, ppq;        /* tempo; position in quarter notes (advanced per block between host updates) */
    int playing, running;    /* host says playing; we have sent Start */
    long clock;              /* 24-PPQN ticks sent since Start */
    int in_ch;               /* 0 = omni, 1..16 */
    unsigned rev;            /* bumped when the plugin changes a value by itself (CC, genre) */
    int src, root, chord, oct;   /* chord source (0 internal, 1 MIDI in) and the internal chord */
    uint8_t inotes[8];       /* internal chord notes the module holds now */
    int ninotes;
    int rescan;              /* frames until the next device scan */
    struct { unsigned char c, p, seen; } link[MAX_LINKS];   /* external ports connected to MIDI In */
    int nlinks;
} inst_t;

static midi_fx_api_v1_t *api;
static host_api_v1_t host;
static double g_bpm = 120.0, g_ppq = -1;
static int g_running;
static int instances;

/* The genres in bank order (the module sorts its grooves by genre), read once from the module's "genre_list". */
static char g_genre[MAX_GENRES][24];
static int g_genre_first[MAX_GENRES], g_ngenres = -1;

/* The genre option list in params.json, in the shipped library's order. Matched by name, so a different library
 * still works: a genre the library lacks is skipped. */
static const char *const OPT_GENRES[] = {"HOUSE", "TECHNO", "GARAGE", "DNB", "HIPHOP", "TRAP", "FUNK", "SOUL", "JAZZ",
                                         "LATIN", "REGGAE", "ROCK", "AFRO", "WORLD"};
#define N_OPT_GENRES ((int)(sizeof OPT_GENRES / sizeof OPT_GENRES[0]))

/* the internal chord: intervals over the root */
static const struct { const char *name; signed char iv[6]; int n; } CHORDS[] = {
    {"1 NOTE", {0}, 1}, {"5TH", {0, 7}, 2}, {"OCT", {0, 12}, 2}, {"MAJ", {0, 4, 7}, 3}, {"MIN", {0, 3, 7}, 3},
    {"SUS2", {0, 2, 7}, 3}, {"SUS4", {0, 5, 7}, 3}, {"7", {0, 4, 7, 10}, 4}, {"MAJ7", {0, 4, 7, 11}, 4},
    {"MIN7", {0, 3, 7, 10}, 4}, {"MIN9", {0, 3, 7, 10, 14}, 5}, {"DIM", {0, 3, 6}, 3}, {"AUG", {0, 4, 8}, 3},
};
#define N_CHORDS ((int)(sizeof CHORDS / sizeof CHORDS[0]))
static const char *const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
#define N_OCT 6   /* OCT 0..5, Akai naming: note 60 = C3, so OCT n starts at 12 * (n + 2) */

/* CC 20.. on the input channel -> these keys (the Q-Link order, LATCH last instead of MIDI IN CH). */
static const char *const cc_keys[] = {"genre", "pattern", "variant", "swing", "gate", "strum", "accent", "latch",
                                      "root", "chord", "oct"};
#define N_CC ((int)(sizeof cc_keys / sizeof cc_keys[0]))

static int dbg = -1;   /* MIDIFX_DEBUG=1: print what goes out (offline tests: no ALSA there) */
static void send_out(inst_t *in, const uint8_t *m, int len) {
    if (len < 1 || m[0] >= 0xF8) return;   /* realtime messages are ours to the module, not output */
    if (dbg < 0) dbg = getenv("MIDIFX_DEBUG") != NULL;
    if (dbg) fprintf(stderr, "midi out:%s %02X %02X %02X\n", in->running ? "" : " (stopped)", m[0], len > 1 ? m[1] : 0, len > 2 ? m[2] : 0);
    if (!in->seq || in->port < 0) return;
    seq_ev_t ev;
    memset(&ev, 0, sizeof ev);
    A.me_reset(in->enc);
    if (A.me_encode(in->enc, m, len, &ev) <= 0 || ev.type == 0) return;
    ev.source.port = (unsigned char)in->port;
    ev.dest.client = 254;   /* SND_SEQ_ADDRESS_SUBSCRIBERS */
    ev.dest.port = 253;     /* SND_SEQ_ADDRESS_UNKNOWN */
    ev.queue = 253;         /* SND_SEQ_QUEUE_DIRECT */
    A.out_direct(in->seq, &ev);
}

static void emit(inst_t *in, uint8_t out[][3], int lens[], int n) {
    for (int i = 0; i < n; i++) send_out(in, out[i], lens[i]);
}

static void feed(inst_t *in, const uint8_t *m, int len) {
    uint8_t out[MAX_OUT][3];
    int lens[MAX_OUT];
    emit(in, out, lens, api->process_midi(in->mod, m, len, out, lens, MAX_OUT));
}

/* ---- internal chord ---------------------------------------------------------------------------------------------- */
static void note_to_module(inst_t *in, uint8_t status, uint8_t n) { uint8_t m[3] = {status, n, status == 0x90 ? 100 : 0}; feed(in, m, 3); }

/* Release everything the module holds (all 128 keys: external notes may still be down), then hold the internal chord.
 * A fresh press with nothing down also replaces a latched chord in the module. */
static void chord_apply(inst_t *in) {
    for (int n = 0; n < 128; n++) note_to_module(in, 0x80, (uint8_t)n);
    in->ninotes = 0;
    if (in->src != 0) return;
    int base = 12 * (in->oct + 2) + in->root;
    for (int i = 0; i < CHORDS[in->chord].n && in->ninotes < 8; i++) {
        int n = base + CHORDS[in->chord].iv[i];
        if (n < 0 || n > 127) continue;
        in->inotes[in->ninotes++] = (uint8_t)n;
        note_to_module(in, 0x90, (uint8_t)n);
    }
}

static int chord_label(inst_t *in, char *b, int n) {
    if (in->src != 0) return snprintf(b, n, "MIDI IN  %s", in->nlinks ? "+ USB DEVICES" : "PORT ONLY");
    int off = snprintf(b, n, "%s %s  ", NOTE_NAMES[in->root], CHORDS[in->chord].name);
    for (int i = 0; i < in->ninotes && off < n - 1; i++)
        off += snprintf(b + off, n - off, "%s%s%d", i ? " " : "", NOTE_NAMES[in->inotes[i] % 12], in->inotes[i] / 12 - 2);
    return off;
}

/* ---- external USB MIDI devices -> MIDI In ------------------------------------------------------------------------- */
static void scan_devices(inst_t *in) {
    if (!in->seq || in->in_port < 0 || !A.scan_ok) return;
    void *ci = NULL, *pi = NULL;
    if (A.ci_malloc(&ci) < 0 || A.pi_malloc(&pi) < 0) { if (ci) A.ci_free(ci); return; }
    int me = A.client_id(in->seq);
    for (int i = 0; i < in->nlinks; i++) in->link[i].seen = 0;
    A.ci_set_client(ci, -1);
    while (A.next_client(in->seq, ci) >= 0) {
        int c = A.ci_get_client(ci);
        const char *name = A.ci_get_name(ci);
        /* kernel clients only (2 = SND_SEQ_KERNEL_CLIENT): USB/DIN devices; not System (0), Midi Through (14),
         * the Force's own "Akai ..." ports (its surface and its USB port to a computer) or ourselves */
        if (c == me || c == 0 || c == 14 || A.ci_get_type(ci) != 2) continue;
        if (name && (strstr(name, "Akai") || strstr(name, "AKAI") || strstr(name, "Through"))) continue;
        A.pi_set_client(pi, c);
        A.pi_set_port(pi, -1);
        while (A.next_port(in->seq, pi) >= 0) {
            unsigned cap = A.pi_get_cap(pi);
            int p = A.pi_get_port(pi);
            if ((cap & ((1u << 0) | (1u << 5))) != ((1u << 0) | (1u << 5))) continue;   /* READ | SUBS_READ */
            int k;
            for (k = 0; k < in->nlinks; k++)
                if (in->link[k].c == c && in->link[k].p == p) break;
            if (k < in->nlinks) { in->link[k].seen = 1; continue; }
            if (in->nlinks < MAX_LINKS && A.connect_from(in->seq, in->in_port, c, p) >= 0) {
                in->link[in->nlinks].c = (unsigned char)c;
                in->link[in->nlinks].p = (unsigned char)p;
                in->link[in->nlinks++].seen = 1;
            }
        }
    }
    for (int i = 0; i < in->nlinks;)   /* gone (unplugged): forget, so it is connected again when it comes back */
        if (!in->link[i].seen) in->link[i] = in->link[--in->nlinks]; else i++;
    A.pi_free(pi);
    A.ci_free(ci);
}

/* host callbacks (one host, so the transport is global; the current instance's values are mirrored in) */
static void h_log(const char *msg) { (void)msg; }
static inst_t *g_cur;   /* the instance inside a module call, for the send callbacks */
static int h_send(const uint8_t *pkt, int len) {   /* USB-MIDI packet [cable|CIN, status, d1, d2] */
    if (!g_cur || len < 4) return 0;
    int n = (pkt[1] >= 0xC0 && pkt[1] < 0xE0) ? 2 : 3;
    send_out(g_cur, pkt + 1, n);
    return len;
}
static int h_clock_status(void) { return g_running ? MOVE_CLOCK_STATUS_RUNNING : MOVE_CLOCK_STATUS_STOPPED; }
static float h_bpm(void) { return (float)g_bpm; }
static double h_beat(void) { return g_running ? g_ppq : -1.0; }
static int h_recv_channel(void *i) { (void)i; return -1; }

/* ---- module values ----------------------------------------------------------------------------------------------- */
static int mod_int(inst_t *in, const char *k, int dflt) {
    char b[32];
    return api->get_param && api->get_param(in->mod, k, b, sizeof b) > 0 ? atoi(b) : dflt;
}
static void mod_set_int(inst_t *in, const char *k, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    api->set_param(in->mod, k, b);
}

static void genres_load(inst_t *in) {
    if (g_ngenres >= 0 || !api->get_param) return;
    char list[1024];
    g_ngenres = 0;
    if (api->get_param(in->mod, "genre_list", list, sizeof list) <= 0) return;
    int first = 0;
    for (char *save = NULL, *tok = strtok_r(list, "|", &save); tok && g_ngenres < MAX_GENRES; tok = strtok_r(NULL, "|", &save)) {
        char *colon = strrchr(tok, ':');
        if (!colon) break;
        *colon = 0;
        snprintf(g_genre[g_ngenres], sizeof g_genre[0], "%s", tok);
        g_genre_first[g_ngenres++] = first;
        first += atoi(colon + 1);
    }
}

/* option index (OPT_GENRES) of the current groove's genre; -1 if it isn't one of them */
static int genre_get(inst_t *in) {
    char g[32];
    if (!api->get_param || api->get_param(in->mod, "pattern_genre", g, sizeof g) <= 0) return -1;
    for (int i = 0; i < N_OPT_GENRES; i++)
        if (!strcmp(g, OPT_GENRES[i])) return i;
    return -1;
}

/* jump to the first groove of option `idx`; nearest genre the library has if it lacks that one */
static void genre_set(inst_t *in, int idx) {
    genres_load(in);
    if (idx < 0) idx = 0;
    if (idx >= N_OPT_GENRES) idx = N_OPT_GENRES - 1;
    if (genre_get(in) == idx) return;   /* already in it: keep the groove */
    for (int d = 0; d < N_OPT_GENRES; d++) {
        for (int s = -1; s <= 1; s += 2) {
            int o = idx + s * d;
            if (o < 0 || o >= N_OPT_GENRES) continue;
            for (int g = 0; g < g_ngenres; g++)
                if (!strcmp(g_genre[g], OPT_GENRES[o])) { mod_set_int(in, "pattern", g_genre_first[g]); return; }
            if (d == 0) break;
        }
    }
}

static void set_param_inst(inst_t *in, const char *k, const char *v);
/* a CC value 0..127 onto a control */
static void cc_apply(inst_t *in, int which, int v) {
    const char *k = cc_keys[which];
    float f = v / 127.0f;
    if (!strcmp(k, "genre")) genre_set(in, (int)lroundf(f * (N_OPT_GENRES - 1)));
    else if (!strcmp(k, "pattern")) mod_set_int(in, k, (int)lroundf(f * (mod_int(in, "pattern_count", 1) - 1)));
    else if (!strcmp(k, "variant")) mod_set_int(in, k, (int)lroundf(f * (mod_int(in, "variant_count", 1) - 1)));
    else if (!strcmp(k, "swing")) mod_set_int(in, k, (int)lroundf(f * 100));
    else if (!strcmp(k, "gate")) mod_set_int(in, k, 5 + 5 * (int)lroundf(f * 39));
    else if (!strcmp(k, "strum")) mod_set_int(in, k, -100 + 5 * (int)lroundf(f * 40));
    else if (!strcmp(k, "accent")) mod_set_int(in, k, 5 * (int)lroundf(f * 20));
    else if (!strcmp(k, "latch")) mod_set_int(in, k, v >= 64);
    else {   /* root / chord / oct */
        char b[8];
        int hi = !strcmp(k, "root") ? 11 : !strcmp(k, "chord") ? N_CHORDS - 1 : N_OCT - 1;
        snprintf(b, sizeof b, "%d", (int)lroundf(f * hi));
        set_param_inst(in, k, b);
    }
    in->rev++;
}

/* One channel message from either input (VST events or the ALSA port): channel filter, CC control, else the module. */
static void handle_in(inst_t *in, const uint8_t *m, int len) {
    if (len < 1) return;
    if (m[0] >= 0x80 && m[0] < 0xF0) {
        if (in->in_ch && (m[0] & 0x0F) != in->in_ch - 1) return;
        if ((m[0] & 0xF0) == 0xB0 && len >= 3 && m[1] >= CC_FIRST && m[1] < CC_FIRST + N_CC) {
            cc_apply(in, m[1] - CC_FIRST, m[2]);
            return;
        }
        if (in->src == 0 && ((m[0] & 0xF0) == 0x80 || (m[0] & 0xF0) == 0x90)) return;   /* internal chord: notes ignored */
    }
    feed(in, m, len);
}

/* Drain the ALSA input port: decode the sequencer events by hand (no running status, no decoder state). */
static void poll_alsa_in(inst_t *in) {
    if (!in->seq || in->in_port < 0 || !A.input) return;
    for (int n = 0; n < MAX_IN_EVENTS; n++) {
        seq_ev_t *ev = NULL;
        if (A.input(in->seq, &ev) < 0 || !ev) break;   /* -EAGAIN: nothing more (non-blocking) */
        uint8_t m[3];
        int len = 3;
        unsigned char ch = ev->data[0] & 0x0F;
        unsigned int param;
        int value;
        memcpy(&param, ev->data + 4, 4);
        memcpy(&value, ev->data + 8, 4);
        switch (ev->type) {
        case EV_NOTEON: m[0] = 0x90 | ch; m[1] = ev->data[1] & 0x7F; m[2] = ev->data[2] & 0x7F; break;
        case EV_NOTEOFF: m[0] = 0x80 | ch; m[1] = ev->data[1] & 0x7F; m[2] = ev->data[2] & 0x7F; break;
        case EV_KEYPRESS: m[0] = 0xA0 | ch; m[1] = ev->data[1] & 0x7F; m[2] = ev->data[2] & 0x7F; break;
        case EV_CONTROLLER: m[0] = 0xB0 | ch; m[1] = param & 0x7F; m[2] = (uint8_t)(value & 0x7F); break;
        case EV_PGMCHANGE: m[0] = 0xC0 | ch; m[1] = (uint8_t)(value & 0x7F); m[2] = 0; len = 2; break;
        case EV_CHANPRESS: m[0] = 0xD0 | ch; m[1] = (uint8_t)(value & 0x7F); m[2] = 0; len = 2; break;
        case EV_PITCHBEND: { int b = value + 8192; b = b < 0 ? 0 : b > 16383 ? 16383 : b;
                             m[0] = 0xE0 | ch; m[1] = b & 0x7F; m[2] = (uint8_t)(b >> 7); break; }
        default: continue;   /* clock, start/stop, sysex...: the transport comes from the host */
        }
        handle_in(in, m, len);
    }
}

/* ---- engine ------------------------------------------------------------------------------------------------------ */
static void *create(const char *dir) {
    if (!api) return NULL;
    inst_t *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->bpm = 120;
    in->ppq = -1;
    in->port = in->in_port = -1;
    in->src = 0; in->root = 2; in->chord = 4; in->oct = 2;   /* D minor, D2 (params.json defaults) */
    g_cur = in;
    in->mod = api->create_instance(dir && dir[0] ? dir : MODULE_DIR, NULL);
    g_cur = NULL;
    if (!in->mod) { free(in); return NULL; }
    genres_load(in);
#ifdef MIDIFX_INIT   /* per-port start-up settings for the MPC, "key=val;key=val" (vst.json "defines"), e.g. sync=clock */
    {
        char init[256], *save = NULL;
        snprintf(init, sizeof init, "%s", MIDIFX_INIT);
        for (char *kv = strtok_r(init, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(kv, '=');
            if (!eq) continue;
            *eq = 0;
            api->set_param(in->mod, kv, eq + 1);
        }
    }
#endif
    alsa_load();
#ifdef PLUG_EFFECT
    const int mode = 3;   /* SND_SEQ_OPEN_DUPLEX: MIDI Out and MIDI In */
#else
    const int mode = 1;   /* SND_SEQ_OPEN_OUTPUT */
#endif
    if (A.ok == 1 && A.open(&in->seq, "default", mode, 0) >= 0) {
        char name[64];
        int n = ++instances;
        if (n > 1) snprintf(name, sizeof name, "%s %d", PLUG_NAME, n);
        else snprintf(name, sizeof name, "%s", PLUG_NAME);
        A.set_name(in->seq, name);
        in->port = A.port(in->seq, "MIDI Out", (1u << 0) | (1u << 5) /* READ | SUBS_READ */,
                          (1u << 1) | (1u << 20) /* MIDI_GENERIC | APPLICATION */);
#ifdef PLUG_EFFECT
        if (A.input)
            in->in_port = A.port(in->seq, "MIDI In", (1u << 1) | (1u << 6) /* WRITE | SUBS_WRITE */,
                                 (1u << 1) | (1u << 20) /* MIDI_GENERIC | APPLICATION */);
#endif
        if (A.nonblock) A.nonblock(in->seq, 1);
        if (in->port < 0 || A.me_new(64, &in->enc) < 0) { A.close(in->seq); in->seq = NULL; in->port = in->in_port = -1; }
    }
    g_cur = in;
    chord_apply(in);
    g_cur = NULL;
    return in;
}

static void destroy(void *p) {
    inst_t *in = p;
    if (!in) return;
    if (in->running) { uint8_t stop = 0xFC; g_cur = in; feed(in, &stop, 1); g_cur = NULL; }
    api->destroy_instance(in->mod);
    if (in->enc) A.me_free(in->enc);
    if (in->seq) A.close(in->seq);
    free(in);
}

static void midi(void *p, const uint8_t *m, int len) {
    inst_t *in = p;
    g_cur = in;
    handle_in(in, m, len);
    g_cur = NULL;
}

/* the plugin's own state (in_ch) rides along in the module's JSON state */
static void set_param_inst(inst_t *in, const char *k, const char *v) {
    if (!strcmp(k, "genre")) genre_set(in, atoi(v));
    else if (!strcmp(k, "in_ch")) { int c = atoi(v); in->in_ch = c < 0 ? 0 : c > 16 ? 16 : c; }
    else if (!strcmp(k, "src") || !strcmp(k, "root") || !strcmp(k, "chord") || !strcmp(k, "oct")) {
        int x = atoi(v);
        int *dst = !strcmp(k, "src") ? &in->src : !strcmp(k, "root") ? &in->root : !strcmp(k, "chord") ? &in->chord : &in->oct;
        int hi = dst == &in->src ? 1 : dst == &in->root ? 11 : dst == &in->chord ? N_CHORDS - 1 : N_OCT - 1;
        x = x < 0 ? 0 : x > hi ? hi : x;
        if (x != *dst) {
            *dst = x;
            chord_apply(in);
            if (dst == &in->src && x == 1) { in->rescan = 0; scan_devices(in); }
        }
    }
    else if (strstr(k, "_prev") || strstr(k, "_next")) { /* stepper arrows: the wrapper steps their param */ }
    else {
        if (!strcmp(k, "state")) {
            static const char *const keys[] = {"\"in_ch\"", "\"src\"", "\"root\"", "\"chord\"", "\"oct\""};
            static const int his[] = {16, 1, 11, N_CHORDS - 1, N_OCT - 1};
            int *dsts[] = {&in->in_ch, &in->src, &in->root, &in->chord, &in->oct};
            for (int i = 0; i < 5; i++) {
                const char *s = strstr(v, keys[i]);
                if (s && (s = strchr(s, ':'))) { int c = atoi(s + 1); *dsts[i] = c < 0 ? 0 : c > his[i] ? his[i] : c; }
            }
        }
        api->set_param(in->mod, k, v);
        if (!strcmp(k, "state")) { chord_apply(in); in->rev++; }
    }
}

static void set_param(void *p, const char *k, const char *v) {
    inst_t *in = p;
    g_cur = in;
    set_param_inst(in, k, v);
    g_cur = NULL;
}

static int get_param(void *p, const char *k, char *b, int n) {
    inst_t *in = p;
    if (!strcmp(k, "genre")) { int g = genre_get(in); return snprintf(b, n, "%d", g < 0 ? 0 : g); }
    if (!strcmp(k, "in_ch")) return snprintf(b, n, "%d", in->in_ch);
    if (!strcmp(k, "src")) return snprintf(b, n, "%d", in->src);
    if (!strcmp(k, "root")) return snprintf(b, n, "%d", in->root);
    if (!strcmp(k, "chord")) return snprintf(b, n, "%d", in->chord);
    if (!strcmp(k, "oct")) return snprintf(b, n, "%d", in->oct);
    if (!strcmp(k, "chord_label")) return chord_label(in, b, n);
    if (!strcmp(k, "display_rev")) return snprintf(b, n, "%u.%d", in->rev, mod_int(in, "preview_rev", 0));
    if (!api->get_param) return -1;
    int r = api->get_param(in->mod, k, b, n);
    if (r > 0 && !strcmp(k, "state")) {   /* {"pattern":..} -> {"pattern":..,"in_ch":n} */
        char *end = strrchr(b, '}');
        if (end) {
            int at = (int)(end - b);
            int w = snprintf(end, n - at, ",\"in_ch\":%d,\"src\":%d,\"root\":%d,\"chord\":%d,\"oct\":%d}",
                             in->in_ch, in->src, in->root, in->chord, in->oct);
            if (w > 0 && w < n - at) r = at + w;
            else { b[at] = '}'; b[at + 1] = 0; r = at + 1; }   /* no room: the module's own state */
        }
    }
    return r;
}

/* Transport from the wrapper (once per host buffer): start/stop, and resync the position after a jump. */
void mpc_engine_transport(void *p, double bpm, double ppq, int playing) {
    inst_t *in = p;
    if (!in) return;
    if (bpm > 0) in->bpm = bpm;
    g_cur = in;
    if (playing && !in->running) {
        in->ppq = ppq >= 0 ? ppq : 0;
        in->clock = 0;
        in->running = 1;
        uint8_t start = 0xFA;
        feed(in, &start, 1);
        uint8_t tick = 0xF8;   /* the first clock coincides with Start */
        feed(in, &tick, 1);
        in->clock = 1;
    } else if (!playing && in->running) {
        in->running = 0;
        uint8_t stop = 0xFC;
        feed(in, &stop, 1);
    } else if (playing && ppq >= 0 && fabs(ppq - in->ppq) > 1.0 / 24) {
        in->ppq = ppq;   /* loop or locate: follow the host; the module keeps counting clocks */
    }
    in->playing = playing;
    g_cur = NULL;
}

/* one block of sequencing: input notes, clock ticks inside the block, the module's tick */
static void run(inst_t *in, int frames) {
    g_cur = in;
    if ((in->rescan -= frames) <= 0) { in->rescan = RESCAN_FRAMES; scan_devices(in); }   /* both sources: CCs */
    poll_alsa_in(in);
    g_bpm = in->bpm;
    g_running = in->running;
    g_ppq = in->ppq;
    if (in->running) {   /* clock ticks that fall inside this block */
        double start = in->ppq, end = start + frames * in->bpm / (60.0 * SR);
        long first = (long)floor(start * 24) + 1, last = (long)floor(end * 24);
        uint8_t tick = 0xF8;
        for (long t = first; t <= last; t++) feed(in, &tick, 1);
        in->ppq = end;
        g_ppq = end;
    }
    uint8_t outm[MAX_OUT][3];
    int lens[MAX_OUT];
    emit(in, outm, lens, api->tick(in->mod, frames, SR, outm, lens, MAX_OUT));
    g_cur = NULL;
}

static void render(void *p, int16_t *out, int frames) {
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
    run(p, frames);
}

#ifdef PLUG_EFFECT
/* insert effect: the track's audio goes through untouched */
static void process(void *p, const int16_t *in_lr, int16_t *out_lr, int frames) {
    memcpy(out_lr, in_lr, (size_t)frames * 2 * sizeof(int16_t));
    run(p, frames);
}
static const mpc_engine_t engine = { create, destroy, midi, set_param, get_param, render, process };
#else
static const mpc_engine_t engine = { create, destroy, midi, set_param, get_param, render, NULL };
#endif

const mpc_engine_t *mpc_engine(void) {
    if (!api) {
        memset(&host, 0, sizeof host);
        host.api_version = 1;
        host.sample_rate = SR;
        host.frames_per_block = 128;
        host.log = h_log;
        host.midi_send_internal = h_send;
        host.midi_send_external = h_send;
        host.midi_inject_to_move = h_send;
        host.get_clock_status = h_clock_status;
        host.get_bpm = h_bpm;
        host.slot_recv_channel = h_recv_channel;
        host.get_beat_position = h_beat;
        api = move_midi_fx_init(&host);
    }
    return api ? &engine : NULL;
}

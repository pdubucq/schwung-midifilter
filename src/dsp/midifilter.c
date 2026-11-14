/*
 * MIDI Filter
 *
 * A chain MIDI FX that lets through only a note range, transposes it by whole
 * octaves, optionally drops the sustain pedal, and sends what it lets through
 * to this slot's synth, to a stock Move track (injected on cable 2 on
 * `move_channel`), or to both.
 *
 * One instance per slot. All slots listen on the keyboard's channel and each
 * instance filters for itself; instances never talk to each other.
 *
 * REALTIME: process_midi, tick, set_param and get_param run on the SPI
 * callback. No allocation, locks, file I/O, logging or threads anywhere below
 * except create/destroy (which run on the slot loader thread).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

static const host_api_v1_t *g_host = NULL;

/* held_move[ch][note]: 0, or DEST_MOVE | move channel (0..15) */
#define DEST_MOVE   0x10
#define DEST_PEND   0x80            /* Move note-off still owed (inject refused) */

#define OUT_SCHWUNG 0               /* this slot's synth only */
#define OUT_MOVE    1               /* stock Move track only */
#define OUT_BOTH    2

#define TRANSPOSE_MAX 10            /* octaves, either way */

#define ARM_NONE 0
#define ARM_LOW  1
#define ARM_HIGH 2

#define LEARN_TIMEOUT_SEC 10

#define COALESCE_MAX       16      /* pending bend / pressure / CC values for Move */
#define COALESCE_PER_TICK  2       /* inject budget per tick for those */
#define RELEASE_PER_TICK   8       /* note-off retries / releases per tick */

typedef struct {
    uint8_t status;                 /* 0 = free slot */
    uint8_t d1;
    uint8_t d2;
} coalesce_t;

typedef struct {
    const host_api_v1_t *host;
    void *host_ctx;

    int low_note;                   /* 0..127 */
    int high_note;                  /* 0..127, always >= low_note */
    int output;                     /* OUT_* */
    int transpose;                  /* octaves, -TRANSPOSE_MAX..TRANSPOSE_MAX */
    int move_channel;               /* 0..15 */
    int sustain;                    /* 0/1 */

    int armed;                      /* ARM_* */
    int arm_frames;
    int learn_swallow_ch;           /* note-off of the learned key, -1 = none */
    int learn_swallow_note;

    /* Indexed by the INPUT channel and note. held_note is the transposed note
     * both destinations were given, so a note-off finds its note-on even if
     * transpose changed while the key was down. */
    uint8_t held_chain[16][128];    /* 0/1 */
    uint8_t held_move[16][128];
    uint8_t held_note[16][128];
    int pending_offs;               /* number of DEST_PEND entries */
    int release_move_all;           /* release every Move note on the next ticks */

    int pedal_chain;                /* last passed pedal-down reached the chain */
    int pedal_chain_ch;             /* input channel of that pedal-down */
    int pedal_move;                 /* ... and/or Move, on pedal_move_ch */
    int pedal_move_ch;
    int pedal_release_chain;        /* a CC64=0 is still owed here */
    int pedal_release_chain_ch;
    int pedal_release_move;
    int pedal_release_move_ch;

    coalesce_t coalesce[COALESCE_MAX];
} mf_instance_t;

/* ------------------------------------------------------------------------ */
/* Value parsing                                                            */
/* ------------------------------------------------------------------------ */

/* A note arrives as its number ("60", the knob grid's index wire) or as its
 * name in Live's numbering ("C3" = 60, "C-2" = 0, "C#3", "Db3"), which is what
 * the grid shows. Returns -1 when it is neither. */
static int parse_note(const char *val) {
    if (!val) return -1;
    while (*val == ' ') val++;
    if (!*val) return -1;

    if (isdigit((unsigned char)*val)) {
        char *end;
        long n = strtol(val, &end, 10);
        if (*end == '.') {          /* "60.000" from a float-formatted write */
            n = (long)(strtod(val, &end) + 0.5);
        }
        while (*end == ' ') end++;
        if (*end) return -1;
        return (n >= 0 && n <= 127) ? (int)n : -1;
    }

    static const int pc_of[7] = { 9, 11, 0, 2, 4, 5, 7 }; /* A B C D E F G */
    char c = (char)toupper((unsigned char)*val);
    if (c < 'A' || c > 'G') return -1;
    int pc = pc_of[c - 'A'];
    val++;
    if (*val == '#') { pc++; val++; }
    else if (*val == 'b') { pc--; val++; }

    char *end;
    long oct = strtol(val, &end, 10);
    if (end == val) return -1;
    while (*end == ' ') end++;
    if (*end) return -1;

    long n = (oct + 2) * 12 + pc;
    return (n >= 0 && n <= 127) ? (int)n : -1;
}

/* "schwung only" / "move only" / "both", or their 0..2 index on the grid's
 * index wire. -1 = unrecognised. */
static int parse_output(const char *val) {
    if (!val) return -1;
    if (!strcmp(val, "schwung only") || !strcmp(val, "0")) return OUT_SCHWUNG;
    if (!strcmp(val, "move only")    || !strcmp(val, "1")) return OUT_MOVE;
    if (!strcmp(val, "both")         || !strcmp(val, "2")) return OUT_BOTH;
    return -1;
}

static const char *output_str(int output) {
    return output == OUT_MOVE ? "move only" : output == OUT_BOTH ? "both" : "schwung only";
}

/* A whole number of octaves, "-2" or "-2.000" from a float-formatted write. */
static int parse_transpose(const char *val, int *out) {
    if (!val || !*val) return 0;
    char *end;
    double d = strtod(val, &end);
    if (end == val) return 0;
    while (*end == ' ') end++;
    if (*end) return 0;
    if (!(d > -1000.0 && d < 1000.0)) return 0;     /* also rejects NaN */
    long n = (long)(d < 0 ? d - 0.5 : d + 0.5);
    if (n < -TRANSPOSE_MAX || n > TRANSPOSE_MAX) return 0;
    *out = (int)n;
    return 1;
}

/* off/on switch. -1 = unrecognised (ignored, never guessed). */
static int parse_bool(const char *val) {
    if (!val) return -1;
    if (!strcmp(val, "on") || !strcmp(val, "1") || !strcmp(val, "true")) return 1;
    if (!strcmp(val, "off") || !strcmp(val, "0") || !strcmp(val, "false")) return 0;
    return -1;
}

/* "1".."16" -> 0..15, -1 otherwise. */
static int parse_channel(const char *val) {
    if (!val) return -1;
    char *end;
    long n = strtol(val, &end, 10);
    if (end == val || *end) return -1;
    return (n >= 1 && n <= 16) ? (int)(n - 1) : -1;
}

/* A trigger fires on anything but its idle spelling or an index-0 write. */
static int trigger_fires(const char *val) {
    if (!val || !*val) return 0;
    if (!strcmp(val, "-") || !strcmp(val, "0")) return 0;
    return 1;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    const char *pos, *p, *end;
    int len;
    if (!json || !key || !out || out_len < 1) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return 0;
    p = strchr(pos + strlen(search), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        end = strchr(p, '"');
        if (!end) return 0;
    } else {
        end = p;
        while (*end && *end != ',' && *end != '}' && *end != ' ') end++;
    }
    len = (int)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return len > 0;
}

/* ------------------------------------------------------------------------ */
/* Move injection                                                           */
/* ------------------------------------------------------------------------ */

static int inject(mf_instance_t *inst, uint8_t status, uint8_t d1, uint8_t d2) {
    if (!inst->host || !inst->host->midi_inject_to_move) return 0;
    uint8_t pkt[4] = { (uint8_t)(0x20 | (status >> 4)), status, d1, d2 };
    return inst->host->midi_inject_to_move(pkt, 4) > 0;
}

/* 1 when Move passthrough can inject on move_channel without looping back
 * into this slot. Move echoes cable 2 on MIDI_OUT, and slots are fed from that
 * echo, so a slot hearing move_channel (or All) would receive its own output. */
static int move_route_ok(mf_instance_t *inst) {
    if (!inst->host || !inst->host->midi_inject_to_move) return 0;
    if (!inst->host->slot_recv_channel) return 1;
    int recv = inst->host->slot_recv_channel(inst->host_ctx);
    if (recv == -1) return 0;
    if (recv == inst->move_channel) return 0;
    return 1;
}

static void coalesce_put(mf_instance_t *inst, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t type = status & 0xF0;
    int free_i = -1;
    for (int i = 0; i < COALESCE_MAX; i++) {
        coalesce_t *c = &inst->coalesce[i];
        if (!c->status) { if (free_i < 0) free_i = i; continue; }
        if (c->status == status && (type != 0xB0 || c->d1 == d1)) {
            c->d1 = d1;
            c->d2 = d2;
            return;
        }
    }
    if (free_i >= 0) {
        inst->coalesce[free_i].status = status;
        inst->coalesce[free_i].d1 = d1;
        inst->coalesce[free_i].d2 = d2;
        return;
    }
    inject(inst, status, d1, d2);   /* table full: best effort */
}

static void coalesce_clear(mf_instance_t *inst) {
    memset(inst->coalesce, 0, sizeof(inst->coalesce));
}

/* Inject a Move note-off now; if refused, owe it (retried from tick). */
static void move_note_off(mf_instance_t *inst, int in_ch, int in_note) {
    uint8_t d = inst->held_move[in_ch][in_note];
    if (inject(inst, (uint8_t)(0x80 | (d & 0x0F)), inst->held_note[in_ch][in_note], 0)) {
        inst->held_move[in_ch][in_note] = 0;
    } else {
        if (!(d & DEST_PEND)) inst->pending_offs++;
        inst->held_move[in_ch][in_note] = (uint8_t)(d | DEST_PEND);
    }
}

static int sends_to_chain(const mf_instance_t *inst) { return inst->output != OUT_MOVE; }
static int sends_to_move(const mf_instance_t *inst)  { return inst->output != OUT_SCHWUNG; }

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

static void* mf_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;
    mf_instance_t *inst = (mf_instance_t *)calloc(1, sizeof(mf_instance_t));
    if (!inst) return NULL;

    /* Each slot hands init() its own host_api copy just before create, so
     * capture it per instance: g_host is overwritten by the next slot. */
    inst->host = g_host;
    inst->host_ctx = g_host ? g_host->mod_host_ctx : NULL;

    inst->low_note = 0;
    inst->high_note = 127;
    inst->output = OUT_SCHWUNG;
    inst->transpose = 0;
    inst->move_channel = 1;         /* channel 2 */
    inst->sustain = 1;
    inst->learn_swallow_ch = -1;
    inst->learn_swallow_note = -1;
    return inst;
}

static void mf_destroy_instance(void *instance) {
    /* Held Move notes are NOT released here: destroy runs on the slot loader
     * thread, and the inject ring's other producers run on the SPI thread. */
    free(instance);
}

/* ------------------------------------------------------------------------ */
/* MIDI                                                                     */
/* ------------------------------------------------------------------------ */

static int emit(uint8_t out_msgs[][3], int out_lens[], int max_out, int n,
                const uint8_t *msg, int len) {
    if (n >= max_out) return n;
    if (len > 3) len = 3;
    memcpy(out_msgs[n], msg, (size_t)len);
    out_lens[n] = len;
    return n + 1;
}

static int mf_process_midi(void *instance,
                           const uint8_t *in_msg, int in_len,
                           uint8_t out_msgs[][3], int out_lens[],
                           int max_out) {
    mf_instance_t *inst = (mf_instance_t *)instance;
    if (!inst || !in_msg || in_len < 1) return 0;

    uint8_t status = in_msg[0];

    /* System and realtime: always to the chain, never to Move. */
    if (status >= 0xF0 || status < 0x80) {
        return emit(out_msgs, out_lens, max_out, 0, in_msg, in_len);
    }

    uint8_t type = status & 0xF0;
    int ch = status & 0x0F;
    uint8_t d1 = in_len > 1 ? (in_msg[1] & 0x7F) : 0;
    uint8_t d2 = in_len > 2 ? (in_msg[2] & 0x7F) : 0;
    int is_on  = (type == 0x90 && d2 > 0);
    int is_off = (type == 0x80 || (type == 0x90 && d2 == 0));

    /* --- MIDI learn: the first note-on is taken, and its note-off with it --- */
    if (is_off && ch == inst->learn_swallow_ch && d1 == inst->learn_swallow_note) {
        inst->learn_swallow_ch = -1;
        inst->learn_swallow_note = -1;
        return 0;
    }
    if (is_on && inst->armed != ARM_NONE) {
        if (inst->armed == ARM_LOW) {
            inst->low_note = d1;
            if (inst->high_note < d1) inst->high_note = d1;
        } else {
            inst->high_note = d1;
            if (inst->low_note > d1) inst->low_note = d1;
        }
        inst->armed = ARM_NONE;
        inst->learn_swallow_ch = ch;
        inst->learn_swallow_note = d1;
        return 0;
    }

    /* --- Notes: decided on the note-on, the note-off follows it --- */
    if (is_on) {
        if (inst->held_move[ch][d1] & DEST_PEND) {  /* a retry is owed: send it before the new on */
            move_note_off(inst, ch, d1);
            if (inst->held_move[ch][d1] & DEST_PEND) return 0;   /* still owed: drop this on */
            inst->pending_offs--;
        }
        if (d1 < inst->low_note || d1 > inst->high_note) {
            return 0;
        }
        int note = d1 + inst->transpose * 12;
        if (note < 0 || note > 127) return 0;       /* transposed off the keyboard: dropped, never clamped */

        int n = 0;
        if (sends_to_move(inst) && move_route_ok(inst) &&
            inject(inst, (uint8_t)(0x90 | inst->move_channel), (uint8_t)note, d2)) {
            inst->held_move[ch][d1] = (uint8_t)(DEST_MOVE | inst->move_channel);
            inst->held_note[ch][d1] = (uint8_t)note;
        }
        if (sends_to_chain(inst)) {
            uint8_t m[3] = { in_msg[0], (uint8_t)note, d2 };
            inst->held_chain[ch][d1] = 1;
            inst->held_note[ch][d1] = (uint8_t)note;
            n = emit(out_msgs, out_lens, max_out, n, m, 3);
        }
        return n;
    }

    if (is_off || type == 0xA0) {
        int n = 0;
        if (inst->held_chain[ch][d1]) {
            uint8_t m[3] = { in_msg[0], inst->held_note[ch][d1], d2 };
            if (is_off) inst->held_chain[ch][d1] = 0;
            n = emit(out_msgs, out_lens, max_out, n, m, 3);
        }
        uint8_t dest = inst->held_move[ch][d1];
        if ((dest & DEST_MOVE) && !(dest & DEST_PEND)) {
            if (is_off) move_note_off(inst, ch, d1);
            else inject(inst, (uint8_t)(0xA0 | (dest & 0x0F)), inst->held_note[ch][d1], d2);
        }
        return n;                   /* nothing held: not ours (dropped on) */
    }

    /* --- Sustain pedal --- */
    if (type == 0xB0 && d1 == 64) {
        if (!inst->sustain) return 0;
        int chain = sends_to_chain(inst);
        int move = sends_to_move(inst) && move_route_ok(inst);
        if (move) inject(inst, (uint8_t)(0xB0 | inst->move_channel), 64, d2);
        if (d2 >= 64) {
            inst->pedal_chain = chain;
            inst->pedal_chain_ch = ch;
            inst->pedal_move = move;
            inst->pedal_move_ch = inst->move_channel;
        } else {
            inst->pedal_chain = 0;
            inst->pedal_move = 0;
        }
        return chain ? emit(out_msgs, out_lens, max_out, 0, in_msg, in_len) : 0;
    }

    /* --- Everything else on the channel: CC, bend, pressure, program --- */
    int n = 0;
    if (sends_to_chain(inst)) n = emit(out_msgs, out_lens, max_out, 0, in_msg, in_len);
    if (sends_to_move(inst) && move_route_ok(inst)) {
        uint8_t mstatus = (uint8_t)(type | inst->move_channel);
        if (type == 0xB0 && d1 >= 120) {
            inject(inst, mstatus, d1, d2);              /* channel mode: never thinned */
        } else if (type == 0xC0) {
            inject(inst, mstatus, d1, 0);
        } else {
            coalesce_put(inst, mstatus, d1, d2);        /* bend, pressure, CC */
        }
    }
    return n;
}

static int mf_tick(void *instance, int frames, int sample_rate,
                   uint8_t out_msgs[][3], int out_lens[], int max_out) {
    mf_instance_t *inst = (mf_instance_t *)instance;
    if (!inst) return 0;
    int n = 0;

    /* Learn timeout */
    if (inst->armed != ARM_NONE) {
        if (sample_rate <= 0) sample_rate = 44100;
        inst->arm_frames += frames;
        if (inst->arm_frames >= LEARN_TIMEOUT_SEC * sample_rate) {
            inst->armed = ARM_NONE;
            inst->arm_frames = 0;
        }
    }

    /* A pedal left down on a destination we no longer send to */
    if (inst->pedal_release_chain) {
        uint8_t m[3] = { (uint8_t)(0xB0 | inst->pedal_release_chain_ch), 64, 0 };
        n = emit(out_msgs, out_lens, max_out, n, m, 3);
        inst->pedal_release_chain = 0;
    }
    if (inst->pedal_release_move &&
        inject(inst, (uint8_t)(0xB0 | inst->pedal_release_move_ch), 64, 0)) {
        inst->pedal_release_move = 0;
    }

    /* Owed Move note-offs, and the release of every Move note */
    if (inst->pending_offs > 0 || inst->release_move_all) {
        int budget = RELEASE_PER_TICK;
        int left = 0;
        for (int c = 0; c < 16; c++) {
            for (int k = 0; k < 128; k++) {
                uint8_t d = inst->held_move[c][k];
                if (!(d & DEST_MOVE)) continue;
                int owed = (d & DEST_PEND) != 0;
                if (!owed && !inst->release_move_all) continue;
                if (budget <= 0) { left = 1; continue; }
                budget--;
                if (inject(inst, (uint8_t)(0x80 | (d & 0x0F)), inst->held_note[c][k], 0)) {
                    inst->held_move[c][k] = 0;
                    if (owed) inst->pending_offs--;
                } else {
                    if (!owed) { inst->held_move[c][k] = (uint8_t)(d | DEST_PEND); inst->pending_offs++; }
                    left = 1;
                }
            }
        }
        if (!left) inst->release_move_all = 0;
        if (inst->pending_offs < 0) inst->pending_offs = 0;
    }

    /* Thinned continuous controllers for Move */
    int sent = 0;
    for (int i = 0; i < COALESCE_MAX && sent < COALESCE_PER_TICK; i++) {
        coalesce_t *c = &inst->coalesce[i];
        if (!c->status) continue;
        if (!sends_to_move(inst)) { c->status = 0; continue; }
        sent++;
        if (inject(inst, c->status, c->d1, c->d2)) c->status = 0;
    }

    return n;
}

/* ------------------------------------------------------------------------ */
/* Parameters                                                               */
/* ------------------------------------------------------------------------ */

/* The pedal is down on a destination we are about to stop sending to. */
static void release_chain_pedal(mf_instance_t *inst) {
    if (inst->pedal_chain) {
        inst->pedal_release_chain = 1;
        inst->pedal_release_chain_ch = inst->pedal_chain_ch;
        inst->pedal_chain = 0;
    }
}

static void release_move_pedal(mf_instance_t *inst) {
    if (inst->pedal_move) {
        inst->pedal_release_move = 1;
        inst->pedal_release_move_ch = inst->pedal_move_ch;
        inst->pedal_move = 0;
    }
}

static void apply_output(mf_instance_t *inst, int v) {
    if (v == inst->output) return;
    if (v == OUT_MOVE) release_chain_pedal(inst);
    if (v == OUT_SCHWUNG) {
        release_move_pedal(inst);
        inst->release_move_all = 1;
        coalesce_clear(inst);
    }
    inst->output = v;
}

static void set_low(mf_instance_t *inst, int v) {
    inst->low_note = v;
    if (inst->high_note < v) inst->high_note = v;
}

static void set_high(mf_instance_t *inst, int v) {
    inst->high_note = v;
    if (inst->low_note > v) inst->low_note = v;
}

static void arm(mf_instance_t *inst, int which) {
    inst->armed = (inst->armed == which) ? ARM_NONE : which;
    inst->arm_frames = 0;
}

static void mf_set_param(void *instance, const char *key, const char *val) {
    mf_instance_t *inst = (mf_instance_t *)instance;
    if (!inst || !key || !val) return;
    int v;

    if (!strcmp(key, "low_note")) {
        if ((v = parse_note(val)) >= 0) set_low(inst, v);
    } else if (!strcmp(key, "high_note")) {
        if ((v = parse_note(val)) >= 0) set_high(inst, v);
    } else if (!strcmp(key, "output")) {
        if ((v = parse_output(val)) >= 0) apply_output(inst, v);
    } else if (!strcmp(key, "transpose")) {
        if (parse_transpose(val, &v)) inst->transpose = v;
    } else if (!strcmp(key, "move_channel")) {
        if ((v = parse_channel(val)) >= 0 && v != inst->move_channel) {
            release_move_pedal(inst);
            coalesce_clear(inst);
            inst->move_channel = v;
        }
    } else if (!strcmp(key, "sustain")) {
        if ((v = parse_bool(val)) >= 0 && v != inst->sustain) {
            if (!v) {
                release_chain_pedal(inst);
                release_move_pedal(inst);
            }
            inst->sustain = v;
        }
    } else if (!strcmp(key, "learn_low")) {
        if (trigger_fires(val)) arm(inst, ARM_LOW);
    } else if (!strcmp(key, "learn_high")) {
        if (trigger_fires(val)) arm(inst, ARM_HIGH);
    } else if (!strcmp(key, "state")) {
        char buf[32];
        /* Both notes first and directly, so a saved low above the current
         * high is not clamped against a value about to be replaced. */
        int lo = -1, hi = -1;
        if (json_get_string(val, "low_note", buf, sizeof(buf))) lo = parse_note(buf);
        if (json_get_string(val, "high_note", buf, sizeof(buf))) hi = parse_note(buf);
        if (lo >= 0) inst->low_note = lo;
        if (hi >= 0) inst->high_note = hi;
        if (inst->high_note < inst->low_note) inst->high_note = inst->low_note;
        if (json_get_string(val, "output", buf, sizeof(buf))) {
            mf_set_param(inst, "output", buf);
        } else if (json_get_string(val, "move_pass", buf, sizeof(buf))) {   /* saved before Output existed */
            int mp = parse_bool(buf);
            if (mp >= 0) apply_output(inst, mp ? OUT_MOVE : OUT_SCHWUNG);
        }
        if (json_get_string(val, "transpose", buf, sizeof(buf))) mf_set_param(inst, "transpose", buf);
        if (json_get_string(val, "move_channel", buf, sizeof(buf))) mf_set_param(inst, "move_channel", buf);
        if (json_get_string(val, "sustain", buf, sizeof(buf))) mf_set_param(inst, "sustain", buf);
    }
}

static const char *status_str(mf_instance_t *inst) {
    if (inst->armed == ARM_LOW) return "learn lo";
    if (inst->armed == ARM_HIGH) return "learn hi";
    if (sends_to_move(inst)) {
        if (!inst->host || !inst->host->midi_inject_to_move) return "no move";
        if (!move_route_ok(inst)) return "ch clash";
    }
    return "ok";
}

static int mf_get_param(void *instance, const char *key, char *buf, int buf_len) {
    mf_instance_t *inst = (mf_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (!strcmp(key, "low_note"))     return snprintf(buf, buf_len, "%d", inst->low_note);
    if (!strcmp(key, "high_note"))    return snprintf(buf, buf_len, "%d", inst->high_note);
    if (!strcmp(key, "output"))       return snprintf(buf, buf_len, "%s", output_str(inst->output));
    if (!strcmp(key, "transpose"))    return snprintf(buf, buf_len, "%d", inst->transpose);
    if (!strcmp(key, "move_channel")) return snprintf(buf, buf_len, "%d", inst->move_channel + 1);
    if (!strcmp(key, "sustain"))      return snprintf(buf, buf_len, "%s", inst->sustain ? "on" : "off");
    if (!strcmp(key, "learn_low"))    return snprintf(buf, buf_len, "-");
    if (!strcmp(key, "learn_high"))   return snprintf(buf, buf_len, "-");
    if (!strcmp(key, "status"))       return snprintf(buf, buf_len, "%s", status_str(inst));
    if (!strcmp(key, "name"))         return snprintf(buf, buf_len, "MIDI Filter");
    if (!strcmp(key, "state")) {
        return snprintf(buf, buf_len,
            "{\"low_note\":%d,\"high_note\":%d,\"output\":\"%s\",\"transpose\":\"%d\","
            "\"move_channel\":\"%d\",\"sustain\":\"%s\"}",
            inst->low_note, inst->high_note,
            output_str(inst->output),
            inst->transpose,
            inst->move_channel + 1,
            inst->sustain ? "on" : "off");
    }
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = mf_create_instance,
    .destroy_instance = mf_destroy_instance,
    .process_midi = mf_process_midi,
    .tick = mf_tick,
    .set_param = mf_set_param,
    .get_param = mf_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}

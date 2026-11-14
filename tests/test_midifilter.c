/* White-box tests for src/dsp/midifilter.c (included directly). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#include "../src/dsp/midifilter.c"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- fake host ---------------------------------------------------------- */

static int test_host_ctx;
static int fake_recv_channel = 0;        /* slot Recv Ch: channel 1 */
static int inject_refuse = 0;            /* 1 = ring full */
static uint8_t injected[256][4];
static int n_injected = 0;

static int fake_slot_recv_channel(void *instance) {
    return instance == &test_host_ctx ? fake_recv_channel : -2;
}

static int fake_inject(const uint8_t *msg, int len) {
    if (len != 4 || inject_refuse) return 0;
    if (n_injected < 256) memcpy(injected[n_injected++], msg, 4);
    return 4;
}

static host_api_v1_t host;
static midi_fx_api_v1_t *api;

static uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
static int out_lens[MIDI_FX_MAX_OUT_MSGS];

static int send3(void *inst, uint8_t s, uint8_t d1, uint8_t d2) {
    uint8_t m[3] = { s, d1, d2 };
    return api->process_midi(inst, m, 3, out, out_lens, MIDI_FX_MAX_OUT_MSGS);
}

static int tick(void *inst) {
    return api->tick(inst, 128, 44100, out, out_lens, MIDI_FX_MAX_OUT_MSGS);
}

static int injected_is(int i, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    return i < n_injected && injected[i][0] == b0 && injected[i][1] == b1 &&
           injected[i][2] == b2 && injected[i][3] == b3;
}

static void *fresh(void) {
    n_injected = 0;
    inject_refuse = 0;
    fake_recv_channel = 0;
    return api->create_instance(".", NULL);
}

static const char *get(void *inst, const char *key) {
    static char buf[256];
    buf[0] = '\0';
    api->get_param(inst, key, buf, sizeof(buf));
    return buf;
}

/* ---- tests -------------------------------------------------------------- */

static void test_defaults_pass_everything(void) {
    void *inst = fresh();
    CHECK(send3(inst, 0x90, 0, 100) == 1);
    CHECK(send3(inst, 0x90, 127, 100) == 1);
    CHECK(send3(inst, 0xB0, 64, 127) == 1);
    CHECK(send3(inst, 0xE0, 0, 64) == 1);
    CHECK(!strcmp(get(inst, "low_note"), "0"));
    CHECK(!strcmp(get(inst, "high_note"), "127"));
    CHECK(!strcmp(get(inst, "output"), "schwung only"));
    CHECK(!strcmp(get(inst, "transpose"), "0"));
    CHECK(!strcmp(get(inst, "move_channel"), "2"));
    CHECK(!strcmp(get(inst, "sustain"), "on"));
    CHECK(!strcmp(get(inst, "status"), "ok"));
    CHECK(n_injected == 0);
    api->destroy_instance(inst);
}

static void test_range_inclusive(void) {
    void *inst = fresh();
    api->set_param(inst, "low_note", "60");
    api->set_param(inst, "high_note", "72");
    CHECK(send3(inst, 0x90, 59, 100) == 0);
    CHECK(send3(inst, 0x90, 60, 100) == 1);
    CHECK(out[0][0] == 0x90 && out[0][1] == 60 && out[0][2] == 100);
    CHECK(send3(inst, 0x90, 72, 100) == 1);
    CHECK(send3(inst, 0x90, 73, 100) == 0);
    /* offs follow their ons */
    CHECK(send3(inst, 0x80, 59, 0) == 0);
    CHECK(send3(inst, 0x80, 60, 0) == 1);
    CHECK(send3(inst, 0x90, 72, 0) == 1);   /* vel-0 on is an off */
    CHECK(send3(inst, 0x80, 72, 0) == 0);   /* already released */
    api->destroy_instance(inst);
}

static void test_note_names(void) {
    void *inst = fresh();
    api->set_param(inst, "low_note", "C3");
    CHECK(!strcmp(get(inst, "low_note"), "60"));
    api->set_param(inst, "low_note", "C-2");
    CHECK(!strcmp(get(inst, "low_note"), "0"));
    api->set_param(inst, "low_note", "C#1");
    CHECK(!strcmp(get(inst, "low_note"), "37"));
    api->set_param(inst, "low_note", "Db1");
    CHECK(!strcmp(get(inst, "low_note"), "37"));
    api->set_param(inst, "high_note", "G8");
    CHECK(!strcmp(get(inst, "high_note"), "127"));
    api->set_param(inst, "high_note", "60.000");
    CHECK(!strcmp(get(inst, "high_note"), "60"));
    /* garbage and out of range are ignored */
    api->set_param(inst, "high_note", "H2");
    api->set_param(inst, "high_note", "128");
    api->set_param(inst, "high_note", "");
    CHECK(!strcmp(get(inst, "high_note"), "60"));
    api->destroy_instance(inst);
}

static void test_low_high_order(void) {
    void *inst = fresh();
    api->set_param(inst, "high_note", "50");
    api->set_param(inst, "low_note", "70");     /* low above high: high follows */
    CHECK(!strcmp(get(inst, "high_note"), "70"));
    api->set_param(inst, "high_note", "40");    /* high below low: low follows */
    CHECK(!strcmp(get(inst, "low_note"), "40"));
    api->destroy_instance(inst);
}

static void test_range_change_while_held(void) {
    void *inst = fresh();
    CHECK(send3(inst, 0x90, 60, 100) == 1);
    api->set_param(inst, "low_note", "80");
    CHECK(send3(inst, 0x80, 60, 0) == 1);       /* off still reaches the synth */
    CHECK(send3(inst, 0x90, 60, 100) == 0);     /* new on is filtered */
    api->set_param(inst, "low_note", "0");
    CHECK(send3(inst, 0x80, 60, 0) == 0);       /* its off too */
    api->destroy_instance(inst);
}

static void test_sustain(void) {
    void *inst = fresh();
    api->set_param(inst, "sustain", "off");
    CHECK(send3(inst, 0xB0, 64, 127) == 0);
    CHECK(send3(inst, 0xB0, 1, 50) == 1);       /* mod wheel */
    CHECK(send3(inst, 0xB0, 123, 0) == 1);      /* all notes off */
    CHECK(send3(inst, 0xE0, 0, 64) == 1);
    api->destroy_instance(inst);

    /* pedal down, then sustain off: exactly one release */
    inst = fresh();
    CHECK(send3(inst, 0xB3, 64, 127) == 1);
    api->set_param(inst, "sustain", "off");
    CHECK(tick(inst) == 1);
    CHECK(out[0][0] == 0xB3 && out[0][1] == 64 && out[0][2] == 0);
    CHECK(tick(inst) == 0);
    api->destroy_instance(inst);

    /* pedal up before switching: nothing owed */
    inst = fresh();
    send3(inst, 0xB0, 64, 127);
    send3(inst, 0xB0, 64, 0);
    api->set_param(inst, "sustain", "off");
    CHECK(tick(inst) == 0);
    api->destroy_instance(inst);
}

static void test_learn(void) {
    void *inst = fresh();
    api->set_param(inst, "learn_low", "Learn");
    CHECK(!strcmp(get(inst, "status"), "learn lo"));
    CHECK(send3(inst, 0xB0, 1, 10) == 1);       /* non-notes pass while armed */
    CHECK(send3(inst, 0x90, 48, 90) == 0);      /* learned and swallowed */
    CHECK(!strcmp(get(inst, "low_note"), "48"));
    CHECK(!strcmp(get(inst, "status"), "ok"));
    CHECK(send3(inst, 0x90, 50, 90) == 1);      /* next note plays */
    CHECK(send3(inst, 0x80, 48, 0) == 0);       /* learned key's off swallowed */
    CHECK(send3(inst, 0x80, 50, 0) == 1);

    api->set_param(inst, "learn_high", "1");    /* index wire fires too */
    CHECK(send3(inst, 0x90, 40, 90) == 0);      /* high below low: low follows */
    CHECK(!strcmp(get(inst, "high_note"), "40"));
    CHECK(!strcmp(get(inst, "low_note"), "40"));
    send3(inst, 0x80, 40, 0);

    /* firing the same trigger again cancels; the idle spelling does nothing */
    api->set_param(inst, "learn_high", "Learn");
    api->set_param(inst, "learn_high", "Learn");
    CHECK(!strcmp(get(inst, "status"), "ok"));
    api->set_param(inst, "learn_low", "-");
    api->set_param(inst, "learn_low", "0");
    CHECK(!strcmp(get(inst, "status"), "ok"));

    /* the other trigger switches */
    api->set_param(inst, "learn_low", "Learn");
    api->set_param(inst, "learn_high", "Learn");
    CHECK(!strcmp(get(inst, "status"), "learn hi"));

    /* timeout after ~10 s of ticks */
    for (int i = 0; i < 3500; i++) tick(inst);
    CHECK(!strcmp(get(inst, "status"), "ok"));
    CHECK(send3(inst, 0x90, 40, 90) == 1);
    api->destroy_instance(inst);
}

static void test_move_passthrough(void) {
    void *inst = fresh();
    api->set_param(inst, "low_note", "60");
    api->set_param(inst, "output", "move only");
    api->set_param(inst, "move_channel", "3");
    CHECK(!strcmp(get(inst, "status"), "ok"));

    CHECK(send3(inst, 0x90, 64, 99) == 0);
    CHECK(n_injected == 1 && injected_is(0, 0x29, 0x92, 64, 99));
    CHECK(send3(inst, 0x90, 50, 99) == 0);      /* out of range: nothing */
    CHECK(n_injected == 1);

    /* channel changes mid-hold: the off goes where the on went */
    api->set_param(inst, "move_channel", "5");
    CHECK(send3(inst, 0x80, 64, 0) == 0);
    CHECK(n_injected == 2 && injected_is(1, 0x28, 0x82, 64, 0));

    /* vel-0 on is an off */
    send3(inst, 0x90, 65, 80);
    CHECK(send3(inst, 0x90, 65, 0) == 0);
    CHECK(injected_is(3, 0x28, 0x84, 65, 0));

    /* pedal goes straight through, on the Move channel */
    n_injected = 0;
    CHECK(send3(inst, 0xB0, 64, 127) == 0);
    CHECK(injected_is(0, 0x2B, 0xB4, 64, 127));

    /* bend is thinned: a stream of 10 is at most COALESCE_PER_TICK per tick */
    n_injected = 0;
    for (int i = 0; i < 10; i++) send3(inst, 0xE0, 0, (uint8_t)(64 + i));
    CHECK(n_injected == 0);
    tick(inst);
    CHECK(n_injected == 1 && injected_is(0, 0x2E, 0xE4, 0, 73));
    tick(inst);
    CHECK(n_injected == 1);

    /* channel mode is never thinned */
    n_injected = 0;
    send3(inst, 0xB0, 123, 0);
    CHECK(n_injected == 1 && injected_is(0, 0x2B, 0xB4, 123, 0));

    /* clock passes to the chain, never to Move */
    n_injected = 0;
    uint8_t clk = 0xF8;
    CHECK(api->process_midi(inst, &clk, 1, out, out_lens, MIDI_FX_MAX_OUT_MSGS) == 1);
    CHECK(n_injected == 0);
    api->destroy_instance(inst);
}

static void test_move_loop_guard(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "move only");
    api->set_param(inst, "move_channel", "1");  /* = slot Recv Ch */
    CHECK(!strcmp(get(inst, "status"), "ch clash"));
    CHECK(send3(inst, 0x90, 60, 100) == 0);
    CHECK(n_injected == 0);
    fake_recv_channel = -1;                     /* Recv Ch = All */
    api->set_param(inst, "move_channel", "4");
    CHECK(!strcmp(get(inst, "status"), "ch clash"));
    CHECK(send3(inst, 0x90, 60, 100) == 0);
    CHECK(n_injected == 0);
    fake_recv_channel = 0;
    CHECK(!strcmp(get(inst, "status"), "ok"));
    api->destroy_instance(inst);
}

static void test_move_no_inject_pointer(void) {
    host.midi_inject_to_move = NULL;
    api = move_midi_fx_init(&host);
    void *inst = fresh();
    api->set_param(inst, "output", "move only");
    CHECK(!strcmp(get(inst, "status"), "no move"));
    CHECK(send3(inst, 0x90, 60, 100) == 0);
    CHECK(send3(inst, 0x80, 60, 0) == 0);
    tick(inst);
    api->destroy_instance(inst);
    host.midi_inject_to_move = fake_inject;
    api = move_midi_fx_init(&host);
}

static void test_move_refused_off_is_retried(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "move only");
    send3(inst, 0x90, 60, 100);
    inject_refuse = 1;
    send3(inst, 0x80, 60, 0);
    CHECK(n_injected == 1);
    tick(inst);
    CHECK(n_injected == 1);
    inject_refuse = 0;
    tick(inst);
    CHECK(n_injected == 2 && injected_is(1, 0x28, 0x81, 60, 0));
    tick(inst);
    CHECK(n_injected == 2);                     /* sent once */
    api->destroy_instance(inst);
}

static void test_move_off_releases_held(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "move only");
    for (int k = 60; k < 72; k++) send3(inst, 0x90, (uint8_t)k, 100);
    send3(inst, 0xB0, 64, 127);
    CHECK(n_injected == 13);
    api->set_param(inst, "output", "schwung only");
    n_injected = 0;
    tick(inst);
    tick(inst);
    tick(inst);
    int offs = 0, pedal = 0;
    for (int i = 0; i < n_injected; i++) {
        if (injected[i][0] == 0x28 && injected[i][1] == 0x81) offs++;
        if (injected[i][0] == 0x2B && injected[i][1] == 0xB1 && injected[i][2] == 64 && injected[i][3] == 0) pedal++;
    }
    CHECK(offs == 12);
    CHECK(pedal == 1);
    /* later offs of those keys go nowhere (and do not reach the synth) */
    n_injected = 0;
    CHECK(send3(inst, 0x80, 60, 0) == 0);
    CHECK(n_injected == 0);
    /* new notes now go to the chain */
    CHECK(send3(inst, 0x90, 60, 100) == 1);
    api->destroy_instance(inst);
}

static void test_toggle_to_move_keeps_chain_offs(void) {
    void *inst = fresh();
    CHECK(send3(inst, 0x90, 60, 100) == 1);
    api->set_param(inst, "output", "move only");
    CHECK(send3(inst, 0x80, 60, 0) == 1);       /* chain note released in chain */
    CHECK(n_injected == 0);
    api->destroy_instance(inst);
}

static void test_state_roundtrip(void) {
    void *inst = fresh();
    api->set_param(inst, "low_note", "36");
    api->set_param(inst, "high_note", "59");
    api->set_param(inst, "output", "both");
    api->set_param(inst, "transpose", "-2");
    api->set_param(inst, "move_channel", "7");
    api->set_param(inst, "sustain", "off");
    char state[256];
    strcpy(state, get(inst, "state"));
    api->destroy_instance(inst);

    inst = fresh();
    api->set_param(inst, "high_note", "10");    /* would clamp a naive restore */
    api->set_param(inst, "state", state);
    CHECK(!strcmp(get(inst, "low_note"), "36"));
    CHECK(!strcmp(get(inst, "high_note"), "59"));
    CHECK(!strcmp(get(inst, "output"), "both"));
    CHECK(!strcmp(get(inst, "transpose"), "-2"));
    CHECK(!strcmp(get(inst, "move_channel"), "7"));
    CHECK(!strcmp(get(inst, "sustain"), "off"));

    /* partial state leaves the rest alone; unknown keys ignored */
    api->set_param(inst, "state", "{\"sustain\":\"on\",\"bogus\":1}");
    CHECK(!strcmp(get(inst, "sustain"), "on"));
    CHECK(!strcmp(get(inst, "low_note"), "36"));

    /* state saved before Output existed */
    api->set_param(inst, "state", "{\"move_pass\":\"off\"}");
    CHECK(!strcmp(get(inst, "output"), "schwung only"));
    api->set_param(inst, "state", "{\"move_pass\":\"on\"}");
    CHECK(!strcmp(get(inst, "output"), "move only"));
    api->destroy_instance(inst);
}

static void test_output_values(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "1");        /* index wire */
    CHECK(!strcmp(get(inst, "output"), "move only"));
    api->set_param(inst, "output", "2");
    CHECK(!strcmp(get(inst, "output"), "both"));
    api->set_param(inst, "output", "0");
    CHECK(!strcmp(get(inst, "output"), "schwung only"));
    api->set_param(inst, "output", "3");
    CHECK(!strcmp(get(inst, "output"), "schwung only"));
    api->destroy_instance(inst);
}

static void test_both(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "both");
    api->set_param(inst, "move_channel", "3");

    CHECK(send3(inst, 0x90, 64, 99) == 1);
    CHECK(out[0][0] == 0x90 && out[0][1] == 64 && out[0][2] == 99);
    CHECK(n_injected == 1 && injected_is(0, 0x29, 0x92, 64, 99));

    CHECK(send3(inst, 0xA0, 64, 30) == 1);      /* poly aftertouch follows to both */
    CHECK(n_injected == 2 && injected_is(1, 0x2A, 0xA2, 64, 30));

    CHECK(send3(inst, 0x80, 64, 0) == 1);
    CHECK(out[0][0] == 0x80 && out[0][1] == 64);
    CHECK(n_injected == 3 && injected_is(2, 0x28, 0x82, 64, 0));

    /* pedal reaches both; one release each when Sustain goes off */
    n_injected = 0;
    CHECK(send3(inst, 0xB0, 64, 127) == 1);
    CHECK(n_injected == 1 && injected_is(0, 0x2B, 0xB2, 64, 127));
    api->set_param(inst, "sustain", "off");
    CHECK(tick(inst) == 1);
    CHECK(out[0][0] == 0xB0 && out[0][1] == 64 && out[0][2] == 0);
    CHECK(n_injected == 2 && injected_is(1, 0x2B, 0xB2, 64, 0));
    CHECK(tick(inst) == 0);

    /* a controller reaches the chain now and Move on the next tick */
    n_injected = 0;
    CHECK(send3(inst, 0xE0, 0, 70) == 1);
    CHECK(n_injected == 0);
    tick(inst);
    CHECK(n_injected == 1 && injected_is(0, 0x2E, 0xE2, 0, 70));
    api->destroy_instance(inst);
}

static void test_both_channel_clash_still_plays_chain(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "both");
    api->set_param(inst, "move_channel", "1");  /* = slot Recv Ch */
    CHECK(!strcmp(get(inst, "status"), "ch clash"));
    CHECK(send3(inst, 0x90, 60, 100) == 1);
    CHECK(n_injected == 0);
    CHECK(send3(inst, 0x80, 60, 0) == 1);
    api->destroy_instance(inst);
}

static void test_both_to_schwung_releases_move_only(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "both");
    send3(inst, 0x90, 60, 100);
    n_injected = 0;
    api->set_param(inst, "output", "schwung only");
    tick(inst);
    CHECK(n_injected == 1 && injected_is(0, 0x28, 0x81, 60, 0));
    CHECK(send3(inst, 0x80, 60, 0) == 1);       /* the synth note is still released */
    CHECK(n_injected == 1);
    api->destroy_instance(inst);
}

static void test_transpose(void) {
    void *inst = fresh();
    api->set_param(inst, "transpose", "1");
    CHECK(send3(inst, 0x91, 60, 100) == 1);
    CHECK(out[0][0] == 0x91 && out[0][1] == 72 && out[0][2] == 100);

    /* the off carries the note that was sounded, even after a change */
    api->set_param(inst, "transpose", "-1");
    CHECK(send3(inst, 0xA1, 60, 20) == 1);
    CHECK(out[0][0] == 0xA1 && out[0][1] == 72 && out[0][2] == 20);
    CHECK(send3(inst, 0x81, 60, 0) == 1);
    CHECK(out[0][0] == 0x81 && out[0][1] == 72);
    CHECK(send3(inst, 0x91, 60, 100) == 1);
    CHECK(out[0][1] == 48);
    CHECK(send3(inst, 0x91, 60, 0) == 1);       /* vel-0 on is an off */
    CHECK(out[0][1] == 48);

    /* off the keyboard: dropped, with no stray off */
    api->set_param(inst, "transpose", "10");
    CHECK(send3(inst, 0x90, 20, 100) == 0);
    CHECK(send3(inst, 0x80, 20, 0) == 0);
    api->set_param(inst, "transpose", "-10");
    CHECK(send3(inst, 0x90, 100, 100) == 0);

    /* the range filters the key pressed, not the sounded note */
    api->set_param(inst, "transpose", "1");
    api->set_param(inst, "low_note", "60");
    api->set_param(inst, "high_note", "60");
    CHECK(send3(inst, 0x90, 60, 100) == 1 && out[0][1] == 72);
    CHECK(send3(inst, 0x90, 72, 100) == 0);

    /* out of range or garbage is ignored */
    api->set_param(inst, "transpose", "11");
    api->set_param(inst, "transpose", "x");
    CHECK(!strcmp(get(inst, "transpose"), "1"));
    api->destroy_instance(inst);

    /* Move gets the transposed note too, and its off matches */
    inst = fresh();
    api->set_param(inst, "output", "both");
    api->set_param(inst, "transpose", "-1");
    send3(inst, 0x90, 60, 100);
    CHECK(injected_is(0, 0x29, 0x91, 48, 100));
    api->set_param(inst, "transpose", "2");
    send3(inst, 0x80, 60, 0);
    CHECK(injected_is(1, 0x28, 0x81, 48, 0));
    api->destroy_instance(inst);
}

static void test_bad_values_ignored(void) {
    void *inst = fresh();
    api->set_param(inst, "output", "maybe");
    api->set_param(inst, "move_channel", "0");
    api->set_param(inst, "move_channel", "17");
    api->set_param(inst, "sustain", "");
    CHECK(!strcmp(get(inst, "output"), "schwung only"));
    CHECK(!strcmp(get(inst, "move_channel"), "2"));
    CHECK(!strcmp(get(inst, "sustain"), "on"));
    char buf[8];
    CHECK(api->get_param(inst, "nope", buf, sizeof(buf)) == -1);
    api->destroy_instance(inst);
}

int main(void) {
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.mod_host_ctx = &test_host_ctx;
    host.slot_recv_channel = fake_slot_recv_channel;
    host.midi_inject_to_move = fake_inject;
    api = move_midi_fx_init(&host);
    if (!api) { fprintf(stderr, "init failed\n"); return 1; }

    test_defaults_pass_everything();
    test_range_inclusive();
    test_note_names();
    test_low_high_order();
    test_range_change_while_held();
    test_sustain();
    test_learn();
    test_move_passthrough();
    test_move_loop_guard();
    test_move_no_inject_pointer();
    test_move_refused_off_is_retried();
    test_move_off_releases_held();
    test_toggle_to_move_keeps_chain_offs();
    test_state_roundtrip();
    test_output_values();
    test_both();
    test_both_channel_clash_still_plays_chain();
    test_both_to_schwung_releases_move_only();
    test_transpose();
    test_bad_values_ignored();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: midifilter\n");
    return 0;
}

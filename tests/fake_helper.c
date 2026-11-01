#include "fake_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fake_helper_misuse(const char* what, uint32_t v) {
    fprintf(stderr, "fake_helper: %s (%u)\n", what, (unsigned)v);
    abort();
}

void fake_helper_init(FakeHelper* f, FakeGba* gba) {
    if(gba != fake_gba_current()) fake_helper_misuse("the FakeGba isn't the current one", 0);
    memset(f, 0, sizeof(*f));
    f->gba = gba;
    hp_init(&f->hp);
    f->wire = MlinkWireMulti;
    f->send = HP_READY;
}

void fake_helper_set_wire(FakeHelper* f, MlinkWire wire) {
    f->wire = wire;
    /* The boot mode the BIOS left at header 0xC4 (GBATEK "Multiboot Header"), as HELLO reports it */
    f->gba->own[0xC4] = wire == MlinkWireNormal ? 2u : 3u;
}

/* Transfer n never gets to the helper: the cable is out, or the GBA is off. */
static bool fake_helper_lost(const FakeHelper* f, uint32_t n) {
    return f->dead || (n >= f->unplug_from && n - f->unplug_from < f->unplug_len);
}

/* Normal wire: the slave isn't ready for slot n at time `at`: the cable is out, the GBA is off, or
 * a command still runs. */
static bool fake_helper_notready(const FakeHelper* f, uint32_t n, uint64_t at) {
    return fake_helper_lost(f, n) || (f->busy && at < f->busy_end_us);
}

/* The helper's side of one transfer that reaches it, at time `at`: its reply. On the Normal wire
 * none reaches it while a command runs. */
static uint16_t fake_helper_xfer(FakeHelper* f, uint16_t in, uint64_t at) {
    if(f->busy && f->wire == MlinkWireNormal) {
        /* The command is done. The response's first word went in when it ended, and the helper
         * has waited for a transfer since, as for any word: this one, unless it timed out. */
        f->busy = false;
        f->send = f->first;
        f->heard_us = f->busy_end_us;
    }
    if(f->busy) {
        /* The Flipper's words aren't read. The first transfer once the command's time is over is
         * the fresh one the helper waits for: it still answers BUSY, then the response is loaded. */
        if(at >= f->busy_end_us) {
            f->busy = false;
            f->send = f->first;
            f->heard_us = at;
        }
        return HP_BUSY;
    }
    if(at - f->heard_us >= FAKE_HELPER_RX_TIMEOUT_US) {
        f->send = hp_step(&f->hp, 0, HP_STEP_TIMEOUT);
    }
    f->heard_us = at;
    const uint16_t reply = f->send;
    f->send = hp_step(&f->hp, in, 0);
    if(hp_exec_pending(&f->hp)) {
        fake_gba_take_busy_us(f->gba);
        f->first = hp_execute(&f->hp);
        f->busy_end_us = at + fake_gba_take_busy_us(f->gba);
        f->busy = true;
        f->send = HP_BUSY;
        if(f->on_command) {
            f->on_command(f->on_command_ctx, f->hp.code, f->hp.rx_n ? f->hp.req[0] : 0);
        }
        if(f->gba->frozen) f->dead = true;
    }
    return reply;
}

void fake_helper_frame(FakeHelper* f, const MlinkTx* tx, MlinkRx* rx, uint32_t elapsed_us) {
    if(tx->count > MLINK_BURST_LEN) {
        fake_helper_misuse("more than MLINK_BURST_LEN words in a frame", tx->count);
    }
    memset(rx, 0, sizeof(*rx));
    rx->count = tx->count;
    const bool normal = f->wire == MlinkWireNormal;
    uint32_t ticks = 0; /* the frame's time so far: where slot t starts */
    for(uint8_t t = 0; t < tx->count; t++) {
        const uint32_t n = f->transfers++;
        const uint16_t bit = (uint16_t)(1u << t);
        const uint64_t at = f->now_us + (uint64_t)ticks * MLINK_TICK_US;
        if(f->gba->frozen) f->dead = true;
        if(normal && fake_helper_notready(f, n, at)) {
            /* The driver waited its ticks for the slave and clocked nothing */
            rx->word[t] = MLINK_NO_DATA;
            rx->timeout_mask |= bit;
            rx->notready_mask |= bit;
            ticks += MLINK_NORMAL_STALL_TICKS;
            continue;
        }
        ticks++;
        if(fake_helper_lost(f, n)) {
            rx->word[t] = MLINK_NO_DATA;
            rx->timeout_mask |= bit;
            continue;
        }
        uint16_t reply = fake_helper_xfer(f, tx->word[t], at);
        if(f->corrupt && f->corrupt_at == n) {
            reply ^= 0x0001u;
            f->corrupt = false;
        }
        rx->word[t] = reply;
    }
    if(!ticks) ticks = 1; /* an idle tick */
    rx->ticks = (uint16_t)ticks;
    const uint64_t took = (uint64_t)ticks * MLINK_TICK_US;
    f->now_us += normal && took > elapsed_us ? took : elapsed_us;
}

void fake_helper_unplug_at(FakeHelper* f, uint32_t transfer, uint32_t len) {
    f->unplug_from = transfer;
    f->unplug_len = len;
}

void fake_helper_replug(FakeHelper* f) {
    f->unplug_len = 0;
}

void fake_helper_corrupt_at(FakeHelper* f, uint32_t transfer) {
    f->corrupt = true;
    f->corrupt_at = transfer;
}

void fake_helper_restart(FakeHelper* f) {
    fake_gba_power_cycle(f->gba);
    hp_init(&f->hp);
    f->send = HP_READY;
    f->busy = false;
    f->dead = false;
    f->heard_us = f->now_us;
}

uint32_t fake_helper_transfers(const FakeHelper* f) {
    return f->transfers;
}

void fake_helper_on_command(
    FakeHelper* f,
    void (*cb)(void* ctx, uint8_t code, uint16_t arg0),
    void* ctx) {
    f->on_command = cb;
    f->on_command_ctx = ctx;
}

/* ---- The transport ---- */

static bool fake_helper_settled(Gen3HlinkState s) {
    return s == Gen3HlinkIdle || s == Gen3HlinkDone || s == Gen3HlinkFailed;
}

/* Driver frames until the pump is done with what the app gave it. */
static void fake_helper_run(FakeHelper* f) {
    if(!f->pump) fake_helper_misuse("no transport (fake_helper_init ends it)", 0);
    while(!fake_helper_settled(gen3_hlink_state(f->pump))) {
        if(f->pump_transfers >= FAKE_HELPER_MAX_TRANSFERS) {
            fake_helper_misuse("transport: gave up after this many transfers", f->pump_transfers);
        }
        f->pump_transfers += f->pending.count ? f->pending.count : 1u;
        MlinkRx rx;
        /* At least a tick per slot; on the Normal wire, not-ready slots take longer */
        fake_helper_frame(f, &f->pending, &rx, mlink_frame_us(MlinkModeStream, f->pending.count));
        gen3_hlink_frame(f->pump, &rx, &f->pending, (uint32_t)rx.ticks * MLINK_TICK_US);
    }
}

static Gen3HlinkFail fake_helper_exchange(
    void* ctx,
    const Gen3HlinkReq* req,
    uint8_t* status,
    uint16_t* resp_words) {
    FakeHelper* f = ctx;
    gen3_hlink_submit(f->pump, req);
    fake_helper_run(f);
    Gen3HlinkFail fail;
    gen3_hlink_result(f->pump, status, resp_words, &fail);
    return fail;
}

static bool fake_helper_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    FakeHelper* f = ctx;
    gen3_hlink_sync(f->pump, need, timeout_us);
    fake_helper_run(f);
    return gen3_hlink_state(f->pump) == Gen3HlinkDone;
}

Gen3HTransport fake_helper_transport(FakeHelper* f, Gen3Hlink* pump) {
    gen3_hlink_init(pump);
    f->pump = pump;
    memset(&f->pending, 0, sizeof(f->pending));
    f->pump_transfers = 0;
    return (Gen3HTransport){
        .ctx = f,
        .exchange = fake_helper_exchange,
        .sync = fake_helper_sync,
    };
}

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <hp_core.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/mlink_types.h>
#include "fake_gba.h"

/* Test-only: the GBA helper as the Flipper's link partner. It wraps the helper's real portable core
 * (gba/helper/src) on a FakeGba, word by word, as gba/helper/gba/main.c runs it
 * (multiboot-and-save.md §4.8, §6.1), and gives the Flipper's real packet pump a transport over
 * it. It never calls the Flipper's save or cart code. It is on one of two wires
 * (fake_helper_set_wire).
 *
 * MultiPlay, the default: the helper as multiplayer child 1. Per transfer that reaches it, the
 * helper answers with the word it loaded after the previous one (the one-transfer delay), then
 * takes the Flipper's word with hp_step. Once a packet is complete it runs hp_execute at once, so
 * the flash changes then, and answers HP_BUSY, ignoring the Flipper's words, until the time the
 * FakeGba models for the command (fake_gba_take_busy_us) has passed, plus one more transfer: the
 * helper waits for a fresh transfer before it loads the response. Then the response goes out word
 * by word. The command runs whether or not the Flipper clocks meanwhile: time ends BUSY, not
 * transfers. A transfer that doesn't reach the helper gets no reply (its timeout bit,
 * MLINK_NO_DATA).
 *
 * Normal: the helper as the slave of 32-bit Normal-mode transfers, behind the Flipper's
 * Normal-wire driver (plan 6 D3, D5). Each slot the Flipper sends is either delivered, a word as
 * on MultiPlay (the one-transfer delay, hp_step), or not ready: nothing is clocked, the helper
 * isn't stepped, and the driver reports the slot's timeout and not-ready bits with MLINK_NO_DATA.
 * There is no HP_BUSY: the command runs right after the packet's last word, and until its modelled
 * time has passed every slot is not ready. The first slot delivered after that carries the
 * response's SOF, if it comes within FAKE_HELPER_RX_TIMEOUT_US of the command's end; later, the
 * helper has dropped the response, as for any word it waits for. With the cable out, or the helper
 * dead, every slot is not ready too.
 *
 * Time comes from fake_helper_frame's elapsed_us. On MultiPlay the transfers of a frame are
 * MLINK_TICK_US apart. On the Normal wire each slot starts when the one before it ends: a
 * delivered slot takes MLINK_TICK_US, a not-ready one MLINK_NORMAL_STALL_TICKS of them. After
 * FAKE_HELPER_RX_TIMEOUT_US with no transfer reaching it, outside a command, the helper steps with
 * HP_STEP_TIMEOUT: a partial packet, or a response not yet out, is dropped.
 *
 * A power cut (fake_gba_power_cut_after) that freezes the chip ends the helper: from then on no
 * transfer reaches it, until fake_helper_restart.
 *
 * Never a stack local in a test (it holds an Hp): static or heap. */

/* The helper's receive timeout: 196 units of 1.0224 ms (multiboot-and-save.md §6.5), taken as
 * 200 ms, a little sooner than the GBA's */
#define FAKE_HELPER_RX_TIMEOUT_US 200000u
/* fake_helper_transport aborts the test once it has run this many transfers (an idle tick counts as
 * one, and so does a not-ready slot): a test that hangs fails in seconds. A 1 MiB ROM read is
 * about 530,000. */
#define FAKE_HELPER_MAX_TRANSFERS 2000000u

typedef struct FakeHelper FakeHelper;

struct FakeHelper {
    FakeGba* gba;
    Hp hp;
    MlinkWire wire;
    uint16_t send; /* the word the helper loaded for the next transfer that reaches it */
    /* a command runs (MultiPlay: HP_BUSY, or the transfer after it the helper waits for; Normal:
     * not ready) */
    bool busy;
    uint64_t busy_end_us; /* when the command's modelled time is over */
    uint16_t first; /* hp_execute's word: the response's first, loaded once the command is done */
    bool dead; /* the power cut came: nothing replies */
    uint64_t now_us; /* the start of the next frame */
    uint64_t heard_us; /* when the helper last heard a transfer, or timed out waiting */
    uint32_t transfers; /* the Flipper's transfers so far, those that never arrived or weren't
                         * ready included */
    uint32_t unplug_from;
    uint32_t unplug_len;
    bool corrupt;
    uint32_t corrupt_at;
    void (*on_command)(void* ctx, uint8_t code, uint16_t arg0);
    void* on_command_ctx;
    /* fake_helper_transport's */
    Gen3Hlink* pump;
    MlinkTx pending; /* the pump's tx for the next frame */
    uint32_t pump_transfers;
};

/* The helper just started on gba, which must be fake_gba_current(): hp_init, HP_READY loaded, time
 * 0, the MultiPlay wire, no unplug, no corruption, no hook and no transport. */
void fake_helper_init(FakeHelper* f, FakeGba* gba);
/* The wire the Flipper's driver clocks, from the next frame on, and the boot mode the BIOS left
 * at the helper's header 0xC4, which HELLO reports: 2 on the Normal wire, 3 on MultiPlay (as
 * fake_gba_init sets it). */
void fake_helper_set_wire(FakeHelper* f, MlinkWire wire);
/* One driver frame: tx is the Flipper's (count 0..MLINK_BURST_LEN; 0 is an idle tick), rx gets the
 * replies as the driver reports them, and rx->ticks the frame's time in ticks: max(count, 1) on
 * MultiPlay; on the Normal wire 1 per delivered slot, MLINK_NORMAL_STALL_TICKS per not-ready one,
 * and 1 for an idle tick. The fake's clock then moves on by elapsed_us, on the Normal wire by at
 * least rx->ticks ticks. */
void fake_helper_frame(FakeHelper* f, const MlinkTx* tx, MlinkRx* rx, uint32_t elapsed_us);
/* A pulled cable: from the Flipper's transfer number `transfer` (counted by this fake from 0, every
 * slot the Flipper sends, delivered or not, so fake_helper_transfers is the next one's), `len`
 * transfers never reach the helper. The Flipper gets no reply (its timeout bit, MLINK_NO_DATA; on
 * the Normal wire the slot is not ready), and the helper isn't stepped; time still passes. */
void fake_helper_unplug_at(FakeHelper* f, uint32_t transfer, uint32_t len);
/* Ends any unplug window now. */
void fake_helper_replug(FakeHelper* f);
/* XORs 0x0001 into the reply to that transfer (one at a time). */
void fake_helper_corrupt_at(FakeHelper* f, uint32_t transfer);
/* The GBA was power-cycled and the helper sent again: fake_gba_power_cycle (the flash array is
 * kept), hp_init (the cart and probe caches, the arming, the last packet are gone), HP_READY loaded,
 * and replying again after a power cut. The unplug window, the corruption, the hook and the wire
 * stay. */
void fake_helper_restart(FakeHelper* f);
uint32_t fake_helper_transfers(const FakeHelper* f);
/* Called after every hp_execute, a duplicate answered from memory included: the command code and
 * its first argument word (0 for a command without arguments). It may call the fake_helper_* and
 * fake_gba_* functions. */
void fake_helper_on_command(
    FakeHelper* f,
    void (*cb)(void* ctx, uint8_t code, uint16_t arg0),
    void* ctx);
/* A transport that runs pump against this fake in stream mode, frame after frame, until the pump is
 * Done or Failed, as the driver's frame callback would, each frame's time rx.ticks * MLINK_TICK_US
 * (on MultiPlay, mlink_frame_us). It initialises pump; a later fake_helper_init ends it. Aborts
 * after FAKE_HELPER_MAX_TRANSFERS. */
Gen3HTransport fake_helper_transport(FakeHelper* f, Gen3Hlink* pump);

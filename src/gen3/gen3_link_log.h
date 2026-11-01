#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <src/gen3/mlink_types.h>

/* ISR-safe frame log: one producer (link frame callback, interrupt context) and one consumer
 * (logging thread). No locks, no allocation. */

#define GEN3_LOG_RING_LEN 64

typedef struct {
    uint32_t frame, idle_before;
    uint8_t phase, count;
    uint16_t timeout_mask, framing_mask;
    uint16_t tx[MLINK_BURST_LEN], rx[MLINK_BURST_LEN];
} Gen3LogRec;

struct Gen3LogRing {
    Gen3LogRec rec[GEN3_LOG_RING_LEN];
    volatile uint32_t head; /* written by producer only; counts records ever stored */
    volatile uint32_t tail; /* written by consumer only; counts records ever popped */
    volatile uint32_t dropped; /* producer only */
    /* Producer-private state. */
    uint32_t idle_pending;
    uint8_t last_phase;
    bool have_phase;
    /* Consumer-private state. */
    bool failed;
};

typedef struct Gen3LogRing Gen3LogRing;

void gen3_log_init(Gen3LogRing* ring);

/* Producer. Idle frames are counted, not stored. A frame is idle when the phase is unchanged AND
 * either (count==1 and rx.word[0]==0xFFFF) or (count==9, tx.word[1..8] and rx.word[1..8] all 0,
 * masks 0). A stored record carries the idle frames before it. Full ring -> dropped++. */
void gen3_log_frame(
    Gen3LogRing* ring,
    uint32_t frame,
    uint8_t phase,
    const MlinkTx* tx,
    const MlinkRx* rx);

/* Consumer. */
bool gen3_log_pop(Gen3LogRing* ring, Gen3LogRec* out);
uint32_t gen3_log_dropped(const Gen3LogRing* ring);

/* Formats one line (with trailing newline). Returns the length written (excluding NUL), or 0 if
 * it did not fit. */
size_t gen3_log_format(const Gen3LogRec* rec, char* buf, size_t len);

typedef bool (*Gen3LogWriter)(void* ctx, const char* line, size_t len);

/* Pops everything; formats and writes each record. After the first writer failure, keeps
 * discarding without calling the writer, and gen3_log_failed() returns true. */
void gen3_log_drain(Gen3LogRing* ring, Gen3LogWriter writer, void* ctx);
bool gen3_log_failed(const Gen3LogRing* ring);

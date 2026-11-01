#pragma once

#include <stdint.h>

/* Driver contract between the Game Link hardware driver and the link layer.
 * The MlinkRx passed to the protocol is the result of the MlinkTx the protocol
 * produced in the previous frame. */

#define MLINK_BURST_LEN 9
#define MLINK_NO_DATA   0xFFFFu

typedef struct {
    uint8_t count; /* frame mode 1..9; stream mode 0..9, 0 = idle tick (no transfer) */
    uint16_t word[MLINK_BURST_LEN];
    /* Normal wire: the upper half of each 32-bit word, zeroed by the driver before every frame
     * callback on the Normal wire, so only a producer that wants one sets it. The MultiPlay wire
     * neither zeroes nor reads it. */
    uint16_t hi[MLINK_BURST_LEN];
} MlinkTx;

typedef struct {
    uint8_t count; /* == count of the MlinkTx that produced it */
    uint16_t word[MLINK_BURST_LEN]; /* GBA's word per transfer, MLINK_NO_DATA if no answer */
    uint16_t timeout_mask; /* bit t: no start bit seen in transfer t */
    uint16_t framing_mask; /* bit t: stop bit low in transfer t */
    /* Normal wire: bit t set when the slave never got ready and nothing was clocked (word t is
     * MLINK_NO_DATA and timeout_mask has the bit too). Always 0 on the MultiPlay wire. */
    uint16_t notready_mask;
    /* Stream mode: the frame's time in ticks (MLINK_TICK_US each). On the MultiPlay wire
     * max(count, 1), a frame the cable guard skips included. */
    uint16_t ticks;
} MlinkRx;

/* Which wire the driver clocks: the 16-bit MultiPlay words, or 32-bit Normal-mode words (stream
 * mode only; a word's reply is its upper half). */
typedef enum { MlinkWireMulti, MlinkWireNormal } MlinkWire;

/* Normal wire: ticks a slot waits for the slave to get ready before it is reported not ready. */
#define MLINK_NORMAL_STALL_TICKS 16u

/* Frame: frames start at the GBA frame rate (the Cable Club link). Stream: a frame starts on the
 * tick after the previous frame's callback, and count 0 is an idle tick (multiboot and the helper
 * protocol). */
typedef enum { MlinkModeFrame, MlinkModeStream } MlinkMode;

/* One tick, start to start (mlink_hw_furi.c MLINK_TICK_DECI_US / 10). */
#define MLINK_TICK_US 1073u
/* Frame mode: the mean period, frame start to frame start; each real frame is 15 or 16 ticks. */
#define MLINK_FRAME_US 16740u

/* Time one frame takes, for protocol timeouts. Stream: max(count, 1) ticks.
 * Frame: MLINK_FRAME_US. One exception in stream mode: a frame the cable guard skips lasts one
 * tick but reports count timed-out words, so with the cable out, timeouts measured this way run
 * up to MLINK_BURST_LEN times fast. They only ever fail early. */
static inline uint32_t mlink_frame_us(MlinkMode mode, uint8_t count) {
    if(mode == MlinkModeFrame) return MLINK_FRAME_US;
    return (count ? count : 1u) * MLINK_TICK_US;
}

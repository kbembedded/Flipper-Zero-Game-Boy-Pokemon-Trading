#pragma once

#include <stdbool.h>
#include <furi_hal_gpio.h>
#include <src/gen3/mlink_types.h>

/* GBA multiplayer parent (SIO master) over the Game Link GPIO module.
 *
 * Every TIM2 tick (1073 us start to start, leaving the GBA's ~751.5 us between transfers) may run
 * one 16-bit multiplayer transfer, bit-banged at 115200 baud.
 * Transfers come in frames: transfer t of a frame runs on tick t of that frame, t < tx.count.
 * After a frame's last transfer the frame callback runs, in the TIM2 interrupt with interrupts
 * enabled. It gets that frame's result and fills the next frame's tx. The mode (MlinkMode) sets
 * when frames start:
 * - frame mode (mlink_hw_start; the Cable Club link): at the GBA frame rate, every 15 or 16
 *   ticks, with idle ticks between frames; count 1..MLINK_BURST_LEN;
 * - stream mode (multiboot and the helper protocol, multiboot-and-save.md §6.9): on the tick
 *   after the previous frame's callback, with no frame-rate gate; count 0..MLINK_BURST_LEN. A
 *   count-0 frame runs no transfer: its callback runs on its start tick.
 *
 * Cable guard: in frame mode SI is read on every idle tick between frames (one that runs no
 * transfer) and latched if high; in both modes it is read at each frame start, before any line
 * moves. High means the cable is out or reversed (a reversed cable can read low until the GBA
 * opens its link, so a check at start is not enough; a GBA master's SO, on our SI, dips low
 * during its own transfers, so one read per frame can miss it). The idle tick that first latches
 * it releases SC and SO to input with pull-up at once (SD already is, between transfers). If SI
 * is high at the frame start or was latched since the previous one, the frame runs no transfer at
 * all: SC and SO are released (again), and the callback gets the frame at once, every word
 * MLINK_NO_DATA and every timeout_mask bit set, as if the GBA never answered. The next frame that
 * runs transfers first makes SC and SO push-pull outputs again, driven high.
 *
 * The wire (MlinkWire, mlink_hw_start_wire) sets what a transfer is. Everything above is the
 * MultiPlay wire, the default. The Normal wire (GBA Normal mode, over a GBC cable; stream mode
 * only) clocks one 32-bit word per transfer on SC at 256 kHz, MSB first: (tx.hi[t] << 16) |
 * tx.word[t] out, and the reply's upper half in as rx.word[t]. SD isn't used. Gated (the default,
 * mlink_hw_set_normal_gate), a slot runs only once the slave is ready (SI low): while SI reads
 * high it waits, one tick at a time, for up to MLINK_NORMAL_STALL_TICKS ticks, then it is reported
 * not ready (rx.notready_mask and rx.timeout_mask bit t, word MLINK_NO_DATA, nothing clocked) and
 * the frame goes on with the next slot. Ungated, every slot clocks on its tick with no SI check
 * (a slave that isn't there reads 0xFFFF) and is never not ready. rx.ticks counts the frame's
 * ticks: 1 per delivered slot plus each tick it waited, 1 for a count-0 frame. The driver zeroes
 * next.hi before every Normal-wire frame callback. The Normal wire has no cable guard.
 *
 * alloc/free/check_cable/start/stop/set_normal_gate are app-context calls; use them from one
 * thread, never from the frame callback. mlink_hw_cable_ok may be called from any thread. */

typedef struct {
    /* SC: frame clock, driven by the parent: push-pull output, idle high. SC and SO keep their
     * pull-up on and are released to input with pull-up while the cable guard trips. Normal
     * wire: the bit clock, push-pull, idle high, never released. */
    const GpioPin* sc;
    /* SD: shared data line, pulled up. Pushed and pulled only for the Flipper's own word, then
     * released to input with pull-up (MLINK_SD_PUSH_PULL 1, the default); or open drain the whole
     * time (MLINK_SD_PUSH_PULL 0). Normal wire: not used, left analog. */
    const GpioPin* sd;
    /* SO: goes to the GBA's SI; push-pull output, idle high, released as SC. Normal wire: the
     * Flipper's data bits, push-pull, idle high. */
    const GpioPin* so;
    /* SI: grounded by the cable's parent plug. Normal wire: the GBA's SO, input with pull-up: low
     * = slave ready, then its data bits. */
    const GpioPin* si;
} MlinkPins;

typedef enum {
    MlinkCableOk,
    MlinkCableReversed,
} MlinkCable;

/* Called in ISR context after each frame's last transfer: rx is that frame's result; fill next.
 * next arrives holding the frame just sent. Must not block, allocate or log; return quickly. */
typedef void (*MlinkFrameCallback)(void* ctx, const MlinkRx* rx, MlinkTx* next);

typedef struct MlinkHw MlinkHw;

MlinkHw* mlink_hw_alloc(const MlinkPins* pins);

/* Stops the driver first if it is running. */
void mlink_hw_free(MlinkHw* hw);

/* SI read low with pull-up => Flipper holds the parent plug. No cable reads as reversed.
 * When stopped, configures SI as input with pull-up, waits 1 ms, reads it, and puts SI back to
 * analog. When running, only reads SI. */
MlinkCable mlink_hw_check_cable(MlinkHw* hw);

/* false (and nothing enabled) if TIM2 is already on (furi_hal_bus_is_enabled), the cable is
 * reversed, or the driver is already running. first is the first frame's tx; count is clamped to
 * 1..MLINK_BURST_LEN. */
bool mlink_hw_start(MlinkHw* hw, MlinkFrameCallback cb, void* ctx, const MlinkTx* first);

/* mlink_hw_start with a mode. MlinkModeFrame is exactly mlink_hw_start. MlinkModeStream: a frame
 * starts on the first tick after start and on every tick after a frame's callback (no frame-rate
 * gate); transfer t on tick t; count 0 makes an idle tick whose callback gets rx.count 0, masks 0.
 * The cable guard still reads SI at every frame start (there are no idle ticks between frames to
 * latch on). A frame it skips lasts one tick, but its callback gets rx.count = count, every word
 * timed out (mlink_frame_us then over-counts its time). rx.ticks is max(count, 1), a skipped
 * frame included. Same failure rules as mlink_hw_start. Exactly
 * mlink_hw_start_wire(hw, mode, MlinkWireMulti, cb, ctx, first). */
bool mlink_hw_start_mode(
    MlinkHw* hw,
    MlinkMode mode,
    MlinkFrameCallback cb,
    void* ctx,
    const MlinkTx* first);

/* mlink_hw_start_mode with a wire. MlinkWireMulti is exactly mlink_hw_start_mode.
 * MlinkWireNormal (see the top of this file): false with MlinkModeFrame; no cable check, so it
 * fails only if TIM2 is already on or the driver is already running. SC and SO become push-pull
 * outputs, idle high, SI an input with pull-up; SD stays analog. Its first frame starts on the
 * first tick after start. first.hi is sent as given; zero it unless the first frame needs it. */
bool mlink_hw_start_wire(
    MlinkHw* hw,
    MlinkMode mode,
    MlinkWire wire,
    MlinkFrameCallback cb,
    void* ctx,
    const MlinkTx* first);

/* The cable guard's latest decision: true from mlink_hw_start or mlink_hw_start_mode (its own
 * check passed) until a frame start finds SI high (read then, or, in frame mode only, latched on
 * an idle tick since the previous frame start; in stream mode nothing latches), and true again
 * once a frame start finds it low with nothing latched. The Normal wire has no guard: true from
 * a Normal start until the next MultiPlay start. Keeps its value when stopped; false before the
 * first start. */
bool mlink_hw_cable_ok(const MlinkHw* hw);

/* Pins back to GpioModeAnalog, TIM2 off, ISR cleared. Does nothing if not running. */
void mlink_hw_stop(MlinkHw* hw);

/* Normal wire: true (from mlink_hw_alloc, D3) waits for the slave's ready (SI low) before each
 * slot; false clocks every slot on its tick, no SI check, no settle, 1 tick a slot, never not
 * ready (the BIOS phase of multiboot, multiboot-and-save.md §7.3). The ISR reads it once per slot,
 * so it takes effect from the next slot. May be called while the driver is stopped or running;
 * kept across starts and stops. The MultiPlay wire ignores it. */
void mlink_hw_set_normal_gate(MlinkHw* hw, bool gate);

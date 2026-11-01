#include <src/gen3/mlink_hw_furi.h>

#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <stm32wbxx_ll_tim.h>

/* ---- Tunables for the hardware test (docs/gen3/link-protocol.md A2 "Wire level", U1, U2) ---- */

/* Order of the 16 data bits on SD (GBATEK: LSB first; U1). If every received word comes back
 * bit-reversed (e.g. 059D instead of B9A0), set this to 0. Applies to sending and receiving. */
#define MLINK_LSB_FIRST 1

/* 1: SD is a push-pull output for the Flipper's own frame (start bit through stop bit), so its
 * 0->1 edges don't wait on the pull-ups; it is released to input with pull-up before SO goes low,
 * and idles that way between transfers. 0: SD is an open-drain output with pull-up the whole
 * time, released = high (the original design). */
#define MLINK_SD_PUSH_PULL 1

/* DWT->CYCCNT and TIM2 both count the 64 MHz core clock. */
#define MLINK_CPU_HZ 64000000u
#define MLINK_BAUD 115200u

/* Bit timing. Every edge is computed from one reference time, so rounding never accumulates.
 * One bit = 64000000 / 115200 = 555.56 cycles = 8.68 us.
 * MLINK_EDGE_CYCLES(n): whole-bit edge n. MLINK_HALF_BIT_CYCLES(h): h half bits (odd h = the
 * centre of a bit); MLINK_HALF_BIT_CYCLES(2 * n) == MLINK_EDGE_CYCLES(n). */
#define MLINK_EDGE_CYCLES(n) (((n) * MLINK_CPU_HZ + MLINK_BAUD / 2u) / MLINK_BAUD)
#define MLINK_HALF_BIT_CYCLES(h) (((h) * (MLINK_CPU_HZ / 2u) + MLINK_BAUD / 2u) / MLINK_BAUD)

#define MLINK_DATA_BITS 16u
#define MLINK_FRAME_BITS 18u /* start 0, 16 data bits, stop 1 */

/* Parent's word: SC low at edge 0, start bit at edge 1 (one bit after SC low), stop bit ends at
 * edge 19 = 10556 cycles = 164.9 us. */
#define MLINK_TX_LEAD_BITS 1u

/* Child's word, in half bits from its start edge: data bit k sampled at 3 + 2k (1.5 + k bits),
 * stop bit at 35 (17.5 bits). SC goes high at 36, the end of the child's stop bit
 * (10000 cycles = 156.25 us). */
#define MLINK_RX_FIRST_SAMPLE_HALF 3u
#define MLINK_RX_STOP_SAMPLE_HALF 35u
#define MLINK_RX_END_HALF 36u

/* After SO goes low, how long the child gets to start its word (U1). */
#define MLINK_CHILD_TIMEOUT_US 100u
#define MLINK_CHILD_TIMEOUT_CYCLES (MLINK_CHILD_TIMEOUT_US * (MLINK_CPU_HZ / 1000000u)) /* 6400 */

/* TIM2 tick, one transfer per tick, measured start to start. The GBA leaves 751.5 us from the END
 * of one transfer to the START of the next (timer 3 reload, started from SendRecvDone; U2). A
 * nominal transfer is about 321 us (164.9 us parent word + 156.25 us child word), so
 * 751.5 + 321.5 = 1073 us = 68672 cycles = (PSC + 1) * (ARR + 1). The real gap is
 * 1073 - (321 + child reply delay) us with a child, 808 us without one. To change it, change
 * MLINK_TIM_ARR and MLINK_TICK_DECI_US together (checked below). */
#define MLINK_TIM_PRESCALER 0u
#define MLINK_TIM_ARR 68671u

/* Frame cadence in 0.1 us (frame mode only): +10730 per tick; when the sum reaches 167400
 * (16.740 ms = 59.74 Hz, a hair faster than the GBA's 59.7275 Hz), subtract it and start a frame.
 * That is 15.6 ticks per frame; frames start on tick boundaries, 15 or 16 ticks apart. A
 * 9-transfer burst takes 9 ticks = 9.66 ms, inside the 16.74 ms frame. Stream mode has no
 * cadence: a frame starts on the tick after the previous frame's callback. */
#define MLINK_TICK_DECI_US 10730u
#define MLINK_FRAME_DECI_US 167400u

/* mlink_hw_check_cable: settle time for SI's pull-up before reading it. */
#define MLINK_CABLE_SETTLE_US 1000u

/* The Normal wire (GBATEK "SIO Normal Mode"; the GBC cable crosses SO and SI and carries SC; SD
 * isn't used). One 32-bit word per transfer, MSB first, clocked on SC at 256 kHz: SC low, then
 * each side puts its next bit on SO; SC high, then each side samples SI. SC idles high. The slave
 * says it is ready by holding its SO, our SI, low. Each bit cell starts with SC low and lasts
 * MLINK_NORMAL_BIT_CYCLES; SC rises half way through, and SI is read MLINK_NORMAL_SAMPLE_CYCLES
 * into the cell (62 cycles = 0.97 us after SC rises). Before the first bit, SI must still read
 * low after MLINK_NORMAL_SETTLE_CYCLES (4 us), so a slave that is still setting up its transfer
 * gets the time. 32 bits = 8000 cycles = 125 us. */
#define MLINK_NORMAL_BITS 32u
#define MLINK_NORMAL_BIT_CYCLES 250u
#define MLINK_NORMAL_SETTLE_CYCLES 256u
#define MLINK_NORMAL_SAMPLE_CYCLES 187u

#define MLINK_GPIO_SPEED GpioSpeedVeryHigh

#define MLINK_TIM TIM2
#define MLINK_TIM_BUS FuriHalBusTIM2
#define MLINK_TIM_IRQ FuriHalInterruptIdTIM2

_Static_assert(
    (MLINK_TIM_PRESCALER + 1u) * (MLINK_TIM_ARR + 1u) * 10u ==
        MLINK_TICK_DECI_US * (MLINK_CPU_HZ / 1000000u),
    "MLINK_TICK_DECI_US must match the TIM2 period");
_Static_assert(
    (MLINK_BURST_LEN * MLINK_TICK_DECI_US) < MLINK_FRAME_DECI_US,
    "a burst must fit in one frame");
/* The interrupts-masked part of a transfer (parent word + longest wait + child word,
 * 26956 cycles = 421 us at the defaults) must end well before the next tick. */
_Static_assert(
    MLINK_EDGE_CYCLES(MLINK_TX_LEAD_BITS + MLINK_FRAME_BITS) + MLINK_CHILD_TIMEOUT_CYCLES +
            MLINK_HALF_BIT_CYCLES(MLINK_RX_END_HALF) <
        (MLINK_TIM_PRESCALER + 1u) * (MLINK_TIM_ARR + 1u),
    "one transfer must fit in one tick");
_Static_assert(MLINK_BURST_LEN <= 16, "timeout_mask and framing_mask are 16 bits");
_Static_assert(
    MLINK_TICK_DECI_US == MLINK_TICK_US * 10u,
    "MLINK_TICK_US (mlink_types.h) must match MLINK_TICK_DECI_US");
_Static_assert(
    MLINK_FRAME_DECI_US == MLINK_FRAME_US * 10u,
    "MLINK_FRAME_US (mlink_types.h) must match MLINK_FRAME_DECI_US");
_Static_assert(
    MLINK_NORMAL_BIT_CYCLES * 256000u == MLINK_CPU_HZ,
    "the Normal wire clocks at 256 kHz");
_Static_assert(
    MLINK_NORMAL_SETTLE_CYCLES == 4u * (MLINK_CPU_HZ / 1000000u),
    "the Normal wire's ready check settles for 4 us");
_Static_assert(
    MLINK_NORMAL_SAMPLE_CYCLES > MLINK_NORMAL_BIT_CYCLES / 2u &&
        MLINK_NORMAL_SAMPLE_CYCLES < MLINK_NORMAL_BIT_CYCLES,
    "the Normal wire samples SI after SC rises and before the next bit's SC fall");
/* The interrupts-masked part of a Normal transfer (settle + 32 bits, 8256 cycles = 129 us) must
 * end well before the next tick. */
_Static_assert(
    MLINK_NORMAL_SETTLE_CYCLES + MLINK_NORMAL_BITS * MLINK_NORMAL_BIT_CYCLES <
        (MLINK_TIM_PRESCALER + 1u) * (MLINK_TIM_ARR + 1u),
    "one Normal transfer must fit in one tick");
_Static_assert(
    MLINK_NORMAL_STALL_TICKS >= 1u && MLINK_NORMAL_STALL_TICKS <= 255u &&
        MLINK_BURST_LEN * MLINK_NORMAL_STALL_TICKS <= 0xFFFFu,
    "MlinkHw.wait is 8 bits and MlinkRx.ticks 16 bits");

/* One pin's 2-bit MODER field, for mlink_moder_write. */
typedef struct {
    uint32_t keep; /* the port's MODER with this pin's field cleared (= input) */
    uint32_t out; /* this pin's field set to 01 (output) */
} MlinkModer;

struct MlinkHw {
    MlinkPins pins;
    MlinkModer sd_moder;
    MlinkModer sc_moder;
    MlinkModer so_moder;
    MlinkFrameCallback cb;
    void* ctx;
    bool running;
    MlinkMode mode; /* set by mlink_hw_start_wire, read by the ISR */
    /* MultiPlay: written by the ISR at each frame start; Normal: true from the start. Read by any
     * thread. */
    volatile bool cable_ok;
    /* Normal wire: wait for the slave's ready (SI low) before each slot (D3). Written by the app
     * (mlink_hw_set_normal_gate), read by the ISR once per slot; true from mlink_hw_alloc. */
    volatile bool normal_gate;

    /* Owned by the TIM2 ISR while running. */
    volatile uint8_t si_seen_high; /* 1: an idle tick read SI high since the last frame start */
    bool in_frame;
    uint8_t transfer; /* next transfer of the current frame */
    uint8_t wait; /* Normal: ticks the current slot has waited for the slave to get ready */
    uint32_t frame_acc; /* 0.1 us, see MLINK_FRAME_DECI_US */
    MlinkTx tx; /* frame being sent */
    MlinkTx next; /* callback output, copied into tx */
    MlinkRx rx; /* frame being received */
};

static FURI_ALWAYS_INLINE uint16_t mlink_data_bit(uint32_t k) {
#if MLINK_LSB_FIRST
    return (uint16_t)(1u << k);
#else
    return (uint16_t)(0x8000u >> k);
#endif
}

/* count clamped to 1..MLINK_BURST_LEN in frame mode, 0..MLINK_BURST_LEN in stream mode. */
static void mlink_load_tx(MlinkTx* dst, const MlinkTx* src, MlinkMode mode) {
    uint8_t count = src->count;
    if(count < 1 && mode == MlinkModeFrame) count = 1;
    if(count > MLINK_BURST_LEN) count = MLINK_BURST_LEN;
    *dst = *src;
    dst->count = count;
}

/* Line levels of one word in send order, bit 0 first: start 0, 16 data bits, stop 1. */
static uint32_t mlink_frame_levels(uint16_t word) {
    uint32_t levels = 1u << (MLINK_FRAME_BITS - 1u);
    for(uint32_t k = 0; k < MLINK_DATA_BITS; k++) {
        if(word & mlink_data_bit(k)) levels |= 1u << (k + 1u);
    }
    return levels;
}

/* Pin access and waits inside the masked window. furi_hal_gpio_read/write are static inline, but
 * -Os outlines the read; these are always inlined so every edge has the same fixed latency. */
static FURI_ALWAYS_INLINE void mlink_pin_write(const GpioPin* pin, bool high) {
    pin->port->BSRR = high ? pin->pin : ((uint32_t)pin->pin << 16); /* upper half = reset */
}

static FURI_ALWAYS_INLINE bool mlink_pin_read(const GpioPin* pin) {
    return (pin->port->IDR & pin->pin) != 0;
}

static FURI_ALWAYS_INLINE void mlink_wait_until(uint32_t t0, uint32_t cycles) {
    while(DWT->CYCCNT - t0 < cycles) {
    }
}

/* Switch one pin between input (MODER 00, with the pull-up mlink_pins_run set) and output
 * (MODER 01; OTYPER is already push-pull, so it drives ODR). Used for SD's own word
 * (MLINK_SD_PUSH_PULL) and for SC and SO when the cable guard releases them. One
 * read-modify-write of MODER, about 4 cycles; callers mask every interrupt around it, so no
 * other write to the same port's MODER can land in between. */
static FURI_ALWAYS_INLINE void
    mlink_moder_write(GPIO_TypeDef* port, uint32_t keep_mask, uint32_t field) {
    port->MODER = (port->MODER & keep_mask) | field;
}

static MlinkModer mlink_moder_of(const GpioPin* pin) {
    const uint32_t shift = 2u * (uint32_t)__builtin_ctz(pin->pin);
    return (MlinkModer){.keep = ~(3u << shift), .out = 1u << shift};
}

/* SI seen high (cable guard): SC and SO to input with pull-up, as SD is between transfers, so
 * neither fights a GBA that drives SC as multiplayer master. ODR goes high first, as before every
 * mode switch of these pins, so whatever switches them back drives them high. ISR context; a few
 * register writes. */
static void mlink_lines_release(const MlinkHw* hw) {
    const GpioPin* sc = hw->pins.sc;
    const GpioPin* so = hw->pins.so;
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    mlink_pin_write(sc, true);
    mlink_pin_write(so, true);
    mlink_moder_write(sc->port, hw->sc_moder.keep, 0);
    mlink_moder_write(so->port, hw->so_moder.keep, 0);
    __set_PRIMASK(primask);
}

/* A frame is about to run transfers: SC and SO back to push-pull outputs at their idle level.
 * ODR high first, then MODER 01, so each comes back driven high and never glitches low. Already
 * outputs (the usual case), they just stay high. ISR context; a few register writes. */
static void mlink_lines_drive(const MlinkHw* hw) {
    const GpioPin* sc = hw->pins.sc;
    const GpioPin* so = hw->pins.so;
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    mlink_pin_write(sc, true);
    mlink_pin_write(so, true);
    mlink_moder_write(sc->port, hw->sc_moder.keep, hw->sc_moder.out);
    mlink_moder_write(so->port, hw->so_moder.keep, hw->so_moder.out);
    __set_PRIMASK(primask);
}

/* The firmware starts the cycle counter at boot; gblink and furi_delay_us rely on it. Make sure
 * the same way the firmware does it: every wait in a transfer spins on it with interrupts off. */
static void mlink_cyccnt_ensure(void) {
    if(!(DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk)) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
    furi_check(DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk);
}

/* One transfer: the parent's word on SD, hand-over on SO, then the child's word on SD. Every
 * interrupt is masked from SC low to SC high: 265 us with no child, 321 us plus the child's
 * reply delay with one, 421 us at most. SD is released (input with pull-up, or open drain high)
 * before SO goes low, and stays released when this returns. SC and SO are push-pull outputs
 * here (mlink_lines_drive at the frame start) and are high again when this returns. */
static void mlink_hw_transfer(MlinkHw* hw, uint8_t t) {
    const GpioPin* sc = hw->pins.sc;
    const GpioPin* sd = hw->pins.sd;
    const GpioPin* so = hw->pins.so;
    const uint32_t levels = mlink_frame_levels(hw->tx.word[t]);
#if MLINK_SD_PUSH_PULL
    GPIO_TypeDef* const sd_port = sd->port;
    const uint32_t sd_keep = hw->sd_moder.keep;
    const uint32_t sd_out = hw->sd_moder.out;
#endif
    uint16_t word = 0;
    bool answered = false;
    bool stop_high = false;

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    const uint32_t t0 = DWT->CYCCNT;
    mlink_pin_write(sc, false);
#if MLINK_SD_PUSH_PULL
    mlink_pin_write(sd, false); /* start bit into ODR; SD is still an input, the line stays high */
    mlink_wait_until(t0, MLINK_EDGE_CYCLES(MLINK_TX_LEAD_BITS));
    mlink_moder_write(sd_port, sd_keep, sd_out); /* start bit: drive SD through the stop bit */
#else
    mlink_wait_until(t0, MLINK_EDGE_CYCLES(MLINK_TX_LEAD_BITS));
    mlink_pin_write(sd, false); /* start bit */
#endif
    for(uint32_t b = 1; b < MLINK_FRAME_BITS; b++) {
        const bool level = (levels >> b) & 1u;
        mlink_wait_until(t0, MLINK_EDGE_CYCLES(MLINK_TX_LEAD_BITS + b));
        mlink_pin_write(sd, level);
    }
    mlink_wait_until(t0, MLINK_EDGE_CYCLES(MLINK_TX_LEAD_BITS + MLINK_FRAME_BITS));
#if MLINK_SD_PUSH_PULL
    mlink_moder_write(sd_port, sd_keep, 0); /* release: input with pull-up */
#else
    mlink_pin_write(sd, true); /* release: open drain high */
#endif
    mlink_pin_write(so, false); /* child's SI low: its turn */

    const uint32_t t_so = DWT->CYCCNT;
    uint32_t c0 = t_so;
    for(;;) {
        const uint32_t now = DWT->CYCCNT;
        if(!mlink_pin_read(sd)) {
            c0 = now;
            answered = true;
            break;
        }
        if(now - t_so >= MLINK_CHILD_TIMEOUT_CYCLES) break;
    }
    if(answered) {
        for(uint32_t k = 0; k < MLINK_DATA_BITS; k++) {
            mlink_wait_until(c0, MLINK_HALF_BIT_CYCLES(MLINK_RX_FIRST_SAMPLE_HALF + 2u * k));
            if(mlink_pin_read(sd)) word |= mlink_data_bit(k);
        }
        mlink_wait_until(c0, MLINK_HALF_BIT_CYCLES(MLINK_RX_STOP_SAMPLE_HALF));
        stop_high = mlink_pin_read(sd);
        mlink_wait_until(c0, MLINK_HALF_BIT_CYCLES(MLINK_RX_END_HALF));
    }

    mlink_pin_write(sc, true);
    mlink_pin_write(so, true);
    __set_PRIMASK(primask);

    if(answered) {
        hw->rx.word[t] = word;
        if(!stop_high) hw->rx.framing_mask |= (uint16_t)(1u << t);
    } else {
        hw->rx.word[t] = MLINK_NO_DATA;
        hw->rx.timeout_mask |= (uint16_t)(1u << t);
    }
}

/* Frame done: the callback gets hw->rx and fills the next frame's tx. Interrupts are enabled. */
static void mlink_frame_done(MlinkHw* hw) {
    hw->in_frame = false;
    hw->next = hw->tx;
    hw->cb(hw->ctx, &hw->rx, &hw->next);
    mlink_load_tx(&hw->tx, &hw->next, hw->mode);
}

static void mlink_hw_isr(void* context) {
    MlinkHw* hw = context;

    if(!LL_TIM_IsActiveFlag_UPDATE(MLINK_TIM)) return;
    /* Clear first, so a tick that comes due while this one runs long stays pending. */
    LL_TIM_ClearFlag_UPDATE(MLINK_TIM);

    /* Frame mode's cadence and the idle ticks between its frames. Stream mode never touches
     * frame_acc: every tick outside a frame starts one (multiboot-and-save.md §6.9). */
    if(hw->mode == MlinkModeFrame) {
        hw->frame_acc += MLINK_TICK_DECI_US;
        if(!hw->in_frame) {
            if(hw->frame_acc < MLINK_FRAME_DECI_US) {
                /* Idle tick, no transfer: latch SI high for the next frame start. A GBA master's
                 * SO (on our SI) idles high but dips low in its own transfers, so one read per
                 * frame can miss it. Usually one pin read, nothing else. The tick that first
                 * latches it also releases SC and SO at once, so they stop fighting a GBA master
                 * within a tick (~1 ms) rather than at the next frame start (~15 ms): at most one
                 * release per frame, only while SI reads high. Safe: no transfer runs until a
                 * frame start, that start trips on the latch (its release repeats the same
                 * writes), and every start that passes drives SC and SO again. */
                if(mlink_pin_read(hw->pins.si) && !hw->si_seen_high) {
                    hw->si_seen_high = 1;
                    mlink_lines_release(hw);
                }
                return;
            }
            hw->frame_acc -= MLINK_FRAME_DECI_US;
        }
    }
    if(!hw->in_frame) {
        hw->transfer = 0;
        hw->rx.count = hw->tx.count;
        for(uint8_t i = 0; i < MLINK_BURST_LEN; i++) {
            hw->rx.word[i] = MLINK_NO_DATA;
        }
        hw->rx.timeout_mask = 0;
        hw->rx.framing_mask = 0;
        hw->rx.notready_mask = 0;
        /* The frame's time in ticks, read in stream mode: max(count, 1), a frame the cable guard
         * skips (one tick) included, so the protocol's timeouts run as they always have. */
        hw->rx.ticks = (uint16_t)(hw->tx.count ? hw->tx.count : 1u);

        /* Cable guard, before any line moves (interrupts on). The parent plug grounds SI; high
         * through the pull-up, now or on an idle tick since the last frame start, means no
         * cable or a reversed one. Skip the frame: SC and SO released (SD already is), no
         * transfer, and the callback sees a GBA that never answered. */
        const bool cable_ok = !mlink_pin_read(hw->pins.si) && !hw->si_seen_high;
        hw->si_seen_high = 0;
        hw->cable_ok = cable_ok;
        if(!cable_ok) {
            mlink_lines_release(hw);
            hw->rx.timeout_mask = (uint16_t)((1u << hw->rx.count) - 1u); /* count is 0..9 */
            mlink_frame_done(hw);
            return;
        }
        if(!hw->tx.count) {
            /* Stream mode's idle tick (count 0, never in frame mode): no transfer and no line
             * moves; the callback gets rx.count 0 and both masks 0. */
            mlink_frame_done(hw);
            return;
        }
        mlink_lines_drive(hw); /* undoes a skipped frame's release; else SC and SO stay high */
        hw->in_frame = true;
    }

    mlink_hw_transfer(hw, hw->transfer);
    hw->transfer++;
    if(hw->transfer < hw->tx.count) return;
    mlink_frame_done(hw);
}

/* Slot t of a Normal frame, on this tick. Gated (gate, D3): SI high: the slave isn't ready, and
 * the slot waits for the next tick, up to MLINK_NORMAL_STALL_TICKS ticks in all; on the last it is
 * reported not ready (word MLINK_NO_DATA, timeout_mask and notready_mask bit t), nothing clocked.
 * SI low: one 32-bit transfer, (hi << 16) | word out, the reply's upper half in. Ungated: the
 * transfer runs at once, with no SI check and no settle. Every interrupt is masked from the
 * settle to the last bit's sample: 4 us, or 128 us when it clocks (124 us ungated). SD is never
 * touched; SC and SO are push-pull outputs (mlink_pins_run_normal) and are high again when this
 * returns. Returns true when the slot is done, delivered or not ready. */
static bool mlink_normal_slot(MlinkHw* hw, uint8_t t, bool gate) {
    const GpioPin* sc = hw->pins.sc;
    const GpioPin* so = hw->pins.so;
    const GpioPin* si = hw->pins.si;
    const uint32_t out = (uint32_t)hw->tx.hi[t] << 16 | hw->tx.word[t];
    uint32_t reply = 0;
    bool ready = false;

    if(!gate || !mlink_pin_read(si)) {
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();

        /* Gated: still low after the settle, so the slave is ready and has had the time to start
         * its side of the transfer. Every edge below is timed from t0. */
        const uint32_t ts = DWT->CYCCNT;
        const uint32_t settle = gate ? MLINK_NORMAL_SETTLE_CYCLES : 0u;
        if(gate) mlink_wait_until(ts, settle);
        ready = !gate || !mlink_pin_read(si);
        if(ready) {
            const uint32_t t0 = ts + settle;
            for(uint32_t n = 0; n < MLINK_NORMAL_BITS; n++) {
                const uint32_t b = MLINK_NORMAL_BITS - 1u - n; /* bit 31 first */
                const uint32_t edge = n * MLINK_NORMAL_BIT_CYCLES; /* (31 - b) * BIT */
                const bool level = (out >> b) & 1u;
                mlink_wait_until(t0, edge);
                mlink_pin_write(sc, false);
                mlink_pin_write(so, level);
                mlink_wait_until(t0, edge + MLINK_NORMAL_BIT_CYCLES / 2u);
                mlink_pin_write(sc, true);
                mlink_wait_until(t0, edge + MLINK_NORMAL_SAMPLE_CYCLES);
                reply = (reply << 1) | (mlink_pin_read(si) ? 1u : 0u);
            }
            mlink_pin_write(sc, true);
            mlink_pin_write(so, true);
        }

        __set_PRIMASK(primask);
    }

    if(ready) {
        hw->rx.word[t] = (uint16_t)(reply >> 16);
        return true;
    }
    if(++hw->wait < MLINK_NORMAL_STALL_TICKS) return false;
    hw->rx.word[t] = MLINK_NO_DATA;
    hw->rx.timeout_mask |= (uint16_t)(1u << t);
    hw->rx.notready_mask |= (uint16_t)(1u << t);
    return true;
}

/* The Normal wire's TIM2 ISR (stream mode only; no cable guard, no idle-tick latch). A frame
 * starts on the tick after the previous frame's callback. Each tick serves the frame's current
 * slot (mlink_normal_slot, gated or not as normal_gate reads at that tick); the next slot starts
 * on the tick after the one that finished it. Every tick of the frame counts into rx.ticks: 1 per
 * delivered slot plus each tick it waited, so a slot that never gets ready costs
 * MLINK_NORMAL_STALL_TICKS (ungated, every slot is 1); a count-0 frame is 1 tick, its callback on
 * its start tick. */
static void mlink_hw_isr_normal(void* context) {
    MlinkHw* hw = context;

    if(!LL_TIM_IsActiveFlag_UPDATE(MLINK_TIM)) return;
    /* Clear first, so a tick that comes due while this one runs long stays pending. */
    LL_TIM_ClearFlag_UPDATE(MLINK_TIM);

    if(!hw->in_frame) {
        hw->in_frame = true;
        hw->transfer = 0;
        hw->wait = 0;
        hw->rx.count = hw->tx.count;
        for(uint8_t i = 0; i < MLINK_BURST_LEN; i++) {
            hw->rx.word[i] = MLINK_NO_DATA;
        }
        hw->rx.timeout_mask = 0;
        hw->rx.framing_mask = 0;
        hw->rx.notready_mask = 0;
        hw->rx.ticks = 0;
    }

    hw->rx.ticks++;
    if(hw->transfer < hw->tx.count) {
        if(!mlink_normal_slot(hw, hw->transfer, hw->normal_gate)) return; /* waits a tick */
        hw->transfer++;
        hw->wait = 0;
        if(hw->transfer < hw->tx.count) return;
    }
    /* The frame is sent, so its hi[] is no longer needed: zero it, and next.hi arrives zeroed in
     * the callback (only a producer that wants an upper half sets one). */
    memset(hw->tx.hi, 0, sizeof(hw->tx.hi));
    mlink_frame_done(hw);
}

/* Each output gets its idle level before it becomes an output, so nothing glitches low.
 * SC and SO get their pull-up now, though it does nothing while they drive: the cable guard
 * releases them by MODER alone (mlink_lines_release), and that makes them inputs with pull-up. */
static void mlink_pins_run(const MlinkPins* pins) {
    furi_hal_gpio_write(pins->so, true); /* the GBA's SI must idle high */
    furi_hal_gpio_init(pins->so, GpioModeOutputPushPull, GpioPullUp, MLINK_GPIO_SPEED);
    furi_hal_gpio_write(pins->sc, true);
    furi_hal_gpio_init(pins->sc, GpioModeOutputPushPull, GpioPullUp, MLINK_GPIO_SPEED);
    furi_hal_gpio_write(pins->sd, true);
#if MLINK_SD_PUSH_PULL
    /* Released between transfers: input with pull-up. OTYPER push-pull only takes effect while
     * mlink_hw_transfer switches MODER to output for the Flipper's own word (start bit through
     * stop bit, mlink_moder_write). */
    furi_hal_gpio_init(pins->sd, GpioModeInput, GpioPullUp, MLINK_GPIO_SPEED);
    FURI_CRITICAL_ENTER();
    pins->sd->port->OTYPER &= ~(uint32_t)pins->sd->pin;
    FURI_CRITICAL_EXIT();
#else
    furi_hal_gpio_init(pins->sd, GpioModeOutputOpenDrain, GpioPullUp, MLINK_GPIO_SPEED);
#endif
    furi_hal_gpio_init(pins->si, GpioModeInput, GpioPullUp, MLINK_GPIO_SPEED);
}

/* The Normal wire: SC and SO push-pull outputs, each driven high before it becomes an output;
 * SI input with pull-up (high = slave not ready, or no slave); SD isn't used: analog. */
static void mlink_pins_run_normal(const MlinkPins* pins) {
    furi_hal_gpio_init_simple(pins->sd, GpioModeAnalog);
    furi_hal_gpio_write(pins->so, true);
    furi_hal_gpio_init(pins->so, GpioModeOutputPushPull, GpioPullNo, MLINK_GPIO_SPEED);
    furi_hal_gpio_write(pins->sc, true);
    furi_hal_gpio_init(pins->sc, GpioModeOutputPushPull, GpioPullNo, MLINK_GPIO_SPEED);
    furi_hal_gpio_init(pins->si, GpioModeInput, GpioPullUp, MLINK_GPIO_SPEED);
}

static void mlink_pins_off(const MlinkPins* pins) {
    furi_hal_gpio_init_simple(pins->sd, GpioModeAnalog);
    furi_hal_gpio_init_simple(pins->sc, GpioModeAnalog);
    furi_hal_gpio_init_simple(pins->so, GpioModeAnalog);
    furi_hal_gpio_init_simple(pins->si, GpioModeAnalog);
}

MlinkHw* mlink_hw_alloc(const MlinkPins* pins) {
    furi_check(pins);
    furi_check(pins->sc && pins->sd && pins->so && pins->si);
    furi_check(pins->sd->pin != 0 && pins->sc->pin != 0 && pins->so->pin != 0);

    MlinkHw* hw = malloc(sizeof(MlinkHw));
    memset(hw, 0, sizeof(*hw));
    hw->pins = *pins;
    hw->sd_moder = mlink_moder_of(pins->sd);
    hw->sc_moder = mlink_moder_of(pins->sc);
    hw->so_moder = mlink_moder_of(pins->so);
    hw->normal_gate = true;
    return hw;
}

void mlink_hw_free(MlinkHw* hw) {
    furi_check(hw);
    mlink_hw_stop(hw);
    free(hw);
}

MlinkCable mlink_hw_check_cable(MlinkHw* hw) {
    furi_check(hw);
    const GpioPin* si = hw->pins.si;
    bool si_high;

    if(hw->running) {
        si_high = furi_hal_gpio_read(si);
    } else {
        furi_hal_gpio_init(si, GpioModeInput, GpioPullUp, MLINK_GPIO_SPEED);
        furi_delay_us(MLINK_CABLE_SETTLE_US);
        si_high = furi_hal_gpio_read(si);
        furi_hal_gpio_init_simple(si, GpioModeAnalog);
    }
    return si_high ? MlinkCableReversed : MlinkCableOk;
}

bool mlink_hw_start(MlinkHw* hw, MlinkFrameCallback cb, void* ctx, const MlinkTx* first) {
    return mlink_hw_start_mode(hw, MlinkModeFrame, cb, ctx, first);
}

bool mlink_hw_start_mode(
    MlinkHw* hw,
    MlinkMode mode,
    MlinkFrameCallback cb,
    void* ctx,
    const MlinkTx* first) {
    return mlink_hw_start_wire(hw, mode, MlinkWireMulti, cb, ctx, first);
}

bool mlink_hw_start_wire(
    MlinkHw* hw,
    MlinkMode mode,
    MlinkWire wire,
    MlinkFrameCallback cb,
    void* ctx,
    const MlinkTx* first) {
    furi_check(hw);
    furi_check(cb);
    furi_check(first);
    mlink_cyccnt_ensure();

    const bool normal = wire == MlinkWireNormal;
    if(normal && mode != MlinkModeStream) return false; /* the Normal wire is stream mode only */
    /* TIM2 first: if gblink holds it, it may be using SI too, so don't touch any pin. */
    if(hw->running || furi_hal_bus_is_enabled(MLINK_TIM_BUS)) return false;
    /* The Normal wire has no cable guard: SI high there just means the slave isn't ready. */
    if(!normal && mlink_hw_check_cable(hw) != MlinkCableOk) return false;

    /* Claim TIM2 atomically: furi_hal_bus_enable crashes if the bus is already on. */
    FURI_CRITICAL_ENTER();
    const bool busy = furi_hal_bus_is_enabled(MLINK_TIM_BUS);
    if(!busy) furi_hal_bus_enable(MLINK_TIM_BUS);
    FURI_CRITICAL_EXIT();
    if(busy) return false;

    hw->cb = cb;
    hw->ctx = ctx;
    hw->mode = mode;
    mlink_load_tx(&hw->tx, first, mode);
    hw->next = hw->tx;
    memset(&hw->rx, 0, sizeof(hw->rx));
    hw->in_frame = false;
    hw->transfer = 0;
    hw->wait = 0;
    /* Frame mode: first frame after 16 ticks (17.2 ms) of idle lines. Stream mode doesn't use it
     * and starts its first frame on the first tick. */
    hw->frame_acc = 0;
    hw->si_seen_high = 0;
    /* MultiPlay: checked above, and the ISR re-checks SI at every frame start. Normal: no cable
     * guard, so it stays true until the next MultiPlay start. */
    hw->cable_ok = true;

    if(normal) {
        mlink_pins_run_normal(&hw->pins);
    } else {
        mlink_pins_run(&hw->pins);
    }
    /* Stop mode would halt TIM2; plain WFI sleep keeps it running. */
    furi_hal_power_insomnia_enter();

    LL_TIM_InitTypeDef tim = {0};
    tim.Prescaler = MLINK_TIM_PRESCALER;
    tim.CounterMode = LL_TIM_COUNTERMODE_UP;
    tim.Autoreload = MLINK_TIM_ARR;
    tim.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    LL_TIM_Init(MLINK_TIM, &tim);
    /* LL_TIM_Init forces an update event to load the prescaler; drop its flag. */
    LL_TIM_ClearFlag_UPDATE(MLINK_TIM);

    hw->running = true;
    furi_hal_interrupt_set_isr(MLINK_TIM_IRQ, normal ? mlink_hw_isr_normal : mlink_hw_isr, hw);
    LL_TIM_EnableIT_UPDATE(MLINK_TIM);
    LL_TIM_EnableCounter(MLINK_TIM);
    return true;
}

void mlink_hw_set_normal_gate(MlinkHw* hw, bool gate) {
    furi_check(hw);
    hw->normal_gate = gate;
}

bool mlink_hw_cable_ok(const MlinkHw* hw) {
    furi_check(hw);
    return hw->cable_ok;
}

void mlink_hw_stop(MlinkHw* hw) {
    furi_check(hw);
    if(!hw->running) return;

    LL_TIM_DisableIT_UPDATE(MLINK_TIM);
    LL_TIM_DisableCounter(MLINK_TIM);
    furi_hal_interrupt_set_isr(MLINK_TIM_IRQ, NULL, NULL);
    LL_TIM_ClearFlag_UPDATE(MLINK_TIM);
    furi_hal_bus_disable(MLINK_TIM_BUS);
    furi_hal_power_insomnia_exit();

    mlink_pins_off(&hw->pins);
    hw->running = false;
}

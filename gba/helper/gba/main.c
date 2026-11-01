#include "hp_core.h"
#include "hp_hal.h"
#include "hp_link.h"

/* The helper's main loop: the slave side of the link, one hp_step per transfer, with interrupts
 * off and everything polled (multiboot-and-save.md §4.7). The BIOS's boot mode (header 0xC4)
 * picks the transport: MultiPlay child (§4.8), or the Normal-mode slave over a Game Boy Color
 * cable (§7.4). */

/* WAITCNT: SRAM 8 cycles (field 3), ROM wait state 0 at 3/1 cycles, wait state 2 at 8, prefetch
 * on. 0x0003 (SRAM 8 cycles only) is the fallback if a cart misbehaves with it. */
#ifndef HP_WAITCNT
#define HP_WAITCNT 0x4317u
#endif

/* How a transfer's end is seen. 1: IF bit 7, which is expected to latch with IME = 0
 * (multiboot-and-save.md M16), acknowledged by writing 1. 0: SIOCNT bit 7 (busy) going 1 -> 0. */
#ifndef HP_POLL_IF
#define HP_POLL_IF 1
#endif

#define HP_REG16(a) (*(volatile uint16_t*)(a))
#define HP_DISPCNT HP_REG16(0x04000000u)
#define HP_SIOMULTI0 HP_REG16(0x04000120u) /* the Flipper's word of the last transfer */
#define HP_SIOCNT HP_REG16(0x04000128u)
#define HP_SIOMLT_SEND HP_REG16(0x0400012Au) /* our word for the next transfer */
#define HP_SIODATA32 (*(volatile uint32_t*)0x04000120u) /* Normal mode: the whole 32-bit word */
#define HP_RCNT HP_REG16(0x04000134u)
#define HP_IE HP_REG16(0x04000200u)
#define HP_IF HP_REG16(0x04000202u)
#define HP_WAITCNT_REG HP_REG16(0x04000204u)
#define HP_IME HP_REG16(0x04000208u)

/* MultiPlay, IRQ request on, 115200 bit/s: the value of pokeruby: src/link.c: EnableSerial */
#define HP_SIOCNT_MULTI 0x6003u
#define HP_SIOCNT_ERROR 0x0040u
#define HP_SIOCNT_BUSY 0x0080u
#define HP_IRQ_SERIAL 0x0080u

/* The BIOS writes the boot mode at header 0xC4: 2 = Normal (GBC cable), 3 = MultiPlay */
#define HP_BOOT_MODE_OFFSET 0xC4u
#define HP_BOOT_NORMAL 2u
/* Normal mode, 32-bit, IRQ request on, external clock (we are the slave). SO (bit 3) low means
 * "ready" to the Flipper, which clocks a transfer only while it reads its SI low. */
#define HP_SIOCNT_NORMAL 0x5000u
#define HP_SIOCNT_START 0x0080u
#define HP_SIOCNT_SO_HIGH 0x0008u

/* 200 ms without a transfer (multiboot-and-save.md §6.5), in 1.0224 ms timer units */
#define HP_RX_TIMEOUT_UNITS 196u

static Hp hp;

/* Waits for the end of the next transfer; with timed, false after 200 ms without one. */
static bool hp_transfer_end(bool timed) {
    hp_hal_timer_start();
#if HP_POLL_IF
    while(!(HP_IF & HP_IRQ_SERIAL)) {
        if(timed && hp_hal_timer_units() >= HP_RX_TIMEOUT_UNITS) return false;
    }
    HP_IF = HP_IRQ_SERIAL;
#else
    bool busy = false;
    for(;;) {
        if(HP_SIOCNT & HP_SIOCNT_BUSY) {
            busy = true;
        } else if(busy) {
            break;
        }
        if(timed && hp_hal_timer_units() >= HP_RX_TIMEOUT_UNITS) return false;
    }
#endif
    return true;
}

/* Loads out for the next transfer and waits for it: right after the previous one ended, so each
 * word goes out exactly once (multiboot-and-save.md §4.8 rule 1). Returns the Flipper's word of
 * that transfer, with HP_STEP_ERR or HP_STEP_TIMEOUT in *flags. */
static uint16_t hp_xfer(uint16_t out, uint8_t* flags) {
    HP_SIOMLT_SEND = out;
    if(!hp_transfer_end(true)) {
        *flags = HP_STEP_TIMEOUT;
        return 0;
    }
    *flags = (HP_SIOCNT & HP_SIOCNT_ERROR) ? HP_STEP_ERR : 0;
    return HP_SIOMULTI0;
}

/* Normal wire: arms one 32-bit transfer, waits for it, and leaves SO high ("not ready") again.
 * The Flipper's word is the low half, ours goes in the high half. Returns the Flipper's word,
 * with HP_STEP_TIMEOUT in *flags after 200 ms without a transfer; there is no error bit. */
static uint16_t hp_xfer_normal(uint16_t out, uint8_t* flags) {
    HP_SIODATA32 = (uint32_t)out << 16;
    /* A transfer-end flag latched after the last arming's timeout (its transfer came just too
     * late) isn't this one's: drop it before arming, so the wait below can't end before this
     * transfer does. Before SO goes low, so that the window where SO is low but nothing is armed
     * stays as short as it was. */
    HP_IF = HP_IRQ_SERIAL;
    HP_SIOCNT = HP_SIOCNT_NORMAL;
    /* Armed with SO low: GBATEK is unclear whether bit 3 drives SO while the slave waits or only
     * after the transfer, and low (ready) is right under either reading. */
    HP_SIOCNT = HP_SIOCNT_NORMAL | HP_SIOCNT_START;
    const bool ended = hp_transfer_end(true);
    /* Not ready (start clear, SO high) at once, also after a timeout: the Flipper samples SI only
     * about 0.9 ms later, and must not see a ready slave that is no longer armed. */
    HP_SIOCNT = HP_SIOCNT_NORMAL | HP_SIOCNT_SO_HIGH;
    if(!ended) {
        *flags = HP_STEP_TIMEOUT;
        return 0;
    }
    *flags = 0;
    return (uint16_t)HP_SIODATA32;
}

/* The Normal-wire loop. The Flipper sees SO high while no transfer is armed and takes that as
 * busy, so a pending command runs at once, with no HP_BUSY transfer; the next armed transfer
 * carries the response's first word. */
static void hp_run_normal(void) {
    HP_RCNT = 0;
    HP_SIOCNT = HP_SIOCNT_NORMAL | HP_SIOCNT_SO_HIGH;
    HP_IE = HP_IRQ_SERIAL;
    HP_DISPCNT = 0;
    hp_hal_status(HpStatusReady);
    hp_init(&hp);
    HP_IF = HP_IRQ_SERIAL;
    uint16_t out = HP_READY;
    for(;;) {
        uint8_t flags;
        const uint16_t in = hp_xfer_normal(out, &flags);
        out = hp_step(&hp, in, flags);
        if(hp_exec_pending(&hp)) {
            hp_hal_status(HpStatusBusy);
            out = hp_execute(&hp);
            hp_hal_status(HpStatusReady);
        }
    }
}

int main(void) {
    HP_IME = 0;
    HP_WAITCNT_REG = HP_WAITCNT;
    if(hp_hal_own_header()[HP_BOOT_MODE_OFFSET] == HP_BOOT_NORMAL) hp_run_normal();
    HP_RCNT = 0;
    HP_SIOCNT = HP_SIOCNT_MULTI;
    HP_IE = HP_IRQ_SERIAL;
    HP_DISPCNT = 0; /* no layers: the backdrop shows the status */
    hp_hal_status(HpStatusReady);
    hp_init(&hp);
    HP_IF = HP_IRQ_SERIAL; /* one left from the BIOS's transfers isn't ours */
    uint16_t out = HP_READY;
    for(;;) {
        uint8_t flags;
        const uint16_t in = hp_xfer(out, &flags);
        out = hp_step(&hp, in, flags);
        if(hp_exec_pending(&hp)) {
            /* The hardware answers every transfer with whatever SEND holds, so the Flipper sees
             * HP_BUSY until the response is ready (multiboot-and-save.md §4.8 rule 2). Then the
             * transfer-end flag left from that time is stale: wait for a fresh one before the
             * response's first word goes into SEND. */
            HP_SIOMLT_SEND = HP_BUSY;
            hp_hal_status(HpStatusBusy);
            out = hp_execute(&hp);
            HP_IF = HP_IRQ_SERIAL;
            hp_transfer_end(false);
            hp_hal_status(HpStatusReady);
        }
    }
}

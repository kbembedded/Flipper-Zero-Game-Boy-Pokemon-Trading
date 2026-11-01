#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>

/* Test-only model of a GBA BIOS held at Start+Select, as the multiboot slave (child 1) under a
 * stream-mode parent: multiboot-and-save.md §2.1-§2.3 and GBATEK "BIOS Multi Boot (Single Game
 * Pak)", at the word level. It shares no code with src/gen3/gen3_mb.c: its decryption and CRC
 * are written here from multiboot-and-save.md §2.3 and GBATEK (the CRC in GBATEK's form), and
 * every reply comes from multiboot-and-save.md §2.2's table.
 *
 * Each transfer replies with the slave's current SEND word (what it loaded after the previous
 * transfer), then handles the parent's word and loads the next SEND.
 *
 * It is on one of two wires (fake_bios_set_wire), as the parent's driver reports them:
 * - MultiPlay, the default: 16-bit words. A transfer that gets no answer (off, starting up,
 *   booted) times out: its timeout bit, MLINK_NO_DATA. Each slot takes one tick.
 * - Normal (multiboot-and-save.md §7.2): 32-bit words, the parent's value in the low half (word)
 *   with the upper half (hi) zero, the reply the slave's upper half. The words and replies are
 *   MultiPlay's but for the data block: each data transfer takes one encrypted 32-bit word,
 *   decrypted with Normal mode's CRC init, polynomial and key, and loads the low 16 bits of the
 *   next word's offset as its reply. The driver's ready gate (fake_bios_set_gate) decides what a
 *   slot does while the BIOS isn't in its loop (off, during the 2 startup transfers, once
 *   booted). Gated, the default: the slot is not ready: nothing clocked, MLINK_NO_DATA, its
 *   timeout and not-ready bits, and MLINK_NORMAL_STALL_TICKS ticks. Ungated (the runner's BIOS
 *   phase, multiboot-and-save.md §7.3): the slot is clocked all the same and reads 0xFFFF, SI
 *   pulled high, with no mask bit, in one tick. A delivered slot takes one tick.
 * Time: each slot starts when the one before it in the frame ends, and a count-0 frame is one idle
 * tick. rx->ticks is the frame's time in ticks.
 *
 * It refuses, as the BIOS does or more strictly:
 * - any word out of multiboot-and-save.md §2.2's sequence: 0x6102 not right after a 0x7202
 *   reply, a header that fails multiboot-and-save.md §2.1 (the logo's first word, 0x96, the
 *   complement; checked as the 96th halfword arrives), an undocumented palette byte, 0x64hh
 *   not right after a 0x73cc reply or with the wrong hh, a length outside 0x100..0x3FF40
 *   payload bytes or not a multiple of 16, 0x0066 not right after a 0x0075 reply, a CRC that
 *   differs from its own;
 * - a transfer that starts less than 36 us after the previous one ends (GBATEK "Multiboot
 *   Communication"), taking 430 us as the longest transfer;
 * - any transfer within 1/16 s after the 0x64hh transfer ends (the silent wait,
 *   multiboot-and-save.md §2.2 step 9);
 * - on the Normal wire, any word but a data word with a non-zero upper half ("upper half not
 *   zero").
 * Each refusal sets fake_bios_error and sends it back to detection with SEND 0x0000: how the real
 * BIOS recovers is not documented (multiboot-and-save.md M4). Not modelled: the full 156-byte
 * logo, the BIOS's own timeouts (multiboot-and-save.md M3), and anything after the boot
 * (transfers then time out). */

#define FAKE_BIOS_MEM_LEN 0x40000u /* 0x02000000..0x0203FFFF: header, then payload */

typedef enum {
    FakeBiosOff, /* every transfer times out (Normal: not ready, or 0xFFFF ungated) */
    FakeBiosWaiting, /* the BIOS waits for multiboot */
    FakeBiosBooted, /* it took an image; transfers time out (Normal: as off) */
} FakeBiosPhase;

typedef struct FakeBios FakeBios;

struct FakeBios {
    MlinkWire wire;
    bool gate; /* Normal: the driver's ready gate (fake_bios_set_gate) */
    FakeBiosPhase phase;
    uint8_t cc; /* its byte in 0x73cc */
    uint8_t rr; /* its byte in 0x73rr */
    uint32_t palette_busy_us; /* 0x7202, not 0x73cc, until this long after the 0x6202 transfer */
    uint32_t final_busy_us; /* 0x0074, not 0x0075, until this long after the first 0x0065 */
    uint32_t corrupt_at; /* transfer number from power-on (1 = the first), 0 = none */
    bool bad_crc;
    uint8_t step; /* where it is in multiboot-and-save.md §2.2's sequence */
    uint8_t startup; /* transfers that still time out (Normal: as off) after power-on */
    uint16_t send; /* SIOMULTI_SEND (Normal: the upper half of SIODATA32) */
    uint16_t prev_reply; /* the previous transfer's reply (FAKE_BIOS_NO_REPLY: none) */
    uint64_t busy_until_us; /* palette or final step: busy before this time */
    bool final_started;
    bool has_last;
    uint8_t pp;
    uint8_t hh;
    uint32_t count; /* header halfwords received */
    uint32_t len; /* file length from the length word */
    uint32_t pos; /* byte offset of the next payload halfword (Normal: word) */
    uint16_t lo; /* MultiPlay: low half of the word being received */
    uint16_t crc_final;
    uint32_t seed;
    uint32_t crc;
    uint64_t now_us;
    uint64_t last_end_us;
    uint64_t handshake_end_us;
    uint32_t transfers; /* since power-on, not-ready slots included */
    uint32_t boots;
    const char* error;
    uint8_t mem[FAKE_BIOS_MEM_LEN];
};

/* Starts Off, on the MultiPlay wire, gated. cc and rr are the slave's two random bytes. */
void fake_bios_init(FakeBios* b, uint8_t cc, uint8_t rr);
/* The wire the parent's driver clocks, from the next frame on. */
void fake_bios_set_wire(FakeBios* b, MlinkWire wire);
/* Normal wire: whether the driver waits for SI low before it clocks a slot (true, the default, as
 * mlink_hw_set_normal_gate), from the next frame on. MultiPlay ignores it. */
void fake_bios_set_gate(FakeBios* b, bool gate);
/* Power on holding Start+Select (or power-cycle): 2 transfers time out (Normal: as off), then
 * SEND 0x0000. */
void fake_bios_power_on(FakeBios* b);
/* How long the slave stays busy, in its own time, on every attempt: palette_us from the end of the
 * 0x6202 transfer (it loads 0x7202, not 0x73cc, after each palette word handled before then), and
 * final_us from the end of the first 0x0065 transfer (0x0074, not 0x0075). Time, not a count of
 * replies, so its meaning doesn't depend on how fast the parent polls. */
void fake_bios_set_busy(FakeBios* b, uint32_t palette_us, uint32_t final_us);
/* xor 0x0100 into the reply of that transfer (1 = the first after power-on, every slot counted,
 * not-ready ones included), on the wire only. */
void fake_bios_corrupt_reply(FakeBios* b, uint32_t transfer);
/* Its own CRC is off by one: in its reply and in its check. */
void fake_bios_force_bad_crc(FakeBios* b, bool on);
/* One driver frame: tx->count transfers (count 0: one idle tick, no transfer). rx->ticks gets the
 * frame's time: max(count, 1) on MultiPlay; on Normal 1 per delivered slot,
 * MLINK_NORMAL_STALL_TICKS per not-ready one, and 1 for an idle tick. tx->hi is read on Normal
 * only. */
void fake_bios_frame(FakeBios* b, const MlinkTx* tx, MlinkRx* rx);
FakeBiosPhase fake_bios_phase(const FakeBios* b);
/* Header and decrypted payload as received; *len is the length the last length word gave. */
const uint8_t* fake_bios_image(const FakeBios* b, uint32_t* len);
uint32_t fake_bios_boots(const FakeBios* b);
/* The last protocol violation, or NULL. */
const char* fake_bios_error(const FakeBios* b);

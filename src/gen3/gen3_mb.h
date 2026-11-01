#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_rules.h>

/* BIOS multiboot sender, the Flipper as the parent and the GBA's BIOS (held at Start+Select) as
 * child 1: GBATEK "BIOS Multi Boot (Single Game Pak)", as specialised in multiboot-and-save.md
 * §2.2-§2.6, on one of two wires, fixed at gen3_mb_init:
 * - MultiPlay (MlinkWireMulti): 16-bit words, the multiplayer parent.
 * - Normal (MlinkWireNormal, multiboot-and-save.md §7.2): the Normal-mode master. Every word is
 *   32 bits; the parent's value goes in the low half with the upper half zero (MlinkTx.hi left as
 *   the driver zeroed it), and the reply is the slave's upper half. The words and replies are
 *   MultiPlay's but for the data block (one encrypted 32-bit word per transfer, MlinkTx.word its
 *   low half and MlinkTx.hi its upper half, answered with its byte offset) and Normal's CRC and
 *   key constants.
 * Pure C, no heap, no file-scope state.
 *
 * ISR-safe: gen3_mb_frame runs in the driver's frame callback (stream mode). It and everything it
 * calls (gen3_mb_crypt_init, gen3_mb_crypt_word, gen3_mb_crypt_final, gen3_mb_crc_step,
 * gen3_mb_hh, gen3_rng_next) only compute: no allocation, logging, blocking or furi call, at most
 * MLINK_BURST_LEN words per frame and one CRC step per 32-bit payload word, no loop over the
 * image. gen3_mb_image_check and gen3_mb_init are app-context calls. */

#define GEN3_MB_CLIENT_BIT 0x02u /* child 1 */
#define GEN3_MB_PALETTE 0x93u /* logo palette: color 1, direction 0, speed 1 */
#define GEN3_MB_HEADER_LEN 0xC0u
#define GEN3_MB_MIN_LEN 0x1C0u
/* our helper cap (the helper is about 6.5-8.5 KB); the BIOS allows 0x40000. The helper's build
 * has its own copy, MAX_LEN in gba/helper/Makefile; tests/test_helper_image.c checks they agree. */
#define GEN3_MB_MAX_LEN 0x3000u
/* Counted failures allowed; one more and the sender gives up, to ask for a power cycle with
 * Start+Select (multiboot-and-save.md §2.6). How the BIOS recovers is not documented:
 * multiboot-and-save.md M4. */
#define GEN3_MB_MAX_RESTARTS 3u
/* Restart wait = GEN3_MB_RESTART_MIN_US + rng % (GEN3_MB_RESTART_RAND_US + 1)
 * (multiboot-and-save.md §2.4), at least GBATEK's 1/16 s. */
#define GEN3_MB_RESTART_MIN_US 70000u
#define GEN3_MB_RESTART_RAND_US 170000u
/* Detection replies that are neither 0x0000, 0xFFFF nor 0x7202 before a restart wait (not
 * counted). After a glitch the slave is still mid-transfer: it answers each 0x6200 with its own
 * header count or data offset until it has had every word it expects. A high limit lets it drain
 * at one word per tick instead of pausing every 16 words. GBATEK ("Repeat 15 times, if failed:
 * delay 1/16s and restart") and afska's LinkCableMultiboot give no reason for the pause and no
 * slave timeout, so pausing less often has no documented cost. 255 is the counter's limit. */
#define GEN3_MB_JUNK_LIMIT 255u
/* Total time for every attempt, from the first header word of the first one: past it the sender
 * gives up with Gen3MbFailPowerCycle. A slave left mid-transfer can take minutes to drain, while
 * turning the GBA off and on, the fallback (multiboot-and-save.md §2.6), takes seconds. Waiting
 * for the GBA before any header word doesn't count. */
#define GEN3_MB_ATTEMPT_US 20000000u
/* Detect and Palette poll back to back, one word per tick, as afska's LinkCableMultiboot does
 * (each try right after the previous reply). Only Final's 0x0065 retries wait one GBA frame
 * between polls, with idle ticks: afska retries those once per VBlank. */
#define GEN3_MB_POLL_US MLINK_FRAME_US
/* Palette: how long the slave may answer 0x7202 (busy) before 0x73cc
 * (multiboot-and-save.md §2.2 step 7). GBATEK sets no limit; afska gives up after 16
 * back-to-back tries. A wait that saw only valid 0x7202 replies (or, on the Normal wire gated,
 * not-ready slots) is a slow slave, not a broken link: it restarts without counting. */
#define GEN3_MB_PALETTE_US 4000000u
/* Silent wait before the length word: at least GBATEK's 1/16 s (multiboot-and-save.md §2.2
 * step 9); why 100 ms: multiboot-and-save.md M20 */
#define GEN3_MB_LENGTH_DELAY_US 100000u
/* Waiting for 0x0075 (multiboot-and-save.md §2.2 step 12) */
#define GEN3_MB_FINAL_US 5000000u

/* One CRC step: 32 rounds over data with poly, 0xA517 on MultiPlay (multiboot-and-save.md §2.3),
 * 0xC37B on Normal (§7.2). */
uint32_t gen3_mb_crc_step(uint32_t crc, uint32_t data, uint16_t poly);
/* The handshake byte for one child: (0x11 + cc + 0xFF + 0xFF) & 0xFF, absent children as 0xFF. */
uint8_t gen3_mb_hh(uint8_t cc);

typedef struct {
    uint32_t seed;
    uint32_t crc;
    uint32_t key; /* the data key */
    uint16_t poly; /* the CRC polynomial */
} Gen3MbCrypt;

/* seed = pp | cc << 8 | 0xFFFF0000 on both wires. The CRC init, polynomial and data key are the
 * wire's: MultiPlay 0xFFF8, 0xA517, 0x6465646F (multiboot-and-save.md §2.3); Normal 0xC387,
 * 0xC37B, 0x43202F2F (§7.2). */
void gen3_mb_crypt_init(Gen3MbCrypt* k, MlinkWire wire, uint8_t pp, uint8_t cc);
/* Plaintext word at byte offset ptr (0xC0, 0xC4, ...): CRC step, seed step, and the encrypted
 * word e (multiboot-and-save.md §2.3), sent on MultiPlay as e & 0xFFFF then e >> 16, on Normal in
 * one transfer. */
uint32_t gen3_mb_crypt_word(Gen3MbCrypt* k, uint32_t ptr, uint32_t plain);
/* The final CRC: gen3_mb_crc_step(crc, hh | rr << 8 | 0xFFFF0000, k->poly), 16 bits. */
uint16_t gen3_mb_crypt_final(const Gen3MbCrypt* k, uint8_t hh, uint8_t rr);

typedef enum {
    Gen3MbImageOk,
    Gen3MbImageBadLen,
    Gen3MbImageBadHeader,
    Gen3MbImageBadVersion,
} Gen3MbImageCheck;

/* In this order: len in [GEN3_MB_MIN_LEN, GEN3_MB_MAX_LEN] and a multiple of 16; the header (the
 * logo's first word le32(img + 4) == 0x51AEFF24, img[0xB2] == 0x96, img[0xBD] the complement of
 * 0xA0..0xBC, multiboot-and-save.md §2.1); img[0xBC] == version. */
Gen3MbImageCheck gen3_mb_image_check(const uint8_t* img, uint32_t len, uint8_t version);

typedef enum {
    Gen3MbPhaseWait, /* idle ticks: the restart wait */
    Gen3MbPhaseDetect,
    Gen3MbPhaseHello,
    Gen3MbPhaseHeader,
    Gen3MbPhasePalette,
    Gen3MbPhaseHandshake,
    Gen3MbPhaseDelay, /* idle ticks: the silent wait before the length word */
    Gen3MbPhaseLength,
    Gen3MbPhaseData,
    Gen3MbPhaseFinal,
    Gen3MbPhaseCrc,
    Gen3MbPhaseDone, /* the BIOS took the image: idle ticks from now on */
    /* too many counted failures, the attempt budget, or a bad length: idle ticks from now on */
    Gen3MbPhaseFailed,
} Gen3MbPhase;

/* The wire for the next attempt, plan 6 D6: si_low is SI as the cable check reads it now, phase
 * the sender's. MultiPlay stays while SI reads low, or once the GBA has answered (any phase but
 * Wait and Detect); SI high in Wait or Detect moves to Normal (a GBC cable, a reversed GBA cable,
 * or none); Normal stays. Pure. */
MlinkWire gen3_mb_wire_next(MlinkWire wire, bool si_low, Gen3MbPhase phase);

typedef enum {
    Gen3MbFailNone,
    Gen3MbFailHello,
    Gen3MbFailHeader,
    Gen3MbFailPalette,
    Gen3MbFailHandshake,
    Gen3MbFailLength,
    Gen3MbFailData,
    Gen3MbFailFinal,
    Gen3MbFailCrc,
    /* GEN3_MB_ATTEMPT_US ran out: turn the GBA off and on, holding Start+Select */
    Gen3MbFailPowerCycle,
} Gen3MbFail;

typedef struct {
    Gen3MbPhase phase;
    /* The latest failure, counted or not: a palette wait that saw only busy replies (0x7202, or
     * not ready) sets Palette */
    Gen3MbFail last_fail;
    uint8_t restarts; /* counted failures (all after detection succeeded) */
    /* The latest Header, Data, Handshake, Length or Crc failure: the word expected (Handshake and
     * Length: 0x7300, the high byte the reply needs) and the word that came (MLINK_NO_DATA for a
     * marked one); 0, 0 until one happens. Other failures keep them. */
    uint16_t fail_want;
    uint16_t fail_got;
    uint8_t cc; /* the slave's byte from its latest 0x73cc; 0 until one came */
    uint8_t rr; /* the slave's byte from its latest 0x73rr; 0 until one came */
    /* Every restart wait, counted or not (a detection drain, a palette wait that saw only busy
     * replies), up to UINT16_MAX */
    uint16_t all_restarts;
    /* Valid 0x7202 replies in the latest palette wait, and not-ready slots (Normal wire, gated),
     * up to UINT16_MAX: how long a slave took against GEN3_MB_PALETTE_US, at one reply a tick
     * (a not-ready slot takes MLINK_NORMAL_STALL_TICKS) */
    uint16_t palette_busy;
    /* The latest Final that got its 0x0075: the time from its first 0x0065 to the end of the frame
     * that 0x0075 came in, in link ticks (real time), against GEN3_MB_FINAL_US; 0 until one came */
    uint32_t final_us;
    uint32_t sent; /* payload bytes acknowledged (Normal: 4 per word) */
    uint32_t total; /* payload bytes to send: len - GEN3_MB_HEADER_LEN */
    uint32_t waiting_us; /* time in Wait and Detect since the last 0x7202 (UI hint after 3 s) */
    /* For the log, the first hardware test on a GBC cable: not-ready slots (Normal wire, gated)
     * since gen3_mb_init, up to UINT16_MAX, and the latest reply delivered (a slot neither timed
     * out, framing-errored nor not ready), MLINK_NO_DATA until one */
    uint16_t notready;
    uint16_t last_reply;
} Gen3MbStatus;

/* Holds a pointer to the caller's image; no heap. Fields are private to gen3_mb.c. */
typedef struct Gen3Mb {
    MlinkWire wire;
    const uint8_t* img;
    uint32_t len;
    Gen3Rng rng; /* restart waits */
    Gen3MbCrypt crypt;
    Gen3MbPhase phase;
    Gen3MbFail last_fail;
    uint8_t restarts;
    uint16_t fail_want;
    uint16_t fail_got;
    uint8_t cc; /* the slave's byte from 0x73cc */
    uint8_t rr; /* the slave's byte from 0x73rr */
    uint16_t all_restarts; /* every mb_wait */
    uint16_t palette_busy; /* busy replies since Palette was last entered */
    uint32_t final_us; /* Final: phase_us when the 0x0075 came */
    uint8_t junk; /* Detect: other replies since entering it */
    uint8_t tx_count; /* words in the frame in flight */
    bool stale; /* Detect: the next unmarked reply answers a word sent before Detect began */
    bool palette_other; /* Palette: a reply other than a busy one came back */
    bool attempting; /* a header word has gone out: the attempt budget runs */
    uint16_t pos; /* Header: next word of 98; Data: next payload unit (a halfword; Normal: a word) */
    uint16_t crc; /* Crc: the CRC sent */
    uint32_t e; /* Data (MultiPlay): the encrypted word whose high half goes next */
    uint32_t phase_us; /* time in this phase */
    uint32_t poll_us; /* Final: time since the last 0x0065 */
    uint32_t wait_us; /* Wait: this wait's length */
    uint32_t attempt_us; /* time since the first header frame started */
    uint32_t sent;
    uint32_t waiting_us;
    uint16_t notready; /* not-ready slots since init */
    uint16_t last_reply; /* the latest delivered reply */
} Gen3Mb;

/* Starts in Detect on wire, which the driver must clock; the first gen3_mb_frame call (rx NULL)
 * polls at once. img must pass gen3_mb_image_check and stay valid until Done or Failed. A len
 * outside GEN3_MB_MIN_LEN..GEN3_MB_MAX_LEN or not a multiple of 16 starts Failed, and nothing past
 * len is ever read. */
void gen3_mb_init(Gen3Mb* mb, MlinkWire wire, const uint8_t* img, uint32_t len, uint32_t seed);

/* One call per driver frame (ISR context). rx is the result of the previous tx (NULL on the first
 * call); elapsed_us is that frame's time (rx->ticks * MLINK_TICK_US; 0 on the first call). Fills
 * tx: count 0 (an idle tick), 1 (a poll or a handshake word) or up to MLINK_BURST_LEN (header and
 * data, checked reply by reply). On Normal it sets tx->hi only for data words and relies on the
 * driver having zeroed the rest. A not-ready slot is a marked word, as a timed-out one is, but
 * for the palette wait, where it is a busy slave like a valid 0x7202. */
void gen3_mb_frame(Gen3Mb* mb, const MlinkRx* rx, MlinkTx* tx, uint32_t elapsed_us);

/* Safe to call from a thread while the ISR runs: phase is one aligned field, written once per
 * change. The other fields may be read mid-update: they are progress, and the log's record of the
 * waits, only. */
void gen3_mb_status(const Gen3Mb* mb, Gen3MbStatus* out);

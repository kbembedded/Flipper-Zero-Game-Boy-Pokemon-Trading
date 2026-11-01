#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hp_cart.h"
#include "hp_flash.h"
#include "hp_link.h"

/* The helper's protocol core, portable C: the packet layer and the commands
 * (multiboot-and-save.md §6). The GBA's main loop feeds it one word per transfer; host tests do the
 * same. */

/* Commands (multiboot-and-save.md §6.3). ROM_SHA1 (0x21) isn't implemented, so its code is
 * unknown here. */
#define HP_CMD_HELLO 0x01u
#define HP_CMD_CART_INFO 0x02u
#define HP_CMD_SAVE_PROBE 0x03u
#define HP_CMD_ARM_WRITE 0x04u
#define HP_CMD_DISARM 0x05u
#define HP_CMD_READ_SECTOR 0x10u
#define HP_CMD_READ_FOOTERS 0x11u
#define HP_CMD_SECTOR_CRC32 0x12u
#define HP_CMD_ERASE_SECTOR 0x18u
#define HP_CMD_WRITE_SECTOR 0x19u
#define HP_CMD_COPY_SECTOR 0x1Au
#define HP_CMD_COMMIT_SECTOR 0x1Bu
#define HP_CMD_ROM_READ 0x20u
#define HP_CMD_ROM_CRC32 0x22u

/* Status codes (multiboot-and-save.md §6.4). A response with any status but OK carries two words:
 * a fail code and a fail offset. */
#define HP_ST_OK 0x00u
#define HP_ST_BAD_PACKET 0x01u
#define HP_ST_BAD_ARG 0x02u
#define HP_ST_UNSUPPORTED 0x03u
#define HP_ST_NOT_ARMED 0x04u
#define HP_ST_REFUSED 0x05u
#define HP_ST_ERASE_FAIL 0x06u
#define HP_ST_PROGRAM_FAIL 0x07u
#define HP_ST_VERIFY_FAIL 0x08u
#define HP_ST_NOT_ERASED 0x09u

/* REFUSED's fail code: which check refused, for the Flipper's log. Its fail offset is the sector
 * concerned, or 0. */
#define HP_REFUSE_CART 1u /* CART_INFO hasn't run, or the cart isn't a game the helper writes */
#define HP_REFUSE_CHIP 2u /* SAVE_PROBE hasn't run, or the chip isn't one the helper writes */
#define HP_REFUSE_ID 3u /* ARM_WRITE's flash id isn't the probed one */
#define HP_REFUSE_SLOT 4u /* the save's slots don't allow ARM_WRITE's target and mode */
#define HP_REFUSE_SECTOR 5u /* a sector outside the armed slot, Hall of Fame and after included */
#define HP_REFUSE_VALID 6u /* another sector of the armed slot still holds a valid signature */
#define HP_REFUSE_NOT_PROGRAMMED 7u /* COMMIT_SECTOR on a sector this arming didn't program */
#define HP_REFUSE_LAYOUT 8u /* the cart's game code has no save layout the helper knows */

/* ARM_WRITE's first word, "WR" */
#define HP_ARM_MAGIC 0x5752u
/* WRITE_SECTOR's and COPY_SECTOR's flags: leave 0xFF8 erased for COMMIT_SECTOR. Required: a write
 * without it is BAD_ARG. */
#define HP_FLAG_DEFER_COMMIT 0x0001u

/* CART_INFO's flags word */
#define HP_CART_LOGO_OK 0x0001u
#define HP_CART_FIXED_OK 0x0002u
#define HP_CART_COMPLEMENT_OK 0x0004u
#define HP_CART_OTHER_LIB 0x0008u
#define HP_CART_WRITE_GAME 0x0010u

/* hp_step flags */
#define HP_STEP_ERR 1u /* SIOCNT's error bit was set for this word */
#define HP_STEP_TIMEOUT 2u /* no transfer for 200 ms (TM3 >= 196): `in` is not a word */

typedef struct Hp Hp;

struct Hp {
    /* Receiving (multiboot-and-save.md §6.2) */
    uint16_t rx_pos; /* 0: hunting for a SOF; k: the next word is the packet's w(k) */
    uint16_t rx_n; /* its payload length, from w2 */
    uint32_t rx_crc; /* CRC-32 register over w1 and on */
    uint16_t rx_crc_lo; /* its CRC's low half */
    uint8_t seq; /* from w1 */
    uint8_t code; /* from w1 */
    bool pending; /* a valid packet waits for hp_execute */
    uint16_t req[HP_MAX_PAYLOAD]; /* its payload */
    /* Sending */
    uint16_t tx_len; /* words of the response going out; 0: none */
    uint16_t tx_pos; /* the next of them for hp_step */
    uint16_t resp[3u + HP_MAX_PAYLOAD + 2u]; /* SOF, seq << 8 | status, n, payload, CRC */
    /* What the cart checks found (multiboot-and-save.md §6.3): CART_INFO scans again each time;
     * SAVE_PROBE probes once, and later calls answer from here. */
    HpCart cart;
    bool cart_scanned;
    HpProbe probe;
    bool probed;
    /* The sectors this session erased OK and hasn't programmed since; HELLO and ARM_WRITE start
     * afresh */
    HpFlash flash;
    /* ARM_WRITE allowed writes to the 14 sectors of slot arm_slot (multiboot-and-save.md §6.3).
     * Bit k of each mask is sector 14 * arm_slot + k, since the arming: */
    bool armed;
    uint8_t arm_slot;
    uint16_t programmed; /* a deferred WRITE or COPY wrote it OK, and nothing erased or wrote it
                          * since: COMMIT_SECTOR may complete it */
    uint16_t completed; /* COMMIT_SECTOR gave it its valid signature, and nothing erased or wrote
                         * it since */
    /* WRITE_SECTOR's image, COPY_SECTOR's copy, READ_SECTOR's data */
    uint8_t sector_buf[HP_FLASH_SECTOR_LEN];
    /* The last packet executed (plan 5 D3): its w1 (seq, code), its CRC, and its response's
     * length; the response itself stays in resp until the next one executes */
    bool last_valid;
    uint16_t last_w1;
    uint32_t last_crc;
    uint16_t last_len;
};

void hp_init(Hp* hp);

/* in is P's word of the transfer that just ended; returns H's word for the next transfer:
 * - while a response goes out, its next word, whatever in is: seq << 8 | status, n, the payload,
 *   CRC low, CRC high; then hunting again. HP_STEP_TIMEOUT ends the response early;
 * - HP_READY while hunting, and HP_RECV from the SOF to the packet's second-to-last word;
 * - after the last word of a valid packet, HP_BUSY: hp_exec_pending is then true, and the caller
 *   must call hp_execute before the next hp_step.
 * A packet is dropped silently, and hunting resumes with the next word, on a word flagged
 * HP_STEP_ERR or HP_STEP_TIMEOUT, at w2 if the code is unknown or n isn't the command's request
 * length, and on a CRC mismatch. A HP_SOF as w1 or w2, which no valid packet has there, starts a
 * new packet instead. */
uint16_t hp_step(Hp* hp, uint16_t in, uint8_t flags);
bool hp_exec_pending(const Hp* hp);
/* Runs the pending command and queues its response; returns the response's first word, HP_SOF.
 * A write-class command (ARM_WRITE, ERASE_SECTOR .. COMMIT_SECTOR) with the same seq, code and
 * packet CRC as the last packet executed is not run again: P resent it because the response went
 * missing, and gets that response again (plan 5 D3). */
uint16_t hp_execute(Hp* hp);

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The save chip: the probe (multiboot-and-save.md §3.3 steps 2-3, §3.5) and the 128 KB flash
 * routines (multiboot-and-save.md §3.4, §4.8), which follow pret's agb_flash code. Every function
 * here runs from IWRAM on the GBA.
 *
 * The erase, program and commit routines change the save. They check nothing about the cart: the
 * caller arms them only for a cart the probe and the cart checks accept. They never chip-erase.
 * Every routine after the probe, reads included, starts with the bank command, which would land as
 * data in an SRAM: call them only once the probe has said HpChipFlash128. */

#define HP_FLASH_SECTOR_LEN 4096u
#define HP_FLASH_SECTORS 32u

/* The 128 KB chips the games accept (pokeruby: src/libs/agb_flash_1m.c: sSetupInfos), and the only
 * ones the helper writes. The game also takes 0xD4BF as a 128 KB chip on German Sapphire version 0,
 * which is never written. */
#define HP_ID_MX 0x09C2u /* MX29L010 */
#define HP_ID_SANYO 0x1362u /* LE26FV10N1TS */

/* SAVE_PROBE's class (multiboot-and-save.md §6.3); the values are its wire values. */
typedef enum {
    HpChipFlash128, /* a 128 KB MX-style flash the game itself accepts */
    HpChipFlash64, /* a 64 KB flash: not a Pokemon cart */
    HpChipSram, /* the SRAM probe saw its write land: SRAM, FRAM or a reproduction */
    HpChipNone, /* no flash ID, or ID mode changed nothing: no chip, EEPROM, open bus or a repro */
} HpChip;

typedef struct {
    uint16_t id; /* device << 8 | maker, as read in ID mode */
    HpChip cls;
    uint8_t pre0, pre1; /* bytes 0 and 1 before ID mode */
    uint8_t id0, id1; /* bytes 0 and 1 in ID mode: maker, device */
    bool sram; /* the SRAM probe was positive; no flash command was sent */
} HpProbe;

/* Runs the SRAM probe, then, only if it is negative, reads pre0/pre1 and the flash ID with pret's
 * sequence. code and version are the cart header's game code (0xAC..0xAF) and version (0xBC):
 * ID 0xD4BF is a 128 KB chip only on German Sapphire version 0, as the game says. */
void hp_flash_probe(HpProbe* out, const uint8_t code[4], uint8_t version);

typedef struct {
    uint8_t status; /* HP_ST_* (hp_core.h) */
    uint16_t code; /* the pret fail code: phase | 0xA000 (DQ5) or phase | 0xC000 (time out) */
    uint16_t off; /* the offset in the sector that failed */
} HpFlashResult;

/* What a session knows about the chip. A sector reading all 0xFF may still be half-erased (an erase
 * cut short by a power loss), so a write skips its erase only for a sector this session erased OK
 * and hasn't programmed since. Zero at the start of a session: hp_init and HELLO clear it. */
typedef struct {
    uint32_t erased_ok; /* bit n: sector n was erased OK and nothing has been programmed into it */
} HpFlash;

/* Sector n (0..31) is bank n >> 4, at window offset (n & 15) << 12. Every routine selects the bank
 * itself, every time, and the erase before each of its tries. The status backdrop is red while the
 * chip is being changed, then yellow. */

/* Erases one sector, with up to 5 tries: OK, which sets its erased_ok bit, or ERASE_FAIL with the
 * last try's code, which leaves the bit clear. */
HpFlashResult hp_flash_erase(HpFlash* fl, uint8_t sector);
/* Writes img into one sector, with up to 3 tries of: erase (skipped only if the sector's erased_ok
 * bit is set, and only on the first try), program every byte in ascending order (with
 * defer_commit, all but 0xFF8, which stays 0xFF), then verify. Clears the sector's bit. OK, or the
 * last try's ERASE_FAIL (code), PROGRAM_FAIL (code, offset) or VERIFY_FAIL (offset). */
HpFlashResult
    hp_flash_program(HpFlash* fl, uint8_t sector, const uint8_t img[4096], bool defer_commit);
/* Programs 0x25 at 0xFF8, completing the signature 0x08012025, only if 0xFF8 reads 0xFF
 * (else NOT_ERASED) and 0xFF9..0xFFB read 20 01 08 (else BAD_ARG); then checks 0xFF8..0xFFB:
 * PROGRAM_FAIL or VERIFY_FAIL with the offset. Clears the sector's erased_ok bit. */
HpFlashResult hp_flash_commit(HpFlash* fl, uint8_t sector);
/* Reads len bytes from off in a sector; nothing unless sector < 32 and off + len <= 4096. */
void hp_flash_read(uint8_t sector, uint16_t off, uint8_t* out, uint16_t len);
/* The CRC-32 (IEEE) of a sector's 4096 bytes; 0 for a sector over 31. */
uint32_t hp_flash_crc32(uint8_t sector);

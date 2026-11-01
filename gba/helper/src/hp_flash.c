#include "hp_flash.h"
#include "hp_core.h"
#include "hp_hal.h"
#include "hp_link.h"

/* The save chip's command sequences, polling, retries and timeouts follow pret's agb_flash code,
 * which the games themselves run (pokeruby: src/libs/agb_flash.c, agb_flash_1m.c, agb_flash_mx.c;
 * em: and fr: src/agb_flash*.c are the same), and multiboot-and-save.md §3.4 and §4.8. Every
 * function is HP_IWRAM: on the GBA the code runs from RAM, as reading the flash requires
 * (multiboot-and-save.md §4.6), and in ARM state with interrupts off for the whole helper
 * (multiboot-and-save.md §4.7). */

/* 32 sectors in 2 banks of 16 (pokeruby: include/gba/flash_internal.h: SECTORS_PER_BANK) */
#define HP_FLASH_BANK_SECTORS 16u

/* The command addresses (pokeruby: src/libs/agb_flash.c: SwitchFlashBank, ReadFlashId) */
#define HP_FLASH_CMD1 0x5555u
#define HP_FLASH_CMD2 0x2AAAu

/* Command bytes (multiboot-and-save.md §3.4) */
#define HP_FLASH_ID_ENTER 0x90u
#define HP_FLASH_RESET 0xF0u
#define HP_FLASH_ERASE 0x80u
#define HP_FLASH_ERASE_SECTOR 0x30u
#define HP_FLASH_PROGRAM 0xA0u
#define HP_FLASH_BANK 0xB0u

/* The wait phases and their limits in 1.0224 ms timer units, from mxMaxTime (pokeruby:
 * src/libs/agb_flash_mx.c), which leMaxTime (agb_flash_le.c) repeats: phase 1 byte program 10,
 * phase 2 sector erase 2000. */
#define HP_PHASE_PROGRAM 1u
#define HP_PHASE_ERASE 2u
#define HP_LIMIT_PROGRAM 10u
#define HP_LIMIT_ERASE 2000u

/* WaitForFlashWrite_Common (pokeruby: src/libs/agb_flash_1m.c): DQ5 "exceeded time limit", and
 * the fail codes it returns */
#define HP_FLASH_DQ5 0x20u
#define HP_FAIL_DQ5 0xA000u
#define HP_FAIL_TIMER 0xC000u
#define HP_FAIL_RETRY 0xA000u /* EraseFlashSector_MX tries again while result & 0xA000 */

#define HP_ERASE_TRIES 5u /* EraseFlashSector_MX: numTries 0..4 (it stops at numTries > 3) */
#define HP_PROGRAM_TRIES 3u /* ProgramFlashSectorAndVerify */

/* The sector footer's signature 0x08012025 at 0xFF8 (pokeruby: src/save.c: FILE_SIGNATURE); a
 * trade save programs its first byte last (pokeruby: src/save.c:
 * WriteSomeFlashByte0x25ToPrevSector). */
#define HP_SIG_OFF 0xFF8u
#define HP_SIG0 0x25u
#define HP_SIG1 0x20u
#define HP_SIG2 0x01u
#define HP_SIG3 0x08u

/* Besides HP_ID_MX and HP_ID_SANYO (hp_flash.h), the IDs the probe knows: the 64 KB ones
 * (multiboot-and-save.md §3.4) */
#define HP_ID_SST 0xD4BFu /* also German Sapphire version 0's MX29L010 entry (agb_flash_mx.c) */
#define HP_ID_MX64 0x1CC2u
#define HP_ID_PANASONIC 0x1B32u
#define HP_ID_ATMEL 0x3D1Fu

/* Three writes of a command (pokeruby: src/libs/agb_flash.c: SwitchFlashBank, ReadFlashId). */
HP_IWRAM static void hp_flash_cmd(uint8_t c) {
    hp_hal_flash_write(HP_FLASH_CMD1, 0xAA);
    hp_hal_flash_write(HP_FLASH_CMD2, 0x55);
    hp_hal_flash_write(HP_FLASH_CMD1, c);
}

/* Selects a sector's bank (pokeruby: src/libs/agb_flash.c: SwitchFlashBank) and returns its
 * window offset. */
HP_IWRAM static uint16_t hp_flash_bank(uint8_t sector) {
    hp_flash_cmd(HP_FLASH_BANK);
    hp_hal_flash_write(0x0000, (uint8_t)(sector / HP_FLASH_BANK_SECTORS));
    return (uint16_t)((sector % HP_FLASH_BANK_SECTORS) * HP_FLASH_SECTOR_LEN);
}

/* Polls off until it reads want (pokeruby: src/libs/agb_flash_1m.c: WaitForFlashWrite_Common).
 * 0, or phase | 0xA000 (DQ5) or phase | 0xC000 (timer), after a reset to read mode. */
HP_IWRAM static uint16_t
    hp_flash_wait(uint16_t off, uint8_t want, uint16_t limit, uint16_t phase) {
    hp_hal_timer_start();
    for(;;) {
        const uint8_t s = hp_hal_flash_read(off);
        if(s == want) return 0;
        if(s & HP_FLASH_DQ5) {
            if(hp_hal_flash_read(off) == want) return 0;
            hp_hal_flash_write(HP_FLASH_CMD1, HP_FLASH_RESET);
            return (uint16_t)(phase | HP_FAIL_DQ5);
        }
        if(hp_hal_timer_units() >= limit) {
            if(hp_hal_flash_read(off) == want) return 0;
            hp_hal_flash_write(HP_FLASH_CMD1, HP_FLASH_RESET);
            return (uint16_t)(phase | HP_FAIL_TIMER);
        }
    }
}

/* pokeruby: src/libs/agb_flash_mx.c: EraseFlashSector_MX: up to 5 tries of the sector erase,
 * polled at the sector's first byte. pret selects the bank once, before the tries; here it is
 * selected before each try, so nothing depends on whether the F0 reset after a failed try keeps
 * it. 0 or the last try's fail code. */
HP_IWRAM static uint16_t hp_flash_erase_sector(uint8_t sector) {
    uint16_t r = 0;
    for(uint32_t t = 0; t < HP_ERASE_TRIES; t++) {
        const uint16_t base = hp_flash_bank(sector);
        hp_flash_cmd(HP_FLASH_ERASE);
        hp_hal_flash_write(HP_FLASH_CMD1, 0xAA);
        hp_hal_flash_write(HP_FLASH_CMD2, 0x55);
        hp_hal_flash_write(base, HP_FLASH_ERASE_SECTOR);
        r = hp_flash_wait(base, 0xFF, HP_LIMIT_ERASE, HP_PHASE_ERASE);
        if(!(r & HP_FAIL_RETRY)) break;
    }
    return r;
}

/* pokeruby: src/libs/agb_flash_mx.c: ProgramByte. The bank is already selected. */
HP_IWRAM static uint16_t hp_flash_program_byte(uint16_t off, uint8_t v) {
    hp_flash_cmd(HP_FLASH_PROGRAM);
    hp_hal_flash_write(off, v);
    return hp_flash_wait(off, v, HP_LIMIT_PROGRAM, HP_PHASE_PROGRAM);
}

HP_IWRAM static HpFlashResult hp_flash_result(uint8_t status, uint16_t code, uint16_t off) {
    HpFlashResult r;
    r.status = status;
    r.code = code;
    r.off = off;
    return r;
}

/* One try of hp_flash_program: ProgramFlashSector_MX (pokeruby: src/libs/agb_flash_mx.c), which
 * erases first and then programs every byte in ascending order, stopping at the first failure.
 * The erase is skipped only for a sector this session erased OK and hasn't programmed since; with
 * defer the byte at 0xFF8 is left erased, as WriteSingleChunk does for a trade save (pokeruby:
 * src/save.c). Then VerifyFlashSector (pokeruby: src/libs/agb_flash.c), with 0xFF8 expected
 * erased. */
HP_IWRAM static HpFlashResult
    hp_flash_program_try(HpFlash* fl, uint8_t sector, const uint8_t* img, bool defer) {
    const uint32_t bit = 1u << sector;
    if(!(fl->erased_ok & bit)) {
        const uint16_t code = hp_flash_erase_sector(sector);
        if(code) return hp_flash_result(HP_ST_ERASE_FAIL, code, 0);
    }
    fl->erased_ok &= ~bit;
    const uint16_t base = hp_flash_bank(sector);
    for(uint32_t i = 0; i < HP_FLASH_SECTOR_LEN; i++) {
        if(defer && i == HP_SIG_OFF) continue;
        const uint16_t code = hp_flash_program_byte((uint16_t)(base + i), img[i]);
        if(code) return hp_flash_result(HP_ST_PROGRAM_FAIL, code, (uint16_t)i);
    }
    for(uint32_t i = 0; i < HP_FLASH_SECTOR_LEN; i++) {
        const uint8_t want = (defer && i == HP_SIG_OFF) ? 0xFF : img[i];
        if(hp_hal_flash_read((uint16_t)(base + i)) != want) {
            return hp_flash_result(HP_ST_VERIFY_FAIL, 0, (uint16_t)i);
        }
    }
    return hp_flash_result(HP_ST_OK, 0, 0);
}

HP_IWRAM HpFlashResult hp_flash_erase(HpFlash* fl, uint8_t sector) {
    if(sector >= HP_FLASH_SECTORS) return hp_flash_result(HP_ST_BAD_ARG, 0, 0);
    const uint32_t bit = 1u << sector;
    fl->erased_ok &= ~bit;
    hp_hal_status(HpStatusWriting);
    const uint16_t code = hp_flash_erase_sector(sector);
    hp_hal_status(HpStatusBusy);
    if(code) return hp_flash_result(HP_ST_ERASE_FAIL, code, 0);
    fl->erased_ok |= bit;
    return hp_flash_result(HP_ST_OK, 0, 0);
}

/* pokeruby: src/libs/agb_flash.c: ProgramFlashSectorAndVerify: up to 3 tries. A failed try has
 * programmed the sector, so the next one erases it again. */
HP_IWRAM HpFlashResult
    hp_flash_program(HpFlash* fl, uint8_t sector, const uint8_t img[4096], bool defer_commit) {
    if(sector >= HP_FLASH_SECTORS) return hp_flash_result(HP_ST_BAD_ARG, 0, 0);
    hp_hal_status(HpStatusWriting);
    HpFlashResult r = hp_flash_result(HP_ST_OK, 0, 0);
    for(uint32_t t = 0; t < HP_PROGRAM_TRIES; t++) {
        r = hp_flash_program_try(fl, sector, img, defer_commit);
        if(r.status == HP_ST_OK) break;
    }
    hp_hal_status(HpStatusBusy);
    return r;
}

/* The deferred byte, as WriteSomeFlashByte0x25ToPrevSector programs it (pokeruby: src/save.c),
 * only onto an erased byte that completes the signature. */
HP_IWRAM HpFlashResult hp_flash_commit(HpFlash* fl, uint8_t sector) {
    static const uint8_t sig[4] = {HP_SIG0, HP_SIG1, HP_SIG2, HP_SIG3};
    if(sector >= HP_FLASH_SECTORS) return hp_flash_result(HP_ST_BAD_ARG, 0, 0);
    fl->erased_ok &= ~(1u << sector);
    const uint16_t at = (uint16_t)(hp_flash_bank(sector) + HP_SIG_OFF);
    if(hp_hal_flash_read(at) != 0xFF) return hp_flash_result(HP_ST_NOT_ERASED, 0, HP_SIG_OFF);
    for(uint16_t i = 1; i < 4; i++) {
        if(hp_hal_flash_read((uint16_t)(at + i)) != sig[i]) {
            return hp_flash_result(HP_ST_BAD_ARG, 0, (uint16_t)(HP_SIG_OFF + i));
        }
    }
    hp_hal_status(HpStatusWriting);
    const uint16_t code = hp_flash_program_byte(at, HP_SIG0);
    hp_hal_status(HpStatusBusy);
    if(code) return hp_flash_result(HP_ST_PROGRAM_FAIL, code, HP_SIG_OFF);
    for(uint16_t i = 0; i < 4; i++) {
        if(hp_hal_flash_read((uint16_t)(at + i)) != sig[i]) {
            return hp_flash_result(HP_ST_VERIFY_FAIL, 0, (uint16_t)(HP_SIG_OFF + i));
        }
    }
    return hp_flash_result(HP_ST_OK, 0, 0);
}

HP_IWRAM void hp_flash_read(uint8_t sector, uint16_t off, uint8_t* out, uint16_t len) {
    if(sector >= HP_FLASH_SECTORS || (uint32_t)off + len > HP_FLASH_SECTOR_LEN) return;
    const uint16_t base = hp_flash_bank(sector);
    for(uint32_t i = 0; i < len; i++) out[i] = hp_hal_flash_read((uint16_t)(base + off + i));
}

HP_IWRAM uint32_t hp_flash_crc32(uint8_t sector) {
    if(sector >= HP_FLASH_SECTORS) return 0;
    const uint16_t base = hp_flash_bank(sector);
    uint8_t chunk[16];
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < HP_FLASH_SECTOR_LEN; i += sizeof(chunk)) {
        for(uint32_t j = 0; j < sizeof(chunk); j++) {
            chunk[j] = hp_hal_flash_read((uint16_t)(base + i + j));
        }
        crc = hp_crc32_bytes(crc, chunk, sizeof(chunk));
    }
    return ~crc;
}

/* 0xD4BF is the MX29L010 ID of German Sapphire version 0 only (pokeruby: src/libs/agb_flash_mx.c:
 * MX29L010, #if GERMAN && SAPPHIRE && REVISION == 0); elsewhere it is SST's 64 KB part. */
HP_IWRAM static bool hp_flash_sapphire_de0(const uint8_t code[4], uint8_t version) {
    return code[0] == 'A' && code[1] == 'X' && code[2] == 'P' && code[3] == 'D' && version == 0;
}

/* multiboot-and-save.md §3.3 steps 2-3. */
HP_IWRAM void hp_flash_probe(HpProbe* out, const uint8_t code[4], uint8_t version) {
    out->id = 0;
    out->cls = HpChipNone;
    out->pre0 = 0;
    out->pre1 = 0;
    out->id0 = 0;
    out->id1 = 0;
    out->sram = false;
    /* The SRAM probe, before any command: on SRAM a command sequence would land as data. One lone
     * write that is never 0xAA, the first byte of every sequence, so a flash chip ignores it. */
    const uint8_t a = hp_hal_flash_read(HP_FLASH_CMD1);
    uint8_t v = (uint8_t)(a ^ 0xFFu);
    if(v == 0xAA) v = (uint8_t)(a ^ 0x0Fu);
    hp_hal_flash_write(HP_FLASH_CMD1, v);
    if(hp_hal_flash_read(HP_FLASH_CMD1) == v) {
        hp_hal_flash_write(HP_FLASH_CMD1, a);
        out->sram = true;
        out->cls = HpChipSram;
        return;
    }
    out->pre0 = hp_hal_flash_read(0x0000);
    out->pre1 = hp_hal_flash_read(0x0001);
    /* pokeruby: src/libs/agb_flash.c: ReadFlashId: device byte, then maker; leave with 0xF0
     * twice */
    hp_flash_cmd(HP_FLASH_ID_ENTER);
    hp_hal_id_delay();
    out->id1 = hp_hal_flash_read(0x0001);
    out->id0 = hp_hal_flash_read(0x0000);
    hp_flash_cmd(HP_FLASH_RESET);
    hp_hal_flash_write(HP_FLASH_CMD1, HP_FLASH_RESET);
    hp_hal_id_delay();
    out->id = (uint16_t)(out->id1 << 8 | out->id0);
    /* An array that reads the same in ID mode is no flash chip, whatever its bytes spell. */
    if(out->id0 == out->pre0 && out->id1 == out->pre1) return;
    switch(out->id) {
    case HP_ID_MX:
    case HP_ID_SANYO:
        out->cls = HpChipFlash128;
        break;
    case HP_ID_SST:
        out->cls = hp_flash_sapphire_de0(code, version) ? HpChipFlash128 : HpChipFlash64;
        break;
    case HP_ID_MX64:
    case HP_ID_PANASONIC:
    case HP_ID_ATMEL:
        out->cls = HpChipFlash64;
        break;
    default:
        break;
    }
}

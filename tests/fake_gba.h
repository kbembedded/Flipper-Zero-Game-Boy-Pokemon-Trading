#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <hp_hal.h>

/* Test-only model of the GBA the helper's portable core runs on, behind hp_hal.h: its own header,
 * the cart ROM, the save chip, the timer and the status backdrop. The hp_hal_* functions act on
 * the FakeGba that fake_gba_init initialised last. It shares nothing with the Flipper's code.
 * Never a stack local in a test (128 KB of flash): static or heap.
 *
 * The flash chip is a JEDEC command state machine (multiboot-and-save.md §3.4), written to be no
 * more lenient than a real part:
 * - AA@5555, 55@2AAA, then cmd@5555: 0x90 ID mode, 0xF0 read mode, 0x80 (then AA@5555, 55@2AAA
 *   and 30 at a sector's first byte, or 10@5555 for a chip erase), 0xA0 (the next write programs
 *   one byte: new = old & v) and 0xB0 (the next write, at 0000, selects bank 0 or 1);
 * - 0xF0 written anywhere returns it to read mode (from ID mode or a failure too); any other write
 *   outside a sequence is ignored, and a wrong write inside one ends the sequence
 *   (multiboot-and-save.md M8);
 * - in ID mode, 0000 reads the maker and 0001 the device, but only after hp_hal_id_delay;
 * - erase and program complete at once. An injected failure leaves the bytes unchanged and puts
 *   the chip in its status mode until a 0xF0: every read then returns the old byte with DQ5 (bit 5)
 *   set and DQ7 (bit 7) the complement of the data being written, as JEDEC data polling does, so a
 *   status read never equals the expected byte.
 * Command sequences are decoded, and counted, for every kind of save chip; only FakeSaveFlash acts
 * on them. */

#define FAKE_GBA_OWN_LEN 0x100u
#define FAKE_GBA_HEADER_LEN 0xC0u
#define FAKE_GBA_FLASH_LEN 131072u
#define FAKE_GBA_SRAM_LEN 65536u
#define FAKE_GBA_SECTORS 32u
#define FAKE_GBA_SECTOR_LEN 4096u
#define FAKE_GBA_STRINGS 8u
#define FAKE_GBA_STRING_MAX 16u
#define FAKE_GBA_TRACE 64u

typedef enum {
    FakeSaveFlash, /* a JEDEC flash chip with the id given to fake_gba_save */
    FakeSaveSram, /* a plain 64 KB SRAM: every write lands */
    FakeSaveNone, /* nothing: reads 0xFF, writes vanish */
    FakeSaveRomLike, /* reads the array, ignores every write: ID mode changes nothing */
} FakeSaveKind;

typedef enum {
    FakeGbaOpWrite, /* hp_hal_flash_write(off, val) */
    FakeGbaOpRead, /* hp_hal_flash_read(off) returned val */
    FakeGbaOpDelay, /* hp_hal_id_delay() */
} FakeGbaOpKind;

typedef struct {
    FakeGbaOpKind kind;
    uint16_t off;
    uint8_t val;
} FakeGbaOp;

typedef struct {
    uint32_t erases; /* sector erase commands, failed ones included */
    uint32_t programs; /* byte program commands, commit bytes included */
    uint32_t commits; /* byte programs of a sector's offset 0xFF8, its signature's first byte */
    uint32_t bank_selects;
    uint32_t chip_erases;
    uint32_t sram_writes; /* writes that landed in a FakeSaveSram */
    uint32_t id_entries; /* ID-mode commands */
} FakeGbaCounts;

typedef struct FakeGba FakeGba;

struct FakeGba {
    /* The helper's image from 0x02000000: the header at 0x00..0xBF, then the multiboot entries,
     * where the BIOS writes the boot mode at 0xC4 and the slave id at 0xC5 */
    uint8_t own[FAKE_GBA_OWN_LEN];
    HpStatus status; /* the last hp_hal_status */

    /* Cart ROM: the header, then the pattern halfword ((off * 31) >> 3) & 0xFFFF at even off,
     * with the strings laid over it; past rom_size, open bus (off / 2) & 0xFFFF, or with
     * rom_mirror the ROM again */
    uint8_t header[FAKE_GBA_HEADER_LEN];
    uint32_t rom_size; /* 0: no cart */
    bool rom_mirror;
    uint32_t strings;
    uint32_t string_off[FAKE_GBA_STRINGS];
    uint8_t string_len[FAKE_GBA_STRINGS];
    char string[FAKE_GBA_STRINGS][FAKE_GBA_STRING_MAX];
    bool rom_glitch; /* the next read of the halfword at rom_glitch_off comes back XOR 0x0001 */
    uint32_t rom_glitch_off;

    /* The save chip */
    FakeSaveKind save;
    uint16_t chip_id; /* device << 8 | maker */
    uint8_t flash[FAKE_GBA_FLASH_LEN]; /* Flash: 2 banks of 64 KB; Sram, RomLike: the first 64 KB */
    uint8_t bank; /* tests may set it, to leave the chip in a bank */
    uint8_t mode; /* read, ID or status (after a failure) */
    uint8_t seq; /* how far a command sequence has got */
    bool id_settling; /* ID mode was entered and hp_hal_id_delay hasn't run since */
    uint8_t fail_want; /* in status mode: the byte the failed operation was writing */
    uint8_t fail_erase[FAKE_GBA_SECTORS];
    uint8_t fail_program_sector;
    uint16_t fail_program_off;
    uint8_t fail_program_times;
    uint8_t weak_sector;
    uint16_t weak_off;
    uint8_t weak_times;
    bool weak_poll; /* the next read at weak_poll_off returns weak_poll_val: the poll sees done */
    uint8_t weak_poll_bank;
    uint16_t weak_poll_off;
    uint8_t weak_poll_val;
    bool cut_armed;
    uint32_t cut_ops; /* erases + byte programs still allowed before the chip freezes */
    bool cut_partial; /* the operation the cut freezes is half done (fake_gba_power_cut_partial) */
    bool frozen; /* the power cut came */
    uint8_t cut_want; /* the data of the operation the cut froze */
    uint8_t cut_toggle;
    /* A sector whose erase a partial cut froze: it reads all 0xFF, but weak_old's 0 bits are
     * still weakly there until a whole erase of it. A program into it meanwhile marks it
     * weak_programmed: then the next power cycle lets those bits read 0 again. */
    bool weak_erase;
    uint8_t weak_erase_sector;
    bool weak_programmed;
    uint8_t weak_old[FAKE_GBA_SECTOR_LEN];
    uint8_t late_reads; /* tests set it: the next erase or program to complete shows this many
                         * status reads, DQ5 set, before its data, as if it finished just as DQ5
                         * rose */
    uint8_t late_left;
    uint8_t late_want;

    /* Observations */
    FakeGbaCounts counts;
    uint32_t ops_not_red; /* erases and byte programs issued while the status wasn't Writing */
    uint16_t last_write_off;
    uint8_t last_write_val;
    uint32_t trace_len; /* tests zero it to start a trace; the first FAKE_GBA_TRACE ops are kept */
    bool trace_no_reads; /* trace writes and delays only */
    FakeGbaOp trace[FAKE_GBA_TRACE];

    /* Time */
    uint32_t timer_reads; /* flash reads since hp_hal_timer_start */
    uint64_t busy_us;
    uint64_t rom_bytes; /* ROM bytes read since the last fake_gba_take_busy_us */
};

/* The own header gets a test logo at 0x04..0x9F (a fixed 156-byte pattern, not the real logo, but
 * starting with its first word 0x51AEFF24), 3 (MultiPlay) at 0xC4 and 1 (slave id) at 0xC5; the
 * status is Ready. No cart (every ROM read is open bus) and no save chip (FakeSaveNone); the flash
 * array is all 0xFF. */
void fake_gba_init(FakeGba* g);
FakeGba* fake_gba_current(void);

/* Inserts a cart: its 0xC0-byte header, then the pattern up to rom_size; clears the strings. */
void fake_gba_rom(FakeGba* g, const uint8_t header[0xC0], uint32_t rom_size);
/* Places a library string (without its NUL) at off, over the pattern. */
void fake_gba_rom_string(FakeGba* g, uint32_t off, const char* s);
/* The next read of the ROM halfword at off (even), by hp_hal_rom16 or as half of an hp_hal_rom32,
 * returns it XORed with 0x0001, as a bus glitch would; later reads are right again. */
void fake_gba_rom_glitch_once(FakeGba* g, uint32_t off);
/* Sets the save chip, with id = device << 8 | maker for FakeSaveFlash, in read mode and bank 0.
 * The array is left as it is. */
void fake_gba_save(FakeGba* g, FakeSaveKind kind, uint16_t id);
/* The array: 131072 bytes (Flash) or 65536 (Sram, RomLike) */
uint8_t* fake_gba_flash(FakeGba* g);
/* The next `times` erase commands for that sector fail. */
void fake_gba_fail_erase(FakeGba* g, uint8_t sector, uint8_t times);
/* The next `times` byte programs at that sector's offset fail. */
void fake_gba_fail_program(FakeGba* g, uint8_t sector, uint16_t off, uint8_t times);
/* The next `times` byte programs at that sector's offset are weak: the poll that follows reads the
 * byte as written, but one bit the program should have cleared (the lowest) stays set in the
 * array, so every later read differs. A program that clears no bit can't be weak. */
void fake_gba_weak_program(FakeGba* g, uint8_t sector, uint16_t off, uint8_t times);
/* From now, the next `ops` erases + byte programs complete; the one after them never does: the
 * chip freezes in the middle of it, as a power cut leaves it. It then acts on no write (writes are
 * still counted), every read is that operation's busy status (DQ7 the complement of its data, DQ6
 * toggling, DQ5 clear), and the array stays as it was before that operation. */
void fake_gba_power_cut_after(FakeGba* g, uint32_t ops);
/* fake_gba_power_cut_after, but the operation the cut freezes is half done, as cells cut off
 * mid-change are:
 * - a sector erase leaves the whole sector reading 0xFF, yet weakly erased (weak_erase) until a
 *   whole erase of it: a program into it before one keeps reading right, but the next power cycle
 *   lets the bits that were 0 before the cut erase read 0 again, as data held over the erase;
 * - a byte program clears only the lowest of the bits it should clear (none if it clears none). */
void fake_gba_power_cut_partial(FakeGba* g, uint32_t ops);
/* The GBA is switched off and on again with the cart still in: the chip leaves a power cut's
 * freeze, forgets a power cut still to come, and is back in read mode, out of any command sequence,
 * in bank 0; the status is Ready and the modelled time starts again. A weakly erased sector that
 * was programmed since its cut gets its old 0 bits back (fake_gba_power_cut_partial). The array
 * otherwise, the cart, the chip's id, the faults the test injected and the counts stay. */
void fake_gba_power_cycle(FakeGba* g);
void fake_gba_counts(const FakeGba* g, FakeGbaCounts* out);
/* Time the GBA spent since the last call, in µs, as the fake models it: 300,000 per erase, 25 per
 * byte program, 1 per flash read (a sector CRC is 4096), and ROM reads at 7,000,000 per 16 MiB
 * (a whole-ROM scan). Resets to 0. */
uint32_t fake_gba_take_busy_us(FakeGba* g);

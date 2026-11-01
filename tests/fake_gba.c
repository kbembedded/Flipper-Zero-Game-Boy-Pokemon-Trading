#include "fake_gba.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The chip's mode */
#define FAKE_MODE_READ 0u
#define FAKE_MODE_ID 1u
#define FAKE_MODE_STATUS 2u /* a failed erase or program: status reads until a 0xF0 */

/* How far a command sequence has got */
#define FAKE_SEQ_IDLE 0u
#define FAKE_SEQ_AA 1u /* AA@5555 */
#define FAKE_SEQ_55 2u /* AA@5555, 55@2AAA */
#define FAKE_SEQ_ERASE 3u /* ..., 80@5555 */
#define FAKE_SEQ_ERASE_AA 4u /* ..., AA@5555 */
#define FAKE_SEQ_ERASE_55 5u /* ..., 55@2AAA */
#define FAKE_SEQ_PROGRAM 6u /* ..., A0@5555: the next write programs */
#define FAKE_SEQ_BANK 7u /* ..., B0@5555: the next write, at 0000, selects the bank */

#define FAKE_SECTOR_LEN 4096u
#define FAKE_BANK_LEN 65536u
#define FAKE_ROM_MAX 0x02000000u /* hp_hal_rom16/rom32: off < 32 MiB */
/* A whole 16 MiB ROM scan: the helper's loop is about 27 cycles per 32-bit word at 16.78 MHz,
 * about 6.7 s; rounded up */
#define FAKE_SCAN_US 7000000u
#define FAKE_SCAN_BYTES 0x1000000u
#define FAKE_ERASE_US 300000u
#define FAKE_PROGRAM_US 25u

static FakeGba* fake_gba_now = NULL;

static void fake_gba_misuse(const char* what, uint32_t off) {
    fprintf(stderr, "fake_gba: %s (0x%08X)\n", what, (unsigned)off);
    abort();
}

void fake_gba_init(FakeGba* g) {
    memset(g, 0, sizeof(*g));
    for(uint32_t i = 0; i < 156; i++) g->own[4 + i] = (uint8_t)(i * 37u + 11u);
    /* the real logo's first word, 0x51AEFF24: the ROM-size check looks for it in the cart */
    g->own[4] = 0x24;
    g->own[5] = 0xFF;
    g->own[6] = 0xAE;
    g->own[7] = 0x51;
    g->own[0xC4] = 3;
    g->own[0xC5] = 1;
    g->status = HpStatusReady;
    g->save = FakeSaveNone;
    memset(g->flash, 0xFF, sizeof(g->flash));
    fake_gba_now = g;
}

FakeGba* fake_gba_current(void) {
    return fake_gba_now;
}

void fake_gba_rom(FakeGba* g, const uint8_t header[0xC0], uint32_t rom_size) {
    memcpy(g->header, header, FAKE_GBA_HEADER_LEN);
    g->rom_size = rom_size;
    g->strings = 0;
}

void fake_gba_rom_string(FakeGba* g, uint32_t off, const char* s) {
    const size_t len = strlen(s);
    if(g->strings >= FAKE_GBA_STRINGS || len == 0 || len > FAKE_GBA_STRING_MAX) {
        fake_gba_misuse("fake_gba_rom_string: no room", off);
    }
    g->string_off[g->strings] = off;
    g->string_len[g->strings] = (uint8_t)len;
    memcpy(g->string[g->strings], s, len);
    g->strings++;
}

void fake_gba_rom_glitch_once(FakeGba* g, uint32_t off) {
    if((off & 1u) || off >= FAKE_ROM_MAX) fake_gba_misuse("fake_gba_rom_glitch_once: offset", off);
    g->rom_glitch = true;
    g->rom_glitch_off = off;
}

void fake_gba_save(FakeGba* g, FakeSaveKind kind, uint16_t id) {
    g->save = kind;
    g->chip_id = id;
    g->bank = 0;
    g->mode = FAKE_MODE_READ;
    g->seq = FAKE_SEQ_IDLE;
    g->id_settling = false;
}

uint8_t* fake_gba_flash(FakeGba* g) {
    return g->flash;
}

void fake_gba_fail_erase(FakeGba* g, uint8_t sector, uint8_t times) {
    if(sector >= FAKE_GBA_SECTORS) fake_gba_misuse("fake_gba_fail_erase: sector", sector);
    g->fail_erase[sector] = times;
}

void fake_gba_fail_program(FakeGba* g, uint8_t sector, uint16_t off, uint8_t times) {
    if(sector >= FAKE_GBA_SECTORS || off >= FAKE_SECTOR_LEN) {
        fake_gba_misuse("fake_gba_fail_program: sector or offset", (uint32_t)sector << 16 | off);
    }
    g->fail_program_sector = sector;
    g->fail_program_off = off;
    g->fail_program_times = times;
}

void fake_gba_weak_program(FakeGba* g, uint8_t sector, uint16_t off, uint8_t times) {
    if(sector >= FAKE_GBA_SECTORS || off >= FAKE_SECTOR_LEN) {
        fake_gba_misuse("fake_gba_weak_program: sector or offset", (uint32_t)sector << 16 | off);
    }
    g->weak_sector = sector;
    g->weak_off = off;
    g->weak_times = times;
}

void fake_gba_power_cut_after(FakeGba* g, uint32_t ops) {
    g->cut_armed = true;
    g->cut_ops = ops;
    g->cut_partial = false;
}

void fake_gba_power_cut_partial(FakeGba* g, uint32_t ops) {
    fake_gba_power_cut_after(g, ops);
    g->cut_partial = true;
}

void fake_gba_power_cycle(FakeGba* g) {
    if(g->weak_erase && g->weak_programmed) {
        uint8_t* s = g->flash + (uint32_t)g->weak_erase_sector * FAKE_SECTOR_LEN;
        for(uint32_t i = 0; i < FAKE_SECTOR_LEN; i++) s[i] &= g->weak_old[i];
        g->weak_programmed = false;
    }
    g->frozen = false;
    g->cut_armed = false;
    g->cut_partial = false;
    g->cut_ops = 0;
    g->mode = FAKE_MODE_READ;
    g->seq = FAKE_SEQ_IDLE;
    g->bank = 0;
    g->id_settling = false;
    g->late_left = 0;
    g->weak_poll = false;
    g->status = HpStatusReady;
    g->timer_reads = 0;
    g->busy_us = 0;
    g->rom_bytes = 0;
}

void fake_gba_counts(const FakeGba* g, FakeGbaCounts* out) {
    *out = g->counts;
}

uint32_t fake_gba_take_busy_us(FakeGba* g) {
    const uint64_t us = g->busy_us + g->rom_bytes * FAKE_SCAN_US / FAKE_SCAN_BYTES;
    g->busy_us = 0;
    g->rom_bytes = 0;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* ---- The cart ROM ---- */

static uint8_t fake_rom_byte(const FakeGba* g, uint32_t off) {
    if(off >= g->rom_size) {
        if(g->rom_mirror && g->rom_size) return fake_rom_byte(g, off % g->rom_size);
        const uint16_t open = (uint16_t)(off >> 1);
        return (off & 1u) ? (uint8_t)(open >> 8) : (uint8_t)open;
    }
    if(off < FAKE_GBA_HEADER_LEN) return g->header[off];
    for(uint32_t i = 0; i < g->strings; i++) {
        if(off >= g->string_off[i] && off - g->string_off[i] < g->string_len[i]) {
            return (uint8_t)g->string[i][off - g->string_off[i]];
        }
    }
    const uint16_t hw = (uint16_t)(((off & ~1u) * 31u) >> 3);
    return (off & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

/* XOR 0x0001 if this read of the halfword at off is the glitched one */
static uint16_t fake_rom_glitch(FakeGba* g, uint32_t off) {
    if(!g->rom_glitch || g->rom_glitch_off != off) return 0;
    g->rom_glitch = false;
    return 0x0001u;
}

uint16_t hp_hal_rom16(uint32_t off) {
    if((off & 1u) || off >= FAKE_ROM_MAX) fake_gba_misuse("hp_hal_rom16: bad offset", off);
    fake_gba_now->rom_bytes += 2;
    const uint16_t hw =
        (uint16_t)(fake_rom_byte(fake_gba_now, off) | fake_rom_byte(fake_gba_now, off + 1) << 8);
    return (uint16_t)(hw ^ fake_rom_glitch(fake_gba_now, off));
}

/* The pattern word at off, if all 4 bytes are pattern (not header, string or past the end). */
static bool fake_rom_pattern_word(const FakeGba* g, uint32_t off, uint32_t* w) {
    if(off < FAKE_GBA_HEADER_LEN || off >= g->rom_size || g->rom_size - off < 4) return false;
    for(uint32_t i = 0; i < g->strings; i++) {
        if(off + 4 > g->string_off[i] && off < g->string_off[i] + g->string_len[i]) return false;
    }
    const uint32_t lo = ((off * 31u) >> 3) & 0xFFFFu;
    const uint32_t hi = (((off + 2u) * 31u) >> 3) & 0xFFFFu;
    *w = lo | hi << 16;
    return true;
}

uint32_t hp_hal_rom32(uint32_t off) {
    if((off & 3u) || off >= FAKE_ROM_MAX) fake_gba_misuse("hp_hal_rom32: bad offset", off);
    fake_gba_now->rom_bytes += 4;
    const uint32_t glitch = fake_rom_glitch(fake_gba_now, off) |
                            (uint32_t)fake_rom_glitch(fake_gba_now, off + 2u) << 16;
    uint32_t w = 0;
    if(fake_rom_pattern_word(fake_gba_now, off, &w)) return w ^ glitch;
    for(uint32_t i = 0; i < 4; i++) w |= (uint32_t)fake_rom_byte(fake_gba_now, off + i) << (8 * i);
    return w ^ glitch;
}

/* ---- The save chip ---- */

static void fake_trace(FakeGba* g, FakeGbaOpKind kind, uint16_t off, uint8_t val) {
    if(kind == FakeGbaOpRead && g->trace_no_reads) return;
    if(g->trace_len < FAKE_GBA_TRACE) {
        g->trace[g->trace_len] = (FakeGbaOp){.kind = kind, .off = off, .val = val};
    }
    if(g->trace_len < UINT32_MAX) g->trace_len++;
}

/* A flash chip that still acts on commands: not frozen by a power cut. */
static bool fake_flash_live(const FakeGba* g) {
    return g->save == FakeSaveFlash && !g->frozen;
}

/* An erase or a byte program, writing want, was issued (and counted). True if the chip carries it
 * out; false if it isn't a live flash chip, or the power cut comes first and freezes it in the
 * middle of this operation: then *half is whether a partial cut leaves it half done. */
static bool fake_flash_op(FakeGba* g, uint8_t want, bool* half) {
    *half = false;
    if(g->status != HpStatusWriting) g->ops_not_red++;
    if(!fake_flash_live(g)) return false;
    if(g->cut_armed) {
        if(g->cut_ops == 0) {
            g->frozen = true;
            g->cut_want = want;
            *half = g->cut_partial;
            return false;
        }
        g->cut_ops--;
    }
    return true;
}

static void fake_flash_fail(FakeGba* g, uint8_t want) {
    g->mode = FAKE_MODE_STATUS;
    g->fail_want = want;
}

/* An erase or program completed: with late_reads set, it finished just as DQ5 rose. */
static void fake_flash_done(FakeGba* g, uint8_t want) {
    g->late_left = g->late_reads;
    g->late_want = want;
    g->late_reads = 0;
}

static void fake_flash_erase(FakeGba* g, uint16_t off) {
    const uint8_t sector = (uint8_t)(g->bank * 16u + (off >> 12));
    uint8_t* s = g->flash + (uint32_t)sector * FAKE_SECTOR_LEN;
    g->counts.erases++;
    g->busy_us += FAKE_ERASE_US;
    bool half;
    if(!fake_flash_op(g, 0xFF, &half)) {
        if(half) {
            /* one weak sector at a time: a second partial cut's sector replaces the first */
            g->weak_erase = true;
            g->weak_erase_sector = sector;
            g->weak_programmed = false;
            memcpy(g->weak_old, s, FAKE_SECTOR_LEN);
            memset(s, 0xFF, FAKE_SECTOR_LEN);
        }
        return;
    }
    if(g->fail_erase[sector]) {
        g->fail_erase[sector]--;
        fake_flash_fail(g, 0xFF);
        return;
    }
    memset(s, 0xFF, FAKE_SECTOR_LEN);
    if(g->weak_erase && g->weak_erase_sector == sector) g->weak_erase = false;
    fake_flash_done(g, 0xFF);
}

static void fake_flash_program(FakeGba* g, uint16_t off, uint8_t v) {
    const uint8_t sector = (uint8_t)(g->bank * 16u + (off >> 12));
    const uint16_t in_sector = off & (FAKE_SECTOR_LEN - 1u);
    uint8_t* cell = &g->flash[(uint32_t)g->bank * FAKE_BANK_LEN + off];
    g->counts.programs++;
    if(in_sector == 0xFF8u) g->counts.commits++;
    g->busy_us += FAKE_PROGRAM_US;
    bool half;
    if(!fake_flash_op(g, v, &half)) {
        const uint8_t to_clear = (uint8_t)(*cell & ~v);
        if(half) *cell &= (uint8_t) ~(to_clear & (0u - to_clear));
        return;
    }
    if(g->fail_program_times && sector == g->fail_program_sector &&
       in_sector == g->fail_program_off) {
        g->fail_program_times--;
        fake_flash_fail(g, v);
        return;
    }
    if(g->weak_erase && g->weak_erase_sector == sector) g->weak_programmed = true;
    const uint8_t to_clear = (uint8_t)(*cell & ~v);
    *cell &= v;
    if(g->weak_times && to_clear && sector == g->weak_sector && in_sector == g->weak_off) {
        g->weak_times--;
        *cell |= (uint8_t)(to_clear & (0u - to_clear));
        g->weak_poll = true;
        g->weak_poll_bank = g->bank;
        g->weak_poll_off = off;
        g->weak_poll_val = v;
    }
    fake_flash_done(g, v);
}

static void fake_flash_chip_erase(FakeGba* g) {
    g->counts.chip_erases++;
    g->busy_us += FAKE_ERASE_US;
    bool half;
    if(!fake_flash_op(g, 0xFF, &half)) return;
    memset(g->flash, 0xFF, sizeof(g->flash));
    g->weak_erase = false;
}

/* One bus write, decoded as a JEDEC command for every kind of chip. */
static void fake_flash_decode(FakeGba* g, uint16_t off, uint8_t v) {
    const uint8_t seq = g->seq;
    g->seq = FAKE_SEQ_IDLE;
    if(seq == FAKE_SEQ_PROGRAM) {
        fake_flash_program(g, off, v);
        return;
    }
    if(seq == FAKE_SEQ_BANK && off == 0x0000u && v <= 1u) {
        g->counts.bank_selects++;
        if(fake_flash_live(g)) g->bank = v;
        return;
    }
    if(v == 0xF0u) {
        if(fake_flash_live(g)) {
            g->mode = FAKE_MODE_READ;
            g->id_settling = false;
        }
        return;
    }
    switch(seq) {
    case FAKE_SEQ_IDLE:
        if(off == 0x5555u && v == 0xAAu) g->seq = FAKE_SEQ_AA;
        break;
    case FAKE_SEQ_AA:
        if(off == 0x2AAAu && v == 0x55u) g->seq = FAKE_SEQ_55;
        break;
    case FAKE_SEQ_55:
        if(off != 0x5555u) break;
        if(v == 0x90u) {
            g->counts.id_entries++;
            if(fake_flash_live(g)) {
                g->mode = FAKE_MODE_ID;
                g->id_settling = true;
            }
        } else if(g->mode == FAKE_MODE_ID) {
            /* only the exit (0xF0) is taken in ID mode */
        } else if(v == 0x80u) {
            g->seq = FAKE_SEQ_ERASE;
        } else if(v == 0xA0u) {
            g->seq = FAKE_SEQ_PROGRAM;
        } else if(v == 0xB0u) {
            g->seq = FAKE_SEQ_BANK;
        }
        break;
    case FAKE_SEQ_ERASE:
        if(off == 0x5555u && v == 0xAAu) g->seq = FAKE_SEQ_ERASE_AA;
        break;
    case FAKE_SEQ_ERASE_AA:
        if(off == 0x2AAAu && v == 0x55u) g->seq = FAKE_SEQ_ERASE_55;
        break;
    case FAKE_SEQ_ERASE_55:
        if(v == 0x30u && (off & (FAKE_SECTOR_LEN - 1u)) == 0) {
            fake_flash_erase(g, off);
        } else if(v == 0x10u && off == 0x5555u) {
            fake_flash_chip_erase(g);
        }
        break;
    default:
        break;
    }
}

void hp_hal_flash_write(uint16_t off, uint8_t v) {
    FakeGba* g = fake_gba_now;
    fake_trace(g, FakeGbaOpWrite, off, v);
    g->last_write_off = off;
    g->last_write_val = v;
    if(g->save == FakeSaveSram) {
        g->flash[off] = v;
        g->counts.sram_writes++;
    }
    if(fake_flash_live(g) && g->mode == FAKE_MODE_STATUS) {
        if(v == 0xF0u) {
            g->mode = FAKE_MODE_READ;
            g->seq = FAKE_SEQ_IDLE;
        }
        return;
    }
    fake_flash_decode(g, off, v);
}

static uint8_t fake_flash_value(const FakeGba* g, uint16_t off) {
    switch(g->save) {
    case FakeSaveFlash:
        break;
    case FakeSaveSram:
    case FakeSaveRomLike:
        return g->flash[off];
    case FakeSaveNone:
        return 0xFF;
    }
    const uint8_t data = g->flash[(uint32_t)g->bank * FAKE_BANK_LEN + off];
    if(g->mode == FAKE_MODE_STATUS) {
        return (uint8_t)((data & 0x5Fu) | 0x20u | (~g->fail_want & 0x80u));
    }
    if(g->mode == FAKE_MODE_ID && !g->id_settling) {
        if(off == 0) return (uint8_t)g->chip_id;
        if(off == 1) return (uint8_t)(g->chip_id >> 8);
        return 0x00;
    }
    return data;
}

uint8_t hp_hal_flash_read(uint16_t off) {
    FakeGba* g = fake_gba_now;
    uint8_t v = fake_flash_value(g, off);
    if(g->save == FakeSaveFlash && g->frozen) {
        /* busy for good: DQ7 the complement of the data, DQ6 toggling, DQ5 never set */
        g->cut_toggle ^= 0x40u;
        v = (uint8_t)((~g->cut_want & 0x80u) | g->cut_toggle);
    } else if(fake_flash_live(g) && g->late_left) {
        g->late_left--;
        v = (uint8_t)((v & 0x5Fu) | 0x20u | (~g->late_want & 0x80u));
    } else if(
        fake_flash_live(g) && g->weak_poll && g->bank == g->weak_poll_bank &&
        off == g->weak_poll_off) {
        g->weak_poll = false;
        v = g->weak_poll_val;
    }
    fake_trace(g, FakeGbaOpRead, off, v);
    g->timer_reads++;
    g->busy_us++;
    return v;
}

void hp_hal_id_delay(void) {
    fake_trace(fake_gba_now, FakeGbaOpDelay, 0, 0);
    fake_gba_now->id_settling = false;
}

/* ---- Timer, own header, status ---- */

const uint8_t* hp_hal_own_header(void) {
    return fake_gba_now->own;
}

void hp_hal_timer_start(void) {
    fake_gba_now->timer_reads = 0;
}

/* Time passes with flash reads: one 1.0224 ms unit per 8 reads. */
uint16_t hp_hal_timer_units(void) {
    const uint32_t units = fake_gba_now->timer_reads / 8u;
    return units > UINT16_MAX ? UINT16_MAX : (uint16_t)units;
}

void hp_hal_status(HpStatus s) {
    fake_gba_now->status = s;
}

#include "fake_save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_SAVE_DATA_LEN 0xF80u /* a sector's data area (multiboot-and-save.md §5.2) */
#define FAKE_SAVE_OFF_ID 0xFF4u
#define FAKE_SAVE_OFF_CHECKSUM 0xFF6u
#define FAKE_SAVE_OFF_SIGNATURE 0xFF8u
#define FAKE_SAVE_OFF_COUNTER 0xFFCu
#define FAKE_SAVE_SECTORS 32u

/* multiboot-and-save.md §5.6 */
#define FAKE_SAVE_TRAINER_NAME_LEN 8u /* 7 characters and 0xFF */
#define FAKE_SAVE_OFF_GENDER 0x08u
#define FAKE_SAVE_OFF_TID 0x0Au
#define FAKE_SAVE_OFF_SID 0x0Cu
#define FAKE_SAVE_PARTY_SLOTS 6u
#define FAKE_SAVE_PARTY_MON_LEN 100u
#define FAKE_SAVE_STORAGE_ID 5u /* PokemonStorage fills sections 5..13 */
#define FAKE_SAVE_BOXES_OFF 4u /* storage offset of box 0's first Pokemon */
#define FAKE_SAVE_BOX_MON_LEN 80u
#define FAKE_SAVE_BOXES 14u
#define FAKE_SAVE_BOX_POSITIONS 30u
#define FAKE_SAVE_BOX_NAMES_OFF 0x8344u /* storage offset of the 14 box names, 9 bytes each */
#define FAKE_SAVE_BOX_NAME_LEN 9u

static void fake_save_misuse(const char* what, uint32_t v) {
    fprintf(stderr, "fake_save: %s (%u)\n", what, (unsigned)v);
    abort();
}

static uint8_t* fake_save_sector(uint8_t* img, uint8_t sector) {
    if(sector >= FAKE_SAVE_SECTORS) fake_save_misuse("sector", sector);
    return img + (uint32_t)sector * FAKE_SAVE_SECTOR_LEN;
}

static void fake_save_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fake_save_put32(uint8_t* p, uint32_t v) {
    for(uint32_t i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* multiboot-and-save.md §5.4: ids 0, 4 and 13 are short; the rest fill all 0xF80 bytes. */
uint16_t fake_save_size(FakeGame game, uint8_t id) {
    if(id >= FAKE_SAVE_SLOT_SECTORS) fake_save_misuse("id", id);
    if(id == 13) return 0x7D0;
    if(id != 0 && id != 4) return FAKE_SAVE_DATA_LEN;
    switch(game) {
    case FakeGameRS:
        return id == 0 ? 0x890 : 0xC40;
    case FakeGameFRLG:
        return id == 0 ? 0xF24 : 0xEE8;
    case FakeGameE:
        return id == 0 ? 0xF2C : 0xF08;
    }
    fake_save_misuse("game", (uint32_t)game);
    return 0;
}

/* multiboot-and-save.md §5.3: the 32-bit sum of the first len / 4 little-endian words, folded
 * to 16 bits as hi + lo. */
static uint16_t fake_save_checksum(const uint8_t* data, uint32_t len) {
    uint32_t sum = 0;
    for(uint32_t i = 0; i + 4 <= len; i += 4) {
        sum += (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 | (uint32_t)data[i + 2] << 16 |
               (uint32_t)data[i + 3] << 24;
    }
    return (uint16_t)((sum >> 16) + sum);
}

uint32_t fake_save_phys(const FakeSaveSpec* spec, uint8_t slot, uint8_t id) {
    if(slot > 1 || id >= FAKE_SAVE_SLOT_SECTORS || spec->first[slot] >= FAKE_SAVE_SLOT_SECTORS) {
        fake_save_misuse("slot, id or first", (uint32_t)slot << 8 | id);
    }
    return FAKE_SAVE_SLOT_SECTORS * slot + (id + spec->first[slot]) % FAKE_SAVE_SLOT_SECTORS;
}

/* SaveBlock1's playerPartyCount, in section 1 (multiboot-and-save.md §5.6); the 6 Pokemon follow
 * 4 bytes on */
static uint32_t fake_save_party_count_off(FakeGame game) {
    switch(game) {
    case FakeGameRS:
    case FakeGameE:
        return 0x234;
    case FakeGameFRLG:
        return 0x034;
    }
    fake_save_misuse("game", (uint32_t)game);
    return 0;
}

/* The party and the boxes of a freshly filled sector of id `id`, size bytes: zero */
static void fake_save_clear_mons(uint8_t* s, FakeGame game, uint8_t id, uint16_t size) {
    if(id == 1) {
        memset(
            s + fake_save_party_count_off(game),
            0,
            4 + FAKE_SAVE_PARTY_SLOTS * FAKE_SAVE_PARTY_MON_LEN);
    }
    if(id < FAKE_SAVE_STORAGE_ID) return;
    /* this section's bytes are storage offsets base .. base + size - 1 */
    const uint32_t base = (uint32_t)(id - FAKE_SAVE_STORAGE_ID) * FAKE_SAVE_DATA_LEN;
    for(uint32_t i = 0; i < size && base + i < FAKE_SAVE_BOX_NAMES_OFF; i++) s[i] = 0;
}

void fake_save_build(uint8_t img[131072], const FakeSaveSpec* spec) {
    memset(img, 0xFF, FAKE_SAVE_LEN);
    for(uint8_t slot = 0; slot < 2; slot++) {
        if(!spec->slot_present[slot]) continue;
        for(uint8_t id = 0; id < FAKE_SAVE_SLOT_SECTORS; id++) {
            uint8_t* s = fake_save_sector(img, (uint8_t)fake_save_phys(spec, slot, id));
            const uint16_t size = fake_save_size(spec->game, id);
            memset(s, 0, FAKE_SAVE_SECTOR_LEN);
            uint32_t x = spec->seed ^
                         ((uint32_t)(slot * FAKE_SAVE_SLOT_SECTORS + id + 1u) * 0x9E3779B9u);
            for(uint32_t i = 0; i < size; i++) {
                x = x * 1664525u + 1013904223u;
                s[i] = (uint8_t)(x >> 24);
            }
            if(spec->no_mons) fake_save_clear_mons(s, spec->game, id, size);
            fake_save_put16(s + FAKE_SAVE_OFF_ID, id);
            fake_save_put16(s + FAKE_SAVE_OFF_CHECKSUM, fake_save_checksum(s, size));
            fake_save_put32(s + FAKE_SAVE_OFF_SIGNATURE, 0x08012025u);
            fake_save_put32(s + FAKE_SAVE_OFF_COUNTER, spec->counter[slot]);
        }
    }
}

void fake_save_fix(uint8_t img[131072], uint8_t sector, FakeGame game) {
    uint8_t* s = fake_save_sector(img, sector);
    const uint16_t id = (uint16_t)(s[FAKE_SAVE_OFF_ID] | s[FAKE_SAVE_OFF_ID + 1] << 8);
    const uint32_t size = id < FAKE_SAVE_SLOT_SECTORS ? fake_save_size(game, (uint8_t)id) :
                                                        FAKE_SAVE_DATA_LEN;
    fake_save_put16(s + FAKE_SAVE_OFF_CHECKSUM, fake_save_checksum(s, size));
}

void fake_save_set_counter(uint8_t img[131072], uint8_t sector, uint32_t counter) {
    fake_save_put32(fake_save_sector(img, sector) + FAKE_SAVE_OFF_COUNTER, counter);
}

void fake_save_set_id(uint8_t img[131072], uint8_t sector, uint16_t id) {
    fake_save_put16(fake_save_sector(img, sector) + FAKE_SAVE_OFF_ID, id);
}

void fake_save_break_checksum(uint8_t img[131072], uint8_t sector) {
    uint8_t* s = fake_save_sector(img, sector) + FAKE_SAVE_OFF_CHECKSUM;
    fake_save_put16(s, (uint16_t)((s[0] | s[1] << 8) + 1u));
}

/* The game's text encoding (Bulbapedia, "Character encoding (Generation III)"), for the letters,
 * digits and space only */
static uint8_t fake_save_char(char c) {
    if(c >= 'A' && c <= 'Z') return (uint8_t)(0xBB + (c - 'A'));
    if(c >= 'a' && c <= 'z') return (uint8_t)(0xD5 + (c - 'a'));
    if(c >= '0' && c <= '9') return (uint8_t)(0xA1 + (c - '0'));
    if(c == ' ') return 0x00;
    fake_save_misuse("character", (uint8_t)c);
    return 0;
}

/* At most max_chars characters, the terminator 0xFF, then fill up to len bytes */
static void
    fake_save_name(uint8_t* out, uint32_t len, uint32_t max_chars, const char* name, uint8_t fill) {
    const size_t n = strlen(name);
    if(n > max_chars) fake_save_misuse("name length", (uint32_t)n);
    for(uint32_t i = 0; i < n; i++) out[i] = fake_save_char(name[i]);
    out[n] = 0xFF;
    for(uint32_t i = (uint32_t)n + 1; i < len; i++) out[i] = fill;
}

/* The sector of id in a slot the spec built */
static uint8_t fake_save_slot_sector(const FakeSaveSpec* s, uint8_t slot, uint8_t id) {
    if(slot > 1 || !s->slot_present[slot]) fake_save_misuse("slot not built", slot);
    return (uint8_t)fake_save_phys(s, slot, id);
}

void fake_save_put_trainer(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    const char* name,
    uint8_t gender,
    uint16_t tid,
    uint16_t sid) {
    const uint8_t sector = fake_save_slot_sector(s, slot, 0);
    uint8_t* p = fake_save_sector(img, sector);
    fake_save_name(p, FAKE_SAVE_TRAINER_NAME_LEN, FAKE_SAVE_TRAINER_NAME_LEN - 1, name, 0xFF);
    p[FAKE_SAVE_OFF_GENDER] = gender;
    fake_save_put16(p + FAKE_SAVE_OFF_TID, tid);
    fake_save_put16(p + FAKE_SAVE_OFF_SID, sid);
    fake_save_fix(img, sector, s->game);
}

void fake_save_put_party(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint8_t count,
    const uint8_t mons[][100]) {
    if(count > FAKE_SAVE_PARTY_SLOTS) fake_save_misuse("party count", count);
    const uint8_t sector = fake_save_slot_sector(s, slot, 1);
    uint8_t* p = fake_save_sector(img, sector) + fake_save_party_count_off(s->game);
    memset(p, 0, 4 + FAKE_SAVE_PARTY_SLOTS * FAKE_SAVE_PARTY_MON_LEN);
    p[0] = count;
    for(uint32_t i = 0; i < count; i++) {
        memcpy(p + 4 + i * FAKE_SAVE_PARTY_MON_LEN, mons[i], FAKE_SAVE_PARTY_MON_LEN);
    }
    fake_save_fix(img, sector, s->game);
}

/* len bytes from PokemonStorage offset off, one byte at a time: storage offset o is section
 * 5 + o / 0xF80, at o % 0xF80. Refixes each section it touched. */
static void fake_save_put_storage(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint32_t off,
    const uint8_t* data,
    uint32_t len) {
    uint32_t touched = 0;
    for(uint32_t i = 0; i < len; i++) {
        const uint32_t o = off + i;
        const uint8_t id = (uint8_t)(FAKE_SAVE_STORAGE_ID + o / FAKE_SAVE_DATA_LEN);
        fake_save_sector(img, fake_save_slot_sector(s, slot, id))[o % FAKE_SAVE_DATA_LEN] =
            data[i];
        touched |= 1u << id;
    }
    for(uint8_t id = 0; id < FAKE_SAVE_SLOT_SECTORS; id++) {
        if(touched & (1u << id)) fake_save_fix(img, fake_save_slot_sector(s, slot, id), s->game);
    }
}

void fake_save_put_box_mon(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint8_t box,
    uint8_t pos,
    const uint8_t mon[80]) {
    if(box >= FAKE_SAVE_BOXES || pos >= FAKE_SAVE_BOX_POSITIONS) {
        fake_save_misuse("box or position", (uint32_t)box << 8 | pos);
    }
    const uint32_t i = (uint32_t)box * FAKE_SAVE_BOX_POSITIONS + pos;
    fake_save_put_storage(
        img, s, slot, FAKE_SAVE_BOXES_OFF + i * FAKE_SAVE_BOX_MON_LEN, mon, FAKE_SAVE_BOX_MON_LEN);
}

void fake_save_put_box_name(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint8_t box,
    const char* name) {
    if(box >= FAKE_SAVE_BOXES) fake_save_misuse("box", box);
    uint8_t bytes[FAKE_SAVE_BOX_NAME_LEN];
    fake_save_name(bytes, FAKE_SAVE_BOX_NAME_LEN, FAKE_SAVE_BOX_NAME_LEN - 1, name, 0x00);
    fake_save_put_storage(
        img,
        s,
        slot,
        FAKE_SAVE_BOX_NAMES_OFF + (uint32_t)box * FAKE_SAVE_BOX_NAME_LEN,
        bytes,
        FAKE_SAVE_BOX_NAME_LEN);
}

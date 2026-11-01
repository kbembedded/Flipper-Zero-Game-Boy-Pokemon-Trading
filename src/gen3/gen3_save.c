#include <string.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_save.h>
#include <src/gen3/gen3_text.h>

/* A sector's footer (multiboot-and-save.md §5.2; pokeruby: src/save.c: struct SaveSector) */
#define SAVE_OFF_ID 0xFF4u
#define SAVE_OFF_CHECKSUM 0xFF6u
#define SAVE_OFF_SIGNATURE 0xFF8u
#define SAVE_OFF_COUNTER 0xFFCu
#define SAVE_ALL_IDS 0x3FFFu /* ids 0..13 */

/* multiboot-and-save.md §5.6: SaveBlock1's party count and party in section 1; PokemonStorage
 * from section 5, its 420 Pokemon from storage offset 4, its box names in section 13 */
#define SAVE_PARTY_ID 1u
#define SAVE_STORAGE_ID 5u
#define SAVE_BOXES_OFF 4u
#define SAVE_BOX_NAMES_ID 13u
#define SAVE_BOX_NAMES_OFF 0x744u
#define SAVE_BOX_NAME_LEN 9u
/* SaveBlock2, section 0 */
#define SAVE_TRAINER_GENDER 0x08u
#define SAVE_TRAINER_TID 0x0Au
#define SAVE_TRAINER_SID 0x0Cu
#define SAVE_TRAINER_HOURS 0x0Eu
#define SAVE_TRAINER_MIN 0x10u

_Static_assert(
    SAVE_STORAGE_ID + (SAVE_BOXES_OFF + GEN3_SAVE_BOXES * GEN3_SAVE_BOX_SLOTS * GEN3_BOX_SIZE) /
                          GEN3_SAVE_SECTOR_DATA ==
        13,
    "the boxes end in section 13");

static uint16_t save_le16(const uint8_t* b) {
    return (uint16_t)(b[0] | b[1] << 8);
}

static uint32_t save_le32(const uint8_t* b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void save_put16(uint8_t* b, uint16_t v) {
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void save_put32(uint8_t* b, uint32_t v) {
    for(uint32_t i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}

/* pokeruby: src/save.c: sSaveBlockChunks; fr: src/save.c: sSaveSlotLayout; em: the same. Ids 0
 * (SaveBlock2) and 4 (SaveBlock1's last chunk) differ; 13 is PokemonStorage's last chunk; the
 * rest are whole 0xF80-byte chunks. */
uint16_t gen3_save_section_size(Gen3LinkGame g, uint16_t id) {
    if(id >= GEN3_SAVE_SLOT_SECTORS) return 0;
    uint16_t id0 = 0;
    uint16_t id4 = 0;
    switch(g) {
    case Gen3LinkGameRS:
        id0 = 0x890;
        id4 = 0xC40;
        break;
    case Gen3LinkGameFRLG:
        id0 = 0xF24;
        id4 = 0xEE8;
        break;
    case Gen3LinkGameE:
        id0 = 0xF2C;
        id4 = 0xF08;
        break;
    case Gen3LinkGameUnknown:
        break;
    }
    if(id0 == 0) return 0;
    if(id == 0) return id0;
    if(id == 4) return id4;
    return id == 13 ? 0x7D0 : GEN3_SAVE_SECTOR_DATA;
}

uint16_t gen3_save_checksum(const uint8_t* data, uint16_t size) {
    uint32_t sum = 0;
    for(uint32_t i = 0; i + 4 <= size; i += 4) sum += save_le32(data + i);
    return (uint16_t)((sum >> 16) + sum);
}

void gen3_save_footer_get(const uint8_t sector[GEN3_SAVE_SECTOR_SIZE], Gen3SaveFooter* out) {
    out->id = save_le16(sector + SAVE_OFF_ID);
    out->checksum = save_le16(sector + SAVE_OFF_CHECKSUM);
    out->signature = save_le32(sector + SAVE_OFF_SIGNATURE);
    out->counter = save_le32(sector + SAVE_OFF_COUNTER);
}

void gen3_save_footer_set(uint8_t sector[GEN3_SAVE_SECTOR_SIZE], const Gen3SaveFooter* f) {
    save_put16(sector + SAVE_OFF_ID, f->id);
    save_put16(sector + SAVE_OFF_CHECKSUM, f->checksum);
    save_put32(sector + SAVE_OFF_SIGNATURE, f->signature);
    save_put32(sector + SAVE_OFF_COUNTER, f->counter);
}

void gen3_save_sector_info(
    Gen3LinkGame g,
    const uint8_t sector[GEN3_SAVE_SECTOR_SIZE],
    Gen3SaveSectorInfo* out) {
    gen3_save_footer_get(sector, &out->f);
    out->full_ok = gen3_save_checksum(sector, GEN3_SAVE_SECTOR_DATA) == out->f.checksum;
    /* The game reads an id over 13's size from past the end of its table; such a sector never
     * counts here, so its slot can't be Ok (it leaves 13 sectors for 14 ids). */
    const uint16_t size = gen3_save_section_size(g, out->f.id);
    out->valid = out->f.signature == GEN3_SAVE_SIGNATURE && size != 0 &&
                 gen3_save_checksum(sector, size) == out->f.checksum;
}

/* A sector the game takes: valid, with its id (bounded here too, for an info not made by
 * gen3_save_sector_info) */
static bool save_counts(const Gen3SaveSectorInfo* s) {
    return s->valid && s->f.id < GEN3_SAVE_SLOT_SECTORS;
}

/* One slot as pokeruby: src/save.c: GetSaveValidStatus sees it */
static Gen3SlotStatus save_slot_status(const Gen3SaveSectorInfo* slot, uint32_t* counter) {
    bool any_signature = false;
    uint32_t valid = 0;
    *counter = 0;
    for(uint32_t i = 0; i < GEN3_SAVE_SLOT_SECTORS; i++) {
        if(slot[i].f.signature != GEN3_SAVE_SIGNATURE) continue;
        any_signature = true;
        if(!save_counts(&slot[i])) continue;
        *counter = slot[i].f.counter; /* the last valid sector's, in physical order */
        valid |= 1u << slot[i].f.id;
    }
    if(!any_signature) return Gen3SlotEmpty;
    return valid == SAVE_ALL_IDS ? Gen3SlotOk : Gen3SlotError;
}

void gen3_save_analyze(const Gen3SaveSectorInfo info[28], Gen3SaveSlots* out) {
    memset(out, 0, sizeof(*out));
    memset(out->phys, GEN3_SAVE_NO_SECTOR, sizeof(out->phys));
    for(uint32_t s = 0; s < 2; s++) {
        out->slot[s] = save_slot_status(info + s * GEN3_SAVE_SLOT_SECTORS, &out->counter[s]);
    }
    const bool a = out->slot[0] == Gen3SlotOk;
    const bool b = out->slot[1] == Gen3SlotOk;
    const uint32_t ca = out->counter[0];
    const uint32_t cb = out->counter[1];
    if(a && b) {
        /* The newer counter; 0 is newer than 0xFFFFFFFF */
        if((ca == 0xFFFFFFFFu && cb == 0) || (ca == 0 && cb == 0xFFFFFFFFu)) {
            out->save_counter = (ca + 1u) < (cb + 1u) ? cb : ca;
        } else {
            out->save_counter = ca < cb ? cb : ca;
        }
        out->result = Gen3SaveOk;
    } else if(a || b) {
        out->save_counter = a ? ca : cb;
        out->result = out->slot[a ? 1 : 0] == Gen3SlotError ? Gen3SaveError : Gen3SaveOk;
    } else {
        /* The game shows no saved game then, Empty or Corrupt alike */
        const bool empty = out->slot[0] == Gen3SlotEmpty && out->slot[1] == Gen3SlotEmpty;
        out->result = empty ? Gen3SaveEmpty : Gen3SaveCorrupt;
        return;
    }
    /* pokeruby: src/save.c: sub_81258BC (em/fr: CopySaveSlotData) loads slot save_counter % 2,
     * whichever slot the counter came from: each valid sector's data to its id, a later one over
     * an earlier one, and the first sector is wherever a footer last said id 0. */
    out->active = (uint8_t)(out->save_counter % 2u);
    const uint8_t base = (uint8_t)(out->active * GEN3_SAVE_SLOT_SECTORS);
    bool strict = true;
    uint32_t seen = 0;
    for(uint8_t i = 0; i < GEN3_SAVE_SLOT_SECTORS; i++) {
        const Gen3SaveSectorInfo* s = &info[base + i];
        if(s->f.id == 0) out->first = i;
        if(!save_counts(s)) {
            strict = false;
            continue;
        }
        if(s->f.counter != out->save_counter) strict = false;
        seen |= 1u << s->f.id;
        out->phys[s->f.id] = (uint8_t)(base + i);
    }
    /* 14 valid sectors cover ids 0..13 only with each id once */
    out->strict_ok = strict && seen == SAVE_ALL_IDS;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        if(out->phys[k] != GEN3_SAVE_NO_SECTOR && !info[out->phys[k]].full_ok) out->padded = true;
    }
}

static void save_plan(uint32_t counter, uint8_t first, Gen3SaveWritePlan* out) {
    out->target = (uint8_t)(counter % 2u);
    out->counter = counter;
    out->first = first;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        out->dst[k] =
            (uint8_t)(out->target * GEN3_SAVE_SLOT_SECTORS + (k + first) % GEN3_SAVE_SLOT_SECTORS);
    }
}

bool gen3_save_plan_write(const Gen3SaveSlots* s, Gen3SaveWritePlan* out) {
    if(!s->strict_ok) return false;
    /* As the game's own save (pokeruby: src/save.c: WriteSaveBlockChunks): the counter and the
     * first sector one on, so the new save goes to the slot the game doesn't load */
    save_plan(s->save_counter + 1u, (uint8_t)((s->first + 1u) % GEN3_SAVE_SLOT_SECTORS), out);
    memcpy(out->src, s->phys, sizeof(out->src));
    return true;
}

void gen3_save_plan_fresh(uint32_t counter, uint8_t first, Gen3SaveWritePlan* out) {
    save_plan(counter, (uint8_t)(first % GEN3_SAVE_SLOT_SECTORS), out);
    memset(out->src, GEN3_SAVE_NO_SECTOR, sizeof(out->src));
}

/* SaveBlock1's playerPartyCount in section 1; the 6 Pokemon follow 4 bytes on. 0: no layout. */
static uint16_t save_party_count_off(Gen3LinkGame g) {
    switch(g) {
    case Gen3LinkGameRS:
    case Gen3LinkGameE:
        return 0x234;
    case Gen3LinkGameFRLG:
        return 0x034;
    case Gen3LinkGameUnknown:
        break;
    }
    return 0;
}

uint8_t gen3_save_mon_spans(Gen3LinkGame g, uint8_t box, uint8_t slot, Gen3SaveSpan out[2]) {
    const uint16_t count_off = save_party_count_off(g);
    if(count_off == 0) return 0;
    if(box == GEN3_SAVE_PARTY) {
        if(slot >= GEN3_SAVE_PARTY_MAX) return 0;
        out[0] = (Gen3SaveSpan){
            .id = SAVE_PARTY_ID,
            .off = (uint16_t)(count_off + 4u + slot * GEN3_PARTY_SIZE),
            .len = GEN3_PARTY_SIZE,
        };
        return 1;
    }
    if(box >= GEN3_SAVE_BOXES || slot >= GEN3_SAVE_BOX_SLOTS) return 0;
    /* PokemonStorage offset o is section 5 + o / 0xF80, at o % 0xF80 */
    const uint32_t o =
        SAVE_BOXES_OFF + ((uint32_t)box * GEN3_SAVE_BOX_SLOTS + slot) * GEN3_BOX_SIZE;
    const uint16_t off = (uint16_t)(o % GEN3_SAVE_SECTOR_DATA);
    out[0] = (Gen3SaveSpan){
        .id = (uint8_t)(SAVE_STORAGE_ID + o / GEN3_SAVE_SECTOR_DATA),
        .off = off,
        .len = GEN3_BOX_SIZE,
    };
    if((uint32_t)off + GEN3_BOX_SIZE <= GEN3_SAVE_SECTOR_DATA) return 1;
    /* one of the eight that straddle two sections */
    out[0].len = (uint8_t)(GEN3_SAVE_SECTOR_DATA - off);
    out[1] = (Gen3SaveSpan){
        .id = (uint8_t)(out[0].id + 1u),
        .off = 0,
        .len = (uint8_t)(GEN3_BOX_SIZE - out[0].len),
    };
    return 2;
}

Gen3SaveSpan gen3_save_party_count_span(Gen3LinkGame g) {
    const uint16_t off = save_party_count_off(g);
    return (Gen3SaveSpan){.id = SAVE_PARTY_ID, .off = off, .len = off ? 1 : 0};
}

Gen3SaveSpan gen3_save_box_name_span(uint8_t box) {
    if(box >= GEN3_SAVE_BOXES) return (Gen3SaveSpan){.id = 0, .off = 0, .len = 0};
    return (Gen3SaveSpan){
        .id = SAVE_BOX_NAMES_ID,
        .off = (uint16_t)(SAVE_BOX_NAMES_OFF + box * SAVE_BOX_NAME_LEN),
        .len = SAVE_BOX_NAME_LEN,
    };
}

void gen3_save_trainer(const uint8_t sec0[GEN3_SAVE_SECTOR_SIZE], Gen3SaveTrainer* out) {
    memcpy(out->name_raw, sec0, sizeof(out->name_raw));
    gen3_text_decode(sec0, sizeof(out->name_raw), out->name, sizeof(out->name));
    out->gender = sec0[SAVE_TRAINER_GENDER];
    out->tid = save_le16(sec0 + SAVE_TRAINER_TID);
    out->sid = save_le16(sec0 + SAVE_TRAINER_SID);
    out->hours = save_le16(sec0 + SAVE_TRAINER_HOURS);
    out->minutes = sec0[SAVE_TRAINER_MIN];
}

void gen3_save_mon_summary(const uint8_t* mon, size_t len, Gen3MonSummary* out) {
    memset(out, 0, sizeof(*out));
    out->kind = Gen3MonBad;
    Gen3Pkm p;
    /* pokeruby: src/pokemon_2.c: GetBoxMonData: a checksum that fails makes the Pokemon a Bad
     * Egg, and a Bad Egg reads as the egg species whatever its own */
    if(!gen3_pkm_decrypt(mon, len, &p) || gen3_pkm_get(&p, Gen3FieldBadEgg)) return;
    const uint16_t species = (uint16_t)gen3_pkm_get(&p, Gen3FieldSpecies);
    if(species == 0) {
        out->kind = Gen3MonEmpty;
        return;
    }
    const uint16_t national = gen3_national_from_internal(species);
    const Gen3SpeciesInfo* sp = gen3_species(national);
    if(!sp) return;
    out->kind = gen3_pkm_get(&p, Gen3FieldIsEgg) || gen3_pkm_get(&p, Gen3FieldEggFlag) ?
                    Gen3MonEgg :
                    Gen3MonOk;
    out->national = national;
    out->level = gen3_level_for_exp(sp->growth, gen3_pkm_get(&p, Gen3FieldExp));
    gen3_pkm_nickname(&p, out->nickname, sizeof(out->nickname));
}

bool gen3_save_mon_to_pkm(const uint8_t* mon, size_t len, Gen3Pkm* out) {
    if(!gen3_pkm_decrypt(mon, len, out)) return false;
    if(len == GEN3_BOX_SIZE) {
        /* What the game derives when a box Pokemon joins the party (pokeruby: src/pokemon_1.c:
         * ExpandBoxMon): no status, no mail, stats from its level, full HP */
        gen3_pkm_set(out, Gen3FieldStatus, 0);
        gen3_pkm_set(out, Gen3FieldMail, GEN3_MAIL_NONE);
        gen3_recalc_party(out);
    }
    return true;
}

void gen3_save_pkm_to_mon(const Gen3Pkm* p, size_t len, uint8_t* out) {
    uint8_t game[GEN3_PARTY_SIZE];
    gen3_pkm_encrypt(p, game);
    memcpy(out, game, len < sizeof(game) ? len : sizeof(game));
}

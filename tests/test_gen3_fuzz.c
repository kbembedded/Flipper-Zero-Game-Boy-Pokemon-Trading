#include "test.h"
#include "bulk_file.h"
#include "gen3_golden.h"
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_trade_block.h>

_Static_assert(GEN3_RUBY_ITEMS_COUNT == 349 && GEN3_ITEM_ENIGMA_BERRY == 175, "D8");

/* A deterministic random sequence of editor calls (out-of-range arguments included), 80- and
 * 100-byte imports, direct gen3_edit_load calls, GBA players (D7) and bulk reads that fail at
 * random after a good open; now and then a new editor replaces the old one. After every step the
 * editor's Pokemon must be game-valid (fuzz_valid, which also cross-checks the trade's offer
 * check and gen3_offer_traits), a call that refuses or fails must leave the editor exactly as it
 * was, and pristine must follow gen3_edit.h: cleared by a call that changed the Pokemon's bytes,
 * kept otherwise, and kept by set_trainer. The seed and step count are fixed, so a failure
 * reproduces; the step number is printed with it. Other seeds can be tried with
 * -DFUZZ_SEED=... -DFUZZ_STEPS=... (the coverage counts at the end are for this seed). */

#ifndef FUZZ_SEED
#define FUZZ_SEED 0x5EED2026u
#endif
#ifndef FUZZ_STEPS
#define FUZZ_STEPS 20000
#endif

/* The fuzz's own choices, apart from the editor's RNG (xorshift32) */
static uint32_t fz_state = FUZZ_SEED;

static uint32_t fz(void) {
    uint32_t x = fz_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fz_state = x;
    return x;
}

static uint32_t fz_below(uint32_t n) {
    return fz() % n;
}

static int fz_step;
static const char* fz_what = "";

#define CHECK(cond, why)                                         \
    do {                                                         \
        if(!(cond)) {                                            \
            printf("step %d (%s): %s\n", fz_step, fz_what, why); \
            return false;                                        \
        }                                                        \
    } while(0)

/* ---- Bulk source: the committed file, failing at random once fz_fail_one_in is set ---- */

static uint32_t fz_fail_one_in; /* 0: reads work */

static bool fuzz_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    if(fz_fail_one_in && fz_below(fz_fail_one_in) == 0) return false;
    return bulk_file_read(ctx, offset, buf, len);
}

/* A good open: no read fails while the file is checked */
static bool fuzz_reopen(void) {
    uint32_t keep = fz_fail_one_in;
    fz_fail_one_in = 0;
    bool ok = gen3_bulk_open(fuzz_read, bulk_file_fp);
    fz_fail_one_in = keep;
    return ok;
}

/* ---- The invariant ---- */

static int fz_unfateful_seen; /* fuzz_valid calls on a MEW or DEOXYS without the fateful bit */

static bool is_mail(uint32_t item) {
    return item >= GEN3_ITEM_MAIL_FIRST && item <= GEN3_ITEM_MAIL_LAST;
}

static bool info_empty(const Gen3ImportInfo* i) {
    const Gen3NormaliseInfo* n = &i->normalise;
    return !i->mail_removed && !n->moves_defaulted && !n->duplicates_removed && !n->gaps_packed &&
           !n->exp_clamped && !n->ability_fixed && !n->mail_item_removed && !n->mail_cleared;
}

/* The game-valid invariant, plus the round trips the game and the SD card put it through */
static bool fuzz_valid(const Gen3Pkm* p) {
    CHECK(gen3_pkm_checksum(p) == gen3_pkm_get(p, Gen3FieldChecksum), "checksum");
    CHECK(gen3_pkm_get(p, Gen3FieldHasSpecies) == 1, "HasSpecies 0");
    CHECK(!gen3_pkm_get(p, Gen3FieldIsEgg) && !gen3_pkm_get(p, Gen3FieldEggFlag), "an egg");
    CHECK(!gen3_pkm_get(p, Gen3FieldBadEgg), "a Bad Egg");
    const Gen3SpeciesInfo* info =
        gen3_species(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
    CHECK(info != NULL, "not a real species");

    uint32_t exp = gen3_pkm_get(p, Gen3FieldExp);
    CHECK(exp >= gen3_exp_for_level(info->growth, 1), "exp below level 1");
    CHECK(exp <= gen3_exp_for_level(info->growth, 100), "exp above level 100");
    uint8_t level = gen3_level_for_exp(info->growth, exp);
    CHECK(level >= 1 && level <= 100, "level out of 1..100");
    CHECK(gen3_pkm_get(p, Gen3FieldLevel) == level, "Level != level for exp");
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(p, st);
    for(int s = 0; s < Gen3StatCount; s++)
        CHECK(gen3_pkm_get(p, Gen3FieldMaxHp + s) == st[s], "stats != calc_stats");
    CHECK(gen3_pkm_get(p, Gen3FieldHp) <= st[Gen3StatHp], "Hp > MaxHp");

    CHECK(info->ability[1] != 0 || !gen3_pkm_get(p, Gen3FieldAbilityBit), "ability bit 1");
    int moves = 0;
    bool gap = false;
    for(int s = 0; s < 4; s++) {
        uint32_t m = gen3_pkm_get(p, Gen3FieldMove1 + s);
        if(!m) {
            gap = true;
            continue;
        }
        CHECK(!gap, "moves not packed");
        CHECK(m < GEN3_MOVE_COUNT, "move id out of range");
        for(int t = 0; t < s; t++) CHECK(gen3_pkm_get(p, Gen3FieldMove1 + t) != m, "dup move");
        moves++;
    }
    CHECK(moves >= 1, "no move");
    uint32_t item = gen3_pkm_get(p, Gen3FieldHeldItem);
    CHECK(item < GEN3_ITEM_COUNT, "item id out of range");
    CHECK(!is_mail(item), "holds a Mail item");
    CHECK(gen3_pkm_get(p, Gen3FieldMail) == GEN3_MAIL_NONE, "mail index != 0xFF");
    /* D8 cross-check: the trade refuses a game-valid Pokemon only for an item Ruby lacks (the
     * ENIGMA BERRY, or ITEMS_COUNT and up), with D8's numbers, not the header's */
    bool ruby_lacks = item == 175 || item >= 349;
    CHECK(
        gen3_trade_offer_check(p) == (ruby_lacks ? Gen3RefuseItem : Gen3RefuseNone),
        "offer check (D8)");
    /* The offer traits an FRLG partner checks, with the numbers, not the header's: NonKanto past
     * #151 (or no national number), Unfateful for MEW (151) or DEOXYS (386) without the bit */
    uint16_t national = gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies));
    bool unfateful = (national == 151 || national == 386) && !gen3_pkm_get(p, Gen3FieldFateful);
    uint8_t traits = (national == 0 || national > 151 ? Gen3OfferNonKanto : 0) |
                     (unfateful ? Gen3OfferUnfateful : 0);
    CHECK(gen3_offer_traits(p) == traits, "offer traits");
    fz_unfateful_seen += unfateful;

    /* The game form decrypts back to the same bytes */
    uint8_t game[GEN3_PARTY_SIZE];
    Gen3Pkm back;
    gen3_pkm_encrypt(p, game);
    CHECK(gen3_pkm_decrypt(game, sizeof(game), &back), "decrypt refused");
    CHECK(memcmp(back.raw, p->raw, GEN3_PARTY_SIZE) == 0, "encrypt/decrypt round trip");

    /* Saved as a .pk3 (80 bytes) and loaded again: the same Pokemon, nothing to fix, the party
     * fields derived (full HP, no status) */
    uint8_t box[GEN3_BOX_SIZE];
    Gen3Pkm q;
    Gen3ImportInfo ii;
    gen3_pk3_export(p, box);
    CHECK(gen3_pk3_import(box, sizeof(box), &q, &ii) == Gen3CheckOk, "re-import refused");
    CHECK(info_empty(&ii), "re-import fixed something");
    CHECK(memcmp(q.raw, p->raw, GEN3_BOX_SIZE) == 0, "re-import changed a box byte");
    CHECK(gen3_pkm_get(&q, Gen3FieldLevel) == level, "re-import level");
    for(int s = 0; s < Gen3StatCount; s++)
        CHECK(gen3_pkm_get(&q, Gen3FieldMaxHp + s) == st[s], "re-import stats");
    CHECK(gen3_pkm_get(&q, Gen3FieldHp) == st[Gen3StatHp], "re-import HP");
    CHECK(gen3_pkm_get(&q, Gen3FieldStatus) == 0, "re-import status");
    /* As a 100-byte file, every byte comes back */
    CHECK(gen3_pk3_import(p->raw, GEN3_PARTY_SIZE, &q, &ii) == Gen3CheckOk, "100-byte refused");
    CHECK(info_empty(&ii), "100-byte re-import fixed something");
    CHECK(memcmp(q.raw, p->raw, GEN3_PARTY_SIZE) == 0, "100-byte re-import changed a byte");
    return true;
}

/* The whole editor, the RNG included, field by field (no padding compared) */
static bool editor_eq(const Gen3Editor* a, const Gen3Editor* b) {
    return memcmp(a->pkm.raw, b->pkm.raw, GEN3_PARTY_SIZE) == 0 && a->preset == b->preset &&
           a->preset_applied == b->preset_applied && a->rng.state == b->rng.state &&
           a->pristine == b->pristine && a->has_trainer == b->has_trainer &&
           memcmp(a->trainer.name, b->trainer.name, GEN3_OT_NAME_LEN) == 0 &&
           a->trainer.tid == b->trainer.tid && a->trainer.sid == b->trainer.sid &&
           a->trainer.gender == b->trainer.gender;
}

/* The same OT name bytes, OT ID and OT gender */
static bool ot_eq(const Gen3Pkm* a, const Gen3Pkm* b) {
    return memcmp(a->raw + 0x14, b->raw + 0x14, GEN3_OT_NAME_LEN) == 0 &&
           gen3_pkm_get(a, Gen3FieldOtId) == gen3_pkm_get(b, Gen3FieldOtId) &&
           gen3_pkm_get(a, Gen3FieldOtGender) == gen3_pkm_get(b, Gen3FieldOtGender);
}

/* ---- Inputs ---- */

static uint16_t fz_national(void) {
    switch(fz_below(7)) {
    case 0:
    case 1:
        return GEN3_NATIONAL_UNOWN; /* for the shiny letter limits */
    case 2:
        return (uint16_t)fz_below(400); /* 0 and past 386 included */
    case 3: /* for the fateful rule (FRLG): set_fateful and imports can clear the bit */
        return fz_below(2) ? GEN3_NATIONAL_MEW : GEN3_NATIONAL_DEOXYS;
    default:
        return (uint16_t)(1 + fz_below(GEN3_NATIONAL_COUNT));
    }
}

/* The name shown (to confirm it unchanged), NULL, or a random string of mostly encodable
 * characters up to 3 longer than the field (max_chars). buf holds at least max_chars + 4. */
static const char* fz_name(char* buf, size_t max_chars, bool nickname, const Gen3Pkm* p) {
    static const char chars[] = " AZaz09?!.-,/:\"'\200\201~{\t";
    switch(fz_below(8)) {
    case 0:
        return NULL;
    case 1:
        if(nickname)
            gen3_pkm_nickname(p, buf, max_chars + 1);
        else
            gen3_pkm_ot_name(p, buf, max_chars + 1);
        return buf;
    default: {
        size_t len = fz_below((uint32_t)max_chars + 4);
        for(size_t i = 0; i < len; i++) buf[i] = chars[fz_below(sizeof(chars) - 1)];
        buf[len] = '\0';
        return buf;
    }
    }
}

/* Mostly an OT ID with a known shiny Unown profile: 22222/0 has a shiny PID for every (nature,
 * letter); 64893/0 and 0/64893 (the same tid ^ sid) lack 40 pairs, all on letters 12..19
 * (test_unown_shiny_possible). A shiny Unown gets one of those two more often. */
static uint32_t fz_ot_id(const Gen3Editor* ed) {
    static const uint32_t picks[] = {22222, 64893, 64893u << 16, 0, 0xFFFFFFFFu};
    Gen3Traits t;
    gen3_traits_of(&ed->pkm, &t);
    if(gen3_edit_national(ed) == GEN3_NATIONAL_UNOWN && t.shiny && fz_below(2))
        return picks[1 + fz_below(2)];
    return fz_below(4) ? picks[fz_below(5)] : fz();
}

/* A Pokemon to import or load: the editor's, the golden or noise, with a few fields changed to
 * values that are often out of range, then (usually) the checksum fixed */
static void fz_input(Gen3Pkm* in, const Gen3Pkm* cur) {
    switch(fz_below(4)) {
    case 0:
        for(size_t i = 0; i < GEN3_PARTY_SIZE; i++) in->raw[i] = (uint8_t)fz();
        break;
    case 1:
        memcpy(in->raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
        break;
    default:
        *in = *cur;
        break;
    }
    int changes = 1 + (int)fz_below(4);
    for(int c = 0; c < changes; c++) {
        switch(fz_below(10)) {
        case 0:
            gen3_pkm_set(in, Gen3FieldSpecies, gen3_internal_from_national(fz_national()));
            break;
        case 1:
            gen3_pkm_set(
                in,
                Gen3FieldHeldItem,
                fz_below(3) ? fz_below(GEN3_ITEM_COUNT + 20) :
                              GEN3_ITEM_MAIL_FIRST + fz_below(12));
            break;
        case 2:
            gen3_pkm_set(in, Gen3FieldMail, fz_below(2) ? fz_below(8) : fz());
            break;
        case 3:
            gen3_pkm_set(in, Gen3FieldExp, fz_below(2) ? fz_below(2000000) : fz());
            break;
        case 4: {
            int s = (int)fz_below(4);
            uint32_t m = fz_below(3) ? fz_below(GEN3_MOVE_COUNT + 5) : 0;
            if(fz_below(3) == 0) m = gen3_pkm_get(in, Gen3FieldMove1 + fz_below(4)); /* a dup */
            gen3_pkm_set(in, Gen3FieldMove1 + s, m);
            break;
        }
        case 5:
            gen3_pkm_set(in, Gen3FieldAbilityBit, fz());
            break;
        case 6: {
            static const Gen3Field flags[] = {
                Gen3FieldIsEgg, Gen3FieldEggFlag, Gen3FieldBadEgg, Gen3FieldHasSpecies};
            gen3_pkm_set(in, flags[fz_below(4)], fz());
            break;
        }
        case 7: /* the party fields: Status, Level, Mail, Hp, MaxHp .. Spd */
            gen3_pkm_set(in, Gen3FieldStatus + fz_below(Gen3FieldSpd - Gen3FieldStatus + 1), fz());
            break;
        default:
            gen3_pkm_set(in, (Gen3Field)fz_below(Gen3FieldCount), fz());
            break;
        }
    }
    if(fz_below(8)) gen3_pkm_fix_checksum(in);
}

/* ---- Steps ---- */

typedef struct {
    int unchanged; /* a call that refused, failed or had nothing to change: editor unchanged */
    int no_shiny; /* set_traits refused a shiny Unown want */
    int shiny_lost;
    int read_failures_seen; /* a step that found the bulk source closed after a failure */
    int imported[2]; /* Ok imports of 80 and 100 bytes */
    int loaded;
    int trainer_set;
    int pristine_trainer; /* set_trainer on a pristine editor, which takes the player's OT */
    int trainer_applied; /* use_trainer_ot that had an OT to change */
    int renewed; /* a new editor replaced the old one */
    int fateful_changed; /* set_fateful that flipped the bit */
} FuzzStats;

static FuzzStats stats;

static bool step_traits(Gen3Editor* ed) {
    uint16_t national = gen3_edit_national(ed);
    Gen3Traits cur, want;
    gen3_traits_of(&ed->pkm, &cur);
    want = cur;
    switch(fz_below(7)) {
    case 6: /* a shiny Unown on the letters some OTs can't have shiny */
        want.shiny = true;
        want.nature = (uint8_t)fz_below(GEN3_NATURE_COUNT);
        want.unown_letter = (uint8_t)(12 + fz_below(8));
        break;
    case 0:
        want.nature = (uint8_t)fz_below(GEN3_NATURE_COUNT + 2);
        break;
    case 1:
        want.gender = (Gen3Gender)fz_below(4);
        break;
    case 2:
        want.shiny = !want.shiny;
        break;
    case 3:
        want.ability_slot = (uint8_t)fz_below(3);
        break;
    case 4:
        want.unown_letter = (uint8_t)fz_below(GEN3_UNOWN_LETTER_COUNT + 3);
        break;
    default:
        want.nature = (uint8_t)fz_below(GEN3_NATURE_COUNT);
        want.shiny = fz_below(2);
        want.unown_letter = (uint8_t)fz_below(GEN3_UNOWN_LETTER_COUNT);
        break;
    }
    bool unown = national == GEN3_NATIONAL_UNOWN;
    bool fixed = gen3_species_gender_fixed(national);
    bool two = gen3_species_two_abilities(national);
    /* The normalised want, as gen3_pid_find takes it */
    bool same = want.nature == cur.nature && want.shiny == cur.shiny &&
                (fixed || want.gender == cur.gender) &&
                (!two || want.ability_slot == cur.ability_slot) &&
                (!unown || want.unown_letter == cur.unown_letter);
    bool bad = want.nature >= GEN3_NATURE_COUNT || (two && want.ability_slot > 1) ||
               (unown && want.unown_letter >= GEN3_UNOWN_LETTER_COUNT) ||
               (!fixed && want.gender != Gen3GenderMale && want.gender != Gen3GenderFemale);
    bool impossible = !bad && unown && want.shiny &&
                      !gen3_unown_shiny_possible(
                          gen3_pkm_get(&ed->pkm, Gen3FieldOtId), want.nature, want.unown_letter);
    Gen3Editor before = *ed;
    bool ok = gen3_edit_set_traits(ed, &want);
    CHECK(ok == (same || !(bad || impossible)), "set_traits result");
    if(!ok) {
        CHECK(editor_eq(ed, &before), "set_traits refused but changed the editor");
        stats.unchanged++;
        if(impossible) stats.no_shiny++;
        return true;
    }
    if(same) CHECK(editor_eq(ed, &before), "set_traits to the same traits changed the editor");
    Gen3Traits got;
    gen3_traits_of(&ed->pkm, &got);
    CHECK(got.nature == want.nature && got.shiny == want.shiny, "set_traits nature/shiny");
    CHECK(fixed || got.gender == want.gender, "set_traits gender");
    CHECK(!two || got.ability_slot == want.ability_slot, "set_traits slot");
    CHECK(!unown || got.unown_letter == want.unown_letter, "set_traits letter");
    return true;
}

static bool step_ot_id(Gen3Editor* ed) {
    uint32_t ot_id = fz_ot_id(ed);
    Gen3Traits old, got;
    gen3_traits_of(&ed->pkm, &old);
    Gen3OtIdResult r = gen3_edit_set_ot_id(ed, (uint16_t)ot_id, (uint16_t)(ot_id >> 16));
    CHECK(gen3_pkm_get(&ed->pkm, Gen3FieldOtId) == ot_id, "OT ID not applied");
    gen3_traits_of(&ed->pkm, &got);
    CHECK(got.nature == old.nature && got.gender == old.gender, "set_ot_id nature/gender");
    CHECK(got.ability_slot == old.ability_slot, "set_ot_id ability slot");
    bool unown = gen3_edit_national(ed) == GEN3_NATIONAL_UNOWN;
    CHECK(!unown || got.unown_letter == old.unown_letter, "set_ot_id letter");
    if(r == Gen3OtIdShinyLost) {
        CHECK(old.shiny && !got.shiny, "shiny lost, but it wasn't shiny or still is");
        CHECK(unown, "shiny lost on a non-Unown");
        CHECK(!gen3_unown_shiny_possible(ot_id, old.nature, old.unown_letter), "lost needlessly");
        stats.shiny_lost++;
    } else {
        CHECK(r == Gen3OtIdOk && got.shiny == old.shiny, "set_ot_id shininess");
    }
    return true;
}

/* The GBA player: random name bytes (EOS 0xFF often), an OT ID as fz_ot_id picks them, gender 0
 * or 1 */
static bool step_trainer(Gen3Editor* ed) {
    Gen3Trainer t;
    memset(&t, 0, sizeof(t));
    for(int i = 0; i < GEN3_OT_NAME_LEN; i++) t.name[i] = fz_below(3) ? (uint8_t)fz() : 0xFF;
    uint32_t ot_id = fz_ot_id(ed);
    t.tid = (uint16_t)ot_id;
    t.sid = (uint16_t)(ot_id >> 16);
    t.gender = (uint8_t)fz_below(2);
    Gen3Editor before = *ed;
    Gen3Traits old, got;
    gen3_traits_of(&ed->pkm, &old);
    gen3_edit_set_trainer(ed, &t);
    const Gen3Trainer* kept = gen3_edit_trainer(ed);
    CHECK(ed->has_trainer && kept != NULL, "set_trainer: no player");
    CHECK(
        memcmp(kept->name, t.name, GEN3_OT_NAME_LEN) == 0 && kept->tid == t.tid &&
            kept->sid == t.sid && kept->gender == t.gender,
        "set_trainer: a different player");
    stats.trainer_set++;
    if(!before.pristine) {
        CHECK(memcmp(ed->pkm.raw, before.pkm.raw, GEN3_PARTY_SIZE) == 0, "edited Pokemon changed");
        CHECK(ed->rng.state == before.rng.state, "set_trainer used the RNG");
        return true;
    }
    /* The initial default takes the player's OT, and is otherwise the same Pokemon */
    CHECK(gen3_edit_ot_is_trainer(ed), "pristine editor without the player's OT");
    gen3_traits_of(&ed->pkm, &got);
    CHECK(
        got.nature == old.nature && got.gender == old.gender && got.shiny == old.shiny &&
            got.ability_slot == old.ability_slot,
        "set_trainer changed a trait");
    CHECK(gen3_edit_national(ed) == gen3_edit_national(&before), "set_trainer species");
    CHECK(
        gen3_pkm_get(&ed->pkm, Gen3FieldExp) == gen3_pkm_get(&before.pkm, Gen3FieldExp),
        "set_trainer exp");
    stats.pristine_trainer++;
    return true;
}

static bool step_use_trainer(Gen3Editor* ed) {
    Gen3Editor before = *ed;
    Gen3Traits old, got;
    gen3_traits_of(&ed->pkm, &old);
    bool already = gen3_edit_ot_is_trainer(ed);
    Gen3UseTrainerResult r = gen3_edit_use_trainer_ot(ed);
    CHECK((r == Gen3UseTrainerNoTrainer) == !before.has_trainer, "use_trainer_ot result");
    if(r == Gen3UseTrainerNoTrainer || already) {
        CHECK(r != Gen3UseTrainerShinyLost, "shiny lost, but the OT was the player's");
        CHECK(editor_eq(ed, &before), "use_trainer_ot with nothing to do changed the editor");
        stats.unchanged++;
        return true;
    }
    stats.trainer_applied++;
    CHECK(gen3_edit_ot_is_trainer(ed), "use_trainer_ot: not the player's OT");
    gen3_traits_of(&ed->pkm, &got);
    CHECK(got.nature == old.nature && got.gender == old.gender, "use_trainer_ot nature/gender");
    CHECK(got.ability_slot == old.ability_slot, "use_trainer_ot ability slot");
    bool unown = gen3_edit_national(ed) == GEN3_NATIONAL_UNOWN;
    CHECK(!unown || got.unown_letter == old.unown_letter, "use_trainer_ot letter");
    if(r == Gen3UseTrainerShinyLost) {
        CHECK(old.shiny && !got.shiny && unown, "shiny lost, but not a shiny Unown");
        CHECK(
            !gen3_unown_shiny_possible(
                gen3_pkm_get(&ed->pkm, Gen3FieldOtId), old.nature, old.unown_letter),
            "lost needlessly");
        stats.shiny_lost++;
    } else {
        CHECK(r == Gen3UseTrainerOk && got.shiny == old.shiny, "use_trainer_ot shininess");
    }
    return true;
}

static bool step_import(Gen3Editor* ed) {
    Gen3Pkm in, out;
    Gen3ImportInfo info;
    size_t len = fz_below(2) ? GEN3_BOX_SIZE : GEN3_PARTY_SIZE;
    fz_input(&in, &ed->pkm);
    memset(&out, 0x5A, sizeof(out));
    Gen3Check c = gen3_pk3_import(in.raw, len, &out, &info);

    /* What it should say, short of the bulk data */
    Gen3Pkm as_read;
    memset(&as_read, 0, sizeof(as_read));
    memcpy(as_read.raw, in.raw, len);
    Gen3Check want = gen3_pkm_checksum(&as_read) != gen3_pkm_get(&as_read, Gen3FieldChecksum) ?
                         Gen3CheckBadChecksum :
                         gen3_pkm_check(&as_read);
    bool has_move = false;
    for(int s = 0; s < 4; s++)
        if(gen3_pkm_get(&as_read, Gen3FieldMove1 + s)) has_move = true;
    if(c == Gen3CheckNoData) {
        CHECK(want == Gen3CheckOk && !has_move, "NoData for a Pokemon that needs no bulk data");
    } else {
        CHECK(c == want, "import result");
    }
    if(c != Gen3CheckOk) {
        for(size_t i = 0; i < sizeof(out.raw); i++) CHECK(out.raw[i] == 0x5A, "out touched");
        return true;
    }
    if(!fuzz_valid(&out)) return false;
    stats.imported[len == GEN3_PARTY_SIZE]++;
    /* As the Load scene does: the editor takes it, unchanged (normalising is idempotent) */
    CHECK(gen3_edit_load(ed, &out), "load of an imported Pokemon refused");
    CHECK(memcmp(ed->pkm.raw, out.raw, GEN3_PARTY_SIZE) == 0, "load changed an import");
    CHECK(!ed->preset_applied, "preset_applied after load");
    return true;
}

static bool step_load(Gen3Editor* ed) {
    Gen3Pkm in;
    fz_input(&in, &ed->pkm);
    bool has_move = false;
    for(int s = 0; s < 4; s++)
        if(gen3_pkm_get(&in, Gen3FieldMove1 + s)) has_move = true;
    Gen3Check check = gen3_pkm_check(&in);
    Gen3Editor before = *ed;
    bool ok = gen3_edit_load(ed, &in);
    if(check != Gen3CheckOk) CHECK(!ok, "load took what gen3_pkm_check refuses");
    if(check == Gen3CheckOk && has_move) CHECK(ok, "load refused a valid Pokemon");
    if(!ok) {
        CHECK(editor_eq(ed, &before), "load refused but changed the editor");
        stats.unchanged++;
        return true;
    }
    stats.loaded++;
    CHECK(!ed->preset_applied, "preset_applied after load");
    /* With a move, normalising reads no bulk data: the same as gen3_normalise alone */
    if(has_move) {
        Gen3Pkm n = in;
        CHECK(gen3_normalise(&n, NULL), "normalise refused what load took");
        CHECK(memcmp(n.raw, ed->pkm.raw, GEN3_PARTY_SIZE) == 0, "load != normalise");
    }
    return true;
}

/* One random call. keep: the call refused, failed or had nothing to change, so the editor must be
 * exactly as it was. */
static bool step(Gen3Editor* ed) {
    Gen3Editor before = *ed;
    char name[16];
    bool keep = false;
    bool set_trainer = false;
    bool renewed = false;

    switch(fz_below(19)) {
    case 0: {
        fz_what = "set_species";
        uint16_t national = fz_national();
        bool same = national == gen3_edit_national(ed);
        bool ok = gen3_edit_set_species(ed, national);
        if(!gen3_species(national)) CHECK(!ok, "set_species took an unknown species");
        if(same) CHECK(ok, "set_species refused the current species");
        if(ok && !same) {
            CHECK(gen3_edit_national(ed) == national, "set_species species");
            /* D5, amended by D7: the player's OT once known, else the one it had */
            if(ed->has_trainer)
                CHECK(gen3_edit_ot_is_trainer(ed), "set_species without the player's OT");
            else
                CHECK(ot_eq(&ed->pkm, &before.pkm), "set_species changed the OT");
            CHECK(
                !gen3_is_shiny(
                    gen3_pkm_get(&ed->pkm, Gen3FieldOtId), gen3_pkm_get(&ed->pkm, Gen3FieldPid)),
                "set_species made a shiny");
        }
        keep = !ok || same;
        break;
    }
    case 1: {
        fz_what = "set_level";
        uint8_t level = (uint8_t)fz();
        uint8_t clamped = level < 1 ? 1 : (level > 100 ? 100 : level);
        keep = clamped == gen3_pkm_get(&ed->pkm, Gen3FieldLevel);
        gen3_edit_set_level(ed, level);
        CHECK(gen3_pkm_get(&ed->pkm, Gen3FieldLevel) == clamped, "set_level level");
        break;
    }
    case 2: {
        fz_what = "set_preset";
        uint32_t preset = fz_below(GEN3_PRESET_COUNT + 2);
        gen3_edit_set_preset(ed, (Gen3Preset)preset);
        keep = preset >= GEN3_PRESET_COUNT;
        if(!keep) CHECK(ed->preset == preset && ed->preset_applied, "set_preset");
        break;
    }
    case 3: {
        fz_what = "set_nickname";
        const char* text = fz_name(name, GEN3_NICKNAME_LEN, true, &ed->pkm);
        char shown[GEN3_NICKNAME_LEN + 1];
        gen3_pkm_nickname(&ed->pkm, shown, sizeof(shown));
        bool ok = gen3_edit_set_nickname(ed, text);
        /* Only "" or NULL reads the bulk data (the species name), so only they can fail. A name
         * equal to the one shown changes nothing; "" stands for the species name, not "". */
        bool named = text && text[0];
        if(named) CHECK(ok, "set_nickname refused a name");
        keep = !ok || (named && strcmp(text, shown) == 0);
        break;
    }
    case 4: {
        fz_what = "set_ot_name";
        const char* text = fz_name(name, GEN3_OT_NAME_LEN, false, &ed->pkm);
        char shown[GEN3_OT_NAME_LEN + 1];
        gen3_pkm_ot_name(&ed->pkm, shown, sizeof(shown));
        keep = strcmp(text ? text : "", shown) == 0;
        gen3_edit_set_ot_name(ed, text);
        break;
    }
    case 5:
        fz_what = "set_ot_id";
        if(!step_ot_id(ed)) return false;
        break;
    case 6: {
        fz_what = "set_item";
        /* A failed read of the holdable list gives item 0, which is valid too */
        uint16_t item = (uint16_t)(fz_below(2) ?
                                       fz_below(GEN3_ITEM_COUNT + 30) :
                                       gen3_holdable_by_name_get(fz_below(GEN3_HOLDABLE_COUNT)));
        gen3_edit_set_item(ed, item);
        keep = item != 0 && !gen3_item_holdable(item);
        if(!keep) CHECK(gen3_pkm_get(&ed->pkm, Gen3FieldHeldItem) == item, "set_item item");
        break;
    }
    case 7: {
        fz_what = "set_move";
        uint8_t slot = (uint8_t)fz_below(5);
        uint16_t move = (uint16_t)(fz_below(4) ? fz_below(GEN3_MOVE_COUNT + 5) : 0);
        int others = 0;
        bool dup = false;
        for(int s = 0; s < 4; s++) {
            uint32_t m = gen3_pkm_get(&ed->pkm, Gen3FieldMove1 + s);
            if(s == slot) continue;
            if(m) others++;
            if(move && m == move) dup = true;
        }
        bool want = slot < 4 && move < GEN3_MOVE_COUNT && !dup && (move || others);
        bool ok = gen3_edit_set_move(ed, slot, move);
        CHECK(ok == want, "set_move result");
        keep = !ok;
        break;
    }
    case 8:
        fz_what = "default_moves";
        keep = !gen3_edit_default_moves(ed);
        break;
    case 9: {
        fz_what = "set_ball";
        uint8_t ball = (uint8_t)fz_below(GEN3_EDIT_BALL_MAX + 4);
        gen3_edit_set_ball(ed, ball);
        keep = ball < 1 || ball > GEN3_EDIT_BALL_MAX;
        if(!keep) CHECK(gen3_pkm_get(&ed->pkm, Gen3FieldBall) == ball, "set_ball ball");
        break;
    }
    case 10: {
        fz_what = "set_pokerus";
        uint8_t value = (uint8_t)fz();
        gen3_edit_set_pokerus(ed, value);
        CHECK(gen3_pkm_get(&ed->pkm, Gen3FieldPokerus) == value, "set_pokerus value");
        break;
    }
    case 11:
    case 12:
        fz_what = "set_traits";
        if(!step_traits(ed)) return false;
        break;
    case 13:
        fz_what = "import";
        if(!step_import(ed)) return false;
        break;
    case 14:
        fz_what = "load";
        if(!step_load(ed)) return false;
        break;
    case 15:
        fz_what = "set_trainer";
        set_trainer = true;
        if(!step_trainer(ed)) return false;
        break;
    case 16:
        fz_what = "use_trainer_ot";
        if(!step_use_trainer(ed)) return false;
        break;
    case 17: {
        fz_what = "set_fateful";
        bool on = fz_below(2);
        bool was = gen3_pkm_get(&ed->pkm, Gen3FieldFateful);
        gen3_edit_set_fateful(ed, on);
        CHECK(gen3_pkm_get(&ed->pkm, Gen3FieldFateful) == on, "set_fateful bit");
        if(was != on) stats.fateful_changed++;
        break;
    }
    default: {
        fz_what = "bulk";
        /* Reads fail now and then, often, or not at all; a closed source is opened again. A new
         * editor made meanwhile is game-valid and pristine, or refused; now and then it replaces
         * the editor, as when the app makes a new one. */
        static const uint32_t rates[] = {0, 0, 2, 5, 20};
        fz_fail_one_in = rates[fz_below(5)];
        if(!gen3_bulk_ready()) CHECK(fuzz_reopen(), "reopen failed");
        Gen3Editor fresh;
        if(gen3_edit_init(&fresh, fz())) {
            if(!fuzz_valid(&fresh.pkm)) return false;
            CHECK(fresh.pristine && !fresh.has_trainer, "a new editor: not pristine, or a player");
            if(fz_below(8) == 0) {
                *ed = fresh;
                renewed = true;
                stats.renewed++;
            }
        }
        break;
    }
    }

    /* pristine (gen3_edit.h) */
    bool same_pkm = memcmp(ed->pkm.raw, before.pkm.raw, GEN3_PARTY_SIZE) == 0;
    if(set_trainer)
        CHECK(ed->pristine == before.pristine, "set_trainer changed pristine");
    else if(!renewed)
        CHECK(ed->pristine == (before.pristine && same_pkm), "pristine");

    if(keep) {
        CHECK(editor_eq(ed, &before), "a refused, failed or no-op call changed the editor");
        stats.unchanged++;
    }
    return fuzz_valid(&ed->pkm);
}

TEST(test_fuzz_invariant) {
    Gen3Editor ed;
    ASSERT_TRUE(fuzz_reopen());
    ASSERT_TRUE(gen3_edit_init(&ed, FUZZ_SEED));
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(fuzz_valid(&ed.pkm));
    for(fz_step = 0; fz_step < FUZZ_STEPS; fz_step++) {
        bool was_ready = gen3_bulk_ready();
        ASSERT_TRUE(step(&ed));
        if(was_ready && !gen3_bulk_ready()) stats.read_failures_seen++;
        /* Mostly with the data: a closed source is opened again soon */
        if(!gen3_bulk_ready() && fz_below(4) == 0) ASSERT_TRUE(fuzz_reopen());
    }
    /* Every path was taken (with this seed: 3376 unchanged, 6 no-shiny refusals, 5 shiny lost,
     * 787 closed by a failed read, 312 + 307 imports, 667 loads, 1096 players set, 15 of them on
     * a pristine editor, 664 player OTs applied, 111 new editors, 520 fateful bits flipped, and
     * 656 checks of a Mew or Deoxys without it) */
    ASSERT_TRUE(stats.unchanged > 0);
    ASSERT_TRUE(stats.no_shiny > 0);
    ASSERT_TRUE(stats.shiny_lost > 0);
    ASSERT_TRUE(stats.read_failures_seen > 0);
    ASSERT_TRUE(stats.imported[0] > 0 && stats.imported[1] > 0);
    ASSERT_TRUE(stats.loaded > 0);
    ASSERT_TRUE(stats.trainer_set > 0);
    ASSERT_TRUE(stats.pristine_trainer > 0);
    ASSERT_TRUE(stats.trainer_applied > 0);
    ASSERT_TRUE(stats.renewed > 0);
    ASSERT_TRUE(stats.fateful_changed > 0);
    ASSERT_TRUE(fz_unfateful_seen > 0);
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_fuzz_invariant);
    TEST_EXIT();
}

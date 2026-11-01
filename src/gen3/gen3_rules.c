#include "gen3_rules.h"
#include "gen3_data.h"

uint32_t gen3_exp_for_level(uint8_t growth, uint8_t level) {
    if(level > 100) level = 100;
    if(level <= 1) return level;
    /* Formulas from pokeemerald src/data/pokemon/experience_tables.h, signed 32-bit. */
    int32_t n = level;
    int32_t n3 = n * n * n;
    int32_t r;
    switch(growth) {
    case Gen3GrowthMediumFast:
        r = n3;
        break;
    case Gen3GrowthErratic:
        if(n <= 50)
            r = (100 - n) * n3 / 50;
        else if(n <= 68)
            r = (150 - n) * n3 / 100;
        else if(n <= 98)
            r = ((1911 - 10 * n) / 3) * n3 / 500;
        else
            r = (160 - n) * n3 / 100;
        break;
    case Gen3GrowthFluctuating:
        if(n <= 15)
            r = ((n + 1) / 3 + 24) * n3 / 50;
        else if(n <= 36)
            r = (n + 14) * n3 / 50;
        else
            r = (n / 2 + 32) * n3 / 50;
        break;
    case Gen3GrowthMediumSlow:
        r = 6 * n3 / 5 - 15 * n * n + 100 * n - 140;
        break;
    case Gen3GrowthFast:
        r = 4 * n3 / 5;
        break;
    default:
        r = 5 * n3 / 4;
        break;
    }
    return (uint32_t)r;
}

uint8_t gen3_level_for_exp(uint8_t growth, uint32_t exp) {
    if(exp == 0) return 0;
    uint8_t level = 100;
    while(level > 1 && gen3_exp_for_level(growth, level) > exp) level--;
    return level;
}

int8_t gen3_nature_effect(uint8_t nature, Gen3Stat stat) {
    /* gNatureStatTable in pokeemerald src/pokemon.c: over Atk, Def, Spe, SpA, SpD, nature n
     * raises stat n / 5 and lowers stat n % 5; equal indices are neutral. */
    if(stat == Gen3StatHp || stat >= Gen3StatCount || nature >= 25) return 0;
    uint8_t up = nature / 5;
    uint8_t down = nature % 5;
    if(up == down) return 0;
    uint8_t idx = stat - 1;
    if(idx == up) return 1;
    if(idx == down) return -1;
    return 0;
}

uint16_t gen3_stat_value(
    Gen3Stat stat,
    uint8_t base,
    uint8_t iv,
    uint8_t ev,
    uint8_t level,
    uint8_t nature) {
    uint32_t v = (2u * base + iv + ev / 4u) * level / 100u;
    if(stat == Gen3StatHp) return (uint16_t)(v + level + 10);
    v += 5;
    int8_t e = gen3_nature_effect(nature, stat);
    if(e > 0)
        v = v * 110 / 100;
    else if(e < 0)
        v = v * 90 / 100;
    return (uint16_t)v;
}

void gen3_calc_stats(const Gen3Pkm* p, uint16_t out[Gen3StatCount]) {
    for(int s = 0; s < Gen3StatCount; s++) out[s] = 0;
    uint16_t national = gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies));
    const Gen3SpeciesInfo* info = gen3_species(national);
    if(!info) return;
    uint8_t level = gen3_level_for_exp(info->growth, gen3_pkm_get(p, Gen3FieldExp));
    uint8_t nature = gen3_pkm_get(p, Gen3FieldPid) % 25;
    for(int s = 0; s < Gen3StatCount; s++) {
        out[s] = gen3_stat_value(
            (Gen3Stat)s,
            info->base[s],
            gen3_pkm_get(p, Gen3FieldIvHp + s),
            gen3_pkm_get(p, Gen3FieldEvHp + s),
            level,
            nature);
    }
    if(national == GEN3_NATIONAL_SHEDINJA) out[Gen3StatHp] = 1;
}

void gen3_recalc_party(Gen3Pkm* p) {
    const Gen3SpeciesInfo* info =
        gen3_species(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
    if(!info) return;
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(p, st);
    gen3_pkm_set(
        p, Gen3FieldLevel, gen3_level_for_exp(info->growth, gen3_pkm_get(p, Gen3FieldExp)));
    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(p, Gen3FieldMaxHp + s, st[s]);
    gen3_pkm_set(p, Gen3FieldHp, st[Gen3StatHp]);
}

Gen3Gender gen3_gender_of(uint8_t gender_ratio, uint32_t pid) {
    switch(gender_ratio) {
    case 255:
        return Gen3GenderNone;
    case 254:
        return Gen3GenderFemale;
    case 0:
        return Gen3GenderMale;
    default:
        return gender_ratio > (pid & 0xFF) ? Gen3GenderFemale : Gen3GenderMale;
    }
}

bool gen3_is_shiny(uint32_t ot_id, uint32_t pid) {
    uint32_t tid = ot_id & 0xFFFF;
    uint32_t sid = ot_id >> 16;
    return (tid ^ sid ^ (pid >> 16) ^ (pid & 0xFFFF)) < 8;
}

uint8_t gen3_unown_letter(uint32_t pid) {
    uint32_t v = ((pid & 0x03000000) >> 18) | ((pid & 0x00030000) >> 12) |
                 ((pid & 0x00000300) >> 6) | (pid & 0x00000003);
    return v % GEN3_UNOWN_LETTER_COUNT;
}

char gen3_unown_char(uint8_t letter) {
    if(letter < 26) return (char)('A' + letter);
    return letter == 26 ? '!' : '?';
}

void gen3_traits_of(const Gen3Pkm* p, Gen3Traits* out) {
    uint32_t pid = gen3_pkm_get(p, Gen3FieldPid);
    const Gen3SpeciesInfo* info =
        gen3_species(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
    out->nature = pid % 25;
    out->gender = info ? gen3_gender_of(info->gender_ratio, pid) : Gen3GenderNone;
    out->shiny = gen3_is_shiny(gen3_pkm_get(p, Gen3FieldOtId), pid);
    out->ability_slot = gen3_pkm_get(p, Gen3FieldAbilityBit);
    out->unown_letter = gen3_unown_letter(pid);
}

void gen3_rng_seed(Gen3Rng* rng, uint32_t seed) {
    rng->state = seed ? seed : 0x6D2B79F5u;
}

uint32_t gen3_rng_next(Gen3Rng* rng) {
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

bool gen3_unown_shiny_possible(uint32_t ot_id, uint8_t nature, uint8_t letter) {
    if(nature >= GEN3_NATURE_COUNT || letter >= GEN3_UNOWN_LETTER_COUNT) return false;
    uint32_t tid_sid = (ot_id & 0xFFFF) ^ (ot_id >> 16);
    /* v is the letter's 8-bit value (gen3_unown_letter): bits 7-6 = PID bits 25-24 (high half
     * bits 9-8), 5-4 = PID bits 17-16 (high bits 1-0), 3-2 = PID bits 9-8, 1-0 = PID bits 1-0. */
    for(uint32_t v = letter; v < 256; v += GEN3_UNOWN_LETTER_COUNT) {
        uint32_t lo98 = (v >> 2) & 3;
        /* k < 8 doesn't reach high bits 9-8: they are the low half's xor tid ^ sid's */
        if(((v >> 6) & 3) != (lo98 ^ ((tid_sid >> 8) & 3))) continue;
        uint32_t hi10 = (v >> 4) & 3;
        /* Each shiny PID with these letter bits: the low half's 12 other bits, and high bit 2
         * (k bits 1-0 are whatever gives high bits 1-0 = hi10; k bit 2 is free) */
        for(uint32_t f = 0; f < 4096; f++) {
            uint32_t lo = (v & 3) | (f & 0x3F) << 2 | lo98 << 8 | (f >> 6) << 10;
            uint32_t hi = ((tid_sid ^ lo) & ~3u) | hi10;
            if(((hi << 16) | lo) % GEN3_NATURE_COUNT == nature ||
               (((hi ^ 4) << 16) | lo) % GEN3_NATURE_COUNT == nature)
                return true;
        }
    }
    return false;
}

bool gen3_pid_find(
    Gen3Rng* rng,
    uint16_t national,
    uint32_t ot_id,
    const Gen3Traits* want,
    uint32_t* pid) {
    const Gen3SpeciesInfo* info = gen3_species(national);
    if(!info) return false;
    /* A fixed ratio has one gender for every PID */
    Gen3Gender gender =
        gen3_species_gender_fixed(national) ? gen3_gender_of(info->gender_ratio, 0) : want->gender;
    /* Unsatisfiable wants fail fast instead of spinning through GEN3_PID_MAX_TRIES. */
    bool two_abilities = gen3_species_two_abilities(national);
    bool unown = national == GEN3_NATIONAL_UNOWN;
    if(want->nature >= GEN3_NATURE_COUNT) return false;
    if(two_abilities && want->ability_slot > 1) return false;
    if(unown && want->unown_letter >= GEN3_UNOWN_LETTER_COUNT) return false;
    if(gender != Gen3GenderMale && gender != Gen3GenderFemale &&
       !gen3_species_gender_fixed(national))
        return false;
    if(unown && want->shiny && !gen3_unown_shiny_possible(ot_id, want->nature, want->unown_letter))
        return false;
    uint32_t tid_sid = (ot_id & 0xFFFF) ^ (ot_id >> 16);

    for(uint32_t i = 0; i < GEN3_PID_MAX_TRIES; i++) {
        uint32_t r = gen3_rng_next(rng);
        uint32_t lo = r & 0xFFFF;
        uint32_t hi = r >> 16;
        if(want->shiny) hi = tid_sid ^ lo ^ (gen3_rng_next(rng) & 7);
        uint32_t cand = (hi << 16) | lo;
        if(cand % 25 != want->nature) continue;
        if(gen3_gender_of(info->gender_ratio, cand) != gender) continue;
        if(two_abilities && (cand & 1) != want->ability_slot) continue;
        if(unown && gen3_unown_letter(cand) != want->unown_letter) continue;
        if(gen3_is_shiny(ot_id, cand) != want->shiny) continue;
        *pid = cand;
        return true;
    }
    return false;
}

size_t gen3_default_moves(uint16_t national, uint8_t level, uint16_t moves[4]) {
    for(int i = 0; i < 4; i++) moves[i] = 0;
    uint16_t entries[GEN3_LEARNSET_MAX];
    size_t n = gen3_learnset_get(national, entries, GEN3_LEARNSET_MAX);
    size_t filled = 0;
    for(size_t i = 0; i < n; i++) {
        if((entries[i] >> 9) > level) break;
        uint16_t mv = entries[i] & 0x1FF;
        bool dup = false;
        for(int s = 0; s < 4; s++)
            if(moves[s] == mv) dup = true;
        if(dup) continue;
        int slot = -1;
        for(int s = 0; s < 4 && slot < 0; s++)
            if(moves[s] == 0) slot = s;
        if(slot < 0) {
            for(int s = 0; s < 3; s++) moves[s] = moves[s + 1];
            slot = 3;
        } else {
            filled++;
        }
        moves[slot] = mv;
    }
    return filled;
}

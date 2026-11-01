#include "gen3_pk3.h"
#include "gen3_data.h"
#include <string.h>

const char* gen3_check_text(Gen3Check c) {
    switch(c) {
    case Gen3CheckOk:
        return "OK";
    case Gen3CheckBadSize:
        return "Not a .pk3 (size)";
    case Gen3CheckBadChecksum:
        return "Checksum mismatch";
    case Gen3CheckBadEgg:
        return "Bad Egg";
    case Gen3CheckEgg:
        return "Eggs not supported";
    case Gen3CheckBadSpecies:
        return "Unknown species";
    case Gen3CheckBadMove:
        return "Unknown move";
    case Gen3CheckBadItem:
        return "Unknown item";
    case Gen3CheckFileError:
        return "Can't read file";
    case Gen3CheckNoData:
        return "Gen III data unreadable";
    }
    return "?";
}

Gen3Check gen3_pkm_check(const Gen3Pkm* p) {
    if(gen3_pkm_get(p, Gen3FieldBadEgg)) return Gen3CheckBadEgg;
    if(gen3_pkm_get(p, Gen3FieldIsEgg) || gen3_pkm_get(p, Gen3FieldEggFlag)) return Gen3CheckEgg;
    if(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)) == 0)
        return Gen3CheckBadSpecies;
    for(int i = 0; i < 4; i++) {
        if(gen3_pkm_get(p, Gen3FieldMove1 + i) >= GEN3_MOVE_COUNT) return Gen3CheckBadMove;
    }
    if(gen3_pkm_get(p, Gen3FieldHeldItem) >= GEN3_ITEM_COUNT) return Gen3CheckBadItem;
    return Gen3CheckOk;
}

Gen3Check gen3_pk3_import(const uint8_t* data, size_t len, Gen3Pkm* out, Gen3ImportInfo* info) {
    if(info) memset(info, 0, sizeof(*info));
    if(len != GEN3_BOX_SIZE && len != GEN3_PARTY_SIZE) return Gen3CheckBadSize;

    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    memcpy(p.raw, data, len);
    if(gen3_pkm_checksum(&p) != gen3_pkm_get(&p, Gen3FieldChecksum)) return Gen3CheckBadChecksum;
    Gen3Check c = gen3_pkm_check(&p);
    if(c != Gen3CheckOk) return c;

    /* A box Pokemon has no party fields: no status, no mail index to report as cleared */
    if(len == GEN3_BOX_SIZE) {
        gen3_pkm_set(&p, Gen3FieldStatus, 0);
        gen3_pkm_set(&p, Gen3FieldMail, GEN3_MAIL_NONE);
    }

    Gen3NormaliseInfo ni;
    /* gen3_pkm_check has accepted the species, so the only failure left is a moveless Pokemon
     * whose default moveset can't be read. It also removes a held Mail item, sets the mail index
     * to 0xFF and HasSpecies to 1. */
    if(!gen3_normalise(&p, &ni)) return Gen3CheckNoData;
    /* A box Pokemon has no stored HP: start full. */
    if(len == GEN3_BOX_SIZE) gen3_pkm_set(&p, Gen3FieldHp, gen3_pkm_get(&p, Gen3FieldMaxHp));

    *out = p;
    if(info) {
        info->normalise = ni;
        info->mail_removed = ni.mail_item_removed;
    }
    return Gen3CheckOk;
}

void gen3_pk3_export(const Gen3Pkm* p, uint8_t out[GEN3_BOX_SIZE]) {
    Gen3Pkm q = *p;
    gen3_pkm_fix_checksum(&q);
    memcpy(out, q.raw, GEN3_BOX_SIZE);
}

void gen3_pk3_default_name(const Gen3Pkm* p, char* out, size_t out_size) {
    if(out_size == 0) return;
    char nick[GEN3_NICKNAME_LEN + 2];
    gen3_pkm_nickname(p, nick, sizeof(nick));
    size_t n = 0;
    for(const char* s = nick; *s && n < GEN3_PK3_NAME_MAX && n + 1 < out_size; s++) {
        char ch = *s;
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '-';
        if(ok) out[n++] = ch;
    }
    out[n] = 0;
    if(n == 0) {
        strncpy(out, "POKEMON", out_size - 1);
        out[out_size - 1] = 0;
    }
}

bool gen3_pk3_name_valid(const char* name) {
    size_t n = 0;
    for(; name[n]; n++) {
        char ch = name[n];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if(!ok || n >= GEN3_PK3_NAME_MAX) return false;
    }
    return n >= 1;
}

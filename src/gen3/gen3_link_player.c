#include <src/gen3/gen3_link_player.h>

#include <string.h>

#define LP_OFF_MAGIC1    0x00
#define LP_OFF_VERSION   0x10
#define LP_OFF_FIELD2    0x12
#define LP_OFF_TRAINERID 0x14
#define LP_OFF_NAME      0x18
#define LP_OFF_GENDER    0x23
#define LP_OFF_LINKTYPE  0x24
#define LP_OFF_ID        0x28
#define LP_OFF_LANGUAGE  0x2A
#define LP_OFF_MAGIC2    0x2C
#define MAGIC_SIZE       16
#define MAGIC_CMP_SIZE   15

#define CARD_OFF_GENDER     0x00
#define CARD_OFF_STARS      0x01
#define CARD_OFF_HASPOKEDEX 0x02
#define CARD_OFF_TRAINERID  0x0E
#define CARD_OFF_VAR28      0x28
#define CARD_OFF_NAME       0x30

static const uint8_t gen3_magic[MAGIC_SIZE] =
    {0x47, 0x61, 0x6D, 0x65, 0x46, 0x72, 0x65, 0x61, 0x6B, 0x20, 0x69, 0x6E, 0x63, 0x2E, 0x00, 0x00};

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void gen3_link_player_block_build(const Gen3LinkPlayer* lp, uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE]) {
    memset(out, 0, GEN3_LINK_PLAYER_BLOCK_SIZE);
    memcpy(out + LP_OFF_MAGIC1, gen3_magic, MAGIC_SIZE);
    put_u16(out + LP_OFF_VERSION, lp->version);
    put_u16(out + LP_OFF_FIELD2, lp->lp_field_2);
    put_u32(out + LP_OFF_TRAINERID, lp->trainer_id);
    memcpy(out + LP_OFF_NAME, lp->name, sizeof(lp->name));
    out[LP_OFF_GENDER] = lp->gender;
    put_u32(out + LP_OFF_LINKTYPE, lp->link_type);
    put_u16(out + LP_OFF_ID, lp->id);
    put_u16(out + LP_OFF_LANGUAGE, lp->language);
    memcpy(out + LP_OFF_MAGIC2, gen3_magic, MAGIC_SIZE);
}

bool gen3_link_player_block_parse(const uint8_t in[GEN3_LINK_PLAYER_BLOCK_SIZE], Gen3LinkPlayer* lp) {
    if(memcmp(in + LP_OFF_MAGIC1, gen3_magic, MAGIC_CMP_SIZE) != 0) return false;
    if(memcmp(in + LP_OFF_MAGIC2, gen3_magic, MAGIC_CMP_SIZE) != 0) return false;
    lp->version = get_u16(in + LP_OFF_VERSION);
    lp->lp_field_2 = get_u16(in + LP_OFF_FIELD2);
    lp->trainer_id = get_u32(in + LP_OFF_TRAINERID);
    memcpy(lp->name, in + LP_OFF_NAME, sizeof(lp->name));
    lp->gender = in[LP_OFF_GENDER];
    lp->link_type = get_u32(in + LP_OFF_LINKTYPE);
    lp->id = get_u16(in + LP_OFF_ID);
    lp->language = get_u16(in + LP_OFF_LANGUAGE);
    return true;
}

void gen3_trainer_card_build(const Gen3LinkPlayer* lp, uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE]) {
    memset(out, 0, GEN3_TRAINER_CARD_BLOCK_SIZE);
    out[CARD_OFF_GENDER] = lp->gender;
    out[CARD_OFF_STARS] = 0;
    out[CARD_OFF_HASPOKEDEX] = 1;
    put_u16(out + CARD_OFF_TRAINERID, (uint16_t)(lp->trainer_id & 0xFFFF));
    memset(out + CARD_OFF_VAR28, 0xFF, 8);
    memcpy(out + CARD_OFF_NAME, lp->name, 8);
}

#define LP_NAME_NEVER_READ    (GEN3_LP_PROGRESS_INDEX + 1)
#define LP_NAME_PROGRESS_COPY (GEN3_LP_PROGRESS_INDEX + 2)

uint8_t gen3_link_player_progress(const Gen3LinkPlayer* lp) {
    return lp->name[GEN3_LP_PROGRESS_INDEX];
}

void gen3_link_player_set_progress(Gen3LinkPlayer* lp, uint8_t flags) {
    lp->name[GEN3_LP_PROGRESS_INDEX] = flags;
    lp->name[LP_NAME_NEVER_READ] = 0;
    /* The receiver overwrites it with progressFlags anyway (fr: link.c: ConvertLinkPlayerName) */
    lp->name[LP_NAME_PROGRESS_COPY] = flags;
}

void gen3_trainer_card_build_frlg(
    const Gen3LinkPlayer* lp,
    uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE]) {
    gen3_trainer_card_build(lp, out);
    out[GEN3_FRLG_CARD_VERSION] = (uint8_t)(lp->version & 0xFF);
}

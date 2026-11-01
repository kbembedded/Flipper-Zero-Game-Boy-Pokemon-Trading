#include <src/gen3/gen3_hcli.h>
#include <string.h>

/* A status response's payload: the fail code and the fail offset (multiboot-and-save.md §6.4).
 * Every request leaves room for it. */
#define HCLI_FAIL_WORDS 2u

/* OK responses' payload lengths in words (multiboot-and-save.md §6.3) */
#define HCLI_HELLO_WORDS 4u
#define HCLI_CART_INFO_WORDS 102u /* 96 header words, flags, ROM size (2), lib, lib offset (2) */
#define HCLI_PROBE_WORDS 5u /* id, class, pre0 | pre1 << 8, id0 | id1 << 8, SRAM */
#define HCLI_SECTORS 32u
#define HCLI_FOOTER_WORDS 8u /* per sector: id, checksum, signature (2), counter (2), full, 0 */
#define HCLI_SECTOR_WORDS 2048u
#define HCLI_CRC_WORDS 2u

/* CART_INFO's words after the header, as byte offsets in its payload */
#define HCLI_CART_HEADER_LEN 0xC0u
#define HCLI_CART_FLAGS (2u * 96u)
#define HCLI_CART_ROM_SIZE (2u * 97u)
#define HCLI_CART_LIB (2u * 99u)
#define HCLI_CART_LIB_OFF (2u * 100u)

_Static_assert(
    sizeof(((Gen3Hcli*)0)->resp) >= 2u * HCLI_SECTORS * HCLI_FOOTER_WORDS,
    "READ_FOOTERS fits resp");
_Static_assert(2u * HCLI_CART_INFO_WORDS <= sizeof(((Gen3Hcli*)0)->resp), "CART_INFO fits resp");

static uint16_t hcli_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t hcli_le32(const uint8_t* p) {
    return (uint32_t)hcli_le16(p) | (uint32_t)hcli_le16(p + 2) << 16;
}

void gen3_hcli_init(Gen3Hcli* c, const Gen3HTransport* t) {
    memset(c, 0, sizeof(*c));
    c->t = *t;
}

/* No response yet for this call */
static void hcli_begin(Gen3Hcli* c) {
    c->last_status = GEN3_HST_OK;
    c->last_fail_code = 0;
    c->last_fail_off = 0;
}

/* A whole response came: Ok only for an OK one of exactly ok_words. */
static Gen3HcliResult
    hcli_answer(Gen3Hcli* c, const uint8_t* resp, uint8_t status, uint16_t n, uint16_t ok_words) {
    c->last_status = status;
    if(status != GEN3_HST_OK) {
        if(n != HCLI_FAIL_WORDS) return Gen3HcliBadResponse;
        c->last_fail_code = hcli_le16(resp);
        c->last_fail_off = hcli_le16(resp + 2);
        return Gen3HcliStatusError;
    }
    return n == ok_words ? Gen3HcliOk : Gen3HcliBadResponse;
}

/* Sends req with the next seq; on a link failure, the very same req again (multiboot-and-save.md
 * §6.5). req->resp must have room for max(ok_words, HCLI_FAIL_WORDS) words. */
static Gen3HcliResult hcli_run(Gen3Hcli* c, Gen3HlinkReq* req, uint16_t ok_words) {
    hcli_begin(c);
    req->seq = ++c->seq;
    req->resp_max_words = ok_words > HCLI_FAIL_WORDS ? ok_words : HCLI_FAIL_WORDS;
    for(uint32_t attempt = 0;; attempt++) {
        uint8_t status = 0;
        uint16_t n = 0;
        switch(c->t.exchange(c->t.ctx, req, &status, &n)) {
        case Gen3HlinkFailNone:
            return hcli_answer(c, req->resp, status, n, ok_words);
        case Gen3HlinkFailBusyTimeout:
            return Gen3HcliBusyTimeout;
        case Gen3HlinkFailTooLong:
            return Gen3HcliBadResponse;
        case Gen3HlinkFailNoReady:
        case Gen3HlinkFailNoResponse:
        case Gen3HlinkFailBadCrc:
        case Gen3HlinkFailWire:
        case Gen3HlinkFailSeq:
            break;
        }
        if(attempt >= GEN3_HCLI_RESENDS) return Gen3HcliLinkLost;
        c->resends++;
    }
}

/* A command whose response, OK or not, lands in c->resp */
static Gen3HcliResult hcli_small(Gen3Hcli* c, Gen3HlinkReq* req, uint16_t ok_words) {
    req->resp = c->resp;
    return hcli_run(c, req, ok_words);
}

Gen3HcliResult gen3_hcli_connect(Gen3Hcli* c, Gen3HHello* out) {
    hcli_begin(c);
    if(!c->t.sync(c->t.ctx, GEN3_HCLI_SYNC_READY, GEN3_HCLI_SYNC_US)) return Gen3HcliLinkLost;
    Gen3HlinkReq req = {.code = GEN3_HCMD_HELLO};
    const Gen3HcliResult r = hcli_small(c, &req, HCLI_HELLO_WORDS);
    if(r != Gen3HcliOk) return r;
    const uint8_t* p = c->resp;
    const uint16_t proto = hcli_le16(p);
    if(out) {
        out->proto = proto;
        out->build = hcli_le16(p + 2);
        out->max_payload = hcli_le16(p + 4);
        out->boot_mode = p[6];
        out->slave_id = p[7];
    }
    return proto == GEN3_HELPER_PROTO ? Gen3HcliOk : Gen3HcliBadResponse;
}

Gen3HcliResult gen3_hcli_cart_info(Gen3Hcli* c, Gen3HCartInfo* out) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_CART_INFO,
        .busy_us = GEN3_HCLI_BUSY_SCAN_US,
    };
    const Gen3HcliResult r = hcli_small(c, &req, HCLI_CART_INFO_WORDS);
    if(r != Gen3HcliOk) return r;
    const uint8_t* p = c->resp;
    const uint16_t lib = hcli_le16(p + HCLI_CART_LIB);
    if(lib > GEN3_HLIB_EEPROM) return Gen3HcliBadResponse;
    const uint16_t flags = hcli_le16(p + HCLI_CART_FLAGS);
    memcpy(out->header, p, HCLI_CART_HEADER_LEN);
    out->logo_ok = (flags & GEN3_HCART_LOGO_OK) != 0;
    out->fixed_ok = (flags & GEN3_HCART_FIXED_OK) != 0;
    out->complement_ok = (flags & GEN3_HCART_COMPLEMENT_OK) != 0;
    out->other_lib = (flags & GEN3_HCART_OTHER_LIB) != 0;
    out->write_game = (flags & GEN3_HCART_WRITE_GAME) != 0;
    out->rom_size = hcli_le32(p + HCLI_CART_ROM_SIZE);
    out->lib = (uint8_t)lib;
    out->lib_off = hcli_le32(p + HCLI_CART_LIB_OFF);
    return Gen3HcliOk;
}

Gen3HcliResult gen3_hcli_save_probe(Gen3Hcli* c, Gen3HProbe* out) {
    Gen3HlinkReq req = {.code = GEN3_HCMD_SAVE_PROBE};
    const Gen3HcliResult r = hcli_small(c, &req, HCLI_PROBE_WORDS);
    if(r != Gen3HcliOk) return r;
    const uint8_t* p = c->resp;
    const uint16_t cls = hcli_le16(p + 2);
    const uint16_t sram = hcli_le16(p + 8);
    if(cls > GEN3_HCHIP_NONE || sram > 1u) return Gen3HcliBadResponse;
    out->flash_id = hcli_le16(p);
    out->cls = (uint8_t)cls;
    out->pre0 = p[4];
    out->pre1 = p[5];
    out->id0 = p[6];
    out->id1 = p[7];
    out->sram = sram != 0;
    return Gen3HcliOk;
}

Gen3HcliResult gen3_hcli_arm_write(
    Gen3Hcli* c,
    uint16_t flash_id,
    uint8_t target,
    uint8_t mode,
    uint32_t counter) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_ARM_WRITE,
        .args =
            {GEN3_HARM_MAGIC, flash_id, target, mode, (uint16_t)counter, (uint16_t)(counter >> 16)},
        .nargs = 6,
    };
    return hcli_small(c, &req, 0);
}

Gen3HcliResult gen3_hcli_disarm(Gen3Hcli* c) {
    Gen3HlinkReq req = {.code = GEN3_HCMD_DISARM};
    return hcli_small(c, &req, 0);
}

Gen3HcliResult gen3_hcli_read_sector(Gen3Hcli* c, uint8_t sector, uint8_t out[4096]) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_READ_SECTOR,
        .args = {sector},
        .nargs = 1,
        .resp = out,
    };
    return hcli_run(c, &req, HCLI_SECTOR_WORDS);
}

Gen3HcliResult gen3_hcli_read_footers(Gen3Hcli* c, Gen3HFooter out[32]) {
    Gen3HlinkReq req = {.code = GEN3_HCMD_READ_FOOTERS};
    const Gen3HcliResult r = hcli_small(c, &req, HCLI_SECTORS * HCLI_FOOTER_WORDS);
    if(r != Gen3HcliOk) return r;
    for(uint32_t s = 0; s < HCLI_SECTORS; s++) {
        const uint8_t* p = c->resp + 2u * HCLI_FOOTER_WORDS * s;
        out[s].id = hcli_le16(p);
        out[s].checksum = hcli_le16(p + 2);
        out[s].signature = hcli_le32(p + 4);
        out[s].counter = hcli_le32(p + 8);
        out[s].full_checksum = hcli_le16(p + 12);
    }
    return Gen3HcliOk;
}

Gen3HcliResult gen3_hcli_sector_crc32(Gen3Hcli* c, uint8_t sector, uint32_t* crc) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_SECTOR_CRC32,
        .args = {sector},
        .nargs = 1,
    };
    const Gen3HcliResult r = hcli_small(c, &req, HCLI_CRC_WORDS);
    if(r == Gen3HcliOk) *crc = hcli_le32(c->resp);
    return r;
}

Gen3HcliResult gen3_hcli_erase_sector(Gen3Hcli* c, uint8_t sector) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_ERASE_SECTOR,
        .args = {sector},
        .nargs = 1,
        .busy_us = GEN3_HCLI_BUSY_FLASH_US,
    };
    return hcli_small(c, &req, 0);
}

Gen3HcliResult gen3_hcli_write_sector(
    Gen3Hcli* c,
    uint8_t sector,
    bool defer_commit,
    const uint8_t img[4096]) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_WRITE_SECTOR,
        .args = {sector, defer_commit ? GEN3_HFLAG_DEFER_COMMIT : 0u},
        .nargs = 2,
        .data = img,
        .data_words = HCLI_SECTOR_WORDS,
        .busy_us = GEN3_HCLI_BUSY_FLASH_US,
    };
    return hcli_small(c, &req, 0);
}

Gen3HcliResult gen3_hcli_copy_sector(
    Gen3Hcli* c,
    uint8_t src,
    uint8_t dst,
    bool defer_commit,
    uint32_t counter) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_COPY_SECTOR,
        .args =
            {src,
             dst,
             defer_commit ? GEN3_HFLAG_DEFER_COMMIT : 0u,
             (uint16_t)counter,
             (uint16_t)(counter >> 16)},
        .nargs = 5,
        .busy_us = GEN3_HCLI_BUSY_FLASH_US,
    };
    return hcli_small(c, &req, 0);
}

Gen3HcliResult gen3_hcli_commit_sector(Gen3Hcli* c, uint8_t sector) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_COMMIT_SECTOR,
        .args = {sector},
        .nargs = 1,
    };
    return hcli_small(c, &req, 0);
}

Gen3HcliResult gen3_hcli_rom_read(Gen3Hcli* c, uint32_t addr, uint16_t words, uint8_t* out) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_ROM_READ,
        .args = {(uint16_t)addr, (uint16_t)(addr >> 16), words},
        .nargs = 3,
        .resp = out,
    };
    /* out holds only words words; a status response needs HCLI_FAIL_WORDS */
    if(words >= HCLI_FAIL_WORDS) return hcli_run(c, &req, words);
    const Gen3HcliResult r = hcli_small(c, &req, words);
    if(r == Gen3HcliOk) memcpy(out, c->resp, 2u * words);
    return r;
}

Gen3HcliResult gen3_hcli_rom_crc32(Gen3Hcli* c, uint32_t addr, uint32_t len, uint32_t* crc) {
    Gen3HlinkReq req = {
        .code = GEN3_HCMD_ROM_CRC32,
        .args = {(uint16_t)addr, (uint16_t)(addr >> 16), (uint16_t)len, (uint16_t)(len >> 16)},
        .nargs = 4,
    };
    const Gen3HcliResult r = hcli_small(c, &req, HCLI_CRC_WORDS);
    if(r == Gen3HcliOk) *crc = hcli_le32(c->resp);
    return r;
}

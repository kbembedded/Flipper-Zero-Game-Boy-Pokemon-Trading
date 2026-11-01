#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_hlink.h>

/* The helper client: one call per command of the Flipper <-> helper protocol
 * (multiboot-and-save.md §6.3-§6.5), on top of the packet pump (gen3_hlink.h). Pure C, no heap, no
 * file-scope state. Not ISR code: each call blocks in its transport until the pump is done, so the
 * calls run on a worker thread.
 *
 * Each call takes the next seq and sends its request. When the exchange fails on the link (no
 * READY, no response, a bad CRC, a wire fault, or a sound response for another seq, which the pump
 * rejects) it sends the very same request again, seq included, up to GEN3_HCLI_RESENDS times. The
 * helper remembers the last packet it ran and answers a write-class command (ARM_WRITE,
 * ERASE_SECTOR .. COMMIT_SECTOR) sent again from that memory, without running it again, so a resend
 * never erases, programs or commits twice. Reads simply run again.
 *
 * The client never resets the link itself: after LinkLost, or a BusyTimeout, the caller reconnects
 * with gen3_hcli_connect. Its HELLO also replaces the helper's memory of the last packet, so a seq
 * from before can't be mistaken for a resend. */

#define GEN3_HELPER_PROTO 1u /* HELLO's protocol version; also the image header's 0xBC */
#define GEN3_HELPER_ASSET "gen3_helper.mb" /* the helper image, an APP_ASSETS_PATH name */
#define GEN3_HCLI_RESENDS 3u /* resends after a request's first attempt */

/* connect: READYs in a row (multiboot-and-save.md §6.1), within GEN3_HCLI_SYNC_US. The helper may
 * still be sending a response the pump gave up on, up to GEN3_HLINK_MAX_PAYLOAD + 5 words, about
 * 2.2 s. A helper still BUSY with a lost command sends no READY until it is done (a flash command
 * up to GEN3_HCLI_BUSY_FLASH_US, CART_INFO up to GEN3_HCLI_BUSY_SCAN_US): the caller retries
 * connect for that long. */
#define GEN3_HCLI_SYNC_READY 4u
#define GEN3_HCLI_SYNC_US 3000000u

/* BUSY budgets (multiboot-and-save.md §6.5); every other command gets GEN3_HLINK_BUSY_US, 6 s.
 * With the cable out a wait can only end early, never late (gen3_hlink.h). That is never a risk to
 * the save: the helper finishes a flash command without the link, and a resend of it gets the
 * stored answer. */
/* ERASE_SECTOR, WRITE_SECTOR, COPY_SECTOR: up to 5 erase tries of up to 2.04 s each, then the
 * program and the verify (multiboot-and-save.md §3.4) */
#define GEN3_HCLI_BUSY_FLASH_US 12000000u
/* CART_INFO: its whole-ROM scan, about 6.7 s for 16 MiB and 14.5 s for 32 MiB, so that a 32 MiB
 * cart that isn't a Pokemon game still gets its answer */
#define GEN3_HCLI_BUSY_SCAN_US 20000000u

/* Commands (multiboot-and-save.md §6.3) */
#define GEN3_HCMD_HELLO 0x01u
#define GEN3_HCMD_CART_INFO 0x02u
#define GEN3_HCMD_SAVE_PROBE 0x03u
#define GEN3_HCMD_ARM_WRITE 0x04u
#define GEN3_HCMD_DISARM 0x05u
#define GEN3_HCMD_READ_SECTOR 0x10u
#define GEN3_HCMD_READ_FOOTERS 0x11u
#define GEN3_HCMD_SECTOR_CRC32 0x12u
#define GEN3_HCMD_ERASE_SECTOR 0x18u
#define GEN3_HCMD_WRITE_SECTOR 0x19u
#define GEN3_HCMD_COPY_SECTOR 0x1Au
#define GEN3_HCMD_COMMIT_SECTOR 0x1Bu
#define GEN3_HCMD_ROM_READ 0x20u
#define GEN3_HCMD_ROM_CRC32 0x22u

/* Status codes (multiboot-and-save.md §6.4). Any but OK comes with a fail code and offset. */
#define GEN3_HST_OK 0x00u
#define GEN3_HST_BAD_PACKET 0x01u
#define GEN3_HST_BAD_ARG 0x02u
#define GEN3_HST_UNSUPPORTED 0x03u /* no 128 KB flash */
#define GEN3_HST_NOT_ARMED 0x04u
#define GEN3_HST_REFUSED 0x05u /* the fail code says which check */
#define GEN3_HST_ERASE_FAIL 0x06u
#define GEN3_HST_PROGRAM_FAIL 0x07u
#define GEN3_HST_VERIFY_FAIL 0x08u
#define GEN3_HST_NOT_ERASED 0x09u

#define GEN3_HARM_MAGIC 0x5752u /* ARM_WRITE's first word, "WR" */
/* WRITE_SECTOR's and COPY_SECTOR's flags: leave 0xFF8 erased for COMMIT_SECTOR. The helper takes
 * no write without it. */
#define GEN3_HFLAG_DEFER_COMMIT 0x0001u

/* CART_INFO's flags word */
#define GEN3_HCART_LOGO_OK 0x0001u
#define GEN3_HCART_FIXED_OK 0x0002u
#define GEN3_HCART_COMPLEMENT_OK 0x0004u
#define GEN3_HCART_OTHER_LIB 0x0008u
#define GEN3_HCART_WRITE_GAME 0x0010u

/* Gen3HCartInfo.lib: the save library a ROM links, from its ID string */
#define GEN3_HLIB_NONE 0u
#define GEN3_HLIB_FLASH1M 1u /* "FLASH1M_V": 128 KB flash, as every Gen III Pokemon game */
#define GEN3_HLIB_FLASH 2u /* "FLASH_V" */
#define GEN3_HLIB_FLASH512 3u /* "FLASH512_V" */
#define GEN3_HLIB_SRAM 4u /* "SRAM_V" or "SRAM_F_V" */
#define GEN3_HLIB_EEPROM 5u /* "EEPROM_V" */

/* Gen3HProbe.cls */
#define GEN3_HCHIP_FLASH128 0u /* a 128 KB flash the games accept */
#define GEN3_HCHIP_FLASH64 1u /* a 64 KB flash */
#define GEN3_HCHIP_SRAM 2u /* a write landed: SRAM, FRAM or a reproduction */
#define GEN3_HCHIP_NONE 3u /* no flash ID: no chip, EEPROM, open bus or a reproduction */

/* How the client reaches the pump. Initialise it with designated initializers.
 * - exchange runs one request to Done or Failed (gen3_hlink_submit, the frames, then
 *   gen3_hlink_result): it returns the pump's fail and, on None, the response's status and n.
 * - sync runs gen3_hlink_sync(need, timeout_us) to Done or Failed: true on Done. */
typedef struct {
    void* ctx;
    Gen3HlinkFail (
        *exchange)(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words);
    bool (*sync)(void* ctx, uint8_t need, uint32_t timeout_us);
} Gen3HTransport;

typedef enum {
    Gen3HcliOk,
    /* The helper answered with a status but OK: last_status, last_fail_code, last_fail_off */
    Gen3HcliStatusError,
    /* 1 + GEN3_HCLI_RESENDS attempts failed on the link, or connect saw no READY */
    Gen3HcliLinkLost,
    /* The helper stayed BUSY past the command's budget (it may still be at work). At once. */
    Gen3HcliBusyTimeout,
    /* A sound response the client can't take: longer than the request allows, an OK one of the
     * wrong length, a status one without its two words, a value outside the protocol's, or HELLO
     * from another protocol version. At once. */
    Gen3HcliBadResponse,
} Gen3HcliResult;

/* Fields are read-only outside gen3_hcli.c. Never a stack local in app code (resp is 512 B): a
 * field of the session's state, or heap. */
typedef struct {
    Gen3HTransport t;
    uint8_t seq; /* the last call's */
    uint32_t resends; /* since gen3_hcli_init */
    /* The last call's response: its status and, unless OK, its fail code and offset. 0 when it got
     * no response. */
    uint8_t last_status;
    uint16_t last_fail_code, last_fail_off;
    uint8_t resp[512]; /* small responses (READ_FOOTERS, the largest, is 512 B); sector and ROM
                        * data go to the caller's buffer */
} Gen3Hcli;

typedef struct {
    uint16_t proto, build, max_payload;
    uint8_t boot_mode, slave_id; /* the BIOS's [0x020000C4] and [0x020000C5] */
} Gen3HHello;

/* CART_INFO (multiboot-and-save.md §3.2, §3.3, §3.7). About 210 B: never a stack local in app
 * code. */
typedef struct {
    uint8_t header[0xC0]; /* the cart's header, as the helper read it */
    bool logo_ok, fixed_ok, complement_ok, other_lib, write_game;
    uint32_t rom_size;
    uint8_t lib; /* GEN3_HLIB_* */
    uint32_t lib_off;
} Gen3HCartInfo;

/* SAVE_PROBE (multiboot-and-save.md §3.3) */
typedef struct {
    uint16_t flash_id; /* device << 8 | maker */
    uint8_t cls; /* GEN3_HCHIP_* */
    uint8_t pre0, pre1, id0, id1;
    bool sram;
} Gen3HProbe;

/* One READ_FOOTERS entry: a sector's footer (multiboot-and-save.md §5.2) and its checksum over all
 * 0xF80 data bytes */
typedef struct {
    uint16_t id, checksum;
    uint32_t signature, counter;
    uint16_t full_checksum;
} Gen3HFooter;

void gen3_hcli_init(Gen3Hcli* c, const Gen3HTransport* t);

/* Each call below returns Ok only with a whole, sound response; out is undefined otherwise. Its
 * buffers belong to the call until it returns. */

/* sync(GEN3_HCLI_SYNC_READY, GEN3_HCLI_SYNC_US), then HELLO. out (may be NULL) is filled from any
 * HELLO that arrives whole; BadResponse unless its proto is GEN3_HELPER_PROTO. */
Gen3HcliResult gen3_hcli_connect(Gen3Hcli* c, Gen3HHello* out);
Gen3HcliResult gen3_hcli_cart_info(Gen3Hcli* c, Gen3HCartInfo* out);
Gen3HcliResult gen3_hcli_save_probe(Gen3Hcli* c, Gen3HProbe* out);
/* target is the slot to write (0/1), mode 0 a write over the older slot or 1 a restore onto a
 * cart with no valid save, counter the slot the game loads' (multiboot-and-save.md §6.3) */
Gen3HcliResult gen3_hcli_arm_write(
    Gen3Hcli* c,
    uint16_t flash_id,
    uint8_t target,
    uint8_t mode,
    uint32_t counter);
Gen3HcliResult gen3_hcli_disarm(Gen3Hcli* c);
Gen3HcliResult gen3_hcli_read_sector(Gen3Hcli* c, uint8_t sector, uint8_t out[4096]);
Gen3HcliResult gen3_hcli_read_footers(Gen3Hcli* c, Gen3HFooter out[32]);
Gen3HcliResult gen3_hcli_sector_crc32(Gen3Hcli* c, uint8_t sector, uint32_t* crc);
Gen3HcliResult gen3_hcli_erase_sector(Gen3Hcli* c, uint8_t sector);
/* defer_commit must be true: the helper refuses a write that would program 0xFF8 (BAD_ARG) */
Gen3HcliResult
    gen3_hcli_write_sector(Gen3Hcli* c, uint8_t sector, bool defer_commit, const uint8_t img[4096]);
Gen3HcliResult gen3_hcli_copy_sector(
    Gen3Hcli* c,
    uint8_t src,
    uint8_t dst,
    bool defer_commit,
    uint32_t counter);
Gen3HcliResult gen3_hcli_commit_sector(Gen3Hcli* c, uint8_t sector);
/* words (1..2048) halfwords from addr into out, 2 * words bytes */
Gen3HcliResult gen3_hcli_rom_read(Gen3Hcli* c, uint32_t addr, uint16_t words, uint8_t* out);
/* len: 2..0x40000 bytes, even */
Gen3HcliResult gen3_hcli_rom_crc32(Gen3Hcli* c, uint32_t addr, uint32_t len, uint32_t* crc);

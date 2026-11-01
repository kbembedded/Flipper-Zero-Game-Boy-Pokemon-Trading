#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <hp_core.h>
#include <hp_link.h>

/* Test-only: drives the helper's portable core with request packets, word by word, as the GBA's
 * main loop does (multiboot-and-save.md §6.1-§6.2), and reads its responses back. Shared by the
 * helper's test files. */

#define HELPER_CMD_BAD 0xFFFFu /* helper_cmd_run: the core or its response misbehaved */
#define HELPER_CMD_RESP_MAX (HP_MAX_PAYLOAD + 4u) /* words after a response's SOF, at most */

/* A cart header (multiboot-and-save.md §2.1): an ARM branch, the logo from own (the helper's own
 * header, which the BIOS checked), title, code, maker 01, 0x96, version and the complement. */
static inline void helper_cmd_header(
    uint8_t out[0xC0],
    const uint8_t* own,
    const char* title,
    const char* code,
    uint8_t version) {
    memset(out, 0, 0xC0);
    out[0] = 0x2E;
    out[3] = 0xEA;
    memcpy(out + 4, own + 4, 156);
    memcpy(out + 0xA0, title, strlen(title));
    memcpy(out + 0xAC, code, 4);
    out[0xB0] = '0';
    out[0xB1] = '1';
    out[0xB2] = 0x96;
    out[0xBC] = version;
    uint8_t chk = 0;
    for(uint32_t i = 0xA0; i <= 0xBC; i++) chk -= out[i];
    out[0xBD] = (uint8_t)(chk - 0x19);
}

/* Steps the core through n words with no flags; returns the last step's word. */
static inline uint16_t helper_cmd_feed(Hp* hp, const uint16_t* words, uint32_t n) {
    uint16_t out = 0;
    for(uint32_t i = 0; i < n; i++) out = hp_step(hp, words[i], 0);
    return out;
}

/* Builds a request around args into out (SOF .. CRC high); returns its length in words, 5 + n. */
static inline uint32_t
    helper_cmd_make(uint16_t* out, uint8_t seq, uint8_t code, const uint16_t* args, uint16_t n) {
    out[0] = HP_SOF;
    out[1] = (uint16_t)(seq << 8 | code);
    out[2] = n;
    uint32_t crc = hp_crc32_word(hp_crc32_word(0xFFFFFFFFu, out[1]), out[2]);
    for(uint16_t i = 0; i < n; i++) {
        out[3 + i] = args[i];
        crc = hp_crc32_word(crc, args[i]);
    }
    crc = ~crc;
    out[3 + n] = (uint16_t)crc;
    out[4 + n] = (uint16_t)(crc >> 16);
    return 5u + n;
}

/* Runs the pending command and steps out its response: out gets the words after the SOF
 * (seq << 8 | status, n, the payload, CRC low, CRC high). Returns how many, n + 4, or 0 if nothing
 * was pending, hp_execute didn't return the SOF, n is over max - 4, or the core isn't hunting again
 * (HP_READY) right after. */
static inline uint32_t helper_cmd_respond(Hp* hp, uint16_t* out, uint32_t max) {
    if(max < 4 || !hp_exec_pending(hp) || hp_execute(hp) != HP_SOF || hp_exec_pending(hp)) {
        return 0;
    }
    out[0] = hp_step(hp, 0x0000, 0);
    out[1] = hp_step(hp, 0x0000, 0);
    const uint32_t len = out[1] + 4u;
    if(len > max) return 0;
    for(uint32_t i = 2; i < len; i++) out[i] = hp_step(hp, 0x0000, 0);
    return hp_step(hp, 0x0000, 0) == HP_READY ? len : 0;
}

/* Sends one request (n args), word by word, checking that the core answers HP_RECV to every word
 * but the last and HP_BUSY to that; executes it and reads the response into out
 * (HELPER_CMD_RESP_MAX words: seq << 8 | status, n, the payload at out + 2, CRC low, CRC high).
 * Returns its status, or HELPER_CMD_BAD if the core misbehaved, or the response's seq or CRC is
 * wrong. */
static inline uint16_t helper_cmd_run(
    Hp* hp,
    uint8_t seq,
    uint8_t code,
    const uint16_t* args,
    uint16_t n,
    uint16_t* out) {
    const uint16_t w1 = (uint16_t)(seq << 8 | code);
    uint32_t crc = hp_crc32_word(hp_crc32_word(0xFFFFFFFFu, w1), n);
    for(uint16_t i = 0; i < n; i++) crc = hp_crc32_word(crc, args[i]);
    crc = ~crc;
    const uint32_t len = 5u + n;
    for(uint32_t i = 0; i < len; i++) {
        uint16_t w;
        if(i == 0) {
            w = HP_SOF;
        } else if(i == 1) {
            w = w1;
        } else if(i == 2) {
            w = n;
        } else if(i < len - 2) {
            w = args[i - 3];
        } else {
            w = i == len - 2 ? (uint16_t)crc : (uint16_t)(crc >> 16);
        }
        if(hp_step(hp, w, 0) != (i + 1 < len ? HP_RECV : HP_BUSY)) return HELPER_CMD_BAD;
    }
    const uint32_t got = helper_cmd_respond(hp, out, HELPER_CMD_RESP_MAX);
    if(got == 0 || (out[0] >> 8) != seq) return HELPER_CMD_BAD;
    uint32_t rcrc = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < got - 2; i++) rcrc = hp_crc32_word(rcrc, out[i]);
    rcrc = ~rcrc;
    if((out[got - 2] | (uint32_t)out[got - 1] << 16) != rcrc) return HELPER_CMD_BAD;
    return (uint8_t)out[0];
}

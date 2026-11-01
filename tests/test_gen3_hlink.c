#include "test.h"
#include <stdbool.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/mlink_types.h>

/* The helper-link packet pump against scripted helper words, with no fake helper: each frame()
 * hands the pump the rx of the frame in flight and returns the tx it makes next. The expected
 * words and CRCs are written out (the CRCs from zlib's crc32). */

#define READY GEN3_HLINK_READY
#define BUSY GEN3_HLINK_BUSY
#define RECV GEN3_HLINK_RECV
#define SOF GEN3_HLINK_SOF
#define FRAME_US (MLINK_BURST_LEN * MLINK_TICK_US) /* a full stream-mode frame */

static Gen3Hlink h;
static uint8_t resp[64];
static uint16_t sent[GEN3_HLINK_MAX_PAYLOAD + 5u];
static uint16_t script[8192];
static uint8_t big[4096];

/* One frame: rx_words NULL passes a NULL rx (the first call). */
static MlinkTx
    frame(Gen3Hlink* p, const uint16_t* rx_words, uint8_t n, uint16_t masks, uint32_t elapsed) {
    MlinkTx tx;
    memset(&tx, 0xA5, sizeof(tx));
    if(!rx_words) {
        gen3_hlink_frame(p, NULL, &tx, elapsed);
        return tx;
    }
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = n;
    for(uint8_t t = 0; t < n; t++) rx.word[t] = rx_words[t];
    rx.timeout_mask = masks;
    gen3_hlink_frame(p, &rx, &tx, elapsed);
    return tx;
}

static void fill(uint16_t* w, uint16_t v, uint32_t n) {
    for(uint32_t i = 0; i < n; i++) w[i] = v;
}

static bool idle_tx(const MlinkTx* tx) {
    if(tx->count != MLINK_BURST_LEN) return false;
    for(uint8_t t = 0; t < MLINK_BURST_LEN; t++) {
        if(tx->word[t] != 0x0000u) return false;
    }
    return true;
}

static Gen3HlinkFail fail_of(void) {
    uint8_t status;
    uint16_t words;
    Gen3HlinkFail fail;
    gen3_hlink_result(&h, &status, &words, &fail);
    return fail;
}

/* Submits req and runs it out: the first call (rx NULL), a frame of READY, then one frame of RECV
 * per request frame, as the helper answers while it receives. Fills sent[] with the request words
 * and returns how many slots of the last request frame carried them: that frame's replies from
 * that slot on are the first the response logic sees. Returns 0 if a tx wasn't 9 words. */
static uint8_t send(const Gen3HlinkReq* req) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, req);
    const uint32_t total = 5u + req->nargs + req->data_words;
    uint16_t ready[MLINK_BURST_LEN];
    uint16_t recv[MLINK_BURST_LEN];
    fill(ready, READY, MLINK_BURST_LEN);
    fill(recv, RECV, MLINK_BURST_LEN);
    MlinkTx tx = frame(&h, NULL, 0, 0, 0);
    tx = frame(&h, ready, MLINK_BURST_LEN, 0, FRAME_US);
    uint32_t got = 0;
    for(;;) {
        if(tx.count != MLINK_BURST_LEN) return 0;
        const uint32_t k = total - got < MLINK_BURST_LEN ? total - got : MLINK_BURST_LEN;
        for(uint32_t t = 0; t < k; t++) sent[got + t] = tx.word[t];
        got += k;
        if(got == total) return (uint8_t)k;
        tx = frame(&h, recv, MLINK_BURST_LEN, 0, FRAME_US);
    }
}

/* The helper's words after the request: the first frame's slots before `skip` carry RECV (the
 * request's own replies). Frames of 9, the last one short; word index `bad` (or -1) comes back
 * timed out. Stops once the pump is Done or Failed; returns the tx of the last frame. */
static MlinkTx reply(uint8_t skip, const uint16_t* words, uint32_t n, int32_t bad) {
    MlinkTx tx;
    memset(&tx, 0, sizeof(tx));
    uint32_t i = 0;
    uint8_t slot = skip;
    while(i < n || slot) {
        uint16_t rx[MLINK_BURST_LEN];
        uint16_t masks = 0;
        fill(rx, RECV, slot);
        uint8_t c = slot;
        while(c < MLINK_BURST_LEN && i < n) {
            if((int32_t)i == bad) masks |= (uint16_t)(1u << c);
            rx[c++] = words[i++];
        }
        tx = frame(&h, rx, c, masks, mlink_frame_us(MlinkModeStream, c));
        slot = 0;
        const Gen3HlinkState s = gen3_hlink_state(&h);
        if(s == Gen3HlinkDone || s == Gen3HlinkFailed) break;
    }
    return tx;
}

/* HELLO, seq 1, with room for a 4-word response */
static const Gen3HlinkReq hello = {
    .code = 0x01,
    .seq = 1,
    .nargs = 0,
    .resp = resp,
    .resp_max_words = 4,
};

/* HELLO's OK response (multiboot-and-save.md §6.3): proto 1, build 1, 2064, boot 3 | slave 1 << 8 */
static const uint16_t hello_ok[] =
    {SOF, 0x0100, 0x0004, 0x0001, 0x0001, 0x0810, 0x0103, 0x0F9C, 0xB666};

/* A status-4 response to seq 1, n 2: fail code and offset 0 */
static const uint16_t err_resp[] = {SOF, 0x0104, 0x0002, 0x0000, 0x0000, 0x7B2C, 0x0AD7};

TEST(test_crc32_check) {
    ASSERT_EQ(gen3_crc32((const uint8_t*)"123456789", 9), 0xCBF43926u);
    const uint8_t w[4] = {0x01, 0x01, 0x00, 0x00};
    ASSERT_EQ(gen3_crc32(w, 4), 0x983AD24Eu);
    memset(big, 0xFF, sizeof(big));
    ASSERT_EQ(gen3_crc32(big, sizeof(big)), 0xF154670Au);
    ASSERT_EQ(gen3_crc32(w, 0), 0x00000000u);
    /* The register carries across calls: no init or xorout inside the update. */
    uint32_t reg = 0xFFFFFFFFu;
    reg = gen3_crc32_update(reg, (const uint8_t*)"1234", 4);
    reg = gen3_crc32_update(reg, (const uint8_t*)"56789", 5);
    ASSERT_EQ(~reg, 0xCBF43926u);
    ASSERT_EQ(gen3_crc32_update(0x12345678u, w, 0), 0x12345678u);
}

TEST(test_hlink_idle) {
    gen3_hlink_init(&h);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkIdle);
    MlinkTx tx = frame(&h, NULL, 0, 0, 0);
    ASSERT_EQ(tx.count, 0);
    uint16_t ready[MLINK_BURST_LEN];
    fill(ready, READY, MLINK_BURST_LEN);
    tx = frame(&h, ready, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.count, 0);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkIdle);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNone);
}

TEST(test_hlink_hello_words) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    /* Nothing goes out before a READY: idle words, 9 of them, to clock the helper's replies */
    MlinkTx tx = frame(&h, NULL, 0, 0, 0);
    ASSERT_TRUE(idle_tx(&tx));
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, READY, MLINK_BURST_LEN);
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    const uint16_t want[MLINK_BURST_LEN] = {SOF, 0x0101, 0x0000, 0xD24E, 0x983A, 0, 0, 0, 0};
    ASSERT_EQ(tx.count, MLINK_BURST_LEN);
    ASSERT_MEM_EQ(tx.word, want, sizeof(want));
    /* Then idle words while the response is awaited */
    fill(rx, RECV, 5);
    fill(rx + 5, BUSY, 4);
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_TRUE(idle_tx(&tx));
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
}

TEST(test_hlink_read_sector_words) {
    const Gen3HlinkReq req = {
        .code = 0x10,
        .seq = 2,
        .args = {5},
        .nargs = 1,
        .resp = resp,
        .resp_max_words = 2,
    };
    ASSERT_EQ(send(&req), 6);
    const uint16_t want[] = {SOF, 0x0210, 0x0001, 0x0005, 0x6378, 0x0D1F};
    ASSERT_MEM_EQ(sent, want, sizeof(want));
}

TEST(test_hlink_data_payload) {
    const uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};
    const Gen3HlinkReq req = {
        .code = 0x19,
        .seq = 3,
        .args = {7, 8},
        .nargs = 2,
        .data = data,
        .data_words = 2,
        .resp = resp,
        .resp_max_words = 2,
    };
    /* 9 words: the request fills its frame exactly */
    ASSERT_EQ(send(&req), 9);
    const uint16_t want[] = {SOF, 0x0319, 0x0004, 0x0007, 0x0008, 0x0201, 0x0403, 0x78DC, 0x0B83};
    ASSERT_MEM_EQ(sent, want, sizeof(want));
    /* Every reply to that frame is skipped, a decoy SOF included; the frame after it is all idle
     * words, and every reply to it is the response's. */
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, RECV, MLINK_BURST_LEN);
    rx[8] = SOF;
    MlinkTx tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_TRUE(idle_tx(&tx));
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    const uint16_t r[MLINK_BURST_LEN] = {
        SOF, 0x0300, 0x0000, 0x6145, 0x2302, READY, READY, READY, READY};
    tx = frame(&h, r, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(tx.count, 0);
}

TEST(test_hlink_long_request) {
    /* WRITE_SECTOR's shape: 2 args and a 4 KB image, n = 2050, over 229 frames */
    for(uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7u + 3u);
    const Gen3HlinkReq req = {
        .code = 0x19,
        .seq = 0x7F,
        .args = {14, 1},
        .nargs = 2,
        .data = big,
        .data_words = 2048,
        .resp = resp,
        .resp_max_words = 2,
    };
    const uint8_t last = send(&req);
    ASSERT_EQ(last, (2055 - 1) % 9 + 1);
    ASSERT_EQ(sent[0], SOF);
    ASSERT_EQ(sent[1], 0x7F19);
    ASSERT_EQ(sent[2], 2050);
    ASSERT_EQ(sent[3], 14);
    ASSERT_EQ(sent[4], 1);
    for(uint32_t k = 0; k < 2048; k++) {
        ASSERT_EQ(sent[5 + k], big[2 * k] | big[2 * k + 1] << 8);
    }
    ASSERT_EQ(sent[2053], 0xF45D);
    ASSERT_EQ(sent[2054], 0x2E83);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
}

TEST(test_hlink_response_ok) {
    const uint8_t skip = send(&hello);
    ASSERT_EQ(skip, 5);
    memset(resp, 0xEE, sizeof(resp));
    /* BUSY x3, the response, READY */
    fill(script, BUSY, 3);
    memcpy(script + 3, hello_ok, sizeof(hello_ok));
    script[12] = READY;
    MlinkTx tx = reply(skip, script, 13, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(tx.count, 0);
    uint8_t status = 0xEE;
    uint16_t n = 0xEEEE;
    Gen3HlinkFail fail = Gen3HlinkFailTooLong;
    gen3_hlink_result(&h, &status, &n, &fail);
    ASSERT_EQ(status, 0);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(fail, Gen3HlinkFailNone);
    const uint8_t want[8] = {0x01, 0x00, 0x01, 0x00, 0x10, 0x08, 0x03, 0x01};
    ASSERT_MEM_EQ(resp, want, sizeof(want));
    ASSERT_EQ(resp[8], 0xEE);
    /* Done: idle ticks, and later words are ignored */
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, SOF, MLINK_BURST_LEN);
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.count, 0);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(resp[0], 0x01);
}

TEST(test_hlink_response_same_frame) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, READY, MLINK_BURST_LEN);
    frame(&h, NULL, 0, 0, 0);
    MlinkTx tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[4], 0x983A);
    /* The request's own slots carry decoys, which the pump ignores; the SOF comes in the first
     * slot after the request's last word. */
    const uint16_t a[MLINK_BURST_LEN] = {
        SOF, BUSY, SOF, 0x0100, READY, SOF, 0x0100, 0x0004, 0x0001};
    tx = frame(&h, a, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    ASSERT_TRUE(idle_tx(&tx));
    const uint16_t b[MLINK_BURST_LEN] = {
        0x0001, 0x0810, 0x0103, 0x0F9C, 0xB666, READY, READY, READY};
    tx = frame(&h, b, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(tx.count, 0);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNone);
}

TEST(test_hlink_bad_crc) {
    const uint8_t skip = send(&hello);
    memcpy(script, hello_ok, sizeof(hello_ok));
    script[8] ^= 1u;
    reply(skip, script, 9, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBadCrc);
    /* A payload word changed: the CRC catches it too */
    send(&hello);
    memcpy(script, hello_ok, sizeof(hello_ok));
    script[5] ^= 0x0100u;
    reply(skip, script, 9, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBadCrc);
}

TEST(test_hlink_seq_mismatch) {
    const uint8_t skip = send(&hello);
    const uint16_t words[] = {SOF, 0x0200, 0x0004, 0x0001, 0x0001, 0x0810, 0x0103, 0x699D, 0x2F84};
    reply(skip, words, 9, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailSeq);
    /* The CRC is checked first: a wrong seq with a bad CRC is BadCrc */
    send(&hello);
    memcpy(script, words, sizeof(words));
    script[7] ^= 1u;
    reply(skip, script, 9, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBadCrc);
}

TEST(test_hlink_too_long) {
    Gen3HlinkReq req = hello;
    req.resp_max_words = 2;
    const uint8_t skip = send(&req);
    memset(resp, 0xEE, sizeof(resp));
    /* n = 4 over resp_max 2: the packet is read out to its end, with no payload word stored, so
     * that the helper is hunting again for the next request; only then TooLong */
    const uint16_t rx[MLINK_BURST_LEN] = {
        RECV, RECV, RECV, RECV, RECV, SOF, 0x0100, 0x0004, 0x0001};
    ASSERT_EQ(skip, 5);
    MlinkTx tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    ASSERT_TRUE(idle_tx(&tx));
    reply(0, hello_ok + 4, 4, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    tx = reply(0, hello_ok + 8, 1, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailTooLong);
    ASSERT_EQ(tx.count, 0);
    for(uint32_t i = 0; i < 8; i++) ASSERT_EQ(resp[i], 0xEE);
    /* A HELLO right after it goes out at the next READY and gets its response */
    gen3_hlink_submit(&h, &hello);
    uint16_t ready[MLINK_BURST_LEN];
    fill(ready, READY, MLINK_BURST_LEN);
    frame(&h, ready, 0, 0, MLINK_TICK_US);
    tx = frame(&h, ready, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[0], SOF);
    reply(skip, hello_ok, 9, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(resp[0], 0x01);
    /* A packet too long for resp_max is still Wire, BadCrc or Seq first: those are resent */
    send(&req);
    reply(skip, hello_ok, 9, 5);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    send(&req);
    memcpy(script, hello_ok, sizeof(hello_ok));
    script[8] ^= 1u;
    reply(skip, script, 9, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBadCrc);
    send(&req);
    const uint16_t other_seq[] = {
        SOF, 0x0200, 0x0004, 0x0001, 0x0001, 0x0810, 0x0103, 0x699D, 0x2F84};
    reply(skip, other_seq, 9, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailSeq);
    /* n = 2 fits resp_max 2 */
    send(&req);
    reply(skip, err_resp, 7, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    uint8_t status;
    uint16_t n;
    Gen3HlinkFail fail;
    gen3_hlink_result(&h, &status, &n, &fail);
    ASSERT_EQ(status, 0x04);
    ASSERT_EQ(n, 2);
    /* Past the protocol's limit, whatever resp_max_words says: no sound packet has it, so it is a
     * wire fault (multiboot-and-save.md §6.2: drop and hunt again), at w2 at once, since there is
     * no length to read out to; the client resends */
    req.resp_max_words = 0xFFFF;
    send(&req);
    const uint16_t huge[] = {SOF, 0x0100, GEN3_HLINK_MAX_PAYLOAD + 1u};
    MlinkTx last = reply(skip, huge, 3, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    ASSERT_EQ(last.count, 0);
    send(&req);
    const uint16_t top[] = {SOF, 0x0100, 0xFFFF};
    reply(skip, top, 3, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    /* The limit itself is a length: the packet is read out, then TooLong for this resp_max */
    req.resp_max_words = 2;
    send(&req);
    const uint16_t at_max[] = {SOF, 0x0100, GEN3_HLINK_MAX_PAYLOAD};
    reply(skip, at_max, 3, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
}

TEST(test_hlink_wire_error) {
    /* A timed-out payload word: the rest of the packet is still read, so that the next request
     * meets a helper that is hunting again, and then Wire. */
    const uint8_t skip = send(&hello);
    reply(skip, hello_ok, 8, 4);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    reply(0, hello_ok + 8, 1, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    /* A framing error counts the same */
    send(&hello);
    uint16_t rx[MLINK_BURST_LEN] = {RECV, RECV, RECV, RECV, RECV, SOF, 0x0100, 0x0004, 0x0001};
    MlinkRx r;
    memset(&r, 0, sizeof(r));
    r.count = MLINK_BURST_LEN;
    memcpy(r.word, rx, sizeof(rx));
    r.framing_mask = 1u << 8;
    MlinkTx tx;
    gen3_hlink_frame(&h, &r, &tx, FRAME_US);
    reply(0, hello_ok + 4, 5, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    /* Erased flash reads 0xFFFF, as a timed-out word does: Wire even when the CRC matches */
    Gen3HlinkReq req = hello;
    req.resp_max_words = 2;
    send(&req);
    const uint16_t erased[] = {SOF, 0x0100, 0x0002, 0xFFFF, 0xFFFF, 0x5535, 0x5026};
    reply(skip, erased, 7, 3);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    send(&req);
    reply(skip, erased, 7, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    /* A timed-out w1: the length still comes, so the packet is read out */
    send(&hello);
    reply(skip, hello_ok, 3, 1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    reply(0, hello_ok + 3, 6, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    /* A timed-out n: the length is unknown, so Wire at once */
    send(&hello);
    reply(skip, hello_ok, 3, 2);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
}

TEST(test_hlink_no_response) {
    const uint8_t skip = send(&hello);
    fill(script, READY, 64);
    reply(skip, script, GEN3_HLINK_NO_RESPONSE_XFERS - 1u, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    reply(0, script, 1, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoResponse);
    /* 33 READY after the request */
    send(&hello);
    reply(skip, script, 33, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoResponse);
    /* Timed-out words count; a BUSY starts the count again */
    send(&hello);
    fill(script, RECV, 31);
    script[31] = BUSY;
    fill(script + 32, 0x1234, 31);
    reply(skip, script, 63, 5);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    reply(0, script, 1, 0);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoResponse);
    /* A timed-out word is neither BUSY nor a SOF, whatever it reads */
    send(&hello);
    fill(script, READY, 31);
    script[31] = BUSY;
    reply(skip, script, 32, 31);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoResponse);
    send(&hello);
    memcpy(script + 31, hello_ok, sizeof(hello_ok));
    reply(skip, script, 40, 31);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoResponse);
}

/* After the request: BUSY for `frames` full frames (the first one shares the request's last frame),
 * then the response if `respond`. */
static void busy_then(const Gen3HlinkReq* req, uint32_t frames, bool respond) {
    const uint8_t skip = send(req);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, RECV, skip);
    fill(rx + skip, BUSY, MLINK_BURST_LEN - skip);
    frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    fill(rx, BUSY, MLINK_BURST_LEN);
    for(uint32_t f = 1; f < frames && gen3_hlink_state(&h) == Gen3HlinkWaiting; f++) {
        frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    }
    if(respond) reply(0, hello_ok, 9, -1);
}

TEST(test_hlink_busy_timeout) {
    /* 6.1 s of BUSY */
    busy_then(&hello, 6100000u / FRAME_US + 1u, false);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
    /* 5.9 s, then the response */
    busy_then(&hello, 5900000u / FRAME_US, true);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNone);
    /* The edge: still waiting at GEN3_HLINK_BUSY_US, out one frame past it */
    busy_then(&hello, GEN3_HLINK_BUSY_US / FRAME_US, false);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, BUSY, MLINK_BURST_LEN);
    frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
}

TEST(test_hlink_busy_clock_per_wait) {
    /* 5 s of BUSY before the request and 5 s after it: two waits, each in budget */
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, BUSY, MLINK_BURST_LEN);
    for(uint32_t f = 0; f < 5000000u / FRAME_US; f++) frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    rx[8] = READY;
    MlinkTx tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[0], SOF);
    fill(rx, RECV, 5);
    fill(rx + 5, BUSY, 4);
    frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    fill(rx, BUSY, MLINK_BURST_LEN);
    for(uint32_t f = 1; f < 5000000u / FRAME_US; f++) frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    reply(0, hello_ok, 9, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    /* Time before a wait's first BUSY doesn't count, even after BUSY before the request */
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    fill(rx, BUSY, MLINK_BURST_LEN);
    rx[8] = READY;
    frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    fill(rx, RECV, 5);
    fill(rx + 5, READY, 4);
    frame(&h, rx, MLINK_BURST_LEN, 0, GEN3_HLINK_BUSY_US + 1u);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    rx[0] = BUSY;
    frame(&h, rx, 1, 0, GEN3_HLINK_BUSY_US + 1u);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
}

TEST(test_hlink_busy_per_request) {
    Gen3HlinkReq req = hello;
    req.busy_us = 12000000u;
    busy_then(&req, 11900000u / FRAME_US, true);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    busy_then(&req, 12100000u / FRAME_US + 1u, false);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
    /* The default stands for 0 */
    req.busy_us = 0;
    busy_then(&req, 6100000u / FRAME_US + 1u, false);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
    /* The request's budget holds before the request too: 8 s of BUSY, then READY */
    req.busy_us = 12000000u;
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &req);
    frame(&h, NULL, 0, 0, 0);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, BUSY, MLINK_BURST_LEN);
    for(uint32_t f = 0; f < 8000000u / FRAME_US; f++) frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    fill(rx, READY, MLINK_BURST_LEN);
    MlinkTx tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[0], SOF);
}

TEST(test_hlink_waits_for_ready) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    MlinkTx tx = frame(&h, NULL, 0, 0, 0);
    ASSERT_TRUE(idle_tx(&tx));
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, RECV, MLINK_BURST_LEN);
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_TRUE(idle_tx(&tx));
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_TRUE(idle_tx(&tx));
    /* RECV 19 and 20, then READY: the SOF goes out in the next frame */
    rx[2] = READY;
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.count, MLINK_BURST_LEN);
    ASSERT_EQ(tx.word[0], SOF);
    ASSERT_EQ(tx.word[1], 0x0101);
    /* A timed-out READY doesn't count; BUSY waits */
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    fill(rx, READY, MLINK_BURST_LEN);
    tx = frame(&h, rx, MLINK_BURST_LEN, 0x1FF, FRAME_US);
    ASSERT_TRUE(idle_tx(&tx));
    fill(rx, BUSY, MLINK_BURST_LEN);
    for(uint32_t f = 0; f < 100; f++) {
        tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
        ASSERT_TRUE(idle_tx(&tx));
    }
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    rx[8] = READY;
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[0], SOF);
}

TEST(test_hlink_no_ready) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, RECV, MLINK_BURST_LEN);
    /* 63 */
    MlinkTx tx;
    for(uint32_t f = 0; f < 7; f++) tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    ASSERT_TRUE(idle_tx(&tx));
    /* the 64th, timed out */
    tx = frame(&h, rx, 1, 1, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoReady);
    ASSERT_EQ(tx.count, 0);
    /* RECV x65 */
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    for(uint32_t f = 0; f < 7; f++) frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    frame(&h, rx, 2, 0, FRAME_US);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoReady);
    /* BUSY before the request: the request's budget, then BusyTimeout */
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    fill(rx, BUSY, MLINK_BURST_LEN);
    uint32_t f = 0;
    while(gen3_hlink_state(&h) == Gen3HlinkSending && f < 1000) {
        frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
        f++;
    }
    ASSERT_EQ(f, GEN3_HLINK_BUSY_US / FRAME_US + 1u);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
}

TEST(test_hlink_sync4) {
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 4, 2000000u);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSync);
    MlinkTx tx = frame(&h, NULL, 0, 0, 0);
    ASSERT_TRUE(idle_tx(&tx));
    /* READY READY BUSY READY READY READY, then BUSY: the run of 3 starts again */
    const uint16_t a[MLINK_BURST_LEN] = {
        READY, READY, BUSY, READY, READY, READY, BUSY, READY, READY};
    tx = frame(&h, a, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSync);
    ASSERT_TRUE(idle_tx(&tx));
    /* READY READY BUSY READY READY READY READY: Done at the 7th word */
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 4, 2000000u);
    frame(&h, NULL, 0, 0, 0);
    const uint16_t b[MLINK_BURST_LEN] = {
        READY, READY, BUSY, READY, READY, READY, READY, BUSY, SOF};
    tx = frame(&h, b, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(tx.count, 0);
    uint8_t status = 0xEE;
    uint16_t n = 0xEEEE;
    Gen3HlinkFail fail = Gen3HlinkFailWire;
    gen3_hlink_result(&h, &status, &n, &fail);
    ASSERT_EQ(status, 0);
    ASSERT_EQ(n, 0);
    ASSERT_EQ(fail, Gen3HlinkFailNone);
    /* A timed-out READY resets the run */
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 4, 2000000u);
    frame(&h, NULL, 0, 0, 0);
    fill(script, READY, MLINK_BURST_LEN);
    tx = frame(&h, script, 7, 1u << 3, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSync);
    tx = frame(&h, script, 1, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    /* need 0 counts as 1 */
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 0, 2000000u);
    tx = frame(&h, NULL, 0, 0, 0);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSync);
    tx = frame(&h, script, 1, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
}

TEST(test_hlink_sync_timeout) {
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 4, 2000000u);
    frame(&h, NULL, 0, 0, 0);
    uint16_t rx[MLINK_BURST_LEN] = {READY, READY, READY, BUSY, READY, READY, READY, BUSY, BUSY};
    uint32_t f = 0;
    while(gen3_hlink_state(&h) == Gen3HlinkSync && f < 1000) {
        frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
        f++;
    }
    ASSERT_EQ(f, (2000000u + FRAME_US - 1u) / FRAME_US);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoReady);
    /* A run that completes in the last frame still ends Done */
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 4, 2000000u);
    frame(&h, NULL, 0, 0, 0);
    for(uint32_t i = 1; i < f; i++) frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSync);
    fill(rx, READY, MLINK_BURST_LEN);
    frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
}

TEST(test_hlink_handoff) {
    /* submit and sync act only in Idle, Done or Failed */
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    Gen3HlinkReq other = hello;
    other.seq = 9;
    gen3_hlink_submit(&h, &other);
    gen3_hlink_sync(&h, 4, 1000u);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    uint16_t rx[MLINK_BURST_LEN];
    fill(rx, READY, MLINK_BURST_LEN);
    frame(&h, NULL, 0, 0, 0);
    MlinkTx tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[1], 0x0101);
    /* A new request after Done or Failed resets the results */
    reply(5, err_resp, 7, -1);
    uint8_t status;
    uint16_t n;
    Gen3HlinkFail fail;
    gen3_hlink_result(&h, &status, &n, &fail);
    ASSERT_EQ(status, 0x04);
    ASSERT_EQ(n, 2);
    gen3_hlink_submit(&h, &hello);
    gen3_hlink_result(&h, &status, &n, &fail);
    ASSERT_EQ(status, 0);
    ASSERT_EQ(n, 0);
    ASSERT_EQ(fail, Gen3HlinkFailNone);
    frame(&h, rx, 0, 0, MLINK_TICK_US);
    frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    fill(script, READY, 64);
    reply(5, script, 40, -1);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNoResponse);
    gen3_hlink_submit(&h, &other);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    ASSERT_EQ(fail_of(), Gen3HlinkFailNone);
    frame(&h, rx, 0, 0, MLINK_TICK_US);
    tx = frame(&h, rx, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[1], 0x0901);
}

TEST(test_hlink_request_too_long) {
    /* A request the pump can't frame fails at once, with no transfer */
    gen3_hlink_init(&h);
    Gen3HlinkReq req = hello;
    req.nargs = GEN3_HLINK_MAX_ARGS + 1u;
    gen3_hlink_submit(&h, &req);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailTooLong);
    MlinkTx tx = frame(&h, NULL, 0, 0, 0);
    ASSERT_EQ(tx.count, 0);
    req.nargs = 1;
    req.data = big;
    req.data_words = GEN3_HLINK_MAX_PAYLOAD;
    gen3_hlink_submit(&h, &req);
    ASSERT_EQ(fail_of(), Gen3HlinkFailTooLong);
    /* The longest that fits is taken */
    req.data_words = GEN3_HLINK_MAX_PAYLOAD - 1u;
    gen3_hlink_submit(&h, &req);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
}

/* ---- Not ready: the Normal wire's slave that never got ready (D3, D4) ---- */

/* A not-ready frame: 9 words, none clocked, as the Normal wire reports a slave that stalled
 * MLINK_NORMAL_STALL_TICKS ticks in every slot. */
#define NR_FRAME_US (MLINK_BURST_LEN * MLINK_NORMAL_STALL_TICKS * MLINK_TICK_US)

/* A frame of `words`, with the slots in `nr` (a bit mask) not ready. */
static MlinkTx nr_frame(const uint16_t* words, uint8_t n, uint16_t nr, uint32_t elapsed) {
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = n;
    for(uint8_t t = 0; t < n; t++) {
        rx.word[t] = ((nr >> t) & 1u) ? MLINK_NO_DATA : words[t];
    }
    rx.notready_mask = nr;
    rx.timeout_mask = nr;
    MlinkTx tx;
    memset(&tx, 0xA5, sizeof(tx));
    gen3_hlink_frame(&h, &rx, &tx, elapsed);
    return tx;
}

static MlinkTx nr_all(uint32_t frames) {
    uint16_t w[MLINK_BURST_LEN];
    fill(w, 0, MLINK_BURST_LEN);
    MlinkTx tx;
    memset(&tx, 0, sizeof(tx));
    for(uint32_t f = 0; f < frames; f++) tx = nr_frame(w, MLINK_BURST_LEN, 0x1FFu, NR_FRAME_US);
    return tx;
}

/* HELLO's request has gone out and its replies are skipped */
static void hello_sent(uint32_t busy_us) {
    Gen3HlinkReq r = hello;
    r.busy_us = busy_us;
    send(&r);
}

TEST(test_hlink_notready_waiting_is_busy) {
    ASSERT_EQ(NR_FRAME_US, 154512u);
    hello_sent(0);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    /* 37 frames, 333 words: far over NO_RESPONSE_XFERS, and the helper is still busy */
    nr_all(37);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    reply(0, hello_ok, 9, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
}

TEST(test_hlink_notready_busy_budget) {
    hello_sent(0);
    nr_all(38);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    nr_all(1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
}

TEST(test_hlink_notready_before_ready) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    MlinkTx tx = nr_all(20); /* 180 words */
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    ASSERT_TRUE(idle_tx(&tx));
    uint16_t ready[MLINK_BURST_LEN];
    fill(ready, READY, MLINK_BURST_LEN);
    tx = frame(&h, ready, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting); /* the whole request fits one frame */
    ASSERT_EQ(tx.count, MLINK_BURST_LEN);
    ASSERT_EQ(tx.word[0], SOF);
}

TEST(test_hlink_notready_in_response_is_wire) {
    hello_sent(0); /* HELLO is 5 words: the first frame skips their replies */
    /* SOF, w1, n = 2, a word never delivered, then the rest */
    const uint16_t w[MLINK_BURST_LEN] = {
        RECV, RECV, RECV, RECV, RECV, SOF, 0x0104, 0x0002, 0};
    nr_frame(w, MLINK_BURST_LEN, 1u << 8, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    const uint16_t rest[3] = {0, 0x7B2C, 0x0AD7};
    nr_frame(rest, 3, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
}

TEST(test_hlink_notready_sync_resets_run) {
    gen3_hlink_init(&h);
    gen3_hlink_sync(&h, 4, 3000000u);
    frame(&h, NULL, 0, 0, 0);
    const uint16_t a[MLINK_BURST_LEN] = {READY, READY, 0, READY, READY, READY};
    nr_frame(a, 6, 1u << 2, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSync);
    const uint16_t b[1] = {READY};
    nr_frame(b, 1, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
}

/* Before READY, a helper that is never ready is waited for like a BUSY one: the request fails
 * BusyTimeout once its BUSY budget (the default 6 s) is over, as after the request
 * (test_hlink_notready_busy_budget). */
TEST(test_hlink_notready_before_ready_budget) {
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &hello);
    frame(&h, NULL, 0, 0, 0);
    nr_all(38);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    nr_all(1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailBusyTimeout);
}

/* The cable pulled while a request goes out (after its READY): GEN3_HLINK_NOTREADY_RUN not-ready
 * words in a row fail the packet as Wire at once, and the rest of it isn't sent. A delivered word
 * starts the run again. */
TEST(test_hlink_notready_mid_request_is_wire) {
    ASSERT_EQ(GEN3_HLINK_NOTREADY_RUN, 16);
    /* WRITE_SECTOR's shape (test_hlink_long_request): 2055 words */
    for(uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7u + 3u);
    const Gen3HlinkReq req = {
        .code = 0x19,
        .seq = 0x7F,
        .args = {14, 1},
        .nargs = 2,
        .data = big,
        .data_words = 2048,
        .resp = resp,
        .resp_max_words = 2,
    };
    gen3_hlink_init(&h);
    gen3_hlink_submit(&h, &req);
    frame(&h, NULL, 0, 0, 0);
    uint16_t w[MLINK_BURST_LEN];
    fill(w, READY, MLINK_BURST_LEN);
    MlinkTx tx = frame(&h, w, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(tx.word[0], SOF);
    fill(w, RECV, MLINK_BURST_LEN);
    /* 9 not ready, then 6 more (15 in a row), a delivered word, 2 not ready; then 9 */
    nr_frame(w, MLINK_BURST_LEN, 0x1FFu, FRAME_US);
    nr_frame(w, MLINK_BURST_LEN, 0x1BFu, FRAME_US);
    tx = nr_frame(w, MLINK_BURST_LEN, 0x1FFu, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkSending);
    ASSERT_EQ(tx.count, MLINK_BURST_LEN);
    ASSERT_EQ(tx.word[0], big[2 * (3 * 9 - 5)] | big[2 * (3 * 9 - 5) + 1] << 8);
    /* 11 in a row so far: slot 4 is the 16th */
    tx = nr_frame(w, MLINK_BURST_LEN, 0x01Fu, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    ASSERT_EQ(tx.count, 0);
}

/* The cable pulled in a response, after its SOF: as in a request, GEN3_HLINK_NOTREADY_RUN
 * not-ready words in a row fail it as Wire at once, not after its n words. */
TEST(test_hlink_notready_mid_response_is_wire) {
    hello_sent(0); /* HELLO is 5 words: the first frame skips their replies */
    /* SOF, w1, n = 2000 (read out, too long for resp), a payload word */
    const uint16_t a[MLINK_BURST_LEN] = {RECV, RECV, RECV, RECV, RECV, SOF, 0x0100, 2000, 0};
    nr_frame(a, MLINK_BURST_LEN, 0, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    uint16_t w[MLINK_BURST_LEN];
    fill(w, 0, MLINK_BURST_LEN);
    /* 15 not ready in a row, a delivered word, then 11: still reading */
    nr_frame(w, MLINK_BURST_LEN, 0x1FFu, FRAME_US);
    nr_frame(w, MLINK_BURST_LEN, 0x1BFu, FRAME_US);
    nr_frame(w, MLINK_BURST_LEN, 0x1FFu, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkReceiving);
    /* slot 4 is the 16th in a row */
    MlinkTx tx = nr_frame(w, MLINK_BURST_LEN, 0x01Fu, FRAME_US);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkFailed);
    ASSERT_EQ(fail_of(), Gen3HlinkFailWire);
    ASSERT_EQ(tx.count, 0);
}

/* For the log: the not-ready words of the frames the pump took from the submit (or sync) to Done
 * or Failed, up to UINT16_MAX; the next submit or sync starts at 0 again. */
TEST(test_hlink_notready_count) {
    hello_sent(0);
    ASSERT_EQ(gen3_hlink_notready(&h), 0);
    nr_all(3);
    const uint16_t w[MLINK_BURST_LEN] = {0, 0, 0, 0, 0, SOF, 0x0100, 0x0004, 0x0001};
    nr_frame(w, MLINK_BURST_LEN, 0x01Fu, FRAME_US);
    reply(0, hello_ok + 4, 5, -1);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkDone);
    ASSERT_EQ(gen3_hlink_notready(&h), 3 * 9 + 5);
    nr_all(1); /* settled: not taken */
    ASSERT_EQ(gen3_hlink_notready(&h), 3 * 9 + 5);
    gen3_hlink_sync(&h, 1, 1000000u);
    ASSERT_EQ(gen3_hlink_notready(&h), 0);
    nr_all(2);
    ASSERT_EQ(gen3_hlink_notready(&h), 18);
    /* A long wait saturates */
    hello_sent(UINT32_MAX);
    nr_all(UINT16_MAX / 9u + 1u);
    ASSERT_EQ(gen3_hlink_state(&h), Gen3HlinkWaiting);
    ASSERT_EQ(gen3_hlink_notready(&h), UINT16_MAX);
}

int main(void) {
    RUN(test_crc32_check);
    RUN(test_hlink_idle);
    RUN(test_hlink_hello_words);
    RUN(test_hlink_read_sector_words);
    RUN(test_hlink_data_payload);
    RUN(test_hlink_long_request);
    RUN(test_hlink_response_ok);
    RUN(test_hlink_response_same_frame);
    RUN(test_hlink_bad_crc);
    RUN(test_hlink_seq_mismatch);
    RUN(test_hlink_too_long);
    RUN(test_hlink_wire_error);
    RUN(test_hlink_no_response);
    RUN(test_hlink_busy_timeout);
    RUN(test_hlink_busy_clock_per_wait);
    RUN(test_hlink_busy_per_request);
    RUN(test_hlink_waits_for_ready);
    RUN(test_hlink_no_ready);
    RUN(test_hlink_sync4);
    RUN(test_hlink_sync_timeout);
    RUN(test_hlink_handoff);
    RUN(test_hlink_request_too_long);
    RUN(test_hlink_notready_waiting_is_busy);
    RUN(test_hlink_notready_busy_budget);
    RUN(test_hlink_notready_before_ready);
    RUN(test_hlink_notready_in_response_is_wire);
    RUN(test_hlink_notready_sync_resets_run);
    RUN(test_hlink_notready_before_ready_budget);
    RUN(test_hlink_notready_mid_request_is_wire);
    RUN(test_hlink_notready_mid_response_is_wire);
    RUN(test_hlink_notready_count);
    TEST_EXIT();
}

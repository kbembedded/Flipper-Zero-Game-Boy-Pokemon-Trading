#include "test.h"
#include "fake_gba.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <hp_core.h>
#include <hp_link.h>
#include <hp_version.h>

/* The helper's portable core on the host, stepped word by word as the GBA's main loop steps it:
 * the packet layer (multiboot-and-save.md §6.1-§6.2, with the early length check) and HELLO. */

static Hp hp;
static FakeGba gba;

/* HELLO, seq 1: SOF, seq << 8 | code, n, then the CRC-32 0x983AD24E, low half first. */
static const uint16_t hello1[5] = {0x55AA, 0x0101, 0x0000, 0xD24E, 0x983A};

/* Its response: seq 1, OK, n 4, then proto 1, build 1, max payload 2064, boot mode 3 | slave 1
 * << 8, CRC-32 0xB6660F9C. hp_execute returns the SOF; hp_step returns the rest. */
static const uint16_t hello1_resp[8] =
    {0x0100, 0x0004, 0x0001, 0x0001, 0x0810, 0x0103, 0x0F9C, 0xB666};

static void setup(void) {
    fake_gba_init(&gba);
    hp_init(&hp);
}

/* Runs the pending command and checks that its response is exactly resp (n words after the SOF),
 * then HP_READY. */
static bool served(const uint16_t* resp, uint32_t n) {
    static uint16_t got[HELPER_CMD_RESP_MAX];
    return helper_cmd_respond(&hp, got, HELPER_CMD_RESP_MAX) == n &&
           memcmp(got, resp, n * sizeof(uint16_t)) == 0;
}

/* A good HELLO after whatever came before it is served in full. */
static bool hello_served(void) {
    return helper_cmd_feed(&hp, hello1, 5) == HP_BUSY && served(hello1_resp, 8);
}

TEST(test_hp_crc32_check) {
    ASSERT_EQ((uint32_t)~hp_crc32_bytes(0xFFFFFFFFu, (const uint8_t*)"123456789", 9), 0xCBF43926u);
    ASSERT_EQ((uint32_t)~hp_crc32_word(hp_crc32_word(0xFFFFFFFFu, 0x0101), 0x0000), 0x983AD24Eu);
    const uint8_t le[2] = {0x01, 0x01};
    ASSERT_EQ(hp_crc32_word(0x12345678u, 0x0101), hp_crc32_bytes(0x12345678u, le, 2));
}

TEST(test_hp_idle_ready) {
    setup();
    for(int i = 0; i < 10; i++) {
        ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
        ASSERT_TRUE(!hp_exec_pending(&hp));
    }
}

/* From the SOF on, H answers HP_RECV until the packet's last word; after a valid last word it
 * answers HP_BUSY, since hp_execute runs before the next transfer. */
TEST(test_hp_recv_word) {
    setup();
    for(int i = 0; i < 4; i++) {
        ASSERT_EQ(hp_step(&hp, hello1[i], 0), HP_RECV);
        ASSERT_TRUE(!hp_exec_pending(&hp));
    }
    ASSERT_EQ(hp_step(&hp, hello1[4], 0), HP_BUSY);
    ASSERT_TRUE(hp_exec_pending(&hp));
}

TEST(test_hp_hello) {
    setup();
    helper_cmd_feed(&hp, hello1, 5);
    ASSERT_TRUE(hp_exec_pending(&hp));
    ASSERT_EQ(hp_execute(&hp), HP_SOF);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    for(int i = 0; i < 8; i++) ASSERT_EQ(hp_step(&hp, 0x0000, 0), hello1_resp[i]);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_EQ(HP_BUILD_ID, 1);
    ASSERT_EQ(HP_PROTO_VERSION, 1);
}

/* While the response goes out, the Flipper's words are ignored, even a SOF. */
TEST(test_hp_response_ignores_input) {
    setup();
    helper_cmd_feed(&hp, hello1, 5);
    ASSERT_EQ(hp_execute(&hp), HP_SOF);
    for(int i = 0; i < 8; i++) ASSERT_EQ(hp_step(&hp, hello1[i % 5], HP_STEP_ERR), hello1_resp[i]);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
}

/* 200 ms without a transfer ends a response too: H goes back to hunting, and the Flipper, seeing
 * HP_READY, sends its request again. */
TEST(test_hp_tx_timeout) {
    setup();
    helper_cmd_feed(&hp, hello1, 5);
    ASSERT_EQ(hp_execute(&hp), HP_SOF);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), hello1_resp[0]);
    ASSERT_EQ(hp_step(&hp, 0x0000, HP_STEP_TIMEOUT), HP_READY);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_TRUE(hello_served());
}

TEST(test_hp_bad_crc_dropped) {
    setup();
    uint16_t bad[5];
    memcpy(bad, hello1, sizeof(bad));
    bad[3] ^= 1;
    ASSERT_EQ(helper_cmd_feed(&hp, bad, 5), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_TRUE(hello_served());
}

/* n must be the command's fixed request length: checked as w2 arrives. */
TEST(test_hp_bad_length) {
    setup();
    ASSERT_EQ(helper_cmd_feed(&hp, hello1, 2), HP_RECV);
    ASSERT_EQ(hp_step(&hp, 0x0001, 0), HP_READY);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    ASSERT_TRUE(hello_served());
    /* a READ_SECTOR (n 1) that says n 0 */
    const uint16_t short_read[2] = {HP_SOF, 0x0210};
    helper_cmd_feed(&hp, short_read, 2);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_TRUE(hello_served());
}

TEST(test_hp_unknown_code) {
    setup();
    const uint16_t unknown[2] = {HP_SOF, 0x017F};
    ASSERT_EQ(helper_cmd_feed(&hp, unknown, 2), HP_RECV);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    /* ROM_SHA1 (0x21) isn't implemented: unknown too */
    const uint16_t sha1[2] = {HP_SOF, 0x0121};
    helper_cmd_feed(&hp, sha1, 2);
    ASSERT_EQ(hp_step(&hp, 0x0004, 0), HP_READY);
    ASSERT_TRUE(hello_served());
}

/* An unknown code with n = 0xFFFF is still dropped at w2, not read as a 65535-word packet. */
TEST(test_hp_unknown_code_ffff) {
    setup();
    const uint16_t unknown[3] = {HP_SOF, 0x017F, 0xFFFF};
    ASSERT_EQ(helper_cmd_feed(&hp, unknown, 3), HP_READY);
    ASSERT_TRUE(hello_served());
}

TEST(test_hp_rx_timeout) {
    setup();
    helper_cmd_feed(&hp, hello1, 2);
    ASSERT_EQ(hp_step(&hp, 0x0000, HP_STEP_TIMEOUT), HP_READY);
    ASSERT_EQ(helper_cmd_feed(&hp, hello1 + 2, 3), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    ASSERT_TRUE(hello_served());
}

TEST(test_hp_err_word) {
    setup();
    helper_cmd_feed(&hp, hello1, 2);
    ASSERT_EQ(hp_step(&hp, hello1[2], HP_STEP_ERR), HP_READY);
    ASSERT_EQ(helper_cmd_feed(&hp, hello1 + 3, 2), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    /* an error word is never taken as a SOF */
    ASSERT_EQ(hp_step(&hp, HP_SOF, HP_STEP_ERR), HP_READY);
    ASSERT_EQ(helper_cmd_feed(&hp, hello1 + 1, 4), HP_READY);
    ASSERT_TRUE(!hp_exec_pending(&hp));
    ASSERT_TRUE(hello_served());
}

/* A false SOF swallows no real one: 0x55AA can be neither a w1 (code 0xAA is unknown) nor a w2
 * (n over HP_MAX_PAYLOAD), so it starts a packet again in either place. */
TEST(test_hp_false_sof) {
    setup();
    ASSERT_EQ(hp_step(&hp, HP_SOF, 0), HP_RECV);
    ASSERT_TRUE(hello_served());
    const uint16_t sof_at_w2[2] = {HP_SOF, 0x0101};
    ASSERT_EQ(helper_cmd_feed(&hp, sof_at_w2, 2), HP_RECV);
    ASSERT_TRUE(hello_served());
}

/* A flash read before SAVE_PROBE has found a 128 KB flash answers UNSUPPORTED, with the fail code
 * and offset words every non-OK response carries (n 2). */
TEST(test_hp_unsupported) {
    setup();
    uint16_t req[8];
    const uint16_t sector = 3;
    const uint32_t len = helper_cmd_make(req, 2, HP_CMD_READ_SECTOR, &sector, 1);
    ASSERT_EQ(len, 6);
    ASSERT_EQ(helper_cmd_feed(&hp, req, len), HP_BUSY);
    /* seq 2, status 3, n 2, 0, 0, CRC-32 0x31FA68A8 */
    const uint16_t resp[6] = {0x0203, 0x0002, 0x0000, 0x0000, 0x68A8, 0x31FA};
    ASSERT_TRUE(served(resp, 6));
    ASSERT_EQ(HP_ST_UNSUPPORTED, 0x03);
}

/* Each command's fixed request length (n) is accepted, the longest included. With no cart and no
 * save chip, and these arguments (0, 0x9E37, ...): CART_INFO and SAVE_PROBE answer OK (102 and 5
 * words) and DISARM OK (0 words); ARM_WRITE (magic 0) and the ROM reads (address 0x9E370000) a bad
 * argument; the flash reads UNSUPPORTED and the writes REFUSED (2 words each). */
TEST(test_hp_request_lengths) {
    static const struct {
        uint8_t code;
        uint16_t n;
        uint8_t status;
        uint16_t resp_n;
    } cmds[] = {
        {HP_CMD_CART_INFO, 0, HP_ST_OK, 102},
        {HP_CMD_SAVE_PROBE, 0, HP_ST_OK, 5},
        {HP_CMD_ARM_WRITE, 6, HP_ST_BAD_ARG, 2},
        {HP_CMD_DISARM, 0, HP_ST_OK, 0},
        {HP_CMD_READ_SECTOR, 1, HP_ST_UNSUPPORTED, 2},
        {HP_CMD_READ_FOOTERS, 0, HP_ST_UNSUPPORTED, 2},
        {HP_CMD_SECTOR_CRC32, 1, HP_ST_UNSUPPORTED, 2},
        {HP_CMD_ERASE_SECTOR, 1, HP_ST_REFUSED, 2},
        {HP_CMD_WRITE_SECTOR, 2050, HP_ST_REFUSED, 2},
        {HP_CMD_COPY_SECTOR, 5, HP_ST_REFUSED, 2},
        {HP_CMD_COMMIT_SECTOR, 1, HP_ST_REFUSED, 2},
        {HP_CMD_ROM_READ, 3, HP_ST_BAD_ARG, 2},
        {HP_CMD_ROM_CRC32, 4, HP_ST_BAD_ARG, 2},
    };
    static uint16_t args[2050];
    static uint16_t req[2055];
    for(uint32_t i = 0; i < 2050; i++) args[i] = (uint16_t)(i * 0x9E37u);
    setup();
    for(size_t c = 0; c < sizeof(cmds) / sizeof(cmds[0]); c++) {
        const uint32_t len = helper_cmd_make(req, (uint8_t)c, cmds[c].code, args, cmds[c].n);
        for(uint32_t i = 0; i + 1 < len; i++) ASSERT_EQ(hp_step(&hp, req[i], 0), HP_RECV);
        ASSERT_EQ(hp_step(&hp, req[len - 1], 0), HP_BUSY);
        ASSERT_EQ(hp_execute(&hp), HP_SOF);
        ASSERT_EQ(hp_step(&hp, 0x0000, 0), (uint16_t)(c << 8 | cmds[c].status));
        ASSERT_EQ(hp_step(&hp, 0x0000, 0), cmds[c].resp_n);
        for(uint32_t i = 0; i < cmds[c].resp_n + 2u; i++) hp_step(&hp, 0x0000, 0);
        ASSERT_EQ(hp_step(&hp, 0x0000, 0), HP_READY);
        /* one word more or less is dropped at w2 */
        const uint16_t head[3] = {HP_SOF, req[1], (uint16_t)(cmds[c].n + 1)};
        ASSERT_EQ(helper_cmd_feed(&hp, head, 3), HP_READY);
        if(cmds[c].n) {
            const uint16_t shorter[3] = {HP_SOF, req[1], (uint16_t)(cmds[c].n - 1)};
            ASSERT_EQ(helper_cmd_feed(&hp, shorter, 3), HP_READY);
        }
    }
    ASSERT_TRUE(hello_served());
}

/* HELLO reports what the BIOS wrote into the helper's own header at 0xC4 and 0xC5. */
TEST(test_hp_hello_boot_bytes) {
    setup();
    gba.own[0xC4] = 0x00;
    gba.own[0xC5] = 0x02;
    helper_cmd_feed(&hp, hello1, 5);
    ASSERT_EQ(hp_execute(&hp), HP_SOF);
    for(int i = 0; i < 5; i++) hp_step(&hp, 0x0000, 0);
    ASSERT_EQ(hp_step(&hp, 0x0000, 0), 0x0200);
    ASSERT_TRUE(fake_gba_current() == &gba);
}

int main(void) {
    RUN(test_hp_crc32_check);
    RUN(test_hp_idle_ready);
    RUN(test_hp_recv_word);
    RUN(test_hp_hello);
    RUN(test_hp_response_ignores_input);
    RUN(test_hp_tx_timeout);
    RUN(test_hp_bad_crc_dropped);
    RUN(test_hp_bad_length);
    RUN(test_hp_unknown_code);
    RUN(test_hp_unknown_code_ffff);
    RUN(test_hp_rx_timeout);
    RUN(test_hp_err_word);
    RUN(test_hp_false_sof);
    RUN(test_hp_unsupported);
    RUN(test_hp_request_lengths);
    RUN(test_hp_hello_boot_bytes);
    TEST_EXIT();
}

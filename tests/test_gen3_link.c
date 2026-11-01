#include "test.h"
#include "link_helpers.h"

TEST(test_handshake_idle) {
    Gen3Link l;
    gen3_link_init(&l);
    for(int i = 0; i < 600; i++) {
        MlinkTx tx;
        uint16_t cmd[GEN3_CMD_LEN];
        gen3_link_tx(&l, &tx);
        ASSERT_EQ(tx.count, 1);
        ASSERT_EQ(tx.word[0], 0xB9A0);
        MlinkRx rx = make_rx(&tx, 0xFFFF);
        ASSERT_EQ(gen3_link_rx(&l, &rx, cmd), Gen3LinkEventNone);
    }
    ASSERT_TRUE(!gen3_link_connected(&l));
}

TEST(test_handshake_success) {
    Gen3Link l;
    gen3_link_init(&l);
    uint16_t s;
    ASSERT_EQ(handshake_frame(&l, 0x0000, &s), Gen3LinkEventNone);
    ASSERT_EQ(s, 0xB9A0);
    ASSERT_EQ(handshake_frame(&l, 0xB9A0, &s), Gen3LinkEventNone);
    ASSERT_EQ(s, 0xB9A0);
    ASSERT_EQ(handshake_frame(&l, 0xB9A0, &s), Gen3LinkEventConnected);
    ASSERT_EQ(s, 0x8FFF);
    ASSERT_TRUE(gen3_link_connected(&l));
    MlinkTx tx;
    gen3_link_tx(&l, &tx);
    ASSERT_EQ(tx.count, 9);
}

TEST(test_first_burst_unchecked) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    MlinkTx tx;
    uint16_t g[9] = {0xB9A0};
    uint16_t cmd[GEN3_CMD_LEN];
    ASSERT_EQ(burst(&l, &tx, g, cmd), Gen3LinkEventNone);
    Gen3LinkStats st;
    gen3_link_stats(&l, &st);
    ASSERT_EQ(st.checksum_errors, 0);
}

/* Sends {0x2222, 0x1133, 0...} in one burst; G answers all zero. */
static bool send_checksum_burst(Gen3Link* l) {
    uint16_t c[GEN3_CMD_LEN] = {0x2222, 0x1133};
    MlinkTx tx;
    uint16_t cmd[GEN3_CMD_LEN];
    if(!gen3_link_send(l, c)) return false;
    return burst(l, &tx, NULL, cmd) == Gen3LinkEventNone;
}

TEST(test_checksum_value) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(send_checksum_burst(&l));
    MlinkTx tx;
    gen3_link_tx(&l, &tx);
    ASSERT_EQ(tx.word[0], 0x3355);
}

TEST(test_checksum_match) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(send_checksum_burst(&l));
    MlinkTx tx;
    uint16_t g[9] = {0x3355};
    uint16_t cmd[GEN3_CMD_LEN];
    ASSERT_EQ(burst(&l, &tx, g, cmd), Gen3LinkEventNone);
}

TEST(test_checksum_mismatch) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(send_checksum_burst(&l));
    MlinkTx tx;
    uint16_t g[9] = {0x1234};
    uint16_t cmd[GEN3_CMD_LEN];
    ASSERT_EQ(burst(&l, &tx, g, cmd), Gen3LinkEventChecksumError);
    Gen3LinkStats st;
    gen3_link_stats(&l, &st);
    ASSERT_EQ(st.checksum_errors, 1);
}

TEST(test_checksum_mismatch_keeps_command) {
    /* Transfer #1 checks the previous burst: this burst's command is handed over anyway. */
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(send_checksum_burst(&l));
    MlinkTx tx;
    uint16_t g[9] = {0x1234, 0x2FFE, 0x0001};
    uint16_t cmd[GEN3_CMD_LEN] = {0};
    ASSERT_EQ(burst(&l, &tx, g, cmd), Gen3LinkEventChecksumError);
    ASSERT_MEM_EQ(cmd, &g[1], sizeof(cmd));
    const uint16_t none[9] = {0x1234};
    ASSERT_EQ(burst(&l, &tx, none, cmd), Gen3LinkEventChecksumError);
    for(int i = 0; i < GEN3_CMD_LEN; i++) ASSERT_EQ(cmd[i], 0); /* no command: all zero */
    Gen3LinkStats st;
    gen3_link_stats(&l, &st);
    ASSERT_EQ(st.checksum_errors, 2);
}

TEST(test_command_received) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    MlinkTx tx;
    uint16_t g[9] = {0, 0x2FFE};
    uint16_t cmd[GEN3_CMD_LEN] = {0};
    ASSERT_EQ(burst(&l, &tx, g, cmd), Gen3LinkEventCommand);
    ASSERT_EQ(cmd[0], 0x2FFE);
}

TEST(test_queue_one_per_burst) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    for(uint16_t i = 1; i <= 3; i++) {
        uint16_t c[GEN3_CMD_LEN] = {i, 0, 0, 0, 0, 0, 0, (uint16_t)(i + 10)};
        ASSERT_TRUE(gen3_link_send(&l, c));
    }
    MlinkTx tx;
    uint16_t cmd[GEN3_CMD_LEN];
    for(uint16_t i = 1; i <= 3; i++) {
        burst(&l, &tx, NULL, cmd);
        ASSERT_EQ(tx.word[1], i);
        ASSERT_EQ(tx.word[8], i + 10);
    }
    burst(&l, &tx, NULL, cmd);
    for(int i = 1; i < 9; i++) ASSERT_EQ(tx.word[i], 0);
}

TEST(test_queue_full) {
    Gen3Link l;
    gen3_link_init(&l);
    uint16_t c[GEN3_CMD_LEN] = {1};
    for(int i = 0; i < GEN3_LINK_QUEUE_LEN; i++) ASSERT_TRUE(gen3_link_send(&l, c));
    ASSERT_TRUE(!gen3_link_send(&l, c));
    ASSERT_EQ(gen3_link_queued(&l), GEN3_LINK_QUEUE_LEN);
    gen3_link_flush(&l);
    ASSERT_EQ(gen3_link_queued(&l), 0);
}

/* Connected link with a queued command; G vanishes. */
static bool drop_link(Gen3Link* l) {
    uint16_t c[GEN3_CMD_LEN] = {1};
    uint16_t g[9];
    uint16_t cmd[GEN3_CMD_LEN];
    MlinkTx tx;
    for(int i = 0; i < 9; i++) g[i] = 0xFFFF;
    gen3_link_send(l, c);
    gen3_link_send(l, c);
    return burst(l, &tx, g, cmd) == Gen3LinkEventDisconnected;
}

TEST(test_disconnect) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(drop_link(&l));
    ASSERT_TRUE(!gen3_link_connected(&l));
    ASSERT_EQ(gen3_link_queued(&l), 0);
    MlinkTx tx;
    gen3_link_tx(&l, &tx);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0xB9A0);
}

TEST(test_reconnect_after_disconnect) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(drop_link(&l));
    uint16_t s;
    handshake_frame(&l, 0x0000, &s);
    handshake_frame(&l, 0xB9A0, &s);
    ASSERT_EQ(handshake_frame(&l, 0xB9A0, &s), Gen3LinkEventConnected);
    ASSERT_EQ(s, 0x8FFF);
}

TEST(test_slave_still_handshaking) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    MlinkTx tx;
    uint16_t g[9] = {0, 0xB9A0, 0xB9A0, 0xB9A0, 0xB9A0, 0xB9A0, 0xB9A0, 0xB9A0, 0xB9A0};
    uint16_t cmd[GEN3_CMD_LEN];
    ASSERT_EQ(burst(&l, &tx, g, cmd), Gen3LinkEventDisconnected);
    ASSERT_TRUE(!gen3_link_connected(&l));
}

TEST(test_timeouts_counted) {
    Gen3Link l;
    gen3_link_init(&l);
    connect(&l);
    MlinkTx tx;
    uint16_t cmd[GEN3_CMD_LEN];
    gen3_link_tx(&l, &tx);
    MlinkRx rx = make_rx(&tx, 0);
    rx.timeout_mask = 0x0005;
    gen3_link_rx(&l, &rx, cmd);
    Gen3LinkStats st;
    gen3_link_stats(&l, &st);
    ASSERT_EQ(st.timeouts, 2);
    ASSERT_EQ(st.bursts, 1);
}

int main(void) {
    RUN(test_handshake_idle);
    RUN(test_handshake_success);
    RUN(test_first_burst_unchecked);
    RUN(test_checksum_value);
    RUN(test_checksum_match);
    RUN(test_checksum_mismatch);
    RUN(test_checksum_mismatch_keeps_command);
    RUN(test_command_received);
    RUN(test_queue_one_per_burst);
    RUN(test_queue_full);
    RUN(test_disconnect);
    RUN(test_reconnect_after_disconnect);
    RUN(test_slave_still_handshaking);
    RUN(test_timeouts_counted);
    TEST_EXIT();
}

#include "link_helpers.h"
#include <src/gen3/gen3_block.h>

static void next_burst(Gen3Link* l, uint16_t out[GEN3_CMD_LEN]) {
    MlinkTx tx;
    uint16_t cmd[GEN3_CMD_LEN];
    burst(l, &tx, NULL, cmd);
    memcpy(out, &tx.word[1], GEN3_CMD_LEN * sizeof(uint16_t));
}

TEST(test_block_header) {
    Gen3Link l;
    uint8_t data[60] = {0};
    uint16_t w[GEN3_CMD_LEN];
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(gen3_block_send(&l, data, 60, 0));
    ASSERT_EQ(gen3_link_queued(&l), 6);
    next_burst(&l, w);
    const uint16_t exp[GEN3_CMD_LEN] = {0xBBBB, 0x003C, 0x0080, 0, 0, 0, 0, 0};
    ASSERT_MEM_EQ(w, exp, sizeof(exp));
}

TEST(test_block_chunk_le) {
    Gen3Link l;
    uint8_t data[60];
    uint16_t w[GEN3_CMD_LEN];
    for(int i = 0; i < 60; i++) data[i] = (uint8_t)i;
    gen3_link_init(&l);
    connect(&l);
    ASSERT_TRUE(gen3_block_send(&l, data, 60, 0));
    next_burst(&l, w);
    next_burst(&l, w);
    const uint16_t exp[GEN3_CMD_LEN] = {0x8888, 0x0100, 0x0302, 0x0504, 0x0706, 0x0908, 0x0B0A, 0x0D0C};
    ASSERT_MEM_EQ(w, exp, sizeof(exp));
}

TEST(test_block_100_chunks) {
    Gen3Link l;
    uint8_t data[100] = {0};
    gen3_link_init(&l);
    ASSERT_TRUE(gen3_block_send(&l, data, 100, 0));
    ASSERT_EQ(gen3_link_queued(&l), 9);
}

TEST(test_block_roundtrip) {
    Gen3Link l;
    Gen3BlockRx rx;
    uint8_t data[60];
    uint32_t s = 12345;
    for(int i = 0; i < 60; i++) {
        s = s * 1103515245u + 12345u;
        data[i] = (uint8_t)(s >> 16);
    }
    gen3_link_init(&l);
    connect(&l);
    gen3_block_rx_reset(&rx);
    ASSERT_TRUE(gen3_block_send(&l, data, 60, 1));
    Gen3BlockRxResult r = Gen3BlockRxIgnored;
    for(int i = 0; i < 6; i++) {
        uint16_t w[GEN3_CMD_LEN];
        next_burst(&l, w);
        r = gen3_block_rx_feed(&rx, w);
        if(i == 0) ASSERT_EQ(r, Gen3BlockRxStarted);
        else if(i < 5) ASSERT_EQ(r, Gen3BlockRxProgress);
    }
    ASSERT_EQ(r, Gen3BlockRxDone);
    ASSERT_EQ(rx.size, 60);
    ASSERT_MEM_EQ(rx.data, data, 60);
}

TEST(test_block_too_big) {
    Gen3Link l;
    uint8_t data[257] = {0};
    gen3_link_init(&l);
    ASSERT_TRUE(!gen3_block_send(&l, data, 257, 0));
    ASSERT_EQ(gen3_link_queued(&l), 0);
    ASSERT_TRUE(!gen3_block_send(&l, data, 0, 0));
    ASSERT_EQ(gen3_link_queued(&l), 0);
}

TEST(test_block_no_room) {
    Gen3Link l;
    uint8_t data[100] = {0};
    uint16_t c[GEN3_CMD_LEN] = {0x2222};
    gen3_link_init(&l);
    for(int i = 0; i < 20; i++) ASSERT_TRUE(gen3_link_send(&l, c));
    ASSERT_TRUE(!gen3_block_send(&l, data, 100, 0));
    ASSERT_EQ(gen3_link_queued(&l), 20);
}

TEST(test_rx_chunk_without_header) {
    Gen3BlockRx rx;
    const uint16_t c[GEN3_CMD_LEN] = {0x8888, 1, 2, 3, 4, 5, 6, 7};
    gen3_block_rx_reset(&rx);
    ASSERT_EQ(gen3_block_rx_feed(&rx, c), Gen3BlockRxIgnored);
}

TEST(test_rx_oversize_header) {
    Gen3BlockRx rx;
    const uint16_t c[GEN3_CMD_LEN] = {0xBBBB, 0x0200, 0x81, 0, 0, 0, 0, 0};
    gen3_block_rx_reset(&rx);
    ASSERT_EQ(gen3_block_rx_feed(&rx, c), Gen3BlockRxBad);
}

int main(void) {
    RUN(test_block_header);
    RUN(test_block_chunk_le);
    RUN(test_block_100_chunks);
    RUN(test_block_roundtrip);
    RUN(test_block_too_big);
    RUN(test_block_no_room);
    RUN(test_rx_chunk_without_header);
    RUN(test_rx_oversize_header);
    TEST_EXIT();
}

#include "test.h"
#include <src/gen3/gen3_link_log.h>

static void burst(MlinkTx* tx, MlinkRx* rx, uint16_t t0, uint16_t r0) {
    memset(tx, 0, sizeof(*tx));
    memset(rx, 0, sizeof(*rx));
    tx->count = rx->count = MLINK_BURST_LEN;
    tx->word[0] = t0;
    tx->word[1] = t0; /* a non-zero t0 makes the burst non-idle */
    rx->word[0] = r0;
}

static void hs(MlinkTx* tx, MlinkRx* rx, uint16_t t0, uint16_t r0) {
    memset(tx, 0, sizeof(*tx));
    memset(rx, 0, sizeof(*rx));
    tx->count = rx->count = 1;
    tx->word[0] = t0;
    rx->word[0] = r0;
}

TEST(test_log_fifo) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    for(uint32_t i = 1; i <= 3; i++) {
        burst(&tx, &rx, 0x1111, 0x2222);
        gen3_log_frame(&ring, i, (uint8_t)i, &tx, &rx);
    }
    Gen3LogRec rec;
    for(uint32_t i = 1; i <= 3; i++) {
        ASSERT_TRUE(gen3_log_pop(&ring, &rec));
        ASSERT_EQ(rec.frame, i);
        ASSERT_EQ(rec.phase, i);
    }
    ASSERT_TRUE(!gen3_log_pop(&ring, &rec));
}

TEST(test_log_idle_counted) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    burst(&tx, &rx, 0x1111, 0x2222);
    gen3_log_frame(&ring, 0, 1, &tx, &rx);
    Gen3LogRec rec;
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    ASSERT_EQ(rec.idle_before, 0);
    burst(&tx, &rx, 0, 0);
    for(uint32_t i = 1; i <= 5; i++) gen3_log_frame(&ring, i, 1, &tx, &rx);
    ASSERT_TRUE(!gen3_log_pop(&ring, &rec));
    burst(&tx, &rx, 0x3355, 0);
    gen3_log_frame(&ring, 6, 1, &tx, &rx);
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    ASSERT_EQ(rec.frame, 6);
    ASSERT_EQ(rec.idle_before, 5);
    ASSERT_TRUE(!gen3_log_pop(&ring, &rec));
}

TEST(test_log_handshake_idle) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    hs(&tx, &rx, 0xB9A0, 0x0000);
    gen3_log_frame(&ring, 0, 0, &tx, &rx);
    Gen3LogRec rec;
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    hs(&tx, &rx, 0xB9A0, 0xFFFF);
    gen3_log_frame(&ring, 1, 0, &tx, &rx);
    ASSERT_TRUE(!gen3_log_pop(&ring, &rec));
    /* same data but phase changed -> stored */
    gen3_log_frame(&ring, 2, 1, &tx, &rx);
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    ASSERT_EQ(rec.idle_before, 1);
}

TEST(test_log_first_frame_not_idle) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    hs(&tx, &rx, 0, 0xFFFF);
    gen3_log_frame(&ring, 0, 0, &tx, &rx);
    Gen3LogRec rec;
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
}

TEST(test_log_overflow) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    for(uint32_t i = 0; i < 70; i++) {
        burst(&tx, &rx, 0x1111, 0x2222);
        gen3_log_frame(&ring, i, 1, &tx, &rx);
    }
    ASSERT_EQ(gen3_log_dropped(&ring), 6);
    Gen3LogRec rec;
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    ASSERT_EQ(rec.frame, 0);
    uint32_t n = 1;
    while(gen3_log_pop(&ring, &rec)) n++;
    ASSERT_EQ(n, GEN3_LOG_RING_LEN);
}

TEST(test_log_format_burst) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    burst(&tx, &rx, 0x3355, 0xB9A0);
    tx.word[1] = 0x2222;
    tx.word[2] = 0x1133;
    gen3_log_frame(&ring, 42, 1, &tx, &rx);
    Gen3LogRec rec;
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    char buf[256];
    size_t n = gen3_log_format(&rec, buf, sizeof(buf));
    const char* want =
        "00000042 p1 n9 i0 T:3355 2222 1133 0000 0000 0000 0000 0000 0000 "
        "R:B9A0 0000 0000 0000 0000 0000 0000 0000 0000 E:0000/0000\n";
    ASSERT_STR_EQ(buf, want);
    ASSERT_EQ(n, strlen(want));
}

TEST(test_log_format_handshake) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    hs(&tx, &rx, 0xB9A0, 0x0000);
    gen3_log_frame(&ring, 0, 0, &tx, &rx); /* first, non-idle */
    Gen3LogRec rec;
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    hs(&tx, &rx, 0xB9A0, 0xFFFF);
    for(uint32_t i = 0; i < 3; i++) gen3_log_frame(&ring, 1 + i, 0, &tx, &rx);
    hs(&tx, &rx, 0xB9A0, 0x0000);
    gen3_log_frame(&ring, 7, 0, &tx, &rx);
    ASSERT_TRUE(gen3_log_pop(&ring, &rec));
    char buf[128];
    gen3_log_format(&rec, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "00000007 p0 n1 i3 T:B9A0 R:0000 E:0000/0000\n");
}

typedef struct {
    int calls;
    bool ok;
} W;

static bool writer(void* ctx, const char* line, size_t len) {
    W* w = ctx;
    (void)line;
    (void)len;
    w->calls++;
    return w->ok;
}

TEST(test_log_writer_failure) {
    Gen3LogRing ring;
    gen3_log_init(&ring);
    MlinkTx tx;
    MlinkRx rx;
    W w = {0, true};
    for(uint32_t i = 0; i < 3; i++) {
        burst(&tx, &rx, 0x1111, 0x2222);
        gen3_log_frame(&ring, i, 1, &tx, &rx);
    }
    gen3_log_drain(&ring, writer, &w);
    ASSERT_EQ(w.calls, 3);
    ASSERT_TRUE(!gen3_log_failed(&ring));
    w.ok = false;
    burst(&tx, &rx, 0x1111, 0x2222);
    gen3_log_frame(&ring, 3, 1, &tx, &rx);
    gen3_log_frame(&ring, 4, 1, &tx, &rx);
    gen3_log_drain(&ring, writer, &w);
    ASSERT_EQ(w.calls, 4);
    ASSERT_TRUE(gen3_log_failed(&ring));
    gen3_log_frame(&ring, 5, 1, &tx, &rx);
    gen3_log_drain(&ring, writer, &w);
    ASSERT_EQ(w.calls, 4);
    Gen3LogRec rec;
    ASSERT_TRUE(!gen3_log_pop(&ring, &rec));
}

int main(void) {
    RUN(test_log_fifo);
    RUN(test_log_idle_counted);
    RUN(test_log_handshake_idle);
    RUN(test_log_first_frame_not_idle);
    RUN(test_log_overflow);
    RUN(test_log_format_burst);
    RUN(test_log_format_handshake);
    RUN(test_log_writer_failure);
    TEST_EXIT();
}

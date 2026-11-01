#include "test.h"
#include <src/gen3/gen3_text.h>

TEST(test_encode_name) {
    uint8_t b[11];
    ASSERT_EQ(gen3_text_encode("FLIPPER", b, 11, 7), 7);
    const uint8_t e[11] = {0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_MEM_EQ(b, e, 11);
}

TEST(test_encode_truncates) {
    uint8_t b[8];
    ASSERT_EQ(gen3_text_encode("FLIPPERZERO", b, 8, 7), 7);
    const uint8_t e[8] = {0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF};
    ASSERT_MEM_EQ(b, e, 8);
}

TEST(test_encode_charset) {
    uint8_t b[14];
    ASSERT_EQ(gen3_text_encode("az09 !?.-,/:'", b, 14, 13), 13);
    const uint8_t e[14] =
        {0xD5, 0xEE, 0xA1, 0xAA, 0x00, 0xAB, 0xAC, 0xAD, 0xAE, 0xB8, 0xBA, 0xF0, 0xB4, 0xFF};
    ASSERT_MEM_EQ(b, e, 14);
}

TEST(test_encode_unsupported) {
    uint8_t b[2];
    ASSERT_EQ(gen3_text_encode("#", b, 2, 1), 1);
    const uint8_t e[2] = {0xAC, 0xFF};
    ASSERT_MEM_EQ(b, e, 2);
}

TEST(test_decode_stops_at_eos) {
    const uint8_t in[7] = {0xBB, 0xD5, 0xA1, 0x00, 0xAB, 0xFF, 0xC0};
    char out[16];
    ASSERT_EQ(gen3_text_decode(in, 7, out, sizeof(out)), 5);
    ASSERT_STR_EQ(out, "Aa0 !");
}

TEST(test_decode_unknown) {
    const uint8_t in[2] = {0x01, 0xFF};
    char out[8];
    ASSERT_EQ(gen3_text_decode(in, 2, out, sizeof(out)), 1);
    ASSERT_STR_EQ(out, "?");
}

TEST(test_decode_no_eos_truncates) {
    const uint8_t in[3] = {0xBB, 0xBC, 0xBD};
    char out[3];
    ASSERT_EQ(gen3_text_decode(in, 3, out, sizeof(out)), 2);
    ASSERT_STR_EQ(out, "AB");
}

TEST(test_encode_gender_symbols) {
    uint8_t b[10];
    ASSERT_EQ(gen3_text_encode("NIDORAN\200", b, 10, 10), 8);
    const uint8_t e[10] = {0xC8, 0xC3, 0xBE, 0xC9, 0xCC, 0xBB, 0xC8, 0xB6, 0xFF, 0xFF};
    ASSERT_MEM_EQ(b, e, 10);

    uint8_t m[2];
    ASSERT_EQ(gen3_text_encode("\201", m, 2, 1), 1);
    const uint8_t em[2] = {0xB5, 0xFF};
    ASSERT_MEM_EQ(m, em, 2);

    uint8_t both[3];
    ASSERT_EQ(gen3_text_encode("\200\201", both, 3, 2), 2);
    const uint8_t eb[3] = {0xB6, 0xB5, 0xFF};
    ASSERT_MEM_EQ(both, eb, 3);
}

TEST(test_decode_gender_symbols) {
    const uint8_t in[3] = {0xB5, 0xB6, 0xFF};
    char out[8];
    ASSERT_EQ(gen3_text_decode(in, 3, out, sizeof(out)), 2);
    ASSERT_STR_EQ(out, "\201\200");

    const uint8_t in2[3] = {0xB6, 0xB5, 0xFF};
    ASSERT_EQ(gen3_text_decode(in2, 3, out, sizeof(out)), 2);
    ASSERT_STR_EQ(out, "\200\201");
}

TEST(test_text_encodable) {
    /* decode(encode(x)) == x */
    ASSERT_TRUE(gen3_text_encodable("Leafy", 10));
    ASSERT_TRUE(gen3_text_encodable("", 10));
    ASSERT_TRUE(gen3_text_encodable("az09 !?.-,/:'\"", 14));
    ASSERT_TRUE(gen3_text_encodable("NIDORAN\200", 10));
    ASSERT_TRUE(gen3_text_encodable("\201", 1));
    /* Unsupported chars encode as '?' and decode back as '?' */
    ASSERT_TRUE(!gen3_text_encodable("A_B", 10));
    ASSERT_TRUE(!gen3_text_encodable("x@", 10));
    ASSERT_TRUE(!gen3_text_encodable("#", 1));
    /* Past max_chars the encoding drops the tail */
    ASSERT_TRUE(gen3_text_encodable("FLIPPER", 7));
    ASSERT_TRUE(!gen3_text_encodable("FLIPPERZ", 7));
    ASSERT_TRUE(!gen3_text_encodable("A", 0));
    ASSERT_TRUE(gen3_text_encodable("", 0));
}

int main(void) {
    RUN(test_encode_name);
    RUN(test_encode_truncates);
    RUN(test_encode_charset);
    RUN(test_encode_unsupported);
    RUN(test_decode_stops_at_eos);
    RUN(test_decode_unknown);
    RUN(test_decode_no_eos_truncates);
    RUN(test_encode_gender_symbols);
    RUN(test_decode_gender_symbols);
    RUN(test_text_encodable);
    TEST_EXIT();
}
